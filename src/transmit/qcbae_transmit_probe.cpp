// QCBridgeAE — the Transmit probe (phase A4, Route A).
//
// A Mercury Transmit device that publishes whatever the host pushes into the
// shared ring, unaltered, so qcbae-probe can read it back. It is an
// instrument, not the product (that is A6): its job is to answer A4's
// questions by measurement, and it may not change what it measures.
//
// So, deliberately:
//   * Pixels go in exactly as the host delivered them — native depth, native
//     channel order, 32f as 32f. No shuffle, no half conversion, no range fix.
//     The sidecar states what arrived (order, premultiplied, value_scale) and
//     the probe's dump/viewer interpret it. Folding work into the copy is A6.
//   * Every negotiation is logged: which modes we offered, what the host
//     pre-filled, and per frame which format it actually picked.
//   * What we offer is read from a config file, and re-read without
//     restarting AE (NeedsReset), because A4 needs control conditions — an
//     unset colour space, a _Linear format — that must visibly change the
//     numbers. A result that no control can move proves nothing.
//
// Config: /tmp/qcbridgeae-transmit.conf, key = value, '#' comments. All keys
// optional; defaults are the design in DESIGN-NOTES D1/D5.
//   modes       = argb8, argb16, argb32f, bgra8, bgra16, bgra32f
//                 (also *32f_linear, prgb*, bgrp*, xrgb*, bgrx*, any)
//   colorspace  = working | unset | <a predefined name, e.g. BT.709 RGB Full (Scene)>
//                 | sei:pq2020 | sei:srgb | sei:709   — SEI-tag form, the only
//                 encoding Adobe's sample demonstrates; a control for whether
//                 the host reads the colour-space record at all
//   cs_encoding = both | buffer | name   — how a predefined name is passed.
//                 Measured (A4, Adobe CMS): AE reads the PrSDKString in
//                 ioProfileRec.outName; the raw buffer alone is ignored.
//   latency     = 0   (frames of preroll the host sends ahead of playback)
//   size        = any | WxH   (a fixed raster the host scales to, as a hardware
//                 device asks; any = the host's own size)
//   audio       = off | push | pull | both   (A8: which Transmit audio paths to
//                 declare. push = outPushAudioAvailable, the "mirror" tap of
//                 the host's own audio device; pull = outAudioAvailable, the
//                 device *is* the host's audio device and pulls via
//                 PrSDKPlayModuleAudioSuite)
//   clock       = auto | on | off   (outClockAvailable; auto = on iff pull,
//                 which PrSDKTransmit.h says an audio device must provide)
//   streaming   = off | on   (outHasStreaming; push audio is documented "for
//                 remote devices", so this is a control)
//   push_samples   = 1024   (outSamplesPerFrame asked of the host at StartPushAudio)
//   audio_rate     = 48000  (the one audio mode offered in pull mode)
//   audio_channels = 2
//
// Privacy: the host gives a Transmit device no project paths or comp names,
// and nothing here asks for them (DESIGN-NOTES privacy 5, 6).

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
#include "PrSDKPlayModuleAudioSuite.h"
#include "PrSDKPixelFormat.h"
#include "PrSDKColorSpaces.h"
#include "PrSDKColorProfile.h"
#include "PrSDKColorSEICodes.h"
#include "SPBasic.h"

#include "common/surface/shared_ring.h"

#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <thread>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

using namespace qcbae;

