// The Transmit conversion pass (src/common/convert/half_convert.*).
//
// Anchored to what the next layer consumes, not to the source buffer
// (memory: "assert the consumed property"): a true image img(x, y, c) is laid
// out the way the host lays it out — bottom-up, ARGB or BGRA, padded rows —
// converted, and then read back the way QCView reads the ring — top-down,
// RGBA16F, at the padded stride. Every sample must equal IEEE half of
// img(x, y, c). A test that compared output to the source buffer's own
// indexing would pass a flip bug that mirrored both.
//
// The reference for "IEEE half" is the hardware conversion (__fp16, FCVT on
// AArch64), so the portable float_to_half is held to it bit for bit —
// including NaN payloads, signed zeros, subnormals and the inf boundary.

#include "common/convert/half_convert.h"
#include "common/protocol/frame_desc.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

using namespace qcbae;

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) { ++g_failures; std::printf("FAIL: %s\n", what); }
}

float bits_to_float(uint32_t b) { float f; std::memcpy(&f, &b, 4); return f; }

#if defined(__aarch64__)
uint16_t hw_half(float f) {
    const __fp16 h = static_cast<__fp16>(f);
    uint16_t b; std::memcpy(&b, &h, 2); return b;
}
#endif

// 1. The scalar conversion against the hardware, over a sweep of bit
//    patterns plus every boundary that matters.
void test_scalar_matches_hardware() {
#if defined(__aarch64__)
    std::vector<uint32_t> patterns;
    for (uint64_t b = 0; b <= 0xFFFFFFFFull; b += 4099) patterns.push_back(static_cast<uint32_t>(b));
    const float specials[] = {
        0.0f, -0.0f, 1.0f, -1.0f, 65504.0f, -65504.0f, 65519.99f, 65520.0f, 65536.0f, 1e30f,
        -1e30f, std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(),
        6.1035156e-05f /* smallest normal half */, 5.9604645e-08f /* smallest subnormal half */,
        2.9802322e-08f /* exactly half of it: ties to even -> 0 */, 2.98023259e-08f, 1e-40f,
        -0.005072f, 0.15f, 0.35f, 0.1f, 1.0f / 3.0f,
    };
    for (float f : specials) { uint32_t b; std::memcpy(&b, &f, 4); patterns.push_back(b); }
    // Ties and carries around every half exponent.
    for (int e = -26; e <= 16; ++e)
        for (uint32_t m : {0x0u, 0x1000u, 0x1001u, 0x0FFFu, 0x3000u, 0x7FE000u, 0x7FF000u, 0x7FFFFFu})
            for (uint32_t s : {0u, 0x80000000u})
                patterns.push_back(s | (static_cast<uint32_t>(e + 127) << 23) | m);
    // NaNs with assorted payloads, quiet and signalling, both signs.
    for (uint32_t payload : {0x1u, 0x1FFFu, 0x2000u, 0x400000u, 0x7FFFFFu, 0x3FE000u})
        for (uint32_t s : {0u, 0x80000000u}) patterns.push_back(s | 0x7F800000u | payload);

    int mismatches = 0;
    for (uint32_t b : patterns) {
        const float f = bits_to_float(b);
        const uint16_t want = hw_half(f), got = float_to_half(f);
        if (want != got && ++mismatches <= 8)
            std::printf("  float 0x%08X: hardware 0x%04X, portable 0x%04X\n", b, want, got);
    }
    std::printf("scalar vs hardware: %zu patterns, %d mismatches\n", patterns.size(), mismatches);
    check(mismatches == 0, "portable float_to_half must match hardware FCVT bit for bit");
#else
    std::printf("scalar vs hardware: skipped (no __fp16 hardware reference on this target)\n");
#endif
}

