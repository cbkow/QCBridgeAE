// What each AE tier costs to get onto the wire. This is the measurement that
// settled DESIGN-NOTES D1: converting the integer tiers to half costs 4.5x (8 bpc)
// and 1.2x (16 bpc) against carrying them natively, for precision that is
// either already exact or actively worse.
//
// It lives in the repo rather than a scratch directory so the Windows machine
// can re-run it (lab/HANDOFF-windows.md) — the ratios are what matter, and
// they may not hold on a discrete-GPU box with different memory behaviour.
//
// Not a CTest test: it measures, it doesn't assert. Run it by hand.
//
// Note on methodology: every destination must be consumed or the optimiser
// deletes the loop and the benchmark reports infinity. It did exactly that on
// the first attempt.

#include "common/convert/half_convert.h"
#include "common/protocol/frame_desc.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif
#if defined(_M_X64) || defined(__x86_64__)
#include <immintrin.h>
#define QCBAE_BENCH_F16C 1
#endif
#if defined(_MSC_VER)
#include <intrin.h>
#endif

using namespace qcbae;

namespace {

constexpr uint32_t W = 3840, H = 2160;
constexpr size_t   N = static_cast<size_t>(W) * H * 4;   // RGBA samples

inline void keep(void* p) {
#if defined(_MSC_VER)
    (void)p; _ReadWriteBarrier();
#else
    asm volatile("" : : "r,m"(p) : "memory");
#endif
}

// Portable float -> half. Used for the reference path so the numbers mean the
// same thing on both platforms.
uint16_t to_half(float f) {
    uint32_t bits; std::memcpy(&bits, &f, 4);
    const uint32_t sign = (bits >> 16) & 0x8000u;
    int32_t exp = static_cast<int32_t>((bits >> 23) & 0xFFu) - 127 + 15;
    const uint32_t man = bits & 0x7FFFFFu;
    if (exp <= 0)  return static_cast<uint16_t>(sign);
    if (exp >= 31) return static_cast<uint16_t>(sign | 0x7BFFu);
    return static_cast<uint16_t>(sign | (static_cast<uint32_t>(exp) << 10) | (man >> 13));
}

template <typename F>
void bench(const char* label, size_t bytes_touched, F&& f) {
    f();  // warm
    constexpr int iters = 30;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < iters; ++i) f();
    const auto t1 = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count() / iters;
    std::printf("  %-42s %6.2f ms  %6.1f fps  %5.1f GB/s\n",
                label, ms, 1000.0 / ms, bytes_touched / (ms / 1000.0) / 1e9);
}

}  // namespace