namespace {

constexpr const char* kRingName   = "/qcbae-probe";   // what qcbae-probe opens
#if defined(_WIN32)
std::string scratch_file(const char* name) {   // %TEMP%, see qcbae_transmit.cpp
    const char* t = std::getenv("TEMP");
    return std::string(t && *t ? t : ".") + "\\" + name;
}
const std::string S_configPath = scratch_file("qcbridgeae-transmit.conf");
const std::string S_logPath    = scratch_file("qcbridgeae-transmit-probe.log");
const char* const kConfigPath = S_configPath.c_str();
const char* const kLogPath    = S_logPath.c_str();
#else
constexpr const char* kConfigPath = "/tmp/qcbridgeae-transmit.conf";
constexpr const char* kLogPath    = "/tmp/qcbridgeae-transmit-probe.log";
#endif

// Persistent identity in the host's device list. Generated once; never change
// it, or the host treats the device as new and forgets the user's choice.
constexpr const char* kPluginGUID = "66728618-5F5B-40FF-89A5-DAD8CB7C7DB6";

// --- logging ---------------------------------------------------------------
// Appended, not truncated: NeedsReset restarts the module mid-session and the
// negotiation before a reset is exactly what we want to keep.
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

std::string fourcc(PrPixelFormat pf) {
    const auto v = static_cast<uint32_t>(pf);
    std::string s;
    for (int i = 0; i < 4; ++i) {
        const char c = static_cast<char>((v >> (8 * i)) & 0xFF);
        s.push_back(c >= 32 && c < 127 ? c : '?');
    }
    return s;
}

// --- the formats we understand ----------------------------------------------
// Everything a host can hand a 4:4:4:4 RGB device. The letters are the byte
// order in memory: ARGB is AE native, BGRA Premiere native. P = premultiplied
// alpha, X = alpha implicitly opaque and "may be left filled with garbage"
// (Adobe's guide). _Linear is a *host transform* — "gamma of 1, rather than
// the standard 2.2" — offered only as a control, never by default.
struct FormatInfo {
    const char*  token;
    PrPixelFormat pf;
    PixelFormat  wire;
    SourceTier   tier;
    ChannelOrder order;
    bool         premultiplied;
    bool         opaque_x;
    bool         linear;
};

const FormatInfo kFormats[] = {
    {"argb8",   PrPixelFormat_ARGB_4444_8u,  PixelFormat::RGBA8Unorm,  SourceTier::Int8,    ChannelOrder::ARGB, false, false, false},
    {"argb16",  PrPixelFormat_ARGB_4444_16u, PixelFormat::RGBA16Unorm, SourceTier::Int16,   ChannelOrder::ARGB, false, false, false},
    {"argb32f", PrPixelFormat_ARGB_4444_32f, PixelFormat::RGBA32Float, SourceTier::Float32, ChannelOrder::ARGB, false, false, false},
    {"bgra8",   PrPixelFormat_BGRA_4444_8u,  PixelFormat::RGBA8Unorm,  SourceTier::Int8,    ChannelOrder::BGRA, false, false, false},
    {"bgra16",  PrPixelFormat_BGRA_4444_16u, PixelFormat::RGBA16Unorm, SourceTier::Int16,   ChannelOrder::BGRA, false, false, false},
    {"bgra32f", PrPixelFormat_BGRA_4444_32f, PixelFormat::RGBA32Float, SourceTier::Float32, ChannelOrder::BGRA, false, false, false},

    {"argb32f_linear", PrPixelFormat_ARGB_4444_32f_Linear, PixelFormat::RGBA32Float, SourceTier::Float32, ChannelOrder::ARGB, false, false, true},
    {"bgra32f_linear", PrPixelFormat_BGRA_4444_32f_Linear, PixelFormat::RGBA32Float, SourceTier::Float32, ChannelOrder::BGRA, false, false, true},

    {"prgb8",   PrPixelFormat_PRGB_4444_8u,  PixelFormat::RGBA8Unorm,  SourceTier::Int8,    ChannelOrder::ARGB, true,  false, false},
    {"prgb16",  PrPixelFormat_PRGB_4444_16u, PixelFormat::RGBA16Unorm, SourceTier::Int16,   ChannelOrder::ARGB, true,  false, false},
    {"prgb32f", PrPixelFormat_PRGB_4444_32f, PixelFormat::RGBA32Float, SourceTier::Float32, ChannelOrder::ARGB, true,  false, false},
    {"bgrp8",   PrPixelFormat_BGRP_4444_8u,  PixelFormat::RGBA8Unorm,  SourceTier::Int8,    ChannelOrder::BGRA, true,  false, false},
    {"bgrp16",  PrPixelFormat_BGRP_4444_16u, PixelFormat::RGBA16Unorm, SourceTier::Int16,   ChannelOrder::BGRA, true,  false, false},
    {"bgrp32f", PrPixelFormat_BGRP_4444_32f, PixelFormat::RGBA32Float, SourceTier::Float32, ChannelOrder::BGRA, true,  false, false},
    {"prgb32f_linear", PrPixelFormat_PRGB_4444_32f_Linear, PixelFormat::RGBA32Float, SourceTier::Float32, ChannelOrder::ARGB, true, false, true},
    {"bgrp32f_linear", PrPixelFormat_BGRP_4444_32f_Linear, PixelFormat::RGBA32Float, SourceTier::Float32, ChannelOrder::BGRA, true, false, true},

    {"xrgb8",   PrPixelFormat_XRGB_4444_8u,  PixelFormat::RGBA8Unorm,  SourceTier::Int8,    ChannelOrder::ARGB, false, true,  false},
    {"xrgb16",  PrPixelFormat_XRGB_4444_16u, PixelFormat::RGBA16Unorm, SourceTier::Int16,   ChannelOrder::ARGB, false, true,  false},
    {"xrgb32f", PrPixelFormat_XRGB_4444_32f, PixelFormat::RGBA32Float, SourceTier::Float32, ChannelOrder::ARGB, false, true,  false},
    {"bgrx8",   PrPixelFormat_BGRX_4444_8u,  PixelFormat::RGBA8Unorm,  SourceTier::Int8,    ChannelOrder::BGRA, false, true,  false},
    {"bgrx16",  PrPixelFormat_BGRX_4444_16u, PixelFormat::RGBA16Unorm, SourceTier::Int16,   ChannelOrder::BGRA, false, true,  false},
    {"bgrx32f", PrPixelFormat_BGRX_4444_32f, PixelFormat::RGBA32Float, SourceTier::Float32, ChannelOrder::BGRA, false, true,  false},
};

const FormatInfo* find_format(PrPixelFormat pf) {
    for (const auto& f : kFormats) if (f.pf == pf) return &f;
    return nullptr;
}
const FormatInfo* find_token(const std::string& t) {
    for (const auto& f : kFormats) if (t == f.token) return &f;
    return nullptr;
}

// --- config ----------------------------------------------------------------
enum class CsEncoding { Both, Buffer, Name };

struct Config {
    std::vector<PrPixelFormat> modes;   // PrPixelFormat_Any allowed
    bool        colorspace_set = true;
    std::string colorspace     = kPrWorkingColorSpace;
    bool        sei            = false;   // colorspace names an sei: preset
    prSEIColorCodesRec sei_codes;
    CsEncoding  encoding       = CsEncoding::Both;
    int         latency_frames = 0;
    int         width = 0, height = 0;    // the size asked of the host; 0 = any

    // A8: audio
    enum class Audio { Off, Push, Pull, Both };
    Audio       audio          = Audio::Off;
    int         clock          = -1;      // -1 auto (on iff pull), 0 off, 1 on
    bool        streaming      = false;
    uint32_t    push_samples   = 1024;
    float       audio_rate     = 48000.0f;
    uint32_t    audio_channels = 2;