// A true image with every kind of value in it, position-dependent so a flip,
// a transpose or a channel swap all change the answer.
float img(uint32_t x, uint32_t y, int c) {
    const uint32_t k = (x * 7u + y * 131u + static_cast<uint32_t>(c) * 17u) % 64u;
    switch (k) {
        case 0:  return std::numeric_limits<float>::infinity();
        case 1:  return -std::numeric_limits<float>::infinity();
        case 2:  return std::numeric_limits<float>::quiet_NaN();
        case 3:  return 1e9f;                 // past half's range: must become +inf, not 65504
        case 4:  return -70000.0f;            // must become -inf
        case 5:  return -0.005072f;           // negative in-range: sign kept
        case 6:  return 3e-7f;                // half subnormal
        case 7:  return -0.0f;
        default: return (static_cast<float>(x) - static_cast<float>(y) * 0.37f + c * 1.9f) * 0.013f;
    }
}

struct HostFrame {
    std::vector<uint8_t> bytes;
    ptrdiff_t stride = 0;
};

// Lay img out as a host would: pixel order per `order`, rows bottom-up in
// memory when `bottom_up`, each row padded by `pad` bytes.
HostFrame make_host_frame(uint32_t w, uint32_t h, HostOrder order, bool bottom_up, size_t pad) {
    HostFrame f;
    f.stride = static_cast<ptrdiff_t>(w * 16u + pad);
    f.bytes.assign(static_cast<size_t>(f.stride) * h, 0xCD);
    for (uint32_t y = 0; y < h; ++y) {
        const uint32_t mem_row = bottom_up ? h - 1u - y : y;
        auto* row = reinterpret_cast<float*>(f.bytes.data() + static_cast<size_t>(mem_row) * f.stride);
        for (uint32_t x = 0; x < w; ++x) {
            float* p = row + x * 4u;
            const float R = img(x, y, 0), G = img(x, y, 1), B = img(x, y, 2), A = img(x, y, 3);
            if (order == HostOrder::ARGB) { p[0] = A; p[1] = R; p[2] = G; p[3] = B; }
            else                          { p[0] = B; p[1] = G; p[2] = R; p[3] = A; }
        }
    }
    return f;
}

// 2. Whole frames, read back the way QCView reads them.
void test_frames() {
    int case_no = 0;
    for (HostOrder order : {HostOrder::ARGB, HostOrder::BGRA})
    for (bool bottom_up : {true, false})
    for (uint32_t w : {1u, 3u, 4u, 37u, 1280u})
    for (int path = 0; path < 2; ++path) {
        const uint32_t h = 9;
        HostFrame hf = make_host_frame(w, h, order, bottom_up, 48);
        const size_t dst_stride = aligned_bytes_per_row(w, 8);
        std::vector<uint8_t> dst(dst_stride * h, 0xEE);

        ConvertSource src {hf.bytes.data(), hf.stride, w, h, order, bottom_up};
        const ConvertResult r = path == 0 ? convert_32f_to_rgba16f(src, dst.data(), dst_stride)
                                          : convert_32f_to_rgba16f_portable(src, dst.data(), dst_stride);
        int bad = 0; bool saw_inf = false, saw_nan = false;
        for (uint32_t y = 0; y < h; ++y) {
            const auto* row = reinterpret_cast<const uint16_t*>(dst.data() + y * dst_stride);
            for (uint32_t x = 0; x < w; ++x)
                for (int c = 0; c < 4; ++c) {
                    const float v = img(x, y, c);
                    const uint16_t want = float_to_half(v);   // held to hardware in test 1
                    if (row[x * 4u + c] != want && ++bad <= 4)
                        std::printf("  case %d (%s %s w=%u %s) (%u,%u) c%d: got 0x%04X want 0x%04X\n",
                                    case_no, order == HostOrder::ARGB ? "ARGB" : "BGRA",
                                    bottom_up ? "bottom-up" : "top-down", w,
                                    path == 0 ? "fast" : "portable", x, y, c, row[x * 4u + c], want);
                    if (std::isinf(v) || std::fabs(v) > 65504.0f) saw_inf = true;
                    if (std::isnan(v)) saw_nan = true;
                }
            // Row padding in the destination must be left alone.
            for (size_t b = w * 8u; b < dst_stride; ++b)
                if (dst[y * dst_stride + b] != 0xEE) { ++bad; break; }
        }
        char what[160];
        std::snprintf(what, sizeof what, "frame case %d: every sample is IEEE half of the true image", case_no);
        check(bad == 0, what);
        std::snprintf(what, sizeof what, "frame case %d: inf/NaN flags match the content", case_no);
        check(r.has_inf == saw_inf && r.has_nan == saw_nan, what);
        ++case_no;
    }
    std::printf("frames: %d cases\n", case_no);
}

