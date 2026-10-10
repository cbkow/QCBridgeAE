// QCBridgeAE — the Mercury Transmit device (phase A6, Route A).
//
// After Effects or Premiere pushes the frames it has already rendered; this
// turns each into top-down RGBA16F in a shared ring QCView reads. Everything
// here follows from what the A4 probe measured
// (lab/results/2026-09-21-a4-transmit-probe/), and each rule says where:
//
//   * Offer ARGB_4444_32f then BGRA_4444_32f, nothing else (DESIGN-NOTES D1). The
//     host prefers 32f whenever it is offered; offering an integer tier alone
//     risks a 32 bpc project being clamped to 8 bits.
//   * Request the working colour space in every mode, as a fresh PrSDKString
//     in ioProfileRec.outName (D5; A4 sections 7-8). Under Adobe CMS an unset
//     request means a conversion to Rec.709; the raw buffer field is ignored;
//     and the host owns each string it reads, so one shared across modes left
//     every mode after the first with a spent handle.
//   * One pass: flip (frames arrive bottom-up with positive rowbytes), reorder
//     to RGBA, IEEE convert to half — never clamp, flag inf/NaN (D4).
//   * Straight alpha passes through: Premiere carries it, AE sends opaque.
//   * The ring grows, never shrinks: Premiere scrubs at fractional resolution
//     and the probe rebuilt its ring on every size switch (A4 section 13).
//     Frame geometry is per frame in FrameDesc; the ring only needs room.
//   * The ring is process-wide, not per module: a module reset starts the new
//     module before shutting down the old (A4 section "Also observed").
//   * Host state goes in the ring header, so QCView can explain a freeze —
//     above all the focus-loss pause AE applies by default (A4 section 12).
//   * A frame larger than 3840x2160 makes the device ask the host for the
//     size that fits inside it, aspect kept, so the host scales before it
//     builds the frame. Asked for at "any" size, an 8000x8000 comp costs the
//     host a 977 MiB float frame per push, built and freed inside its
//     playback loop: 13 fps on an M5 Max at 16 bpc, 0.6 fps on a Windows
//     workstation, against 24 fps once capped (measured 2026-10-06; the cap's
//     own size made no difference between 64x64 and 2160x2160, so there is
//     one cap and no setting). The size comes from the frame, never from
//     tmInstance: AE described an instance as 400x400 while pushing 8000x8000
//     through it. And it is forgotten when another instance goes live, since
//     the next comp has another aspect and a kept raster would letterbox or
//     stretch it: that costs one full-size frame per comp switch.
//   * Every pushed frame is converted. An earlier version silently skipped a
//     frame whose PPix unique key matched the last one (each viewer change
//     pushes two frames ~3 ms apart). In AE it published one frame and then
//     nothing: removing it alone brought AE back to 24 fps at 4K (A6 notes),
//     so the key does not identify content there. An unmeasured saving must
//     not be able to drop frames without a trace.
//   * Audio is a "mirror" (push) device only, never the host's primary audio
//     device or its clock (DESIGN-NOTES D6; lab/results/2026-10-10-a8-
//     transmit-audio). Premiere pushes planar float, exactly the 1024 frames
//     asked for, stamped with timeline time, on its own audio thread while
//     PushVideo runs — once the user ticks "Audio Stream" for this device in
//     Preferences > Playback. It goes into a second segment, created once at
//     Startup in Premiere and reused for the whole host session (it never
//     needs re-creating: its geometry fits any format). PushAudio touches
//     nothing but that segment: no log, no suite, no ring growth. After
//     Effects never calls any audio entry point, so it gets no segment.
//
// Privacy: a Transmit device receives no project paths or comp names; the
// only label published is the host's name (DESIGN-NOTES privacy 5, 6).

// The one exported symbol. PrSDKEntry.h has DllExport for this, but it is
// reached only through the play-module headers; spell it out here so the
// entry point below never depends on include order.
#if defined(_WIN32)
#  define QCBAE_EXPORT __declspec(dllexport)
#else
#  define QCBAE_EXPORT __attribute__((visibility("default")))
#endif

#include "PrSDKTransmit.h"
#include "PrSDKPPixSuite.h"
#include "PrSDKTimeSuite.h"
#include "PrSDKStringSuite.h"
#include "PrSDKPixelFormat.h"
#include "PrSDKColorSpaces.h"
#include "PrSDKColorProfile.h"
#include "SPBasic.h"

#include "common/convert/half_convert.h"
#include "common/surface/shared_ring.h"
#include "common/surface/audio_ring.h"
#include "common/surface/audio_publisher.h"

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace qcbae;