    bool pull()  const { return audio == Audio::Pull || audio == Audio::Both; }
    bool push()  const { return audio == Audio::Push || audio == Audio::Both; }
    bool wants_clock() const { return clock == -1 ? pull() : clock == 1; }
    const char* audio_name() const {
        return audio == Audio::Off ? "off" : audio == Audio::Push ? "push" : audio == Audio::Pull ? "pull" : "both";
    }
};

std::string trim(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

Config default_config() {
    Config c;
    for (const char* t : {"argb8", "argb16", "argb32f", "bgra8", "bgra16", "bgra32f"})
        c.modes.push_back(find_token(t)->pf);
    return c;
}

Config load_config() {
    Config c = default_config();
    FILE* f = std::fopen(kConfigPath, "r");
    if (f == nullptr) { logf("config: %s absent, using defaults", kConfigPath); return c; }
    char line[1024];
    while (std::fgets(line, sizeof line, f) != nullptr) {
        std::string s = line;
        if (const auto h = s.find('#'); h != std::string::npos) s.resize(h);
        const auto eq = s.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = trim(s.substr(0, eq)), val = trim(s.substr(eq + 1));
        if (key == "modes") {
            c.modes.clear();
            size_t pos = 0;
            while (pos <= val.size()) {
                const auto comma = val.find(',', pos);
                const std::string tok = trim(val.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos));
                if (tok == "any") c.modes.push_back(PrPixelFormat_Any);
                else if (const FormatInfo* fi = find_token(tok)) c.modes.push_back(fi->pf);
                else if (!tok.empty()) logf("config: unknown mode '%s' ignored", tok.c_str());
                if (comma == std::string::npos) break;
                pos = comma + 1;
            }
        } else if (key == "colorspace") {
            if (val == "unset")        { c.colorspace_set = false; }
            else if (val == "working") { c.colorspace_set = true; c.colorspace = kPrWorkingColorSpace; }
            else if (val.rfind("sei:", 0) == 0) {
                using P = PrColorPrimaries; using T = PrTransferCharacteristic; using M = PrMatrixEquations;
                const std::string k = val.substr(4);
                P pr = P::kBT709; T tr = T::kBT709;
                if (k == "pq2020")    { pr = P::kBT2020; tr = T::kBT2100PQ; }
                else if (k == "srgb") { pr = P::kBT709;  tr = T::kIEC61966_2_1; }
                else if (k != "709")  logf("config: unknown sei preset '%s', using 709", k.c_str());
                c.colorspace_set = true; c.sei = true; c.colorspace = val;
                c.sei_codes.colorPrimariesCode         = static_cast<csSDK_int32>(pr);
                c.sei_codes.transferCharacteristicCode = static_cast<csSDK_int32>(tr);
                c.sei_codes.matrixEquationsCode        = static_cast<csSDK_int32>(M::kBT709);
                c.sei_codes.bitDepth                   = static_cast<csSDK_int32>(PrEncodingBitDepth::k32f);
                c.sei_codes.isFullRange                = kPrTrue;
                c.sei_codes.isRGB                      = kPrTrue;
            }
            else                       { c.colorspace_set = true; c.colorspace = val; }
        } else if (key == "cs_encoding") {
            c.encoding = val == "buffer" ? CsEncoding::Buffer
                       : val == "name"   ? CsEncoding::Name : CsEncoding::Both;
        } else if (key == "latency") {
            c.latency_frames = std::atoi(val.c_str());
        } else if (key == "size") {
            // "WxH" asks the host to scale to that raster, the way a hardware
            // device does; "any" follows the host's size.
            int w = 0, h = 0;
            if (val != "any" && std::sscanf(val.c_str(), "%dx%d", &w, &h) != 2) {
                logf("config: size '%s' is not WxH or any; using any", val.c_str());
                w = h = 0;
            }
            c.width = w; c.height = h;
        } else if (key == "audio") {
            c.audio = val == "push" ? Config::Audio::Push : val == "pull" ? Config::Audio::Pull
                    : val == "both" ? Config::Audio::Both : Config::Audio::Off;
            if (val != "off" && val != "push" && val != "pull" && val != "both")
                logf("config: audio '%s' is not off|push|pull|both; using off", val.c_str());
        } else if (key == "clock") {
            c.clock = val == "on" ? 1 : val == "off" ? 0 : -1;
        } else if (key == "streaming") {
            c.streaming = val == "on" || val == "true" || val == "1";
        } else if (key == "push_samples") {
            c.push_samples = static_cast<uint32_t>(std::max(1, std::atoi(val.c_str())));
        } else if (key == "audio_rate") {
            c.audio_rate = static_cast<float>(std::atof(val.c_str()));
        } else if (key == "audio_channels") {
            c.audio_channels = static_cast<uint32_t>(std::max(1, std::min(16, std::atoi(val.c_str()))));
        } else {
            logf("config: unknown key '%s' ignored", key.c_str());
        }
    }
    std::fclose(f);
    if (c.modes.empty()) { logf("config: no usable modes, using defaults"); c.modes = default_config().modes; }
    return c;
}

time_t config_mtime() {
    struct stat st {};
    return ::stat(kConfigPath, &st) == 0 ? st.st_mtime : 0;
}

// --- module state ------------------------------------------------------------
// The ring lives at file scope, not in Plugin, and outlives module resets.
// A NeedsReset runs Startup for the new plugin BEFORE Shutdown of the old
// (seen in the A4 log), and a ring's destructor unlinks its name — so a ring
// owned by the old Plugin could unlink the name the new one had just created,
// leaving a producer publishing into a mapping no consumer can open. One ring
// per process avoids the race outright.
//
// Frames from a second instance would overwrite the first; each frame's log
// line carries its instance id so that shows.
SharedRing S_ring;
uint32_t   S_ring_w = 0, S_ring_h = 0;

struct Plugin {
    SPBasicSuite*    sp     = nullptr;
    PrSDKPPixSuite*  ppix   = nullptr;
    PrSDKTimeSuite*  time   = nullptr;
    PrSDKStringSuite* str   = nullptr;
    PrSDKPlayModuleAudioSuite* audio = nullptr;   // pull mode only
    PrTime           ticks_per_second = 0;

    Config           cfg;
    time_t           cfg_mtime = 0;
};

struct Instance {
    csSDK_int32 id = 0;
    uint64_t    frames = 0;
    uint64_t    by_mode[3] = {0, 0, 0};    // stopped, playing, scrubbing
    // What the last frame looked like, so the log records every change
    // rather than every frame.
    PrPixelFormat last_pf = PrPixelFormat_Invalid;
    int32_t     last_w = -1, last_h = -1, last_rowbytes = 0;

    // Timing, summarised per burst of frames (A4: what does the host's tier
    // conversion cost?). A burst ends on a gap over kBurstGap, a format
    // change, or 240 frames. Not keyed on play mode: AE's preview playback
    // pushes as playmode_Scrubbing with inTime -1, never as Playing.
    // Arrival interval is host cadence; copy is our host -> ring pass;
    // render is the host's GetRenderTime.
    struct Run {
        uint64_t n = 0;
        double   first = 0, last = 0;       // arrival, seconds (monotonic)
        double   interval_sum = 0, interval_max = 0;
        double   copy_sum = 0, copy_max = 0;
        int64_t  render_sum = 0;
        std::string fmt;
        int32_t  w = 0, h = 0;              // of the first frame; a change is logged
    } run;

    // --- A8: audio -------------------------------------------------------
    // The last video push, for the audio-vs-video time delta. PushAudio runs
    // on its own high-priority thread, concurrently with everything else
    // (PrSDKTransmit.h), so these are atomics.
    std::atomic<int64_t> last_video_time{0};
    std::atomic<double>  last_video_wall{0.0};

    // Push ("mirror") audio, stats only. Touched by the push thread; read
    // at StopPushAudio on the host's thread after the pushes have stopped.
    struct Push {
        bool     active = false;
        uint64_t calls = 0, samples = 0, gaps = 0, silent = 0;
        uint32_t ch = 0, min_n = ~0u, max_n = 0;
        int64_t  first_time = 0, last_time = 0, expect = 0;
        double   first_wall = 0, last_wall = 0, interval_max = 0;
        float    peak = 0;
        float    rate = 0;     // what the sample positions were derived with
    } push;