int main() {
    std::vector<uint8_t>  src8(N);
    std::vector<uint16_t> src16(N);
    std::vector<float>    src32(N);
    for (size_t i = 0; i < N; ++i) {
        src8[i]  = static_cast<uint8_t>(i & 0xFF);
        src16[i] = static_cast<uint16_t>(i % 32769);       // AE 16 bpc: 0..32768
        src32[i] = static_cast<float>(i % 1000) / 500.0f;  // some values above 1.0
    }
    std::vector<uint8_t>  dst8(N);
    std::vector<uint16_t> dst16(N);

    std::printf("4K UHD RGBA, %.1f Mpx, %zu samples\n\n", W * H / 1e6, N);

    std::printf("AE 8 bpc source:\n");
    bench("-> RGBA8Unorm (native, memcpy)", N * 2, [&] {
        std::memcpy(dst8.data(), src8.data(), N); keep(dst8.data());
    });
    bench("-> RGBA16F (converted)", N * 3, [&] {
        for (size_t i = 0; i < N; ++i) dst16[i] = to_half(src8[i] * (1.0f / 255.0f));
        keep(dst16.data());
    });

    std::printf("\nAE 16 bpc source (0..32768):\n");
    bench("-> RGBA16Unorm (native, memcpy)", N * 4, [&] {
        std::memcpy(dst16.data(), src16.data(), N * 2); keep(dst16.data());
    });
    bench("-> RGBA16F (converted)", N * 4, [&] {
        for (size_t i = 0; i < N; ++i) dst16[i] = to_half(src16[i] * (1.0f / 32768.0f));
        keep(dst16.data());
    });

    std::printf("\nAE 32 bpc float source:\n");
    std::vector<float> dst32(N);
    bench("-> RGBA32Float (native, memcpy)", N * 8, [&] {
        std::memcpy(dst32.data(), src32.data(), N * 4); keep(dst32.data());
    });
    bench("-> RGBA16F portable", N * 6, [&] {
        for (size_t i = 0; i < N; ++i) {
            const float v = src32[i];
            dst16[i] = to_half(v > 65504.0f ? 65504.0f : v);
        }
        keep(dst16.data());
    });
#if defined(QCBAE_BENCH_F16C)
    // The x86 twin of the NEON row below, for the same comparison: plain
    // conversion with a clamp, no flip and no reorder.
    if (convert_has_hardware_path()) {
        bench("-> RGBA16F F16C (clamp + vcvtps2ph)", N * 6, [&] {
            const __m256 ceiling = _mm256_set1_ps(65504.0f);
            auto* d = dst16.data();
            for (size_t i = 0; i < N; i += 8) {
                const __m256 a = _mm256_min_ps(_mm256_loadu_ps(src32.data() + i), ceiling);
                _mm_storeu_si128(reinterpret_cast<__m128i*>(d + i),
                                 _mm256_cvtps_ph(a, _MM_FROUND_TO_NEAREST_INT));
            }
            _mm256_zeroupper();
            keep(d);
        });
    } else {
        std::printf("  (no F16C on this CPU: the hardware row is skipped)\n");
    }
#endif
#if defined(__ARM_NEON)
    bench("-> RGBA16F NEON (clamp + fcvtn)", N * 6, [&] {
        const float32x4_t ceiling = vdupq_n_f32(65504.0f);
        auto* d = reinterpret_cast<float16_t*>(dst16.data());
        for (size_t i = 0; i < N; i += 8) {
            const float32x4_t a = vminq_f32(vld1q_f32(src32.data() + i),     ceiling);
            const float32x4_t b = vminq_f32(vld1q_f32(src32.data() + i + 4), ceiling);
            vst1_f16(d + i,     vcvt_f16_f32(a));
            vst1_f16(d + i + 4, vcvt_f16_f32(b));
        }
        keep(d);
    });
#endif
    // The A6 product pass (src/common/convert/half_convert.*): flip a
    // bottom-up host frame, reorder ARGB/BGRA to RGBA and convert to half —
    // IEEE, nothing clamped (DESIGN-NOTES D4) — into a padded destination, in one
    // pass. Compare with the plain conversion above: the flip and reorder
    // should cost next to nothing on top of it.
    std::printf("\nA6 Transmit pass, 4x32f host frame -> top-down RGBA16F (fast path: %s):\n",
                convert_has_hardware_path() ? "hardware" : "portable");
    {
        const size_t dst_stride = aligned_bytes_per_row(W, 8);
        std::vector<uint8_t> dst(dst_stride * H);
        for (HostOrder order : {HostOrder::ARGB, HostOrder::BGRA}) {
            const ConvertSource cs {src32.data(), static_cast<ptrdiff_t>(W * 16), W, H, order, true};
            bench(order == HostOrder::ARGB ? "ARGB bottom-up (AE)       -> RGBA16F"
                                           : "BGRA bottom-up (Premiere) -> RGBA16F",
                  N * 6, [&] { convert_32f_to_rgba16f(cs, dst.data(), dst_stride); keep(dst.data()); });
        }
        const ConvertSource cs {src32.data(), static_cast<ptrdiff_t>(W * 16), W, H, HostOrder::ARGB, true};
        bench("ARGB bottom-up, portable path (fallback)", N * 6,
              [&] { convert_32f_to_rgba16f_portable(cs, dst.data(), dst_stride); keep(dst.data()); });
    }

    std::printf("\nWire bytes per 4K frame: 8bpc %.1f MB, 16bpc %.1f MB, 32bpc %.1f MB\n",
                max_frame_bytes(W, H) / 2.0 / 1048576.0,
                static_cast<double>(max_frame_bytes(W, H)) / 1048576.0,
                static_cast<double>(max_frame_bytes(W, H)) / 1048576.0);
    return 0;
}
