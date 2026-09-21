#include "common/convert/half_convert.h"

#include <cstring>

#if defined(__ARM_NEON) && defined(__aarch64__)
#include <arm_neon.h>
#define QCBAE_HAVE_NEON 1
#endif

namespace qcbae {

uint16_t float_to_half(float f) {
    uint32_t x;
    std::memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000u;
    const uint32_t absx = x & 0x7FFFFFFFu;
    const uint32_t mant = x & 0x007FFFFFu;

    if (absx >= 0x7F800000u) {
        if (absx == 0x7F800000u) return static_cast<uint16_t>(sign | 0x7C00u);   // ±inf
        // NaN: quiet it and keep the top payload bits, as AArch64 FCVT does
        // with default-NaN mode off (macOS's default FPCR).
        return static_cast<uint16_t>(sign | 0x7E00u | ((mant >> 13) & 0x03FFu));
    }

    const int32_t e = static_cast<int32_t>(absx >> 23) - 127;   // unbiased; -127 for float subnormals
    if (e >= 16) return static_cast<uint16_t>(sign | 0x7C00u);   // beyond half's range: inf

    if (e >= -14) {
        // Normal half. Round the 23-bit mantissa to 10, ties to even; a carry
        // out of the mantissa steps the exponent, which at the top of the
        // range correctly lands on inf (0x7C00).
        uint32_t h = (static_cast<uint32_t>(e + 15) << 10) | (mant >> 13);
        const uint32_t rem = mant & 0x1FFFu;
        if (rem > 0x1000u || (rem == 0x1000u && (h & 1u))) ++h;
        return static_cast<uint16_t>(sign | h);
    }

    // Half subnormal (or zero): m = round(|f| / 2^-24), ties to even. With the
    // implicit bit restored, |f| = full * 2^(e-23), so m = full >> (-(e+1)).
    if (absx < 0x00800000u) return static_cast<uint16_t>(sign);   // float subnormal: far below 2^-25
    const uint32_t full  = mant | 0x00800000u;
    const int32_t  shift = -(e + 1);                                // 14 .. 126
    if (shift > 24) return static_cast<uint16_t>(sign);             // below half the smallest subnormal
    uint32_t m = full >> shift;
    const uint32_t rem  = full & ((1u << shift) - 1u);
    const uint32_t half = 1u << (shift - 1);
    if (rem > half || (rem == half && (m & 1u))) ++m;               // may carry into the smallest normal: correct
    return static_cast<uint16_t>(sign | m);
}

namespace {

// Where image row y (top-down) lives in the source.
inline const float* source_row(const ConvertSource& s, uint32_t y) {
    const uint32_t mem_row = s.bottom_up ? (s.height - 1u - y) : y;
    return reinterpret_cast<const float*>(static_cast<const uint8_t*>(s.base)
                                          + static_cast<ptrdiff_t>(mem_row) * s.stride);
}

// Channel positions within a host pixel, for R, G, B, A.
struct Layout { int r, g, b, a; };
inline Layout layout_of(HostOrder o) {
    return o == HostOrder::ARGB ? Layout{1, 2, 3, 0} : Layout{2, 1, 0, 3};
}

inline void classify(uint16_t h, ConvertResult& r) {
    if ((h & 0x7C00u) == 0x7C00u) {
        if (h & 0x03FFu) r.has_nan = true; else r.has_inf = true;
    }
}

void convert_row_portable(const float* src, uint16_t* dst, uint32_t from, uint32_t to,
                          Layout L, ConvertResult& r) {
    for (uint32_t x = from; x < to; ++x) {
        const float* p = src + static_cast<size_t>(x) * 4u;
        uint16_t* q = dst + static_cast<size_t>(x) * 4u;
        q[0] = float_to_half(p[L.r]);
        q[1] = float_to_half(p[L.g]);
        q[2] = float_to_half(p[L.b]);
        q[3] = float_to_half(p[L.a]);
        classify(q[0], r); classify(q[1], r); classify(q[2], r); classify(q[3], r);
    }
}

}  // namespace

ConvertResult convert_32f_to_rgba16f_portable(const ConvertSource& s, void* dst, size_t dst_stride) {
    ConvertResult r;
    const Layout L = layout_of(s.order);
    for (uint32_t y = 0; y < s.height; ++y) {
        auto* out = reinterpret_cast<uint16_t*>(static_cast<uint8_t*>(dst) + y * dst_stride);
        convert_row_portable(source_row(s, y), out, 0, s.width, L, r);
    }
    return r;
}

ConvertResult convert_32f_to_rgba16f(const ConvertSource& s, void* dst, size_t dst_stride) {
#if QCBAE_HAVE_NEON
    // Whole pixels, not channel planes. Four host pixels load with plain LD1
    // and convert with FCVTN/FCVTN2 (IEEE, ties to even, NaN quieted) into two
    // 8-lane half vectors, two pixels each. The channel order is then fixed
    // with one TBL byte shuffle per vector. A first version de-interleaved into
    // planes with LD4/ST4 and cost 3.5x a plain conversion at 4K — the
    // structure loads, not the arithmetic (qcbae-convbench).
    //
    // Non-finite detection is split the same way: a cheap per-row "any
    // exponent all ones?" OR, and the exact inf-vs-NaN classification only for
    // the rare row that has one.
    ConvertResult r;
    const Layout L = layout_of(s.order);
    // Byte indices within an 8-lane half vector (two pixels) that put each
    // pixel's channels in R, G, B, A order.
    static const uint8_t kArgbToRgba[16] = {2, 3, 4, 5, 6, 7, 0, 1, 10, 11, 12, 13, 14, 15, 8, 9};
    static const uint8_t kBgraToRgba[16] = {4, 5, 2, 3, 0, 1, 6, 7, 12, 13, 10, 11, 8, 9, 14, 15};
    const uint8x16_t shuffle = vld1q_u8(s.order == HostOrder::ARGB ? kArgbToRgba : kBgraToRgba);
    const uint16x8_t exp_mask = vdupq_n_u16(0x7C00u);
    const uint32_t vec_end = s.width & ~3u;

    for (uint32_t y = 0; y < s.height; ++y) {
        const float* src = source_row(s, y);
        auto* out = reinterpret_cast<uint16_t*>(static_cast<uint8_t*>(dst) + y * dst_stride);
        uint16x8_t nonfinite = vdupq_n_u16(0);
        for (uint32_t x = 0; x < vec_end; x += 4) {
            const float32x4x4_t p = vld1q_f32_x4(src + static_cast<size_t>(x) * 4u);
            const float16x8_t h01 = vcvt_high_f16_f32(vcvt_f16_f32(p.val[0]), p.val[1]);
            const float16x8_t h23 = vcvt_high_f16_f32(vcvt_f16_f32(p.val[2]), p.val[3]);
            const uint16x8_t o01 = vreinterpretq_u16_u8(vqtbl1q_u8(vreinterpretq_u8_f16(h01), shuffle));
            const uint16x8_t o23 = vreinterpretq_u16_u8(vqtbl1q_u8(vreinterpretq_u8_f16(h23), shuffle));
            vst1q_u16(out + static_cast<size_t>(x) * 4u,      o01);
            vst1q_u16(out + static_cast<size_t>(x) * 4u + 8u, o23);
            nonfinite = vorrq_u16(nonfinite, vceqq_u16(vandq_u16(o01, exp_mask), exp_mask));
            nonfinite = vorrq_u16(nonfinite, vceqq_u16(vandq_u16(o23, exp_mask), exp_mask));
        }
        if (vmaxvq_u16(nonfinite) != 0)                      // rare: classify exactly
            for (uint32_t i = 0; i < vec_end * 4u; ++i) classify(out[i], r);
        convert_row_portable(src, out, vec_end, s.width, L, r);   // 0-3 pixel tail
    }
    return r;
#else
    return convert_32f_to_rgba16f_portable(s, dst, dst_stride);
#endif
}

}  // namespace qcbae
