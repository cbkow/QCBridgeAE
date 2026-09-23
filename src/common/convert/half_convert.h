// QCBridgeAE — the one pass from a host frame to the wire (PLAN.md D1).
//
// A Transmit host hands over 32-bit float pixels in its own layout: ARGB (After
// Effects) or BGRA (Premiere), stored bottom-up with a positive stride
// (lab/results/2026-09-21-a4-transmit-probe/, sections 3 and 13). QCView reads
// top-down RGBA16F. Every per-pixel job — flip, reorder, convert — happens
// here, in the copy that has to happen anyway, so the frame is touched once.
//
// The conversion is standard IEEE float -> half and nothing more (PLAN.md D4):
//   * round to nearest, ties to even;
//   * sign kept — negative values are real in scene-linear work;
//   * subnormals kept, as half subnormals where representable;
//   * magnitudes past half's range become ±inf; ±inf stays ±inf;
//   * NaN stays NaN (quieted, sign and top payload bits kept).
// Nothing is clamped. Clamping would hand QCView a plausible value in place of
// an error, hiding exactly what a QC viewer exists to show. Instead the result
// reports whether any inf or NaN came out, and the sidecar flags the frame
// (kFlagHasInf, kFlagHasNaN) so QCView can surface them.
//
// The hardware paths (NEON vcvt on AArch64, F16C vcvtps2ph on x86 -- chosen
// at run time from CPUID, since F16C is not in the x86-64 baseline) and the
// portable path agree bit for bit, which tests/convert_test.cpp holds them
// to — including NaN payloads.
//
// No SDK types: this builds and is tested without the Adobe SDKs, like the
// rest of src/common.

#pragma once

#include <cstddef>
#include <cstdint>

namespace qcbae {

enum class HostOrder : uint32_t {
    ARGB = 0,   // After Effects native (PrPixelFormat_ARGB_4444_32f)
    BGRA = 1,   // Premiere native      (PrPixelFormat_BGRA_4444_32f)
};

struct ConvertSource {
    const void* base;          // first row in memory
    ptrdiff_t   stride;        // bytes between consecutive rows in memory; may be negative
    uint32_t    width;
    uint32_t    height;
    HostOrder   order;
    bool        bottom_up;     // memory row 0 is the image's bottom row
};

struct ConvertResult {
    bool has_inf = false;      // at least one ±inf in the output
    bool has_nan = false;      // at least one NaN in the output
};

// Writes `src` into `dst` as top-down RGBA16F rows, `dst_stride` bytes apart
// (which must be >= width * 8). Uses NEON or F16C where available, else the
// portable path.
ConvertResult convert_32f_to_rgba16f(const ConvertSource& src, void* dst, size_t dst_stride);

// Whether convert_32f_to_rgba16f will take a hardware path on this machine.
// On x86 without F16C it is the portable path, 4.7x slower at 4K (measured
// on the macOS side, lab/HANDOFF-windows.md); a host on such a machine
// should say so rather than silently drop frames.
bool convert_has_hardware_path();

// The portable path, exposed so tests can hold the fast path to it.
ConvertResult convert_32f_to_rgba16f_portable(const ConvertSource& src, void* dst, size_t dst_stride);

// One sample by the rules above. Portable.
uint16_t float_to_half(float f);

}  // namespace qcbae
