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
//   * A comp larger than 3840x2160 is asked for at the size that fits inside
//     it, aspect kept, so the host scales before it builds the frame. Asked
//     for at "any" size, an 8000x8000 comp costs the host a 977 MiB float
//     frame per push: After Effects on Windows ran at 0.6 fps with this
//     device, 24 fps with a hardware device that names its raster, and the
//     pass below was 48 ms of those 1600 (measured 2026-10-06). Anything that
//     fits is still asked for at "any" size and arrives pixel for pixel.
//   * Every pushed frame is converted. An earlier version silently skipped a
//     frame whose PPix unique key matched the last one (each viewer change
//     pushes two frames ~3 ms apart). In AE it published one frame and then
//     nothing: removing it alone brought AE back to 24 fps at 4K (A6 notes),
//     so the key does not identify content there. An unmeasured saving must
//     not be able to drop frames without a trace.
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

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
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
    char     line[160] {};                 // a "published" line waiting for this push's timing
};
Window S_win;

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
    // to nearest and never to nothing.
    if (W * kMaxHeight >= H * kMaxWidth) {
        const auto oh = static_cast<int32_t>((H * kMaxWidth + W / 2) / W);
        return {kMaxWidth, oh > 0 ? oh : 1};
    }
    const auto ow = static_cast<int32_t>((W * kMaxHeight + H / 2) / H);
    return {ow > 0 ? ow : 1, kMaxHeight};
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
static_assert(same(capped_size(2160, 3840), {1215, 2160}), "portrait");
static_assert(same(capped_size(3841, 100), {3840, 100}),   "one pixel over");
static_assert(same(capped_size(100000, 1), {3840, 1}),     "never rounds to nothing");

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
        logf("startup in %s: ring %s, ticks/s %lld", S.host.label, S.host.ring,
             static_cast<long long>(P->ticks_per_second));

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
        info->outPushAudioAvailable  = kPrFalse;
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

tmResult CreateInstance(const tmStdParms*, tmInstance* inst) {
    return guarded("CreateInstance", [&] {
        inst->ioPrivateInstanceData = nullptr;
        S.instances.insert(inst->inInstanceID);
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
        if (S.instances.empty()) retire_ring("last instance disposed");
        return tmResult_Success;
    });
}

tmResult QueryVideoMode(const tmStdParms* sp, const tmInstance* inst, csSDK_int32 index, tmVideoMode* out) {
    return guarded("QueryVideoMode", [&] {
        constexpr int kCount = static_cast<int>(sizeof kModes / sizeof kModes[0]);
        if (index < 0 || index >= kCount) return tmResult_ErrorInvalidArgument;
        Plugin* P = plugin(sp);

        // "Any" for what fits inside the cap: we follow the host's size. A
        // larger comp is asked for scaled, so the host never builds the
        // full-size float frame. The instance's size is the comp's here (AE
        // makes an instance per comp it shows; only the one it makes before a
        // comp is open reports a 720x480 placeholder).
        const Size want = inst != nullptr ? capped_size(inst->inVideoWidth, inst->inVideoHeight) : Size{0, 0};
        if (index == 0 && want.w != 0)
            logf("instance %d: %dx%d is over %dx%d, asking for %dx%d", inst->inInstanceID,
                 inst->inVideoWidth, inst->inVideoHeight, kMaxWidth, kMaxHeight, want.w, want.h);
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

    // Logged on the first frame, every size change, every 240th frame and any
    // non-finite frame — enough to see frames flowing without a line each.
    const bool resized = uw != S.last_w || uh != S.last_h;
    S.last_w = uw; S.last_h = uh;
    // The line is written by note_push, once this push's own time is known.
    // A frame over the cap means the host did not scale as asked — say so.
    const bool over = uw > static_cast<uint32_t>(kMaxWidth) || uh > static_cast<uint32_t>(kMaxHeight);
    if (++S.published == 1 || resized || (S.published % 240) == 0 || cr.has_inf || cr.has_nan)
        std::snprintf(S_win.line, sizeof S_win.line, "published %llu (%ux%u %s%s%s%s)",
                      (unsigned long long)S.published, uw, uh, order == HostOrder::ARGB ? "ARGB" : "BGRA",
                      cr.has_inf ? ", has inf" : "", cr.has_nan ? ", has NaN" : "",
                      over ? ", over the cap: the host did not scale" : "");
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
        out->CreateInstance     = CreateInstance;
        out->DisposeInstance    = DisposeInstance;
        out->QueryVideoMode     = QueryVideoMode;
        out->ActivateDeactivate = ActivateDeactivate;
        out->PushVideo          = PushVideo;
    } else {
        retire_ring("module unloaded");
        logf("--- module unload ---");
    }
    return tmResult_Success;
}
