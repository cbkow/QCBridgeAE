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

// What the surface holds. One per AE tier (PLAN.md D1): the integer tiers
// travel native and the GPU's texture unit normalizes them for free, because
// converting them to half costs 4.5x (8bpc) and 1.2x (16bpc) for precision
// that is either already exact or actively worse. Only the float tier is
// converted, and only because QCView has no 32f flow (D2).
//
// All three sample as `texture2d<float>` through one shader and one pipeline —
// verified in tests/texture_format_test.mm, because that equivalence is what
// makes per-tier formats cost a switch rather than a code path.
enum class PixelFormat : uint32_t {
    Unknown     = 0,
    RGBA16F     = 1,   // AE 32 bpc, converted. MTLPixelFormatRGBA16Float
    RGBA8Unorm  = 2,   // AE 8 bpc,  native.    MTLPixelFormatRGBA8Unorm
    RGBA16Unorm = 3,   // AE 16 bpc, native.    MTLPixelFormatRGBA16Unorm
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

// GPU row-stride alignment. A linear texture over shared memory has a
// per-device minimum for bytes_per_row: Metal exposes it as
// minimumLinearTextureAlignmentForPixelFormat, D3D11 has an equivalent
// constraint. 256 satisfies every device either API reports, and padding a
// 4K RGBA16F row costs nothing (30720 is already a multiple of it).
//
// The producer owns this: it pads rows on the way in, and bytes_per_row says
// what it did. A consumer that ignores the field and computes width * 8 will
// shear every frame whose width isn't a multiple of 32 pixels.
inline constexpr uint32_t kRowAlignment = 256u;

inline constexpr uint32_t aligned_bytes_per_row(uint32_t width, uint32_t bytes_per_pixel) {
    const uint32_t tight = width * bytes_per_pixel;
    return (tight + kRowAlignment - 1u) / kRowAlignment * kRowAlignment;
}

inline constexpr uint32_t bytes_per_pixel(PixelFormat f) {
    switch (f) {
        case PixelFormat::RGBA8Unorm:  return 4u;
        case PixelFormat::RGBA16F:
        case PixelFormat::RGBA16Unorm: return 8u;
        default:                       return 0u;
    }
}

// The wire format for a given AE tier. Integer tiers stay as they are.
inline constexpr PixelFormat wire_format_for(SourceTier t) {
    switch (t) {
        case SourceTier::Int8:    return PixelFormat::RGBA8Unorm;
        case SourceTier::Int16:   return PixelFormat::RGBA16Unorm;
        case SourceTier::Float32: return PixelFormat::RGBA16F;
        default:                  return PixelFormat::Unknown;
    }
}

// Worst case across every tier, for sizing a ring slot that must survive the
// user changing project bit depth mid-session without a rebuild.
inline constexpr uint64_t max_frame_bytes(uint32_t width, uint32_t height) {
    return static_cast<uint64_t>(aligned_bytes_per_row(width, 8u)) * height;
}

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

    // Multiply what the GPU samples by this to recover the intended value.
    //
    // It exists because AE's 16 bpc white is 32768, not 65535: carried in an
    // RGBA16Unorm texture, hardware normalization lands on 0.50001 and the
    // image is half-bright — a silent transformation, the exact class of bug
    // this project exists to prevent (PLAN.md D3). 65535/32768 corrects it and
    // is exact in fp32.
    //
    // Carried per frame rather than inferred from the format, so the producer
    // states what it did instead of the consumer keeping a table of special
    // cases. 1.0 for every other tier. A consumer must apply it unconditionally
    // and must not special-case the format.
    float    value_scale;

    // Display label for the QCView media item. In memory only — never logged
    // (PLAN.md §Privacy 5).
    char     comp_name[kMaxCompName];
};

static_assert(sizeof(FrameDesc) == 184, "FrameDesc is a wire format — size change needs a version bump");

// AE 16 bpc: PF_MAX_CHAN16 is 32768, the unorm container is 65535.
inline constexpr float kAE16ValueScale = 65535.0f / 32768.0f;

}  // namespace qcbae