namespace {

// /tmp on macOS; %TEMP% on Windows (a plugin inside AE has no /tmp and
// Program Files is read-only). Same file name either side.
#if defined(_WIN32)
std::string scratch_file(const char* name) {
    const char* t = std::getenv("TEMP");
    return std::string(t && *t ? t : ".") + "\\" + name;
}
const std::string S_logPath = scratch_file("qcbridgeae-transmit.log");
const char* const kLogPath = S_logPath.c_str();
#else
constexpr const char* kLogPath = "/tmp/qcbridgeae-transmit.log";
#endif

// Persistent identity in the host's device list. Never change it, or the host
// treats the device as new and forgets the user's choice. (The A4 probe has
// its own.)
constexpr const char* kPluginGUID = "1BB013B9-952D-4D6C-B35E-1E0EF3F52CA8";

// Which host we are in decides the ring name (DESIGN-NOTES A6: one fixed name per
// host, so AE and Premiere running together do not overwrite each other).
struct Host { const char* ring; const char* label; };

// The host executable's name: getprogname() on macOS, the module path of
// the process on Windows ("Adobe Premiere Pro.exe" / "AfterFX.exe").
std::string host_program_name() {
#if defined(_WIN32)
    char buf[MAX_PATH] {};
    const DWORD n = ::GetModuleFileNameA(nullptr, buf, sizeof buf);
    std::string p(buf, n < sizeof buf ? n : sizeof buf - 1);
    const size_t slash = p.find_last_of("\\/");
    return slash == std::string::npos ? p : p.substr(slash + 1);
#else
    const char* prog = getprogname();
    return prog != nullptr ? prog : "";
#endif
}

Host detect_host() {
    const std::string p = host_program_name();
    if (p.find("Premiere") != std::string::npos) return {kRingNamePremiere, "Premiere Pro"};
    return {kRingNameAfterEffects, "After Effects"};   // AE, or an unknown MediaCore host
}

// --- logging ---------------------------------------------------------------
FILE* S_log = nullptr;

void logf(const char* fmt, ...) {
    if (S_log == nullptr) {
        S_log = std::fopen(kLogPath, "a");
        if (S_log == nullptr) return;
    }
    char stamp[32];
    const std::time_t now = std::time(nullptr);
    std::strftime(stamp, sizeof stamp, "%H:%M:%S", std::localtime(&now));
    std::fprintf(S_log, "[%s] ", stamp);
    va_list ap; va_start(ap, fmt);
    std::vfprintf(S_log, fmt, ap);
    va_end(ap);
    std::fputc('\n', S_log);
    std::fflush(S_log);
}

// --- the ring: process-wide, grow-only --------------------------------------
struct Ring {
    SharedRing ring;
    uint64_t   capacity = 0;   // bytes per slot
    Host       host {};
    uint64_t   published = 0;
    uint32_t   last_w = 0, last_h = 0;

    // Host state is derived, not last-writer-wins. AE creates and discards
    // an instance for every item it touches while opening a project (seen in
    // the A6 log: a dozen in one second), and an old instance's deactivation
    // can arrive after its replacement's activation. The ring reads Active
    // while any instance has video on; paused only when none do, with the
    // reason of the last deactivation.
    std::set<csSDK_int32> video_on;

