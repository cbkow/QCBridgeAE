// PLAN.md D1 rests on one claim: the three per-tier wire formats sample
// identically through a single `texture2d<float>` binding, so carrying each AE
// tier natively costs a pixelFormat switch rather than three code paths.
//
// If that were false, D1's cost/benefit would invert and converting everything
// to half would be right after all. So it is a test, not a footnote.
//
// It also pins the value_scale contract: AE 16 bpc white is 32768 inside a
// 0..65535 unorm container, so hardware normalization lands on ~0.5 and the
// image is half-bright until the scale corrects it. That is the silent
// transformation PLAN.md D3 exists to prevent, and this is where it's caught.

#import <Metal/Metal.h>

#include "common/protocol/frame_desc.h"

#include <cmath>
#include <cstdio>
#include <cstring>

using namespace qcbae;

namespace {
int failures = 0;
void check(bool cond, const char* what, float got, float want) {
    std::printf("  %-52s got %.6f  want %.6f  %s\n", what, got, want, cond ? "ok" : "FAIL");
    if (!cond) ++failures;
}

NSString* const kSrc = @R"(
#include <metal_stdlib>
using namespace metal;
kernel void probe(texture2d<float> tex [[texture(0)]],
                  device float4* out [[buffer(0)]],
                  uint2 gid [[thread_position_in_grid]]) {
    if (gid.x == 0 && gid.y == 0) out[0] = tex.read(uint2(0, 0));
}
)";
}  // namespace

int main() { @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (device == nil) { std::printf("no Metal device\n"); return 77; }
    id<MTLCommandQueue> queue = [device newCommandQueue];

    NSError* err = nil;
    id<MTLLibrary> lib = [device newLibraryWithSource:kSrc options:nil error:&err];
    if (lib == nil) { std::printf("shader: %s\n", err.localizedDescription.UTF8String); return 1; }
    id<MTLComputePipelineState> pso =
        [device newComputePipelineStateWithFunction:[lib newFunctionWithName:@"probe"] error:&err];
    if (pso == nil) { std::printf("pipeline: %s\n", err.localizedDescription.UTF8String); return 1; }
    id<MTLBuffer> out = [device newBufferWithLength:16 options:MTLResourceStorageModeShared];

    // ONE pipeline for every format below. That is the point.
    auto sample = [&](MTLPixelFormat pf, const void* px, size_t bpr) -> float {
        MTLTextureDescriptor* td =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pf width:4 height:4 mipmapped:NO];
        td.usage = MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModeShared;
        id<MTLTexture> tex = [device newTextureWithDescriptor:td];
        [tex replaceRegion:MTLRegionMake2D(0, 0, 4, 4) mipmapLevel:0 withBytes:px bytesPerRow:bpr];
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
        [ce setComputePipelineState:pso];
        [ce setTexture:tex atIndex:0];
        [ce setBuffer:out offset:0 atIndex:0];
        [ce dispatchThreads:MTLSizeMake(4, 4, 1) threadsPerThreadgroup:MTLSizeMake(4, 4, 1)];
        [ce endEncoding]; [cb commit]; [cb waitUntilCompleted];
        return static_cast<const float*>(out.contents)[0];
    };

    std::printf("one compute pipeline, texture2d<float>, every AE tier:\n\n");

    // --- AE 8 bpc, native ---------------------------------------------------
    uint8_t p8[4 * 4 * 4];
    std::memset(p8, 128, sizeof p8);
    {
        const float got = sample(MTLPixelFormatRGBA8Unorm, p8, 16);
        check(std::fabs(got - 128.0f / 255.0f) < 1e-5f, "RGBA8Unorm (AE 8bpc, byte 128)",
              got, 128.0f / 255.0f);
    }

    // --- AE 16 bpc, native, and the 32768 correction ------------------------
    uint16_t p16[4 * 4 * 4];
    for (auto& v : p16) v = 32768;              // AE white
    {
        const float raw = sample(MTLPixelFormatRGBA16Unorm, p16, 32);
        check(std::fabs(raw - 32768.0f / 65535.0f) < 1e-5f,
              "RGBA16Unorm raw (hardware normalize by 65535)", raw, 32768.0f / 65535.0f);
        check(std::fabs(raw - 1.0f) > 0.4f,
              "  ... and is NOT white without the scale (half-bright bug)", raw, 0.5f);
        const float corrected = raw * kAE16ValueScale;
        check(std::fabs(corrected - 1.0f) < 1e-5f,
              "  ... x value_scale recovers AE white exactly", corrected, 1.0f);
    }

    // --- AE 32 bpc, converted ----------------------------------------------
    uint16_t phalf[4 * 4 * 4];
    for (auto& v : phalf) v = 0x3800;           // half(0.5)
    {
        const float got = sample(MTLPixelFormatRGBA16Float, phalf, 32);
        check(std::fabs(got - 0.5f) < 1e-5f, "RGBA16Float (AE 32bpc, 0.5)", got, 0.5f);
    }

    // --- the protocol agrees with what Metal just did -----------------------
    std::printf("\nprotocol:\n");
    check(bytes_per_pixel(wire_format_for(SourceTier::Int8)) == 4, "8bpc wire is 4 B/px (half the traffic)",
          static_cast<float>(bytes_per_pixel(wire_format_for(SourceTier::Int8))), 4.0f);
    check(bytes_per_pixel(wire_format_for(SourceTier::Int16)) == 8, "16bpc wire is 8 B/px",
          static_cast<float>(bytes_per_pixel(wire_format_for(SourceTier::Int16))), 8.0f);
    check(bytes_per_pixel(wire_format_for(SourceTier::Float32)) == 8, "32bpc wire is 8 B/px",
          static_cast<float>(bytes_per_pixel(wire_format_for(SourceTier::Float32))), 8.0f);
    check(max_frame_bytes(3840, 2160) == static_cast<uint64_t>(aligned_bytes_per_row(3840, 8)) * 2160,
          "max_frame_bytes sizes for the widest tier", 1.0f, 1.0f);

    std::printf("\n%s (%d failure%s)\n", failures == 0 ? "PASS" : "FAIL",
                failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}}
