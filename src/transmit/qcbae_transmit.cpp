// QCBridgeAE — the Mercury Transmit device (phase A6, Route A).
//
// After Effects or Premiere pushes the frames it has already rendered; this
// turns each into top-down RGBA16F in a shared ring QCView reads. Everything
// here follows from what the A4 probe measured
// (lab/results/2026-09-21-a4-transmit-probe/), and each rule says where:
//
//   * Offer ARGB_4444_32f then BGRA_4444_32f, nothing else (PLAN.md D1). The
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
//   * Each viewer change pushes two frames ~3 ms apart. A frame whose PPix
//     unique key matches the last one published is not converted again.
//
// Privacy: a Transmit device receives no project paths or comp names; the
// only label published is the host's name (PLAN.md §Privacy 5, 6).

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

constexpr const char* kLogPath = "/tmp/qcbridgeae-transmit.log";

// Persistent identity in the host's device list. Never change it, or the host
// treats the device as new and forgets the user's choice. (The A4 probe has
// its own.)
constexpr const char* kPluginGUID = "1BB013B9-952D-4D6C-B35E-1E0EF3F52CA8";

// Which host we are in decides the ring name (PLAN.md A6: one fixed name per
// host, so AE and Premiere running together do not overwrite each other).
struct Host { const char* ring; const char* label; };

Host detect_host() {
    const char* prog = getprogname();
    const std::string p = prog != nullptr ? prog : "";
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
    uint64_t   published = 0, skipped_duplicates = 0;
    std::vector<unsigned char> last_key;

    // Host state is derived, not last-writer-wins. AE creates and discards
    // an instance for every item it touches while opening a project (seen in
    // the A6 log: a dozen in one second), and an old instance's deactivation
    // can arrive after its replacement's activation. The ring reads Active
    // while any instance has video on; paused only when none do, with the
    // reason of the last deactivation.
    std::set<csSDK_int32> video_on;
    HostState last_pause = HostState::Paused;
};
Ring S;

HostState derived_state() {
    return S.video_on.empty() ? S.last_pause : HostState::Active;
}
void publish_state() {
    if (S.ring.valid()) S.ring.set_host_state(derived_state());
}

// Replacing a mapping tells whoever holds the old one to let go first.
void retire_ring(const char* why) {
    if (!S.ring.valid()) return;
    S.ring.set_host_state(HostState::Retired);
    logf("ring retired (%s) after %llu frames, %llu duplicates skipped", why,
         (unsigned long long)S.published, (unsigned long long)S.skipped_duplicates);
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
    size_t            key_size = 0;
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
        if (P->ppix != nullptr && P->ppix->GetUniqueKeySize(&P->key_size) != 0) P->key_size = 0;
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

// The ring outlives module resets on purpose; it is retired only when the
// host unloads the module (xTransmitEntry with loadModule false).
tmResult Shutdown(tmStdParms* sp) {
    return guarded("Shutdown", [&] {
        Plugin* P = plugin(sp);
        if (P == nullptr) return tmResult_Success;
        if (P->ppix) P->sp->ReleaseSuite(kPrSDKPPixSuite,   kPrSDKPPixSuiteVersion);
        if (P->time) P->sp->ReleaseSuite(kPrSDKTimeSuite,   kPrSDKTimeSuiteVersion);
        if (P->str)  P->sp->ReleaseSuite(kPrSDKStringSuite, kPrSDKStringSuiteVersion);
        delete P;
        sp->ioPrivatePluginData = nullptr;
        return tmResult_Success;
    });
}

tmResult CreateInstance(const tmStdParms*, tmInstance* inst) {
    return guarded("CreateInstance", [&] {
        inst->ioPrivateInstanceData = nullptr;
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
        return tmResult_Success;
    });
}

tmResult QueryVideoMode(const tmStdParms* sp, const tmInstance*, csSDK_int32 index, tmVideoMode* out) {
    return guarded("QueryVideoMode", [&] {
        constexpr int kCount = static_cast<int>(sizeof kModes / sizeof kModes[0]);
        if (index < 0 || index >= kCount) return tmResult_ErrorInvalidArgument;
        Plugin* P = plugin(sp);

        out->outWidth       = 0;   // any: we follow the host's size
        out->outHeight      = 0;
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
    // The pair a viewer change pushes: identical key, identical pixels.
    if (P->key_size > 0) {
        std::vector<unsigned char> key(P->key_size);
        if (P->ppix->GetUniqueKey(h, key.data(), key.size()) == 0) {
            if (key == S.last_key) { ++S.skipped_duplicates; return; }
            S.last_key = std::move(key);
        } else {
            S.last_key.clear();
        }
    }

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

    if (++S.published == 1 || (S.published % 500) == 0 || cr.has_inf || cr.has_nan)
        logf("published %llu (%ux%u %s%s%s), %llu duplicates skipped", (unsigned long long)S.published,
             uw, uh, order == HostOrder::ARGB ? "ARGB" : "BGRA",
             cr.has_inf ? ", has inf" : "", cr.has_nan ? ", has NaN" : "",
             (unsigned long long)S.skipped_duplicates);
}

tmResult PushVideo(const tmStdParms* sp, const tmInstance*, const tmPushVideo* pv) {
    Plugin* P = plugin(sp);
    const tmResult r = guarded("PushVideo", [&] {
        if (P != nullptr && P->ppix != nullptr && pv->inFrameCount > 0)
            publish(P, pv, pv->inFrames[0].inFrame);
        return tmResult_Success;
    });
    // "The plug-in is responsible for disposing of all passed in ppix."
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