    // The playback clock (pull / clock mode): a thread that reports wall
    // time to the host and, in pull mode, pulls the audio it would play. A
    // software clock, as Adobe's Transmitter sample does it; it exists so
    // the host keeps playing while we measure, nothing more.
    std::thread          clock_thread;
    std::atomic<bool>    clock_run{false};
    tmClockCallback      clock_cb  = nullptr;
    void*                clock_ctx = nullptr;
    float                clock_speed = 1.0f;
    csSDK_int32          play_id = 0;
    float                pull_rate = 0;
    uint32_t             pull_ch = 0;
    uint64_t             pull_calls = 0, pull_samples = 0, pull_errors = 0;
    float                pull_peak = 0;
    prSuiteError         pull_first_err = 0;
    double               clock_started_wall = 0;
};

double now_s() {
    // Monotonic seconds on both platforms; the probe only ever differences these.
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void flush_run(Instance* I, const char* why) {
    auto& r = I->run;
    if (r.n >= 2) {
        const double span = r.last - r.first;
        logf("BURST %s instance %d: %llu frames of %s %dx%d in %.2f s = %.2f fps; interval mean %.2f max %.2f ms; "
             "copy mean %.3f max %.3f ms; host render mean %.2f ms",
             why, I->id, (unsigned long long)r.n, r.fmt.c_str(), r.w, r.h, span,
             span > 0 ? (r.n - 1) / span : 0.0,
             1000.0 * r.interval_sum / (r.n - 1), 1000.0 * r.interval_max,
             1000.0 * r.copy_sum / r.n, 1000.0 * r.copy_max,
             static_cast<double>(r.render_sum) / r.n);
    }
    r = Instance::Run{};
}

void stop_clock(Instance* I, const char* why);
Plugin*   plugin(const tmStdParms* sp)  { return static_cast<Plugin*>(sp->ioPrivatePluginData); }
Instance* instance(const tmInstance* i) { return static_cast<Instance*>(i->ioPrivateInstanceData); }

template <class T>
void acquire(SPBasicSuite* sp, const char* name, int32_t version, T** out) {
    const void* p = nullptr;
    *out = sp->AcquireSuite(name, version, &p) == 0 ? static_cast<T*>(const_cast<void*>(p)) : nullptr;
    if (*out == nullptr) {
        *out = nullptr;
        logf("suite %s v%d unavailable", name, version);
    }
}

// Exceptions must never cross back into the host: it is C, and an escaping
// C++ exception takes After Effects down with it.
template <class F>
tmResult guarded(const char* where, F&& f) {
    try { return f(); }
    catch (const std::exception& e) { logf("%s: exception: %s", where, e.what()); }
    catch (...)                     { logf("%s: unknown exception", where); }
    return tmResult_ErrorUnknown;
}

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

        P->cfg       = load_config();
        P->cfg_mtime = config_mtime();
        if (P->cfg.pull())
            acquire(P->sp, kPrSDKPlayModuleAudioSuite, kPrSDKPlayModuleAudioSuiteVersion, &P->audio);
        logf("startup: audio %s (pull %d, push %d), clock %d, streaming %d, push_samples %u, mode %u ch @ %.0f Hz",
             P->cfg.audio_name(), P->cfg.pull(), P->cfg.push(), P->cfg.wants_clock(), P->cfg.streaming,
             P->cfg.push_samples, P->cfg.audio_channels, P->cfg.audio_rate);
        logf("startup: interface v%d, %zu mode(s) configured, colour space %s, ticks/s %lld",
             tmInterfaceVersion, P->cfg.modes.size(),
             P->cfg.colorspace_set ? ("'" + P->cfg.colorspace + "'").c_str() : "unset",
             static_cast<long long>(P->ticks_per_second));

        std::snprintf(info->outIdentifier.mGUID, sizeof info->outIdentifier.mGUID, "%s", kPluginGUID);
        info->outPriority            = 0;
        info->outAudioAvailable      = P->cfg.pull() ? kPrTrue : kPrFalse;
        info->outAudioDefaultEnabled = kPrFalse;
        info->outClockAvailable      = P->cfg.wants_clock() ? kPrTrue : kPrFalse;
        info->outVideoAvailable      = kPrTrue;
        info->outVideoDefaultEnabled = kPrFalse;   // the user opts in, in Preferences
        const char16_t name[] = u"QCBridgeAE (A4 probe)";
        static_assert(sizeof(prUTF16Char) == sizeof(char16_t), "prUTF16Char is UTF-16");
        std::memcpy(info->outDisplayName, name, sizeof name);
        info->outHideInUI            = kPrFalse;
        info->outHasSetup            = kPrFalse;
        info->outInterfaceVersion    = tmInterfaceVersion;
        info->outPushAudioAvailable  = P->cfg.push() ? kPrTrue : kPrFalse;
        info->outHasStreaming        = P->cfg.streaming ? kPrTrue : kPrFalse;
        return tmResult_Success;
    });
}

tmResult Shutdown(tmStdParms* sp) {
    return guarded("Shutdown", [&] {
        Plugin* P = plugin(sp);
        if (P == nullptr) return tmResult_Success;
        if (P->audio) P->sp->ReleaseSuite(kPrSDKPlayModuleAudioSuite, kPrSDKPlayModuleAudioSuiteVersion);
        if (P->ppix) P->sp->ReleaseSuite(kPrSDKPPixSuite,   kPrSDKPPixSuiteVersion);
        if (P->time) P->sp->ReleaseSuite(kPrSDKTimeSuite,   kPrSDKTimeSuiteVersion);
        if (P->str)  P->sp->ReleaseSuite(kPrSDKStringSuite, kPrSDKStringSuiteVersion);
        logf("shutdown");
        delete P;
        sp->ioPrivatePluginData = nullptr;
        return tmResult_Success;
    });
}

// Polled by the host. A changed config file resets the module, which re-runs
// Startup and re-queries every instance — the A4 control conditions without
// restarting AE.
tmResult NeedsReset(const tmStdParms* sp, prBool* outReset) {
    return guarded("NeedsReset", [&] {
        Plugin* P = plugin(sp);
        *outReset = kPrFalse;
        if (P != nullptr && config_mtime() != P->cfg_mtime) {
            logf("config changed: requesting module reset");
            *outReset = kPrTrue;
        }
        return tmResult_Success;
    });
}

tmResult CreateInstance(const tmStdParms* sp, tmInstance* inst) {
    return guarded("CreateInstance", [&] {
        auto* I = new Instance;
        I->id = inst->inInstanceID;
        inst->ioPrivateInstanceData = I;
        const Plugin* P = plugin(sp);
        const double fps = (inst->inVideoFrameRate > 0 && P->ticks_per_second > 0)
            ? static_cast<double>(P->ticks_per_second) / static_cast<double>(inst->inVideoFrameRate) : 0.0;
        logf("instance %d: video %d, %dx%d, PAR %d:%d, %.3f fps, field %d, timeline %d, play %d; "
             "audio %d, %u ch, sample type %d, %.0f Hz",
             inst->inInstanceID, inst->inHasVideo, inst->inVideoWidth, inst->inVideoHeight,
             inst->inVideoPARNum, inst->inVideoPARDen, fps, static_cast<int>(inst->inVideoFieldType),
             inst->inTimelineID != 0 ? 1 : 0, inst->inPlayID != 0 ? 1 : 0,
             inst->inHasAudio, inst->inNumChannels, static_cast<int>(inst->inAudioSampleType),
             inst->inAudioSampleRate);
        return tmResult_Success;
    });
}