// 3. The flag trap: an inf and a NaN in the same SIMD lane of different
//    vectors. Per-frame OR-ing of "non-finite" would report only NaN.
void test_flag_lanes() {
    const uint32_t w = 8, h = 1;
    std::vector<float> px(w * 4, 0.5f);
    px[1] = std::numeric_limits<float>::quiet_NaN();   // pixel 0, R (ARGB slot 1)
    px[4 * 4 + 1] = std::numeric_limits<float>::infinity();  // pixel 4, R: same lane, next vector
    std::vector<uint8_t> dst(aligned_bytes_per_row(w, 8));
    ConvertSource src {px.data(), static_cast<ptrdiff_t>(w * 16), w, h, HostOrder::ARGB, false};
    const ConvertResult r = convert_32f_to_rgba16f(src, dst.data(), dst.size());
    check(r.has_nan && r.has_inf, "inf sharing a lane with a NaN is still reported");
    std::vector<float> clean(w * 4, 0.25f);
    ConvertSource csrc {clean.data(), static_cast<ptrdiff_t>(w * 16), w, h, HostOrder::BGRA, false};
    const ConvertResult rc = convert_32f_to_rgba16f(csrc, dst.data(), dst.size());
    check(!rc.has_nan && !rc.has_inf, "a finite frame raises no flags");
}

// 4. A lone non-finite value, at every pixel position and channel of an
//    otherwise clean frame. The fast path checks rows cheaply and classifies
//    only rows that trip the check, so every lane of that cheap check must be
//    live: with specials in every row (test 2) a dead lane hides behind a
//    neighbour that trips the row anyway. That is exactly how a mutation
//    removing half the check went unnoticed.
void test_lone_nonfinite() {
    const uint32_t w = 13, h = 3;   // 3 SIMD groups of 4 plus a 1-pixel tail
    int bad = 0;
    for (int kind = 0; kind < 2; ++kind)
    for (HostOrder order : {HostOrder::ARGB, HostOrder::BGRA})
    for (uint32_t px = 0; px < w; ++px)
    for (int slot = 0; slot < 4; ++slot) {
        std::vector<float> f(static_cast<size_t>(w) * h * 4u, 0.5f);
        f[(static_cast<size_t>(1) * w + px) * 4u + slot] = kind == 0
            ? std::numeric_limits<float>::infinity() : std::numeric_limits<float>::quiet_NaN();
        std::vector<uint8_t> dst(aligned_bytes_per_row(w, 8) * h);
        const ConvertSource src {f.data(), static_cast<ptrdiff_t>(w * 16), w, h, order, true};
        const ConvertResult r = convert_32f_to_rgba16f(src, dst.data(), aligned_bytes_per_row(w, 8));
        const bool ok = kind == 0 ? (r.has_inf && !r.has_nan) : (r.has_nan && !r.has_inf);
        if (!ok && ++bad <= 4)
            std::printf("  lone %s at pixel %u slot %d (%s) not reported\n", kind == 0 ? "inf" : "NaN",
                        px, slot, order == HostOrder::ARGB ? "ARGB" : "BGRA");
    }
    check(bad == 0, "a lone inf or NaN is reported at every pixel position and channel");
}

}  // namespace

int main() {
    test_scalar_matches_hardware();
    test_frames();
    test_flag_lanes();
    test_lone_nonfinite();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
