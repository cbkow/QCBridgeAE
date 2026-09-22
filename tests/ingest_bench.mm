// What QCView's live ingest costs per frame, and what sampling the ring slot
// in place (zero-copy, A1b) would save. The measurement behind the
// 2026-09-22 decision to keep the copy: lab/results/2026-09-22-zero-copy-assessment/.
//
// QCView today:  ring --memcpy--> QImage             (HostBridgeSource reader thread)
//                QImage --replaceRegion--> texture    (render thread, inside drawFrame)
//                GPU resamples it into the canvas
// Zero-copy:     GPU resamples a linear texture over the shm pages directly
//
// Real POSIX shm with the ring's page-aligned slot geometry. Each slot is
// rewritten before it is read, as the producer does, so neither path samples
// memory the GPU has already cached. The GPU pass is a bilinear resample into
// an RGBA16F canvas, the shape of QCView's composite (canvas capped 3840x2160).
//
// Not a CTest test: it measures, it doesn't assert. Run it by hand.

#import <Metal/Metal.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>
#include <algorithm>

using clk = std::chrono::steady_clock;
static double ms(clk::time_point a, clk::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

static const char *kSrc = R"(
#include <metal_stdlib>
using namespace metal;
kernel void resample(texture2d<half, access::sample> src [[texture(0)]],
                     texture2d<half, access::write>  dst [[texture(1)]],
                     uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= dst.get_width() || gid.y >= dst.get_height()) return;
    constexpr sampler s(filter::linear, address::clamp_to_edge);
    float2 uv = (float2(gid) + 0.5) / float2(dst.get_width(), dst.get_height());
    dst.write(src.sample(s, uv), gid);
}
)";

struct Stat { std::vector<double> v; void add(double x){v.push_back(x);}
    double mean() const { double s=0; for(double x:v) s+=x; return s/v.size(); }
    double pct(double p) const { auto c=v; std::sort(c.begin(),c.end()); return c[size_t(p*(c.size()-1))]; } };