tmResult DisposeInstance(const tmStdParms*, tmInstance* inst) {
    return guarded("DisposeInstance", [&] {
        Instance* I = instance(inst);
        if (I != nullptr) {
            stop_clock(I, "instance disposed");
            flush_run(I, "disposed");
            logf("instance %d disposed after %llu frames (stopped %llu, playing %llu, scrubbing %llu)",
                 I->id, (unsigned long long)I->frames, (unsigned long long)I->by_mode[0],
                 (unsigned long long)I->by_mode[1], (unsigned long long)I->by_mode[2]);
            delete I;
        }
        inst->ioPrivateInstanceData = nullptr;
        return tmResult_Success;
    });
}

// Called with index 0, 1, 2… Returning ContinueIterate asks for the next;
// Success ends the list. The host then picks "the best format to use on a
// per-segment basis" — which of these it picks, per project depth, is the
// first thing A4 records.
tmResult QueryVideoMode(const tmStdParms* sp, const tmInstance* inst, csSDK_int32 index, tmVideoMode* out) {
    return guarded("QueryVideoMode", [&] {
        Plugin* P = plugin(sp);
        const auto& modes = P->cfg.modes;
        if (index < 0 || static_cast<size_t>(index) >= modes.size()) {
            logf("QueryVideoMode: index %d past %zu modes", index, modes.size());
            return tmResult_ErrorInvalidArgument;
        }
        const PrSDKColorSpaceType prefilled = out->outColorSpaceRec.outColorSpaceType;

        out->outWidth       = P->cfg.width;   // 0 = any: we follow the host's size
        out->outHeight      = P->cfg.height;
        out->outPARNum      = 0;
        out->outPARDen      = 0;
        out->outFieldType   = prFieldsAny;
        out->outPixelFormat = modes[static_cast<size_t>(index)];
        out->outStreamLabel = PrSDKString{};
        out->outLatency     = inst->inVideoFrameRate * P->cfg.latency_frames;

        // Only the fields we own. inPrivateData is the host's; an unset
        // colour space leaves the record exactly as the host handed it over,
        // so the "unset" control really is the host default.
        if (P->cfg.colorspace_set && P->cfg.sei) {
            out->outColorSpaceRec.outColorSpaceType = kPrSDKColorSpaceType_SEITags;
            out->outColorSpaceRec.outSEICodesRec    = P->cfg.sei_codes;
        } else if (P->cfg.colorspace_set) {
            ColorSpaceRec& cs = out->outColorSpaceRec;
            cs.outColorSpaceType = kPrSDKColorSpaceType_Predefined;
            cs.ioProfileRec.ioBufferSize        = 0;
            cs.ioProfileRec.inDestinationBuffer = nullptr;
            cs.ioProfileRec.outName             = PrSDKString{};
            if (P->cfg.encoding != CsEncoding::Name) {
                cs.ioProfileRec.inDestinationBuffer = const_cast<char*>(P->cfg.colorspace.c_str());
                cs.ioProfileRec.ioBufferSize        = static_cast<csSDK_int32>(P->cfg.colorspace.size() + 1);
            }
            // A fresh string for every mode, never disposed by us. The host
            // takes ownership of what it reads: with one string shared across
            // modes, every mode after the first carried a spent handle and AE
            // fell back to its default Rec.709 conversion (A4, R17). Same
            // contract PrSDKTransmit.h states for the audio output names.
            if (P->cfg.encoding != CsEncoding::Buffer && P->str != nullptr) {
                PrSDKString name {};
                if (P->str->AllocateFromUTF8(reinterpret_cast<const prUTF8Char*>(P->cfg.colorspace.c_str()),
                                             &name) == 0)
                    cs.ioProfileRec.outName = name;
                else
                    logf("QueryVideoMode: could not allocate colour-space name");
            }
        }

        const FormatInfo* fi = find_format(out->outPixelFormat);
        logf("QueryVideoMode instance %d #%d: offer %s (%s), host prefilled colour type %d, colour space %s",
             inst->inInstanceID, index, fi ? fi->token : "any", fourcc(out->outPixelFormat).c_str(),
             static_cast<int>(prefilled),
             P->cfg.colorspace_set ? P->cfg.colorspace.c_str() : "(unset)");

        return static_cast<size_t>(index) + 1 < modes.size() ? tmResult_ContinueIterate : tmResult_Success;
    });
}

tmResult ActivateDeactivate(const tmStdParms*, const tmInstance* inst, PrActivationEvent ev,
                            prBool audioActive, prBool videoActive) {
    return guarded("ActivateDeactivate", [&] {
        logf("instance %d: activation event %d, audio %d, video %d",
             inst->inInstanceID, static_cast<int>(ev), audioActive, videoActive);
        return tmResult_Success;
    });
}

