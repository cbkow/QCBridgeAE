// QCBridgeAE — the sidecar schema.
//
// The shared surface carries numbers; this struct carries what they mean
// (PLAN.md D5). Preserving values while mislabeling them is still a broken
// picture, so everything QCView needs to interpret a frame correctly travels
// here — above all the working colorspace, which AE hands us for real via
// AEGP_ColorSettingsSuite6 rather than us guessing from the bit depth.
//
// This is a wire format shared across two processes and (later) two platforms
// and two compilers. Rules: POD only, fixed-width types, explicit padding,
// no pointers, no std:: containers, never reorder or resize a field — add at
// the end and bump kFrameDescVersion.

#pragma once

#include <cstdint>

namespace qcbae {

inline constexpr uint32_t kRingMagic        = 0x51434145u;  // 'QCAE'
inline constexpr uint32_t kFrameDescVersion = 1u;

inline constexpr uint32_t kMaxCompName = 128u;

// What the surface holds. One value today (PLAN.md D1: RGBA16F for every AE
// tier); the field exists so a future format is a version bump, not a
// reinterpretation of old data.
enum class PixelFormat : uint32_t {
    Unknown   = 0,
    RGBA16F   = 1,
};

// Which AE tier the pixels came FROM, before conversion to the wire format.
// Not redundant with PixelFormat: it tells QCView what precision is really
// present, so a 16bpc source can be reported honestly as ~11 effective bits
// near white rather than implied to be full half precision (PLAN.md D2).
enum class SourceTier : uint32_t {
    Unknown = 0,
    Int8    = 1,   // AE 8 bpc   — exact in half
    Int16   = 2,   // AE 16 bpc  — 0..32768, lossy above ~0.031 (PLAN.md D2/D3)
    Float32 = 3,   // AE 32 bpc  — clamped at 65504 (PLAN.md D4)
};

enum FrameFlags : uint32_t {
    kFlagNone            = 0u,
    kFlagPremultiplied   = 1u << 0,  // alpha is premultiplied (AE's normal state)
    kFlagClampedOverflow = 1u << 1,  // at least one sample hit the 65504 ceiling
};

struct FrameDesc {
    uint32_t width;
    uint32_t height;
    uint32_t bytes_per_row;
    PixelFormat pixel_format;

    SourceTier source_tier;
    uint32_t flags;

    // AE comp time as an exact rational — never a float. A frame number is
    // not derivable from a double without rounding arguments nobody wins.
    int64_t  time_value;
    int64_t  time_scale;

    // From AEGP_ColorSettingsSuite6. The ICC blob itself lives in the ring's
    // profile region (it changes on project settings, not per frame); this
    // generation counter tells the consumer when its cached copy is stale.
    uint64_t icc_generation;
    float    graphics_white;   // nits; 0 = unspecified
    uint32_t _pad0;

    // Display label for the QCView media item. In memory only — never logged
    // (PLAN.md §Privacy 5).
    char     comp_name[kMaxCompName];
};

static_assert(sizeof(FrameDesc) == 184, "FrameDesc is a wire format — size change needs a version bump");

}  // namespace qcbae
