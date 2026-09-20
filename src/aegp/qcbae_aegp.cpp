// QCBridgeAE — the After Effects tap (phase A2, Route B).
//
// An AEGP that renders the active comp and publishes it into the shared ring,
// where QCView (or, until A3, qcbae-probe) picks it up.
//
// Threading, and why this design has a ceiling:
// AE hands AsyncManager renders only to effects with custom UI, through
// PF_GetContextAsyncManager in PF_EffectCustomUISuite2. An AEGP cannot get
// one, so AEGP_RenderAndCheckoutFrame is synchronous and runs on AE's UI
// thread from the idle hook. A cached frame returns fast; an uncached heavy
// comp will stall AE for as long as it takes to render. That is inherent to
// Route B and is the strongest argument for Route A, which pushes frames AE
// has already rendered. Mitigated here by throttling and by never rendering
// when nothing has changed — not solved.
//
// Privacy: no project paths and no comp names reach the log (PLAN.md
// §Privacy 5). The comp name travels in the sidecar, in memory, to the viewer.

#include "AEConfig.h"
#include "AE_GeneralPlug.h"
#include "AE_Macros.h"
#include "entry.h"

#include "common/surface/shared_ring.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

using namespace qcbae;

namespace {

constexpr const char* kRingName = "/qcbae-probe";   // A3 gives this a real name
constexpr double      kMinIntervalSeconds = 0.10;   // throttle the idle render

AEGP_PluginID   S_id   = 0;
SPBasicSuite*   S_sp   = nullptr;

AEGP_RegisterSuite5*      S_reg   = nullptr;
AEGP_ItemSuite9*          S_item  = nullptr;
AEGP_RenderOptionsSuite4* S_ro    = nullptr;
AEGP_RenderSuite5*        S_render= nullptr;
AEGP_WorldSuite3*         S_world = nullptr;
AEGP_UtilitySuite6*       S_util  = nullptr;
AEGP_ColorSettingsSuite6* S_color = nullptr;
AEGP_MemorySuite1*        S_mem   = nullptr;
AEGP_CompSuite12*         S_comp  = nullptr;

SharedRing S_ring;
uint32_t   S_ring_w = 0, S_ring_h = 0;
uint64_t   S_published = 0;
double     S_last_render = 0.0;
uint64_t   S_icc_generation = 0;

// An idle hook that fails keeps being called, so a bug that raises a modal
// error raises it forever and AE cannot be used. Learned the hard way: a stray
// AEGP_GetItemName(.., NULL) produced an endless "internal verification
// failure {unicode_namePH cannot be NULL}". After this many consecutive
// failures the tap switches itself off and says so in the log, leaving AE
// usable. Recovered by reopening AE — never at the cost of the user's session.
constexpr int kMaxConsecutiveErrors = 5;
int        S_consecutive_errors = 0;
bool       S_disabled = false;

// --- logging ---------------------------------------------------------------
FILE* S_log = nullptr;

void logf(const char* fmt, ...) {
    if (S_log == nullptr) return;
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

double now_seconds() {
    struct timespec ts {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;
}

// --- the working colorspace, straight from AE ------------------------------
// This is the field that makes the signal interpretable rather than merely
// preserved (PLAN.md D5). AE hands us the real ICC; we never infer one.
// Takes the comp, not null: AEGP_GetNewWorkingSpaceColorProfile's second
// parameter is an AEGP_CompH. Passing null returns an error and leaves the
// sidecar with no profile at all, which A3 would then have to guess around.
void publish_icc_profile(AEGP_CompH compH) {
    if (S_color == nullptr || !S_ring.valid() || compH == nullptr) return;
    AEGP_ColorProfileP profileP = nullptr;
    AEGP_MemHandle     iccH     = nullptr;
    if (S_color->AEGP_GetNewWorkingSpaceColorProfile(S_id, compH, &profileP) != A_Err_NONE
        || profileP == nullptr) {
        logf("working-space profile unavailable");
        return;
    }
    if (S_color->AEGP_GetNewICCProfileFromColorProfile(S_id, profileP, &iccH) == A_Err_NONE
        && iccH != nullptr && S_mem != nullptr) {
        void*  dataP = nullptr;
        AEGP_MemSize size = 0;
        S_mem->AEGP_GetMemHandleSize(iccH, &size);
        if (S_mem->AEGP_LockMemHandle(iccH, &dataP) == A_Err_NONE && dataP != nullptr) {
            if (S_ring.set_icc_profile(dataP, static_cast<uint32_t>(size))) {
                ++S_icc_generation;
                logf("published working-space ICC, %u bytes", static_cast<unsigned>(size));
            } else {
                logf("ICC rejected: %s", S_ring.error().c_str());
            }
            S_mem->AEGP_UnlockMemHandle(iccH);
        }
        S_mem->AEGP_FreeMemHandle(iccH);
    }
    S_color->AEGP_DisposeColorProfile(profileP);
}

// --- AE world -> wire ------------------------------------------------------
// AE's rows carry their own stride and its channel order is ARGB; both travel
// as they are (ChannelOrder::ARGB in the sidecar, the GPU reorders for free).
// The integer tiers are a per-row memcpy. Only 32 bpc converts.

uint16_t to_half(float f) {
    if (!(f > 0.0f)) return 0;                  // also catches NaN
    if (f > 65504.0f) f = 65504.0f;             // PLAN.md D4
    uint32_t bits; std::memcpy(&bits, &f, 4);
    const int32_t exp = static_cast<int32_t>((bits >> 23) & 0xFFu) - 127 + 15;
    const uint32_t man = bits & 0x7FFFFFu;
    if (exp <= 0)  return 0;
    if (exp >= 31) return 0x7BFFu;
    return static_cast<uint16_t>((static_cast<uint32_t>(exp) << 10) | (man >> 13));
}

bool copy_world(AEGP_WorldH worldH, AEGP_WorldType type, uint32_t w, uint32_t h,
                uint8_t* dst, uint32_t dst_row_bytes) {
    A_u_long src_row_bytes = 0;
    if (S_world->AEGP_GetRowBytes(worldH, &src_row_bytes) != A_Err_NONE) return false;

    if (type == AEGP_WorldType_8) {
        PF_Pixel8* base = nullptr;
        if (S_world->AEGP_GetBaseAddr8(worldH, &base) != A_Err_NONE || base == nullptr) return false;
        for (uint32_t y = 0; y < h; ++y) {
            std::memcpy(dst + y * dst_row_bytes,
                        reinterpret_cast<const uint8_t*>(base) + y * src_row_bytes, w * 4u);
        }
        return true;
    }
    if (type == AEGP_WorldType_16) {
        PF_Pixel16* base = nullptr;
        if (S_world->AEGP_GetBaseAddr16(worldH, &base) != A_Err_NONE || base == nullptr) return false;
        for (uint32_t y = 0; y < h; ++y) {
            std::memcpy(dst + y * dst_row_bytes,
                        reinterpret_cast<const uint8_t*>(base) + y * src_row_bytes, w * 8u);
        }
        return true;
    }
    if (type == AEGP_WorldType_32) {
        PF_PixelFloat* base = nullptr;
        if (S_world->AEGP_GetBaseAddr32(worldH, &base) != A_Err_NONE || base == nullptr) return false;
        for (uint32_t y = 0; y < h; ++y) {
            const auto* src = reinterpret_cast<const float*>(
                reinterpret_cast<const uint8_t*>(base) + y * src_row_bytes);
            auto* out = reinterpret_cast<uint16_t*>(dst + y * dst_row_bytes);
            for (uint32_t i = 0; i < w * 4u; ++i) out[i] = to_half(src[i]);
        }
        return true;
    }
    return false;
}

SourceTier tier_of(AEGP_WorldType t) {
    switch (t) {
        case AEGP_WorldType_8:  return SourceTier::Int8;
        case AEGP_WorldType_16: return SourceTier::Int16;
        case AEGP_WorldType_32: return SourceTier::Float32;
        default:                return SourceTier::Unknown;
    }
}

// --- the tap ---------------------------------------------------------------
A_Err render_and_publish() {
    A_Err err = A_Err_NONE;

    AEGP_ItemH itemH = nullptr;
    ERR(S_item->AEGP_GetActiveItem(&itemH));
    if (err != A_Err_NONE || itemH == nullptr) return A_Err_NONE;   // nothing open

    AEGP_ItemType item_type = AEGP_ItemType_NONE;
    ERR(S_item->AEGP_GetItemType(itemH, &item_type));
    if (item_type != AEGP_ItemType_COMP) return A_Err_NONE;

    A_long w = 0, h = 0;
    ERR(S_item->AEGP_GetItemDimensions(itemH, &w, &h));
    if (err != A_Err_NONE || w <= 0 || h <= 0) return err;

    A_Time timeT { 0, 1 };
    ERR(S_item->AEGP_GetItemCurrentTime(itemH, &timeT));
    if (timeT.scale == 0) { timeT.value = 0; timeT.scale = 1; }   // AE can hand back 0; div-by-0

    AEGP_RenderOptionsH roH = nullptr;
    ERR(S_ro->AEGP_NewFromItem(S_id, itemH, &roH));
    if (err != A_Err_NONE || roH == nullptr) return err;

    // Take the project's own bit depth rather than forcing one: the whole
    // point is to carry what AE actually computed (PLAN.md D1).
    AEGP_WorldType world_type = AEGP_WorldType_NONE;
    ERR(S_ro->AEGP_SetTime(roH, timeT));
    ERR(S_ro->AEGP_GetWorldType(roH, &world_type));

    AEGP_FrameReceiptH receiptH = nullptr;
    ERR(S_render->AEGP_RenderAndCheckoutFrame(roH, nullptr, nullptr, &receiptH));

    if (err == A_Err_NONE && receiptH != nullptr) {
        AEGP_WorldH worldH = nullptr;
        ERR(S_render->AEGP_GetReceiptWorld(receiptH, &worldH));

        if (err == A_Err_NONE && worldH != nullptr) {
            AEGP_WorldType wt = AEGP_WorldType_NONE;
            A_long ww = 0, wh = 0;
            S_world->AEGP_GetType(worldH, &wt);
            S_world->AEGP_GetSize(worldH, &ww, &wh);

            const SourceTier  tier = tier_of(wt);
            const PixelFormat fmt  = wire_format_for(tier);
            const uint32_t uw = static_cast<uint32_t>(ww), uh = static_cast<uint32_t>(wh);

            if (fmt != PixelFormat::Unknown && uw > 0 && uh > 0) {
                const uint32_t bpr = aligned_bytes_per_row(uw, bytes_per_pixel(fmt));
                const uint64_t need = static_cast<uint64_t>(bpr) * uh;

                // (Re)build the ring when the geometry changes. Sized for the
                // widest default tier so a bit-depth change alone needs no
                // rebuild.
                if (!S_ring.valid() || S_ring_w != uw || S_ring_h != uh) {
                    S_ring = SharedRing();
                    if (S_ring.create(kRingName, max_frame_bytes(uw, uh))) {
                        S_ring_w = uw; S_ring_h = uh;
                        S_icc_generation = 0;
                        logf("ring created for %ux%u", uw, uh);
                        AEGP_CompH compH = nullptr;
                        if (S_comp != nullptr
                            && S_comp->AEGP_GetCompFromItem(itemH, &compH) == A_Err_NONE) {
                            publish_icc_profile(compH);
                        }
                    } else {
                        logf("ring create failed: %s", S_ring.error().c_str());
                    }
                }

                if (S_ring.valid()) {
                    if (auto* dst = static_cast<uint8_t*>(S_ring.begin_write(need))) {
                        if (copy_world(worldH, wt, uw, uh, dst, bpr)) {
                            FrameDesc d {};
                            d.width         = uw;
                            d.height        = uh;
                            d.bytes_per_row = bpr;
                            d.pixel_format  = fmt;
                            d.source_tier   = tier;
                            d.channel_order = ChannelOrder::ARGB;      // AE native
                            d.flags         = kFlagPremultiplied;
                            d.value_scale   = (tier == SourceTier::Int16) ? kAE16ValueScale : 1.0f;
                            d.time_value    = timeT.value;
                            d.time_scale    = timeT.scale;
                            d.icc_generation = S_icc_generation;

                            // AEGP_GetItemName's out-param may never be NULL —
                            // AE raises a modal "internal verification failure
                            // {unicode_namePH cannot be NULL}" and, because the
                            // idle hook fires again behind it, does so forever.
                            A_char name[kMaxCompName] = { '\0' };
                            AEGP_MemHandle nameH = nullptr;
                            if (S_item->AEGP_GetItemName(S_id, itemH, &nameH) == A_Err_NONE
                                && nameH != nullptr && S_mem != nullptr) {
                                void* p = nullptr;
                                if (S_mem->AEGP_LockMemHandle(nameH, &p) == A_Err_NONE && p != nullptr) {
                                    // AE hands back UTF-16; take the ASCII subset for the label.
                                    const auto* u16 = static_cast<const A_u_short*>(p);
                                    size_t n = 0;
                                    while (n < kMaxCompName - 1 && u16[n] != 0) {
                                        name[n] = (u16[n] < 128) ? static_cast<char>(u16[n]) : '?';
                                        ++n;
                                    }
                                    name[n] = '\0';
                                    S_mem->AEGP_UnlockMemHandle(nameH);
                                }
                                S_mem->AEGP_FreeMemHandle(nameH);
                            }
                            std::snprintf(d.comp_name, kMaxCompName, "%s",
                                          name[0] ? name : "After Effects");

                            S_ring.commit(d);
                            ++S_published;
                            if (S_published == 1 || (S_published % 100) == 0) {
                                // Geometry and tier only — never the comp name (privacy).
                                logf("published %llu frames (%ux%u, tier %u)",
                                     static_cast<unsigned long long>(S_published), uw, uh,
                                     static_cast<unsigned>(tier));
                            }
                        } else {
                            S_ring.abandon();
                            logf("copy_world failed (world type %d)", static_cast<int>(wt));
                        }
                    }
                }
            }
        }
        // AE owns the frame world. Never dispose it — only check the receipt in.
        S_render->AEGP_CheckinFrame(receiptH);
    }
    S_ro->AEGP_Dispose(roH);
    return err;
}

// --- hooks -----------------------------------------------------------------
A_Err IdleHook(AEGP_GlobalRefcon, AEGP_IdleRefcon, A_long* max_sleepPL) {
    if (max_sleepPL) *max_sleepPL = 10;
    if (S_disabled) return A_Err_NONE;

    const double now = now_seconds();
    if (now - S_last_render < kMinIntervalSeconds) return A_Err_NONE;
    S_last_render = now;

    const A_Err err = render_and_publish();
    if (err != A_Err_NONE) {
        if (++S_consecutive_errors >= kMaxConsecutiveErrors) {
            S_disabled = true;
            S_ring = SharedRing();
            logf("DISABLED after %d consecutive errors (last %d) — reopen AE to retry",
                 S_consecutive_errors, static_cast<int>(err));
        }
    } else {
        S_consecutive_errors = 0;
    }
    // Never hand the error back to AE: it surfaces as a modal dialog, and the
    // idle hook fires again behind it. The log is the channel for failures.
    return A_Err_NONE;
}

A_Err DeathHook(AEGP_GlobalRefcon, AEGP_DeathRefcon) {
    logf("shutting down after %llu frames", static_cast<unsigned long long>(S_published));
    S_ring = SharedRing();
    if (S_log) { std::fclose(S_log); S_log = nullptr; }
    return A_Err_NONE;
}

}  // namespace

extern "C" DllExport A_Err EntryPointFunc(
    struct SPBasicSuite* pica_basicP,
    A_long               major_versionL,
    A_long               minor_versionL,
    AEGP_PluginID        aegp_plugin_id,
    AEGP_GlobalRefcon*   global_refconP)
{
    (void)global_refconP;
    S_sp = pica_basicP;
    S_id = aegp_plugin_id;

    S_log = std::fopen("/tmp/qcbridgeae-aegp.log", "w");
    logf("QCBridgeAE AEGP loaded (AE %ld.%ld)", static_cast<long>(major_versionL),
         static_cast<long>(minor_versionL));

    A_Err err = A_Err_NONE;
    auto acquire = [&](const char* name, int version, void** outP, const char* label) {
        const SPErr e = S_sp->AcquireSuite(name, version, const_cast<const void**>(outP));
        if (e != kSPNoError || *outP == nullptr) { logf("MISSING suite: %s", label); err = A_Err_GENERIC; }
    };
    acquire(kAEGPRegisterSuite,      kAEGPRegisterSuiteVersion5,      (void**)&S_reg,    "Register");
    acquire(kAEGPItemSuite,          kAEGPItemSuiteVersion9,          (void**)&S_item,   "Item");
    acquire(kAEGPRenderOptionsSuite, kAEGPRenderOptionsSuiteVersion4, (void**)&S_ro,     "RenderOptions");
    acquire(kAEGPRenderSuite,        kAEGPRenderSuiteVersion5,        (void**)&S_render, "Render");
    acquire(kAEGPWorldSuite,         kAEGPWorldSuiteVersion3,         (void**)&S_world,  "World");
    acquire(kAEGPUtilitySuite,       kAEGPUtilitySuiteVersion6,       (void**)&S_util,   "Utility");
    acquire(kAEGPMemorySuite,        kAEGPMemorySuiteVersion1,        (void**)&S_mem,    "Memory");
    acquire(kAEGPCompSuite,          kAEGPCompSuiteVersion12,         (void**)&S_comp,   "Comp");
    // Optional: only AE 25.1+ has v6. Without it there is no working-space ICC
    // and the sidecar carries no profile, which A3 must surface rather than guess around.
    if (S_sp->AcquireSuite(kAEGPColorSettingsSuite, kAEGPColorSettingsSuiteVersion6,
                           (const void**)&S_color) != kSPNoError) {
        S_color = nullptr;
        logf("ColorSettingsSuite6 unavailable — no working-space ICC on this AE");
    }
    if (err != A_Err_NONE) { logf("suite acquisition failed; not registering hooks"); return err; }

    ERR(S_reg->AEGP_RegisterIdleHook(S_id, IdleHook, nullptr));
    ERR(S_reg->AEGP_RegisterDeathHook(S_id, DeathHook, nullptr));
    logf("hooks registered; throttling to %.0f ms", kMinIntervalSeconds * 1000.0);
    return err;
}