// Copies one host frame into the ring, byte for byte. Rows may be padded and
// rowbytes may be negative (PrSDKPPixSuite: "May be negative"), i.e. stored
// bottom-up; row y is at base + y * rowbytes either way, so walking by the
// signed stride lands the ring top-down. A4 confirms the orientation with a
// comp that differs top to bottom.
void publish(Plugin* P, Instance* I, const tmInstance* inst, const tmPushVideo* pv, PPixHand h) {
    PrPixelFormat pf = PrPixelFormat_Invalid;
    prRect bounds {};
    csSDK_int32 rowbytes = 0, render_ms = -1;
    char* base = nullptr;
    P->ppix->GetPixelFormat(h, &pf);
    P->ppix->GetBounds(h, &bounds);
    P->ppix->GetRowBytes(h, &rowbytes);
    P->ppix->GetRenderTime(h, &render_ms);
    P->ppix->GetPixels(h, PrPPixBufferAccess_ReadOnly, &base);

    const int32_t w = std::abs(bounds.right - bounds.left);
    const int32_t hgt = std::abs(bounds.bottom - bounds.top);
    const int mode = pv->inPlayMode == playmode_Playing ? 1 : pv->inPlayMode == playmode_Scrubbing ? 2 : 0;
    ++I->frames;
    ++I->by_mode[mode];
    const double arrived = now_s();
    I->last_video_time.store(pv->inTime);
    I->last_video_wall.store(arrived);
    constexpr double kBurstGap = 0.5;
    if (I->run.n > 0 && (arrived - I->run.last > kBurstGap || pf != I->last_pf
                         || w != I->run.w || hgt != I->run.h)) flush_run(I, "ended");

    const FormatInfo* fi = find_format(pf);
    const bool changed = pf != I->last_pf || w != I->last_w || hgt != I->last_h
                      || (rowbytes < 0) != (I->last_rowbytes < 0);
    if (changed || I->frames <= 5 || (I->frames % 100) == 0) {
        logf("frame %llu instance %d: %s (%s)%s%s%s, %dx%d, rowbytes %d, time %lld, mode %d, quality %d, render %d ms",
             (unsigned long long)I->frames, inst->inInstanceID, fi ? fi->token : "UNSUPPORTED",
             fourcc(pf).c_str(), fi && fi->premultiplied ? " premultiplied" : "",
             fi && fi->opaque_x ? " opaque-X" : "", fi && fi->linear ? " LINEAR" : "",
             w, hgt, rowbytes, static_cast<long long>(pv->inTime), mode,
             static_cast<int>(pv->inQuality), render_ms);
    }
    I->last_pf = pf; I->last_w = w; I->last_h = hgt; I->last_rowbytes = rowbytes;

    if (fi == nullptr || base == nullptr || w <= 0 || hgt <= 0) return;

    const auto uw = static_cast<uint32_t>(w), uh = static_cast<uint32_t>(hgt);
    // Sized for native 32f (16 B/px), the widest thing the probe carries, so
    // a tier change never needs a rebuild — only a geometry change does.
    if (!S_ring.valid() || S_ring_w != uw || S_ring_h != uh) {
        S_ring = SharedRing();
        if (S_ring.create(kRingName, frame_bytes(uw, uh, PixelFormat::RGBA32Float))) {
            S_ring_w = uw; S_ring_h = uh;
            logf("ring created for %ux%u", uw, uh);
        } else {
            logf("ring create failed: %s", S_ring.error().c_str());
            return;
        }
    }

    const uint32_t bpp     = bytes_per_pixel(fi->wire);
    const uint32_t dst_row = aligned_bytes_per_row(uw, bpp);
    const size_t   tight   = static_cast<size_t>(uw) * bpp;
    if (static_cast<size_t>(std::abs(rowbytes)) < tight) {
        logf("rowbytes %d shorter than a %zu-byte row; frame skipped", rowbytes, tight);
        return;
    }
    auto* dst = static_cast<uint8_t*>(S_ring.begin_write(static_cast<uint64_t>(dst_row) * uh));
    if (dst == nullptr) { logf("begin_write refused %ux%u", uw, uh); return; }
    const double copy_start = now_s();
    for (uint32_t y = 0; y < uh; ++y)
        std::memcpy(dst + static_cast<size_t>(y) * dst_row,
                    base + static_cast<ptrdiff_t>(y) * rowbytes, tight);
    const double copy_s = now_s() - copy_start;

    {
        auto& r = I->run;
        if (r.n == 0) { r.first = arrived; r.fmt = fi->token; r.w = w; r.h = hgt; }
        else {
            const double iv = arrived - r.last;
            r.interval_sum += iv;
            if (iv > r.interval_max) r.interval_max = iv;
        }
        r.last = arrived;
        ++r.n;
        r.copy_sum += copy_s;
        if (copy_s > r.copy_max) r.copy_max = copy_s;
        r.render_sum += render_ms > 0 ? render_ms : 0;
        if (r.n == 240) flush_run(I, "window");
    }

    FrameDesc d {};
    d.width         = uw;
    d.height        = uh;
    d.bytes_per_row = dst_row;
    d.pixel_format  = fi->wire;
    d.source_tier   = fi->tier;
    d.channel_order = fi->order;
    d.flags         = fi->premultiplied ? kFlagPremultiplied : kFlagNone;
    d.time_value    = pv->inTime;
    d.time_scale    = P->ticks_per_second;
    d.value_scale   = fi->tier == SourceTier::Int16 ? kAE16ValueScale : 1.0f;
    std::snprintf(d.comp_name, sizeof d.comp_name, "%s", fi->token);   // no comp name reaches a Transmit device
    S_ring.commit(d);
}

// --- A8: audio ----------------------------------------------------------------

double secs(const Plugin* P, PrTime t) {
    return P->ticks_per_second > 0 ? static_cast<double>(t) / static_cast<double>(P->ticks_per_second) : 0.0;
}

// Pull mode: the one audio mode we offer. "Only one audio mode is currently
// supported" (PrSDKTransmit.h), so index 0 ends the list.
tmResult QueryAudioMode(const tmStdParms* sp, const tmInstance* inst, csSDK_int32 index, tmAudioMode* out) {
    return guarded("QueryAudioMode", [&] {
        Plugin* P = plugin(sp);
        logf("QueryAudioMode instance %d #%d: host says audio %d, %u ch, sample type %d, %.0f Hz, play %d; "
             "offering %u ch @ %.0f Hz, latency %d frame(s)",
             inst->inInstanceID, index, inst->inHasAudio, inst->inNumChannels,
             static_cast<int>(inst->inAudioSampleType), inst->inAudioSampleRate, inst->inPlayID != 0 ? 1 : 0,
             P->cfg.audio_channels, P->cfg.audio_rate, P->cfg.latency_frames);
        if (index != 0) return tmResult_ErrorInvalidArgument;
        const uint32_t ch = std::min<uint32_t>(P->cfg.audio_channels, kMaxTransmitAudioChannels);
        out->outAudioSampleRate = P->cfg.audio_rate;
        out->outMaxBufferSize   = 48000;      // as Adobe's sample: the largest single pull we make
        out->outNumChannels     = ch;
        out->outLatency         = inst->inVideoFrameRate * P->cfg.latency_frames;
        for (uint32_t i = 0; i < ch; ++i) {
            out->outChannelLabels[i] = ch == 2 && i == 0 ? kPrAudioChannelLabel_FrontLeft
                                     : ch == 2 && i == 1 ? kPrAudioChannelLabel_FrontRight
                                     : kPrAudioChannelLabel_Discrete;
            // "allocated by the plug-in and NOT disposed by the plug-in" — a
            // fresh string each, the same contract as the colour-space name.
            char name[48];
            std::snprintf(name, sizeof name, "QCBridgeAE probe %u", i + 1);
            PrSDKString str {};
            if (P->str != nullptr && P->str->AllocateFromUTF8(reinterpret_cast<const prUTF8Char*>(name), &str) == 0)
                out->outAudioOutputNames[i] = str;
        }
        return tmResult_Success;
    });
}

