// GPU-side cost of each candidate wire format at 4K.
//
// The CPU side says 32 bpc native (memcpy) and converted-to-half cost the same
// (2.65 vs 2.67 ms) because both are memory bound. The remaining question is
// what the consumer pays: a 4K RGBA32Float texture is 132.7 MB against 63.3 MB
// for RGBA16F, and the GPU has to read all of it on every sample.
//
// Measures a full-screen 4K pass per format — the shape of what QCView does
// when it samples the wire texture and runs the OCIO chain.

#import <Metal/Metal.h>

#include "common/protocol/frame_desc.h"

#include <chrono>
#include <cstdio>
#include <vector>

using namespace qcbae;

namespace {
NSString* const kSrc = @R"(
#include <metal_stdlib>
using namespace metal;
struct VSOut { float4 pos [[position]]; float2 uv; };
vertex VSOut v_main(uint vid [[vertex_id]]) {
    const float2 p[3] = { float2(-1,-3), float2(-1,1), float2(3,1) };
    VSOut o; o.pos = float4(p[vid],0,1); o.uv = (p[vid]+1)*0.5; return o;
}
fragment float4 f_main(VSOut in [[stage_in]], texture2d<float> tex [[texture(0)]]) {
    constexpr sampler s(filter::nearest, address::clamp_to_edge);
    // Stand in for an OCIO chain: enough ALU that the pass isn't pure fill.
    float4 c = tex.sample(s, in.uv);
    c.rgb = pow(max(c.rgb, 0.0), float3(1.0/2.2));
    return c;
}
)";
}  // namespace

int main() { @autoreleasepool {
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    if (dev == nil) { std::printf("no Metal device\n"); return 77; }
    id<MTLCommandQueue> q = [dev newCommandQueue];
    NSError* e = nil;
    id<MTLLibrary> lib = [dev newLibraryWithSource:kSrc options:nil error:&e];
    if (lib == nil) { std::printf("%s\n", e.localizedDescription.UTF8String); return 1; }

    constexpr uint32_t W = 3840, H = 2160;
    std::printf("full-screen %ux%u pass, sample + a gamma's worth of ALU\n", W, H);
    std::printf("device: %s\n\n", dev.name.UTF8String);

    struct Case { const char* name; MTLPixelFormat pf; uint32_t bpp; };
    const Case cases[] = {
        {"RGBA8Unorm   (AE 8 bpc native)",  MTLPixelFormatRGBA8Unorm,  4},
        {"RGBA16Unorm  (AE 16 bpc native)", MTLPixelFormatRGBA16Unorm, 8},
        {"RGBA16Float  (AE 32 bpc -> half)", MTLPixelFormatRGBA16Float, 8},
        {"RGBA32Float  (AE 32 bpc native)", MTLPixelFormatRGBA32Float, 16},
    };

    // Render target stays RGBA16F throughout: QCView's pipeline currency, so
    // only the SOURCE format varies and the comparison is honest.
    MTLTextureDescriptor* rtd = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float width:W height:H mipmapped:NO];
    rtd.usage = MTLTextureUsageRenderTarget;
    rtd.storageMode = MTLStorageModePrivate;
    id<MTLTexture> rt = [dev newTextureWithDescriptor:rtd];

    for (const auto& c : cases) {
        MTLTextureDescriptor* td = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:c.pf width:W height:H mipmapped:NO];
        td.usage = MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModeShared;
        id<MTLTexture> tex = [dev newTextureWithDescriptor:td];
        std::vector<uint8_t> zero(static_cast<size_t>(W) * c.bpp * H, 0x20);
        [tex replaceRegion:MTLRegionMake2D(0,0,W,H) mipmapLevel:0
                 withBytes:zero.data() bytesPerRow:W * c.bpp];

        MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = [lib newFunctionWithName:@"v_main"];
        pd.fragmentFunction = [lib newFunctionWithName:@"f_main"];
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float;
        id<MTLRenderPipelineState> pso = [dev newRenderPipelineStateWithDescriptor:pd error:&e];

        auto pass = [&] {
            MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = rt;
            rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
            [enc setRenderPipelineState:pso];
            [enc setFragmentTexture:tex atIndex:0];
            [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [enc endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
        };

        pass();
        constexpr int iters = 40;
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < iters; ++i) pass();
        const auto t1 = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count() / iters;
        const double mb = static_cast<double>(W) * H * c.bpp / 1048576.0;
        std::printf("  %-34s %6.2f ms  %6.1f fps   source %6.1f MB\n",
                    c.name, ms, 1000.0 / ms, mb);
    }
    return 0;
}}
