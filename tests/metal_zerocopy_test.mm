// A1b exit criterion: is the ring actually zero-copy on the GPU side?
//
// the design notes justified shared memory over IOSurface on the claim that
// makeBuffer(bytesNoCopy:) over these pages gives the GPU the exact memory the
// CPU wrote. That is a claim, not a fact, until something checks it — and
// "the window looks right" cannot distinguish a shared mapping from a silent
// upload behind the scenes.
//
// So this test proves it the only way that actually discriminates: build the
// texture, have the GPU read it, then mutate the shared memory from the CPU
// with NO Metal call of any kind, and have the GPU read again. If the second
// read reflects the CPU's write, the two are looking at the same pages. If it
// doesn't, we copied, and the IOSurface decision has to be revisited.

#import <Metal/Metal.h>

#include "common/surface/shared_ring.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace qcbae;

namespace {
int failures = 0;
void check(bool cond, const char* what) {
    std::printf("%-62s %s\n", what, cond ? "ok" : "FAIL");
    if (!cond) ++failures;
}

// Half-float encode, just enough for a test pattern.
uint16_t to_half(float f) {
    uint32_t bits;
    __builtin_memcpy(&bits, &f, 4);
    const uint32_t sign = (bits >> 16) & 0x8000u;
    int32_t exp = static_cast<int32_t>((bits >> 23) & 0xFFu) - 127 + 15;
    uint32_t man = bits & 0x7FFFFFu;
    if (exp <= 0)  return static_cast<uint16_t>(sign);
    if (exp >= 31) return static_cast<uint16_t>(sign | 0x7C00u);
    return static_cast<uint16_t>(sign | (static_cast<uint32_t>(exp) << 10) | (man >> 13));
}
}  // namespace

int main() {
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (device == nil) { std::printf("no Metal device\n"); return 77; }
        std::printf("device: %s (unified memory: %s)\n\n",
                    device.name.UTF8String, device.hasUnifiedMemory ? "yes" : "no");

        constexpr uint32_t kW = 256, kH = 128;
        const uint32_t bpp = bytes_per_pixel(PixelFormat::RGBA16F);
        const uint32_t bpr = aligned_bytes_per_row(kW, bpp);
        const uint64_t frame_bytes = static_cast<uint64_t>(bpr) * kH;

        const NSUInteger min_align =
            [device minimumLinearTextureAlignmentForPixelFormat:MTLPixelFormatRGBA16Float];
        check(bpr % min_align == 0,
              "kRowAlignment satisfies this device's linear texture alignment");

        SharedRing producer;
        check(producer.create("/qcbae-mtl", frame_bytes), "creates the ring");

        // --- publish a frame, all pixels a known value -----------------------
        const float kValueA = 0.25f, kValueB = 0.75f;
        {
            auto* px = static_cast<uint16_t*>(producer.begin_write(frame_bytes));
            check(px != nullptr, "producer gets a slot");
            for (uint32_t y = 0; y < kH; ++y) {
                auto* row = reinterpret_cast<uint16_t*>(reinterpret_cast<uint8_t*>(px) + y * bpr);
                for (uint32_t x = 0; x < kW * 4; ++x) row[x] = to_half(kValueA);
            }
            FrameDesc d{};
            d.width = kW; d.height = kH; d.bytes_per_row = bpr;
            d.pixel_format = PixelFormat::RGBA16F;
            d.source_tier  = SourceTier::Float32;
            d.flags = kFlagPremultiplied;
            d.time_value = 1; d.time_scale = 24;
            producer.commit(d);
        }

        SharedRing consumer;
        check(consumer.open("/qcbae-mtl"), "consumer opens the ring");

        uint64_t last_seen = 0;
        FrameDesc desc{};
        const void* pixels = nullptr;
        check(consumer.acquire_latest(&last_seen, &desc, &pixels), "consumer acquires the frame");

        // --- wrap the slot, no copy ------------------------------------------
        const uint64_t cap = consumer.header()->pixels_capacity;
        id<MTLBuffer> buffer =
            [device newBufferWithBytesNoCopy:const_cast<void*>(pixels)
                                      length:cap
                                     options:MTLResourceStorageModeShared
                                 deallocator:nil];
        check(buffer != nil, "newBufferWithBytesNoCopy accepts the page-aligned slot");
        if (buffer == nil) { std::printf("\nFAIL — cannot continue\n"); return 1; }
        check(buffer.contents == pixels,
              "MTLBuffer.contents is the ring's own memory (no relocation)");

        MTLTextureDescriptor* td = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
                                         width:desc.width
                                        height:desc.height
                                     mipmapped:NO];
        td.usage       = MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModeShared;
        id<MTLTexture> texture = [buffer newTextureWithDescriptor:td
                                                           offset:0
                                                      bytesPerRow:desc.bytes_per_row];
        check(texture != nil, "linear texture over the slot");
        if (texture == nil) { std::printf("\nFAIL — cannot continue\n"); return 1; }

        // --- have the GPU read it -------------------------------------------
        id<MTLCommandQueue> queue = [device newCommandQueue];
        id<MTLBuffer> readback = [device newBufferWithLength:frame_bytes
                                                     options:MTLResourceStorageModeShared];

        auto gpu_read_first_pixel = [&]() -> float {
            id<MTLCommandBuffer> cb = [queue commandBuffer];
            id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
            [blit copyFromTexture:texture
                      sourceSlice:0
                      sourceLevel:0
                     sourceOrigin:MTLOriginMake(0, 0, 0)
                       sourceSize:MTLSizeMake(desc.width, desc.height, 1)
                         toBuffer:readback
                destinationOffset:0
           destinationBytesPerRow:desc.bytes_per_row
         destinationBytesPerImage:frame_bytes];
            [blit endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            const auto* out = static_cast<const uint16_t*>(readback.contents);
            // Decode half back to float, crudely but exactly enough for 0.25/0.75.
            const uint16_t h = out[0];
            const int exp = ((h >> 10) & 0x1F) - 15 + 127;
            const uint32_t bits = (static_cast<uint32_t>(h & 0x8000u) << 16)
                                | (static_cast<uint32_t>(exp) << 23)
                                | (static_cast<uint32_t>(h & 0x3FFu) << 13);
            float f; __builtin_memcpy(&f, &bits, 4); return f;
        };

        const float first = gpu_read_first_pixel();
        check(first == kValueA, "GPU reads back what the producer wrote");

        // --- the discriminating step -----------------------------------------
        // Mutate the shared pages directly. No Metal call, no didModifyRange,
        // no blit, no re-creation of buffer or texture. If Metal had copied,
        // the GPU would still see the old value.
        {
            auto* px = static_cast<uint16_t*>(const_cast<void*>(pixels));
            for (uint32_t y = 0; y < kH; ++y) {
                auto* row = reinterpret_cast<uint16_t*>(reinterpret_cast<uint8_t*>(px) + y * bpr);
                for (uint32_t x = 0; x < kW * 4; ++x) row[x] = to_half(kValueB);
            }
        }
        const float second = gpu_read_first_pixel();
        check(second == kValueB,
              "GPU sees a bare CPU write with no upload — ZERO COPY CONFIRMED");

        std::printf("\n%s (%d failure%s)\n", failures == 0 ? "PASS" : "FAIL",
                    failures, failures == 1 ? "" : "s");
        return failures == 0 ? 0 : 1;
    }
}