// The software clock. Reports wall-clock ticks (non-speed-adjusted, the host
// applies speed) every few ms, and in pull mode asks the host for the audio
// those ticks cover so the pull path is exercised and its return codes seen.
void clock_loop(Plugin* P, Instance* I) {
    std::vector<std::vector<float>> bufs(I->pull_ch, std::vector<float>(48000));
    std::vector<float*> ptrs;
    for (auto& b : bufs) ptrs.push_back(b.data());
    double last = now_s(), carry = 0.0;
    while (I->clock_run.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        const double now = now_s();
        const double dt  = now - last;
        last = now;
        if (P->audio != nullptr && I->play_id != 0 && I->pull_ch > 0 && I->pull_rate > 0) {
            carry += dt * I->pull_rate * std::fabs(I->clock_speed);
            auto n = static_cast<unsigned>(carry);
            if (n > 0) {
                n = std::min<unsigned>(n, 48000);
                carry -= n;
                const prSuiteError e = P->audio->GetNextAudioBuffer(I->play_id, nullptr, ptrs.data(), n);
                ++I->pull_calls;
                if (e != 0) { if (I->pull_errors++ == 0) I->pull_first_err = e; }
                else {
                    I->pull_samples += n;
                    for (unsigned i = 0; i < n; ++i) I->pull_peak = std::max(I->pull_peak, std::fabs(bufs[0][i]));
                }
            }
        }
        if (I->clock_cb != nullptr && I->clock_run.load())
            I->clock_cb(I->clock_ctx, static_cast<PrTime>(dt * static_cast<double>(P->ticks_per_second)));
    }
}

void stop_clock(Instance* I, const char* why) {
    if (!I->clock_run.exchange(false)) return;
    if (I->clock_thread.joinable()) I->clock_thread.join();
    logf("clock stopped (%s) instance %d after %.2f s: %llu pull calls, %llu samples, %llu errors (first %d), peak %.3f",
         why, I->id, now_s() - I->clock_started_wall, (unsigned long long)I->pull_calls,
         (unsigned long long)I->pull_samples, (unsigned long long)I->pull_errors,
         static_cast<int>(I->pull_first_err), I->pull_peak);
    I->clock_cb = nullptr; I->clock_ctx = nullptr;
}

tmResult StartPlaybackClock(const tmStdParms* sp, const tmInstance* inst, const tmPlaybackClock* c) {
    return guarded("StartPlaybackClock", [&] {
        Plugin* P = plugin(sp);
        Instance* I = instance(inst);
        logf("StartPlaybackClock instance %d: start %lld (%.3f s), mode %d, speed %.2f, in %.3f, out %.3f, loop %d, "
             "audio offset %lld, video offset %lld, play %d, host audio %d %u ch %.0f Hz",
             inst->inInstanceID, static_cast<long long>(c->inStartTime), secs(P, c->inStartTime),
             static_cast<int>(c->inPlayMode), c->inSpeed, secs(P, c->inInTime), secs(P, c->inOutTime), c->inLoop,
             static_cast<long long>(c->inAudioOffset), static_cast<long long>(c->inVideoOffset),
             inst->inPlayID != 0 ? 1 : 0, inst->inHasAudio, inst->inNumChannels, inst->inAudioSampleRate);
        if (I == nullptr) return tmResult_Success;
        // "Start may be called multiple times without a stop in between to
        // update playback parameters": restart the thread with the new ones.
        stop_clock(I, "restart");
        I->clock_cb    = c->inClockCallback;
        I->clock_ctx   = c->inCallbackContext;
        I->clock_speed = c->inSpeed;
        I->play_id     = inst->inPlayID;
        I->pull_rate   = inst->inAudioSampleRate > 0 ? inst->inAudioSampleRate : P->cfg.audio_rate;
        I->pull_ch     = inst->inNumChannels > 0 ? std::min<uint32_t>(inst->inNumChannels, 16) : P->cfg.audio_channels;
        I->pull_calls = I->pull_samples = I->pull_errors = 0; I->pull_peak = 0; I->pull_first_err = 0;
        I->clock_started_wall = now_s();
        // "Invoke the callback immediately during StartPlaybackClock with a
        // negative number for preroll"
        if (c->inClockCallback != nullptr)
            c->inClockCallback(c->inCallbackContext, -(inst->inVideoFrameRate * P->cfg.latency_frames));
        I->clock_run.store(true);
        I->clock_thread = std::thread(clock_loop, P, I);
        return tmResult_Success;
    });
}

tmResult StopPlaybackClock(const tmStdParms*, const tmInstance* inst) {
    return guarded("StopPlaybackClock", [&] {
        logf("StopPlaybackClock instance %d", inst->inInstanceID);
        if (Instance* I = instance(inst)) stop_clock(I, "StopPlaybackClock");
        return tmResult_Success;
    });
}

// Push ("mirror") audio: the host's own audio device plays, and the samples it
// plays are pushed here too. The question A8 asks is whether this happens at
// all with Adobe Desktop Audio as the primary, and in what form.
tmResult StartPushAudio(const tmStdParms* sp, const tmInstance* inst, PrTime start, float speed, PrTime in,
                        PrTime out, prBool loop, prBool scrubbing, csSDK_uint32* outSamplesPerFrame) {
    return guarded("StartPushAudio", [&] {
        Plugin* P = plugin(sp);
        Instance* I = instance(inst);
        *outSamplesPerFrame = P->cfg.push_samples;
        logf("StartPushAudio instance %d: start %lld (%.3f s), speed %.2f, in %.3f, out %.3f, loop %d, scrubbing %d, "
             "host audio %d %u ch type %d %.0f Hz, play %d; asked %u samples per push",
             inst->inInstanceID, static_cast<long long>(start), secs(P, start), speed, secs(P, in), secs(P, out),
             loop, scrubbing, inst->inHasAudio, inst->inNumChannels, static_cast<int>(inst->inAudioSampleType),
             inst->inAudioSampleRate, inst->inPlayID != 0 ? 1 : 0, P->cfg.push_samples);
        if (I != nullptr) {
            I->push = Instance::Push{};
            I->push.active = true;
            I->push.rate   = inst->inAudioSampleRate > 0 ? inst->inAudioSampleRate : P->cfg.audio_rate;
        }
        return tmResult_Success;
    });
}

