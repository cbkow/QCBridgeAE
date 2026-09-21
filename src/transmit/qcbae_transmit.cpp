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
// optional; defaults are the design in PLAN.md D1/D5.
//   modes       = argb8, argb16, argb32f, bgra8, bgra16, bgra32f
//                 (also *32f_linear, prgb*, bgrp*, xrgb*, bgrx*, any)
//   colorspace  = working | unset | <a predefined name, e.g. BT.709 RGB Full (Scene)>
//   cs_encoding = both | buffer | name   — how a predefined name is passed.
//                 The SDK documents the token but no sample fills one, so
//                 which field the host reads is itself an A4 question.
//   latency     = 0   (frames of preroll the host sends ahead of playback)
//
// Privacy: the host gives a Transmit device no project paths or comp names,
// and nothing here asks for them (PLAN.md §Privacy 5, 6).

#include "PrSDKTransmit.h"
#include "PrSDKPPixSuite.h"
#include "PrSDKTimeSuite.h"
#include "PrSDKStringSuite.h"
#include "PrSDKPixelFormat.h"
#include "PrSDKColorSpaces.h"
#include "PrSDKColorProfile.h"
#include "SPBasic.h"

#include "common/surface/shared_ring.h"

#include <sys/stat.h>

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
constexpr const char* kConfigPath = "/tmp/qcbridgeae-transmit.conf";
constexpr const char* kLogPath    = "/tmp/qcbridgeae-transmit.log";

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
    CsEncoding  encoding       = CsEncoding::Both;
    int         latency_frames = 0;
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
            else                       { c.colorspace_set = true; c.colorspace = val; }
        } else if (key == "cs_encoding") {
            c.encoding = val == "buffer" ? CsEncoding::Buffer
                       : val == "name"   ? CsEncoding::Name : CsEncoding::Both;
        } else if (key == "latency") {
            c.latency_frames = std::atoi(val.c_str());
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
    PrTime           ticks_per_second = 0;

    Config           cfg;
    time_t           cfg_mtime = 0;
    PrSDKString      cs_name {};       // allocated for cs_encoding name/both
    bool             cs_name_valid = false;
};

struct Instance {
    csSDK_int32 id = 0;
    uint64_t    frames = 0;
    uint64_t    by_mode[3] = {0, 0, 0};    // stopped, playing, scrubbing
    // What the last frame looked like, so the log records every change
    // rather than every frame.
    PrPixelFormat last_pf = PrPixelFormat_Invalid;
    int32_t     last_w = -1, last_h = -1, last_rowbytes = 0;
};

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
        logf("startup: interface v%d, %zu mode(s) configured, colour space %s, ticks/s %lld",
             tmInterfaceVersion, P->cfg.modes.size(),
             P->cfg.colorspace_set ? ("'" + P->cfg.colorspace + "'").c_str() : "unset",
             static_cast<long long>(P->ticks_per_second));

        std::snprintf(info->outIdentifier.mGUID, sizeof info->outIdentifier.mGUID, "%s", kPluginGUID);
        info->outPriority            = 0;
        info->outAudioAvailable      = kPrFalse;
        info->outAudioDefaultEnabled = kPrFalse;
        info->outClockAvailable      = kPrFalse;   // video only; the host keeps the clock
        info->outVideoAvailable      = kPrTrue;
        info->outVideoDefaultEnabled = kPrFalse;   // the user opts in, in Preferences
        const char16_t name[] = u"QCBridgeAE (A4 probe)";
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

tmResult Shutdown(tmStdParms* sp) {
    return guarded("Shutdown", [&] {
        Plugin* P = plugin(sp);
        if (P == nullptr) return tmResult_Success;
        if (P->cs_name_valid && P->str != nullptr) P->str->DisposeString(&P->cs_name);
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
        logf("instance %d: video %d, %dx%d, PAR %d:%d, %.3f fps, field %d, timeline %d, play %d",
             inst->inInstanceID, inst->inHasVideo, inst->inVideoWidth, inst->inVideoHeight,
             inst->inVideoPARNum, inst->inVideoPARDen, fps, static_cast<int>(inst->inVideoFieldType),
             inst->inTimelineID != 0 ? 1 : 0, inst->inPlayID != 0 ? 1 : 0);
        return tmResult_Success;
    });
}

tmResult DisposeInstance(const tmStdParms*, tmInstance* inst) {
    return guarded("DisposeInstance", [&] {
        Instance* I = instance(inst);
        if (I != nullptr) {
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

        out->outWidth       = 0;             // any: we follow the host's size
        out->outHeight      = 0;
        out->outPARNum      = 0;
        out->outPARDen      = 0;
        out->outFieldType   = prFieldsAny;
        out->outPixelFormat = modes[static_cast<size_t>(index)];
        out->outStreamLabel = PrSDKString{};
        out->outLatency     = inst->inVideoFrameRate * P->cfg.latency_frames;

        // Only the fields we own. inPrivateData is the host's; an unset
        // colour space leaves the record exactly as the host handed it over,
        // so the "unset" control really is the host default.
        if (P->cfg.colorspace_set) {
            ColorSpaceRec& cs = out->outColorSpaceRec;
            cs.outColorSpaceType = kPrSDKColorSpaceType_Predefined;
            cs.ioProfileRec.ioBufferSize        = 0;
            cs.ioProfileRec.inDestinationBuffer = nullptr;
            cs.ioProfileRec.outName             = PrSDKString{};
            if (P->cfg.encoding != CsEncoding::Name) {
                cs.ioProfileRec.inDestinationBuffer = const_cast<char*>(P->cfg.colorspace.c_str());
                cs.ioProfileRec.ioBufferSize        = static_cast<csSDK_int32>(P->cfg.colorspace.size() + 1);
            }
            if (P->cfg.encoding != CsEncoding::Buffer && P->str != nullptr) {
                if (!P->cs_name_valid
                    && P->str->AllocateFromUTF8(reinterpret_cast<const prUTF8Char*>(P->cfg.colorspace.c_str()),
                                                &P->cs_name) == 0)
                    P->cs_name_valid = true;
                if (P->cs_name_valid) cs.ioProfileRec.outName = P->cs_name;
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
    for (uint32_t y = 0; y < uh; ++y)
        std::memcpy(dst + static_cast<size_t>(y) * dst_row,
                    base + static_cast<ptrdiff_t>(y) * rowbytes, tight);

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

extern "C" __attribute__((visibility("default")))
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
    } else {
        logf("--- module unload ---");
    }
    return tmResult_Success;
}