    // Every instance between CreateInstance and DisposeInstance. When the
    // last one goes the ring is retired: on quit AE disposes its instances
    // and then calls neither Shutdown nor the unload entry (measured
    // 2026-10-05), and on macOS a ring that is never unlinked stays in the
    // kernel, pages and all, after the host is gone (582 MiB found behind a
    // dead AE). Across 119 logged sessions the count reached zero at quit and
    // otherwise only 7 times, each a pause of 9 s or more; the next frame
    // simply creates the ring again.
    std::set<csSDK_int32> instances;
    HostState last_pause = HostState::Paused;
};
Ring S;

// --- the audio segment: process-wide, created once per host session -----------
// AudioPublisher owns the mapping and the rule that PushAudio (the host's
// audio thread) and the host thread never collide over it. The host thread
// starts/stops sessions and, on the last instance or module unload, quiesces.
AudioPublisher S_audio;

// What the pushing thread may update: counters only, relaxed, read by the
// host thread for one summary line per session. Nothing here logs.
struct AudioStats {
    std::atomic<uint64_t> pushes{0}, frames{0}, packets{0};
    std::atomic<int64_t>  first_time{0}, last_time{0};
    std::atomic<uint32_t> channels{0};
};
AudioStats S_astats;
std::atomic<uint32_t> S_push_flags{0};   // AudioPacketFlags for the current session

// Premiere plays video in Playing mode without ever starting an audio
// session exactly when "Audio Stream" is unticked for this device. A dozen
// such pushes in a row (half a second at 24 fps) is the signal; one is not,
// because StartPushAudio can trail the first Playing frame.
constexpr uint32_t kPlayingPushesBeforeOff = 12;
uint32_t S_playing_without_audio = 0;

std::string audio_ring_name() { return std::string(S.host.ring) + kAudioRingSuffix; }

// Created when absent, on the host thread: at Startup, at every
// CreateInstance and at StartPushAudio. Premiere creates and disposes
// placeholder instances while opening a project, and the last disposal
// retires the segment like the frame ring (see DisposeInstance), so the
// next instance has to bring it back. Premiere only: AE never pushes audio
// (A8), and an empty segment would read as "audio off" in the viewer.
void ensure_audio_ring(PrTime ticks_per_second) {
    if (S_audio.has_ring() || ticks_per_second <= 0) return;
    if (S.host.ring == nullptr || std::strcmp(S.host.ring, kRingNamePremiere) != 0) return;
    auto ar = std::make_unique<AudioRing>();
    if (!ar->create(audio_ring_name(), ticks_per_second)) {
        logf("audio ring create failed: %s", ar->error().c_str());
        return;
    }
    const auto* h = ar->header();
    logf("audio ring %s created: %u slots x %u frames x %u ch (%llu KiB)", audio_ring_name().c_str(),
         h->slot_count, h->slot_frames, h->max_channels, (unsigned long long)(h->total_size / 1024));
    if (!S_audio.adopt(std::move(ar))) logf("audio ring adopt refused");
}

void quiesce_audio(const char* why) {
    if (!S_audio.has_ring()) return;
    if (S_audio.quiesce(50u))
        logf("audio ring retired (%s)", why);
    else
        logf("audio ring LEAKED (%s): a push was still in flight after 50 ms", why);
}

HostState derived_state() {
    return S.video_on.empty() ? S.last_pause : HostState::Active;
}
void publish_state() {
    if (S.ring.valid()) S.ring.set_host_state(derived_state());
}

// What a push costs the host's thread and how fast pushes come, over the
// frames since the last "published" line. Without it the log says that frames
// flowed and nothing about how long each took: a 1.6 s push looked like any other.
struct Window {
    uint32_t n = 0;
    double   push_sum = 0, push_max = 0;   // ms inside PushVideo, dispose included
    uint32_t gaps = 0;                     // intervals between pushes, idle ones left out
    double   gap_sum = 0;                  // seconds
    std::chrono::steady_clock::time_point last {};
    std::chrono::steady_clock::time_point line_at {};   // when the last line was written
    char     line[160] {};                 // a "published" line waiting for this push's timing
};
Window S_win;

// Every 240th frame is a line every ten seconds at 24 fps and every four
// minutes at 1 fps, which is when the timing matters most.
constexpr double kLineEverySeconds = 10.0;

// A longer wait than this between two pushes is the user doing nothing, not
// the host being slow. Far above the slowest cadence seen (1.6 s a frame).
constexpr double kIdleGapSeconds = 5.0;

void note_push(std::chrono::steady_clock::time_point began) {
    const auto now = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(now - began).count();
    ++S_win.n;
    S_win.push_sum += ms;
    if (ms > S_win.push_max) S_win.push_max = ms;
    if (S_win.last != std::chrono::steady_clock::time_point{}) {
        const double gap = std::chrono::duration<double>(began - S_win.last).count();
        if (gap < kIdleGapSeconds) { ++S_win.gaps; S_win.gap_sum += gap; }
    }
    S_win.last = began;
    if (S_win.line[0] == 0) return;

    char rate[48] = "";
    if (S_win.gaps > 0 && S_win.gap_sum > 0)
        std::snprintf(rate, sizeof rate, ", %.2f fps", S_win.gaps / S_win.gap_sum);
    logf("%s; last %u: push mean %.2f max %.2f ms%s", S_win.line, S_win.n,
         S_win.push_sum / S_win.n, S_win.push_max, rate);
    const auto last = S_win.last;
    S_win = Window{};
    S_win.last = last;
    S_win.line_at = now;
}

// The largest frame the device asks a host for. A comp inside it is taken as
// it comes; a larger one is asked for at the size that fits, aspect kept.
constexpr int32_t kMaxWidth = 3840, kMaxHeight = 2160;

struct Size { int32_t w, h; };

// {0, 0} = "any": the host sends its own size.
constexpr Size capped_size(int32_t w, int32_t h) {
    if (w <= 0 || h <= 0 || (w <= kMaxWidth && h <= kMaxHeight)) return {0, 0};
    const int64_t W = w, H = h;
    // Whichever side reaches its limit first sets the scale; the other rounds
    // to the nearest even number (the host has been seen to take 2160x2160,
    // never an odd raster; half a pixel of aspect is nothing) and never to
    // nothing.
    const auto even = [](int64_t v) { const int32_t e = static_cast<int32_t>((v + 1) / 2 * 2); return e > 0 ? e : 2; };
    if (W * kMaxHeight >= H * kMaxWidth) return {kMaxWidth, even((H * kMaxWidth + W / 2) / W)};
    return {even((W * kMaxHeight + H / 2) / H), kMaxHeight};
}
constexpr bool same(Size a, Size b) { return a.w == b.w && a.h == b.h; }
static_assert(same(capped_size(3840, 2160), {0, 0}),       "what fits is not touched");
static_assert(same(capped_size(1920, 1080), {0, 0}),       "what fits is not touched");
static_assert(same(capped_size(720, 480), {0, 0}),         "AE's placeholder instance");
static_assert(same(capped_size(0, 0), {0, 0}),             "no size reported");
static_assert(same(capped_size(7680, 4320), {3840, 2160}), "8K UHD");
static_assert(same(capped_size(6720, 3780), {3840, 2160}), "16:9 above the cap");
static_assert(same(capped_size(8000, 8000), {2160, 2160}), "square: height limits");
static_assert(same(capped_size(9216, 3164), {3840, 1318}), "wide: width limits");
static_assert(same(capped_size(2160, 3840), {1216, 2160}), "portrait, rounded to even");
static_assert(same(capped_size(4000, 5000), {1728, 2160}), "4:5 portrait");
static_assert(same(capped_size(3000, 8000), {810, 2160}),  "tall");
static_assert(same(capped_size(3841, 100), {3840, 100}),   "one pixel over");
static_assert(same(capped_size(100000, 1), {3840, 2}),     "never rounds to nothing");

// What QueryVideoMode asks every instance for: {0, 0} until a frame over the
// cap has been seen, {0, 0} again when a new instance goes live. Process-wide
// like the ring, because the reset that makes the host ask again starts a new
// module.
Size S_want {0, 0};
bool S_reset_due = false;

// A reset re-creates the host's instances under new ids, and they go live
// before the new module's first frame; a user's switch to another comp also
// arrives as a new instance going live, but after frames have flowed. The
// count of frames since Startup tells the two apart. Focus loss and return
// re-activate an instance already seen, which must not cost a full-size
// frame: hence the set of ids that have been live.
uint64_t S_frames_this_module = 0;
std::set<csSDK_int32> S_been_live;

// Replacing a mapping tells whoever holds the old one to let go first.
void retire_ring(const char* why) {
    if (!S.ring.valid()) return;
    S.ring.set_host_state(HostState::Retired);
    logf("ring retired (%s) after %llu frames", why, (unsigned long long)S.published);
    S.ring = SharedRing();
    S.capacity = 0;
}

bool ensure_capacity(uint64_t bytes) {
    if (S.ring.valid() && bytes <= S.capacity) return true;
    retire_ring("needs more room");
    if (!S.ring.create(S.host.ring, bytes)) {
        logf("ring create failed: %s", S.ring.error().c_str());
        return false;
    }
    S.capacity = S.ring.header()->pixels_capacity;
    logf("ring %s created, %llu KiB per slot", S.host.ring, (unsigned long long)(S.capacity / 1024));
    return true;
}

// --- module state ------------------------------------------------------------
struct Plugin {
    SPBasicSuite*     sp   = nullptr;
    PrSDKPPixSuite*   ppix = nullptr;
    PrSDKTimeSuite*   time = nullptr;
    PrSDKStringSuite* str  = nullptr;
    PrTime            ticks_per_second = 0;
};

Plugin* plugin(const tmStdParms* sp) { return static_cast<Plugin*>(sp->ioPrivatePluginData); }

template <class T>
void acquire(SPBasicSuite* sp, const char* name, int32_t version, T** out) {
    const void* p = nullptr;
    *out = sp->AcquireSuite(name, version, &p) == 0 ? static_cast<T*>(const_cast<void*>(p)) : nullptr;
    if (*out == nullptr) logf("suite %s v%d unavailable", name, version);
}

// Exceptions must never cross back into the host: it is C, and an escaping
// C++ exception takes the application down with it.
template <class F>
tmResult guarded(const char* where, F&& f) {
    try { return f(); }
    catch (const std::exception& e) { logf("%s: exception: %s", where, e.what()); }
    catch (...)                     { logf("%s: unknown exception", where); }
    return tmResult_ErrorUnknown;
}

constexpr PrPixelFormat kModes[] = { PrPixelFormat_ARGB_4444_32f, PrPixelFormat_BGRA_4444_32f };

// --- entry points ------------------------------------------------------------

tmResult Startup(tmStdParms* sp, tmPluginInfo* info) {
    return guarded("Startup", [&] {
        auto* P = new Plugin;
        sp->ioPrivatePluginData = P;
        P->sp = sp->piSuites->utilFuncs->getSPBasicSuite();
        acquire(P->sp, kPrSDKPPixSuite,   kPrSDKPPixSuiteVersion,   &P->ppix);
        acquire(P->sp, kPrSDKTimeSuite,   kPrSDKTimeSuiteVersion,   &P->time);
        acquire(P->sp, kPrSDKStringSuite, kPrSDKStringSuiteVersion, &P->str);
        if (P->time != nullptr) P->time->GetTicksPerSecond(&P->ticks_per_second);
        if (S.host.ring == nullptr) S.host = detect_host();
        S_frames_this_module = 0;
        logf("startup in %s: ring %s, ticks/s %lld", S.host.label, S.host.ring,
             static_cast<long long>(P->ticks_per_second));

        // The audio segment survives a module reset like the frame ring
        // (Startup runs again, the segment is already there).
        ensure_audio_ring(P->ticks_per_second);

        std::snprintf(info->outIdentifier.mGUID, sizeof info->outIdentifier.mGUID, "%s", kPluginGUID);
        info->outPriority            = 0;
        info->outAudioAvailable      = kPrFalse;
        info->outAudioDefaultEnabled = kPrFalse;
        info->outClockAvailable      = kPrFalse;   // video only; the host keeps the clock
        info->outVideoAvailable      = kPrTrue;
        info->outVideoDefaultEnabled = kPrFalse;   // the user opts in, in Preferences
        const char16_t name[] = u"QCBridgeAE → QCView";
        static_assert(sizeof(prUTF16Char) == sizeof(char16_t), "prUTF16Char is UTF-16");
        std::memcpy(info->outDisplayName, name, sizeof name);
        info->outHideInUI            = kPrFalse;
        info->outHasSetup            = kPrFalse;
        info->outInterfaceVersion    = tmInterfaceVersion;
        info->outPushAudioAvailable  = kPrTrue;    // the mirror tap; the user ticks "Audio Stream"
        info->outHasStreaming        = kPrFalse;
        return tmResult_Success;
    });
}

// The ring outlives module resets on purpose. It is retired when the last
// instance is disposed (see DisposeInstance), not here: AE quits without
// calling Shutdown at all.
tmResult Shutdown(tmStdParms* sp) {
    return guarded("Shutdown", [&] {
        Plugin* P = plugin(sp);
        if (P == nullptr) return tmResult_Success;
        if (P->ppix) P->sp->ReleaseSuite(kPrSDKPPixSuite,   kPrSDKPPixSuiteVersion);
        if (P->time) P->sp->ReleaseSuite(kPrSDKTimeSuite,   kPrSDKTimeSuiteVersion);
        if (P->str)  P->sp->ReleaseSuite(kPrSDKStringSuite, kPrSDKStringSuiteVersion);
        delete P;
        sp->ioPrivatePluginData = nullptr;
        logf("shutdown");
        return tmResult_Success;
    });
}

// Polled by the host. A reset shuts every open plug-in down and starts it
// again, which is the only way to have QueryVideoMode asked a second time.
tmResult NeedsReset(const tmStdParms*, prBool* outReset) {
    return guarded("NeedsReset", [&] {
        *outReset = S_reset_due ? kPrTrue : kPrFalse;
        if (S_reset_due) logf("module reset requested, to ask for %dx%d", S_want.w, S_want.h);
        S_reset_due = false;
        return tmResult_Success;
    });
}

tmResult CreateInstance(const tmStdParms* sp, tmInstance* inst) {
    return guarded("CreateInstance", [&] {
        inst->ioPrivateInstanceData = nullptr;
        S.instances.insert(inst->inInstanceID);
        if (const Plugin* P = plugin(sp)) ensure_audio_ring(P->ticks_per_second);
        // Informational only: AE reports a 720x480 placeholder here until a
        // comp is open (A4). Geometry comes from each frame.
        logf("instance %d: %dx%d", inst->inInstanceID, inst->inVideoWidth, inst->inVideoHeight);
        return tmResult_Success;
    });
}

tmResult DisposeInstance(const tmStdParms*, tmInstance* inst) {
    return guarded("DisposeInstance", [&] {
        S.video_on.erase(inst->inInstanceID);
        publish_state();
        logf("instance %d disposed -> state %u", inst->inInstanceID, static_cast<unsigned>(derived_state()));
        S.instances.erase(inst->inInstanceID);
        S_audio.stop(inst->inInstanceID);   // only if this instance owned the session
        if (S.instances.empty()) {
            retire_ring("last instance disposed");
            quiesce_audio("last instance disposed");
        }
        return tmResult_Success;
    });
}

tmResult QueryVideoMode(const tmStdParms* sp, const tmInstance* inst, csSDK_int32 index, tmVideoMode* out) {
    return guarded("QueryVideoMode", [&] {
        constexpr int kCount = static_cast<int>(sizeof kModes / sizeof kModes[0]);
        if (index < 0 || index >= kCount) return tmResult_ErrorInvalidArgument;
        Plugin* P = plugin(sp);

        // "Any" until a frame over the cap has been pushed (see publish):
        // we follow the host's size. After that, the size that frame fits
        // into, so the host never builds the full-size float frame again.
        const Size want = S_want;
        if (index == 0 && want.w != 0 && inst != nullptr)
            logf("instance %d (%dx%d by the host's account): asking for %dx%d", inst->inInstanceID,
                 inst->inVideoWidth, inst->inVideoHeight, want.w, want.h);
        out->outWidth       = want.w;
        out->outHeight      = want.h;
        out->outPARNum      = 0;
        out->outPARDen      = 0;
        out->outFieldType   = prFieldsAny;
        out->outPixelFormat = kModes[index];
        out->outStreamLabel = PrSDKString{};
        out->outLatency     = 0;   // a live viewer wants no preroll

        // Only the fields we own; inPrivateData is the host's.
        ColorSpaceRec& cs = out->outColorSpaceRec;
        cs.outColorSpaceType                = kPrSDKColorSpaceType_Predefined;
        cs.ioProfileRec.ioBufferSize        = 0;
        cs.ioProfileRec.inDestinationBuffer = nullptr;
        cs.ioProfileRec.outName             = PrSDKString{};
        // A fresh string per mode, never disposed by us: the host owns it.
        PrSDKString name {};
        if (P->str != nullptr
            && P->str->AllocateFromUTF8(reinterpret_cast<const prUTF8Char*>(kPrWorkingColorSpace), &name) == 0)
            cs.ioProfileRec.outName = name;
        else
            logf("QueryVideoMode: could not allocate the working-space name; "
                 "under Adobe CMS the host will convert to Rec.709");

        return index + 1 < kCount ? tmResult_ContinueIterate : tmResult_Success;
    });
}

tmResult ActivateDeactivate(const tmStdParms*, const tmInstance* inst, PrActivationEvent ev,
                            prBool, prBool videoActive) {
    return guarded("ActivateDeactivate", [&] {
        if (videoActive) {
            S.video_on.insert(inst->inInstanceID);
            // Another instance going live after frames have flowed is the
            // user looking at something else: ask for its own size until
            // its first frame says otherwise.
            if (S_been_live.insert(inst->inInstanceID).second && S_want.w != 0
                && S_frames_this_module > 0 && !S_reset_due) {
                S_want = {0, 0};
                S_reset_due = true;
                logf("instance %d went live: back to the host's own size", inst->inInstanceID);
            }
        } else {
            S.video_on.erase(inst->inInstanceID);
            S.last_pause = ev == PrActivationEvent_ApplicationLostFocus ? HostState::PausedFocus
                                                                        : HostState::Paused;
        }
        publish_state();
        logf("instance %d: activation event %d, video %d -> state %u", inst->inInstanceID,
             static_cast<int>(ev), videoActive, static_cast<unsigned>(derived_state()));
        return tmResult_Success;
    });
}

void publish(Plugin* P, const tmPushVideo* pv, PPixHand h) {
    PrPixelFormat pf = PrPixelFormat_Invalid;
    prRect bounds {};
    csSDK_int32 rowbytes = 0;
    char* base = nullptr;
    P->ppix->GetPixelFormat(h, &pf);
    P->ppix->GetBounds(h, &bounds);
    P->ppix->GetRowBytes(h, &rowbytes);
    P->ppix->GetPixels(h, PrPPixBufferAccess_ReadOnly, &base);

    HostOrder order;
    if (pf == PrPixelFormat_ARGB_4444_32f)      order = HostOrder::ARGB;
    else if (pf == PrPixelFormat_BGRA_4444_32f) order = HostOrder::BGRA;
    else { logf("unexpected pixel format 0x%08x; frame dropped", static_cast<unsigned>(pf)); return; }

    const int32_t w = std::abs(bounds.right - bounds.left);
    const int32_t hgt = std::abs(bounds.bottom - bounds.top);
    if (base == nullptr || w <= 0 || hgt <= 0 || std::abs(rowbytes) < w * 16) {
        logf("unusable frame %dx%d rowbytes %d; dropped", w, hgt, rowbytes);
        return;
    }
    const auto uw = static_cast<uint32_t>(w), uh = static_cast<uint32_t>(hgt);
    const uint32_t dst_row = aligned_bytes_per_row(uw, 8u);
    if (!ensure_capacity(static_cast<uint64_t>(dst_row) * uh)) return;

    auto* dst = static_cast<uint8_t*>(S.ring.begin_write(static_cast<uint64_t>(dst_row) * uh));
    if (dst == nullptr) { logf("begin_write refused %ux%u", uw, uh); return; }

    // Both hosts deliver bottom-up with positive rowbytes (A4 sections 3,
    // 13). A negative stride would already be top-down in walk order.
    const ConvertSource src {base, rowbytes, uw, uh, order, rowbytes > 0};
    const ConvertResult cr = convert_32f_to_rgba16f(src, dst, dst_row);

    FrameDesc d {};
    d.width         = uw;
    d.height        = uh;
    d.bytes_per_row = dst_row;
    d.pixel_format  = PixelFormat::RGBA16F;
    d.source_tier   = SourceTier::Float32;
    d.channel_order = ChannelOrder::RGBA;
    d.flags         = (cr.has_inf ? kFlagHasInf : 0u) | (cr.has_nan ? kFlagHasNaN : 0u);
    d.time_value    = pv->inTime;          // -1 from AE's viewer and preview: "immediate"
    d.time_scale    = P->ticks_per_second;
    d.value_scale   = 1.0f;
    std::snprintf(d.comp_name, sizeof d.comp_name, "%s", S.host.label);
    S.ring.commit(d);
    publish_state();   // a new ring starts Active (zeroed); make it tell the truth

    // The "Audio Stream" hint (see kPlayingPushesBeforeOff).
    if (AudioRing* ar = S_audio.ring_for_host()) {
        if (pv->inPlayMode != playmode_Playing || S_audio.owner() != AudioPublisher::kNoOwner) {
            S_playing_without_audio = 0;
        } else if (++S_playing_without_audio == kPlayingPushesBeforeOff) {
            ar->set_host_audio(HostAudio::Off);
            logf("playing without an audio session: Audio Stream is off for this device in Preferences > Playback");
        }
    }

    // Logged on the first frame, every size change, every 240th frame, after
    // ten seconds without a line and on any non-finite frame — enough to see
    // frames flowing without a line each.
    const bool resized = uw != S.last_w || uh != S.last_h;
    S.last_w = uw; S.last_h = uh;
    // A frame over the cap: ask the host, through a module reset, to scale
    // from now on. Asked once per size, so a host that ignores the request
    // is not reset again; its frames keep saying "over the cap" below.
    ++S_frames_this_module;
    const Size fit = capped_size(w, hgt);
    if (fit.w != 0 && !same(fit, S_want)) {
        S_want = fit;
        S_reset_due = true;
        logf("frame %ux%u is over %dx%d: asking the host for %dx%d from now on",
             uw, uh, kMaxWidth, kMaxHeight, fit.w, fit.h);
    }

    // The line is written by note_push, once this push's own time is known.
    const bool slow = std::chrono::duration<double>(std::chrono::steady_clock::now() - S_win.line_at).count()
                      >= kLineEverySeconds;
    if (++S.published == 1 || resized || (S.published % 240) == 0 || slow || cr.has_inf || cr.has_nan)
        std::snprintf(S_win.line, sizeof S_win.line, "published %llu (%ux%u %s%s%s%s)",
                      (unsigned long long)S.published, uw, uh, order == HostOrder::ARGB ? "ARGB" : "BGRA",
                      cr.has_inf ? ", has inf" : "", cr.has_nan ? ", has NaN" : "",
                      fit.w != 0 ? ", over the cap" : "");
}

// --- audio: the mirror tap -----------------------------------------------------

// Asked once per instance as soon as push audio is declared, even though we
// are never the primary audio device; an instance whose module has no entry
// for it is disposed on the spot (measured 2026-10-10: create, dispose, no
// video-mode query). "Only one audio mode is currently supported": we echo
// the instance's own format.
tmResult QueryAudioMode(const tmStdParms* sp, const tmInstance* inst, csSDK_int32 index, tmAudioMode* out) {
    return guarded("QueryAudioMode", [&] {
        if (index != 0) return tmResult_ErrorInvalidArgument;
        Plugin* P = plugin(sp);
        const uint32_t ch = inst->inNumChannels > 0 ? std::min<uint32_t>(inst->inNumChannels, kMaxTransmitAudioChannels) : 2u;
        out->outAudioSampleRate = inst->inAudioSampleRate > 0 ? inst->inAudioSampleRate : 48000.0f;
        out->outMaxBufferSize   = 48000;
        out->outNumChannels     = ch;
        out->outLatency         = 0;
        for (uint32_t i = 0; i < ch; ++i) {
            out->outChannelLabels[i] = ch == 2 && i == 0 ? kPrAudioChannelLabel_FrontLeft
                                     : ch == 2 && i == 1 ? kPrAudioChannelLabel_FrontRight
                                     : inst->inNumChannels > i ? inst->inChannelLabels[i]
                                     : kPrAudioChannelLabel_Discrete;
            // Allocated by us, never disposed by us: the host owns what it
            // reads (the same contract as the colour-space name).
            char name[48];
            std::snprintf(name, sizeof name, "QCView %u", i + 1);
            PrSDKString str {};
            if (P != nullptr && P->str != nullptr
                && P->str->AllocateFromUTF8(reinterpret_cast<const prUTF8Char*>(name), &str) == 0)
                out->outAudioOutputNames[i] = str;
        }
        return tmResult_Success;
    });
}

tmResult StartPushAudio(const tmStdParms* sp, const tmInstance* inst, PrTime start, float speed, PrTime in,
                        PrTime out, prBool loop, prBool scrubbing, csSDK_uint32* outSamplesPerFrame) {
    return guarded("StartPushAudio", [&] {
        *outSamplesPerFrame = kDefaultAudioSlotFrames;
        AudioSession s {};
        s.start_time  = start;
        s.in_time     = in;
        s.out_time    = out;
        s.speed       = speed;
        s.sample_rate = inst->inAudioSampleRate > 0 ? static_cast<uint32_t>(inst->inAudioSampleRate + 0.5f) : 48000u;
        s.channels    = inst->inNumChannels > 0 ? inst->inNumChannels : 2u;
        s.flags       = (loop ? kAudioSessionLoop : 0u) | (scrubbing ? kAudioSessionScrubbing : 0u);
        s.push_frames = kDefaultAudioSlotFrames;
        S_astats.pushes = 0; S_astats.frames = 0; S_astats.packets = 0;
        S_astats.first_time = start; S_astats.last_time = start; S_astats.channels = s.channels;
        S_playing_without_audio = 0;
        S_push_flags.store(scrubbing ? kAudioPacketScrubbing : 0u, std::memory_order_relaxed);
        if (const Plugin* P = plugin(sp)) ensure_audio_ring(P->ticks_per_second);
        S_audio.start(inst->inInstanceID, s);
        if (!scrubbing) {
            const Plugin* P = plugin(sp);
            const double sec = P != nullptr && P->ticks_per_second > 0
                             ? static_cast<double>(start) / static_cast<double>(P->ticks_per_second) : 0.0;
            logf("audio session: instance %d at %.3f s, speed %.2f, %u ch %u Hz%s%s", inst->inInstanceID, sec,
                 speed, s.channels, s.sample_rate, loop ? ", loop" : "", S_audio.has_ring() ? "" : " (NO RING)");
        }
        return tmResult_Success;
    });
}

// The host's audio thread, concurrent with everything else. Nothing here but
// the segment and relaxed counters; a failure is a dropped push, not a log.
tmResult PushAudio(const tmStdParms*, const tmInstance* inst, const tmPushAudio* a) {
    if (a == nullptr || a->inBuffers == nullptr || a->inNumSamples == 0) return tmResult_Success;
    const uint32_t rate = inst->inAudioSampleRate > 0 ? static_cast<uint32_t>(inst->inAudioSampleRate + 0.5f) : 48000u;
    // Scrubbing is a session property; carried per packet so a consumer can
    // treat scrub snippets differently without re-reading the session.
    const uint32_t flags = S_push_flags.load(std::memory_order_relaxed);
    const uint32_t n = S_audio.push(inst->inInstanceID, a->inBuffers, a->inNumChannels, rate,
                                    a->inNumSamples, a->inTime, flags);
    if (n > 0) {
        S_astats.pushes.fetch_add(1, std::memory_order_relaxed);
        S_astats.frames.fetch_add(a->inNumSamples, std::memory_order_relaxed);
        S_astats.packets.fetch_add(n, std::memory_order_relaxed);
        S_astats.last_time.store(a->inTime, std::memory_order_relaxed);
    }
    return tmResult_Success;
}

tmResult StopPushAudio(const tmStdParms* sp, const tmInstance* inst) {
    return guarded("StopPushAudio", [&] {
        const bool owned = S_audio.owner() == inst->inInstanceID;
        S_audio.stop(inst->inInstanceID);
        if (owned) {
            const Plugin* P = plugin(sp);
            const double tps = P != nullptr && P->ticks_per_second > 0 ? static_cast<double>(P->ticks_per_second) : 1.0;
            const uint64_t frames = S_astats.frames.load(std::memory_order_relaxed);
            if (frames > 0)
                logf("audio session ended: instance %d, %llu pushes, %llu packets, %llu frames x %u ch, %.3f s .. %.3f s",
                     inst->inInstanceID, (unsigned long long)S_astats.pushes.load(std::memory_order_relaxed),
                     (unsigned long long)S_astats.packets.load(std::memory_order_relaxed),
                     (unsigned long long)frames, S_astats.channels.load(),
                     static_cast<double>(S_astats.first_time.load()) / tps,
                     static_cast<double>(S_astats.last_time.load()) / tps);
        }
        return tmResult_Success;
    });
}

tmResult PushVideo(const tmStdParms* sp, const tmInstance*, const tmPushVideo* pv) {
    const auto began = std::chrono::steady_clock::now();
    Plugin* P = plugin(sp);
    const tmResult r = guarded("PushVideo", [&] {
        if (P != nullptr && P->ppix != nullptr && pv->inFrameCount > 0)
            publish(P, pv, pv->inFrames[0].inFrame);
        return tmResult_Success;
    });
    // "The plug-in is responsible for disposing of all passed in ppix."
    if (P != nullptr && P->ppix != nullptr)
        for (csSDK_size_t i = 0; i < pv->inFrameCount; ++i) P->ppix->Dispose(pv->inFrames[i].inFrame);
    note_push(began);
    return r;
}

}  // namespace

extern "C" QCBAE_EXPORT
tmResult xTransmitEntry(csSDK_int32 interfaceVersion, prBool loadModule, piSuitesPtr, tmModule* out) {
    if (loadModule) {
        logf("--- module load, host interface v%d ---", interfaceVersion);
        std::memset(out, 0, sizeof *out);   // 0 = unsupported, per PrSDKTransmit.h
        out->Startup            = Startup;
        out->Shutdown           = Shutdown;
        out->NeedsReset         = NeedsReset;
        out->CreateInstance     = CreateInstance;
        out->DisposeInstance    = DisposeInstance;
        out->QueryVideoMode     = QueryVideoMode;
        out->ActivateDeactivate = ActivateDeactivate;
        out->PushVideo          = PushVideo;
        out->QueryAudioMode     = QueryAudioMode;
        out->StartPushAudio     = StartPushAudio;
        out->PushAudio          = PushAudio;
        out->StopPushAudio      = StopPushAudio;
    } else {
        retire_ring("module unloaded");
        quiesce_audio("module unloaded");
        logf("--- module unload ---");
    }
    return tmResult_Success;
}