// High-priority host thread; must not block (Adobe's sample). Stats only,
// and a log line for the first few pushes, then one in two hundred.
tmResult PushAudio(const tmStdParms* sp, const tmInstance* inst, const tmPushAudio* a) {
    return guarded("PushAudio", [&] {
        Plugin* P = plugin(sp);
        Instance* I = instance(inst);
        if (I == nullptr || P == nullptr) return tmResult_Success;
        auto& s = I->push;
        const double wall = now_s();
        float peak = 0;
        if (a->inBuffers != nullptr && a->inNumChannels > 0 && a->inBuffers[0] != nullptr)
            for (csSDK_uint32 i = 0; i < a->inNumSamples; ++i) peak = std::max(peak, std::fabs(a->inBuffers[0][i]));
        if (peak == 0) ++s.silent;
        const auto span = static_cast<int64_t>(static_cast<double>(a->inNumSamples) * static_cast<double>(P->ticks_per_second) / s.rate);
        const bool gap = s.calls > 0 && a->inTime != s.expect;
        if (gap) ++s.gaps;
        if (s.calls == 0) { s.first_time = a->inTime; s.first_wall = wall; }
        else s.interval_max = std::max(s.interval_max, wall - s.last_wall);
        const int64_t vt = I->last_video_time.load();
        if (s.calls < 6 || s.calls % 200 == 0 || (gap && s.gaps <= 5)) {
            logf("PushAudio #%llu instance %d: %u samples x %u ch, time %lld (%.3f s), video %.3f s (%+.1f ms, %.0f ms ago), "
                 "peak %.3f, interval %.2f ms%s",
                 (unsigned long long)s.calls + 1, inst->inInstanceID, a->inNumSamples, a->inNumChannels,
                 static_cast<long long>(a->inTime), secs(P, a->inTime), secs(P, vt),
                 1000.0 * secs(P, a->inTime - vt), 1000.0 * (wall - I->last_video_wall.load()), peak,
                 s.calls > 0 ? 1000.0 * (wall - s.last_wall) : 0.0,
                 gap ? " GAP (not contiguous with the previous push)" : "");
        }
        ++s.calls;
        s.samples  += a->inNumSamples;
        s.ch        = a->inNumChannels;
        s.min_n     = std::min(s.min_n, a->inNumSamples);
        s.max_n     = std::max(s.max_n, a->inNumSamples);
        s.last_time = a->inTime;
        s.last_wall = wall;
        s.expect    = a->inTime + span;
        s.peak      = std::max(s.peak, peak);
        return tmResult_Success;
    });
}

tmResult StopPushAudio(const tmStdParms* sp, const tmInstance* inst) {
    return guarded("StopPushAudio", [&] {
        Plugin* P = plugin(sp);
        Instance* I = instance(inst);
        if (I == nullptr) return tmResult_Success;
        auto& s = I->push;
        const double wall = now_s() - s.first_wall;
        logf("StopPushAudio instance %d: %llu pushes, %llu samples x %u ch = %.3f s of audio at %.0f Hz over %.3f s wall "
             "(%.3f s .. %.3f s timeline), %u..%u samples per push, interval mean %.2f max %.2f ms, %llu gaps, "
             "%llu silent pushes, peak %.3f",
             inst->inInstanceID, (unsigned long long)s.calls, (unsigned long long)s.samples, s.ch,
             s.rate > 0 ? static_cast<double>(s.samples) / s.rate : 0.0, s.rate, s.calls > 0 ? wall : 0.0,
             secs(P, s.first_time), secs(P, s.last_time), s.min_n == ~0u ? 0 : s.min_n, s.max_n,
             s.calls > 1 ? 1000.0 * (s.last_wall - s.first_wall) / (s.calls - 1) : 0.0, 1000.0 * s.interval_max,
             (unsigned long long)s.gaps, (unsigned long long)s.silent, s.peak);
        s.active = false;
        return tmResult_Success;
    });
}

// Streaming: declared only as a control for whether push audio depends on it.
bool S_streaming_enabled = false;
tmStreamingStateChangedCallback S_streaming_cb = nullptr;
void* S_streaming_ctx = nullptr;

tmResult SetStreamingStateChangedCallback(const tmStdParms*, void* ctx, tmStreamingStateChangedCallback cb) {
    logf("SetStreamingStateChangedCallback: %s", cb ? "set" : "cleared");
    S_streaming_cb = cb; S_streaming_ctx = ctx;
    return tmResult_Success;
}
tmResult EnableStreaming(const tmStdParms*, prBool on) {
    logf("EnableStreaming: %d", on);
    S_streaming_enabled = on != 0;
    if (S_streaming_cb != nullptr) S_streaming_cb(S_streaming_ctx);
    return tmResult_Success;
}
tmResult IsStreamingEnabled(const tmStdParms*, prBool* out) {
    *out = S_streaming_enabled ? kPrTrue : kPrFalse;
    return tmResult_Success;
}
tmResult IsStreamingActive(const tmStdParms*, prBool* out) {
    *out = S_streaming_enabled ? kPrTrue : kPrFalse;   // "connected" whenever enabled
    return tmResult_Success;
}

tmResult PushVideo(const tmStdParms* sp, const tmInstance* inst, const tmPushVideo* pv) {
    Plugin* P = plugin(sp);
    Instance* I = instance(inst);
    const tmResult r = guarded("PushVideo", [&] {
        if (P != nullptr && I != nullptr && P->ppix != nullptr && pv->inFrameCount > 0)
            publish(P, I, inst, pv, pv->inFrames[0].inFrame);
        if (pv->inFrameCount > 1) logf("PushVideo carried %zu frames; probe publishes the first", pv->inFrameCount);
        return tmResult_Success;
    });
    // "The plug-in is responsible for disposing of all passed in ppix" —
    // every one, whatever happened above.
    if (P != nullptr && P->ppix != nullptr)
        for (csSDK_size_t i = 0; i < pv->inFrameCount; ++i) P->ppix->Dispose(pv->inFrames[i].inFrame);
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
        // A8: every audio entry point is wired whatever the config declares,
        // so a host that calls one it should not is seen in the log.
        out->QueryAudioMode     = QueryAudioMode;
        out->StartPlaybackClock = StartPlaybackClock;
        out->StopPlaybackClock  = StopPlaybackClock;
        out->StartPushAudio     = StartPushAudio;
        out->PushAudio          = PushAudio;
        out->StopPushAudio      = StopPushAudio;
        out->SetStreamingStateChangedCallback = SetStreamingStateChangedCallback;
        out->EnableStreaming    = EnableStreaming;
        out->IsStreamingEnabled = IsStreamingEnabled;
        out->IsStreamingActive  = IsStreamingActive;
    } else {
        logf("--- module unload ---");
    }
    return tmResult_Success;
}