static void run(id<MTLDevice> dev, id<MTLComputePipelineState> pso, id<MTLCommandQueue> q,
                int W, int H, int cw, int ch, int N)
{
    const size_t page = getpagesize();
    const size_t align = [dev minimumLinearTextureAlignmentForPixelFormat:MTLPixelFormatRGBA16Float];
    const size_t rowBytes = (size_t(W) * 8 + align - 1) / align * align;
    const size_t slotBytes = (rowBytes * H + page - 1) / page * page;
    const int slots = 3;

    // Real POSIX shm, as the ring uses.
    const char *name = "/qcbae-zcbench";
    shm_unlink(name);
    int fd = shm_open(name, O_CREAT | O_RDWR, 0600);
    ftruncate(fd, slotBytes * slots);
    uint8_t *ring = (uint8_t *)mmap(nullptr, slotBytes * slots, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    for (int s = 0; s < slots; ++s)             // non-trivial halfs so nothing is a zero page
        for (size_t i = 0; i < slotBytes; i += 2) *(uint16_t *)(ring + s*slotBytes + i) = uint16_t(0x3800 + (i >> 9 & 0x3ff));

    std::vector<uint8_t> qimage(size_t(W) * 8 * H);  // today's QImage (tight rows)

    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
                                                                                 width:W height:H mipmapped:NO];
    td.storageMode = MTLStorageModeShared; td.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> cached = [dev newTextureWithDescriptor:td];

    MTLTextureDescriptor *cd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
                                                                                 width:cw height:ch mipmapped:NO];
    cd.storageMode = MTLStorageModePrivate; cd.usage = MTLTextureUsageShaderWrite;
    id<MTLTexture> canvas = [dev newTextureWithDescriptor:cd];

    // Zero-copy: one no-copy buffer over the whole mapping, one linear texture per slot.
    auto t0 = clk::now();
    id<MTLBuffer> buf = [dev newBufferWithBytesNoCopy:ring length:slotBytes * slots
                                              options:MTLResourceStorageModeShared deallocator:nil];
    std::vector<id<MTLTexture>> lin;
    for (int s = 0; s < slots; ++s)
        lin.push_back([buf newTextureWithDescriptor:td offset:s * slotBytes bytesPerRow:rowBytes]);
    auto t1 = clk::now();
    const bool sameMem = buf.contents == ring;

    auto gpuPass = [&](id<MTLTexture> src) {
        id<MTLCommandBuffer> cb = [q commandBuffer];
        id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
        [e setComputePipelineState:pso];
        [e setTexture:src atIndex:0]; [e setTexture:canvas atIndex:1];
        [e dispatchThreads:MTLSizeMake(cw, ch, 1) threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
        [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
        return (cb.GPUEndTime - cb.GPUStartTime) * 1000.0;
    };

    Stat copy1, copy2, gpuTiled, gpuLin;
    for (int i = 0; i < N; ++i) {
        uint8_t *slot = ring + (i % slots) * slotBytes;
        // The producer (AE) writes the slot fresh before anyone reads it.
        for (int y = 0; y < H; ++y) memcpy(slot + y * rowBytes, qimage.data() + size_t(y) * W * 8, size_t(W) * 8);
        auto a = clk::now();
        for (int y = 0; y < H; ++y) memcpy(qimage.data() + size_t(y) * W * 8, slot + y * rowBytes, size_t(W) * 8);
        auto b = clk::now();
        [cached replaceRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0 withBytes:qimage.data() bytesPerRow:W * 8];
        auto c = clk::now();
        if (i >= 3) { copy1.add(ms(a, b)); copy2.add(ms(b, c)); }
        double g1 = gpuPass(cached);
        double g2 = gpuPass(lin[i % slots]);
        if (i >= 3) { gpuTiled.add(g1); gpuLin.add(g2); }
    }

    const double mb = double(W) * H * 8 / 1e6;
    printf("\n%dx%d RGBA16F (%.1f MB/frame) -> %dx%d canvas, %d frames\n", W, H, mb, cw, ch, N);
    printf("  zero-copy setup (buffer + %d textures, once per ring): %.3f ms, same memory: %s, row pitch %zu (align %zu)\n",
           slots, ms(t0, t1), sameMem ? "yes" : "NO", rowBytes, align);
    printf("  copy 1 ring->QImage   (reader thread): mean %6.3f  p95 %6.3f ms\n", copy1.mean(), copy1.pct(.95));
    printf("  copy 2 replaceRegion  (render thread): mean %6.3f  p95 %6.3f ms\n", copy2.mean(), copy2.pct(.95));
    printf("  GPU resample from replaceRegion tex :  mean %6.3f  p95 %6.3f ms\n", gpuTiled.mean(), gpuTiled.pct(.95));
    printf("  GPU resample from linear ring tex   :  mean %6.3f  p95 %6.3f ms\n", gpuLin.mean(), gpuLin.pct(.95));

    munmap(ring, slotBytes * slots); close(fd); shm_unlink(name);
}

int main() {
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        printf("device: %s\n", dev.name.UTF8String);
        NSError *err = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:@(kSrc) options:nil error:&err];
        if (!lib) { printf("%s\n", err.description.UTF8String); return 1; }
        id<MTLComputePipelineState> pso = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"resample"] error:&err];
        id<MTLCommandQueue> q = [dev newCommandQueue];
        run(dev, pso, q, 1920, 1080, 1920, 1080, 60);
        run(dev, pso, q, 3840, 2160, 3840, 2160, 60);   // canvas cap is 3840x2160
        run(dev, pso, q, 3840, 2160, 1920, 1080, 60);   // 4K into a smaller window
        run(dev, pso, q, 7680, 4320, 3840, 2160, 30);   // 8K comp
    }
    return 0;
}
