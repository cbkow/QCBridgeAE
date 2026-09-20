// QCBridgeAE probe viewer — phase A1b.
//
// Two modes over the same ring, in two processes, because that is the shape
// the real thing has:
//
//   qcbae-probe produce [--fps N] [--width W] [--height H]
//   qcbae-probe view
//
// `produce` stands in for the AE plugin until A2 replaces it. `view` stands in
// for QCView until A3 does. Neither is meant to survive; they exist so the
// transport can be exercised and watched before anything expensive depends on
// it.
//
// The window applies NO transform of any kind — no sRGB encode, no tone map.
// Linear scene-referred content therefore looks dark here, and that is the
// correct behaviour for a probe on a project whose entire premise is an
// untransformed signal. If this window ever starts looking "nice", something
// has been inserted that shouldn't be there.
//
// The test pattern is chosen so tearing is visible rather than inferred: the
// background is a flat colour that changes every frame, so a torn frame shows
// as a horizontal seam between two colours. The moving bar gives motion to
// judge smoothness and the value ramp exceeds 1.0 to keep the HDR path honest.

#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include "common/surface/shared_ring.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace qcbae;

namespace {

constexpr const char* kRingName = "/qcbae-probe";

uint16_t to_half(float f) {
    if (f <= 0.0f) return 0;
    if (f > 65504.0f) f = 65504.0f;             // PLAN.md D4
    uint32_t bits; std::memcpy(&bits, &f, 4);
    int32_t exp = static_cast<int32_t>((bits >> 23) & 0xFFu) - 127 + 15;
    const uint32_t man = bits & 0x7FFFFFu;
    if (exp <= 0)  return 0;
    if (exp >= 31) return 0x7BFFu;
    return static_cast<uint16_t>((static_cast<uint32_t>(exp) << 10) | (man >> 13));
}

// ---------------------------------------------------------------------------
// produce
// ---------------------------------------------------------------------------
int run_producer(uint32_t w, uint32_t h, double fps) {
    const uint32_t bpp = bytes_per_pixel(PixelFormat::RGBA16F);
    const uint32_t bpr = aligned_bytes_per_row(w, bpp);
    const uint64_t frame_bytes = static_cast<uint64_t>(bpr) * h;

    SharedRing ring;
    if (!ring.create(kRingName, frame_bytes)) {
        std::fprintf(stderr, "producer: %s\n", ring.error().c_str());
        return 1;
    }
    std::printf("producing %ux%u RGBA16F at %.1f fps into %s (ctrl-C to stop)\n",
                w, h, fps, kRingName);

    // A stand-in ICC blob so the consumer's profile path is exercised before
    // A2 supplies a real one from AEGP_ColorSettingsSuite6.
    const std::string placeholder(512, '\0');
    ring.set_icc_profile(placeholder.data(), static_cast<uint32_t>(placeholder.size()));

    const auto period = std::chrono::duration<double>(1.0 / fps);
    auto next = std::chrono::steady_clock::now();
    uint64_t frame = 0;

    while (true) {
        auto* px = static_cast<uint16_t*>(ring.begin_write(frame_bytes));
        if (px == nullptr) { std::fprintf(stderr, "begin_write: %s\n", ring.error().c_str()); return 1; }

        // Flat background, new colour every frame — a seam means a tear.
        const float phase = static_cast<float>(frame) * 0.05f;
        const float br = 0.15f + 0.10f * std::sin(phase);
        const float bg = 0.15f + 0.10f * std::sin(phase + 2.094f);
        const float bb = 0.15f + 0.10f * std::sin(phase + 4.189f);
        const uint32_t bar = static_cast<uint32_t>(frame * 7u) % w;

        for (uint32_t y = 0; y < h; ++y) {
            auto* row = reinterpret_cast<uint16_t*>(reinterpret_cast<uint8_t*>(px) + y * bpr);
            // Vertical ramp that runs past 1.0, so clipping is visible and the
            // HDR path has something above diffuse white to carry.
            const float lift = 2.5f * static_cast<float>(y) / static_cast<float>(h);
            for (uint32_t x = 0; x < w; ++x) {
                const bool on_bar = (x >= bar && x < bar + 24u);
                const float r = on_bar ? 3.0f : br * (0.4f + lift);
                const float g = on_bar ? 3.0f : bg * (0.4f + lift);
                const float b = on_bar ? 3.0f : bb * (0.4f + lift);
                row[x * 4 + 0] = to_half(r);
                row[x * 4 + 1] = to_half(g);
                row[x * 4 + 2] = to_half(b);
                row[x * 4 + 3] = to_half(1.0f);
            }
        }

        FrameDesc d{};
        d.width = w; d.height = h; d.bytes_per_row = bpr;
        d.pixel_format = PixelFormat::RGBA16F;
        d.source_tier  = SourceTier::Float32;
        d.flags        = kFlagPremultiplied;
        d.time_value   = static_cast<int64_t>(frame);
        d.time_scale   = 24;
        d.graphics_white = 203.0f;
        d.icc_generation = 1;
        std::snprintf(d.comp_name, kMaxCompName, "probe pattern");
        ring.commit(d);

        ++frame;
        next += std::chrono::duration_cast<std::chrono::steady_clock::duration>(period);
        std::this_thread::sleep_until(next);
    }
}

// ---------------------------------------------------------------------------
// view
// ---------------------------------------------------------------------------
NSString* const kShaderSource = @R"(
#include <metal_stdlib>
using namespace metal;

struct VSOut { float4 pos [[position]]; float2 uv; };

vertex VSOut v_main(uint vid [[vertex_id]]) {
    // Fullscreen triangle.
    const float2 p[3] = { float2(-1,-3), float2(-1, 1), float2(3, 1) };
    VSOut o;
    o.pos = float4(p[vid], 0, 1);
    o.uv  = float2((p[vid].x + 1) * 0.5, 1.0 - (p[vid].y + 1) * 0.5);
    return o;
}

fragment float4 f_main(VSOut in [[stage_in]],
                       texture2d<float> tex [[texture(0)]]) {
    constexpr sampler s(filter::nearest, address::clamp_to_edge);
    // Deliberately no transform. See the file header.
    return tex.sample(s, in.uv);
}
)";

}  // namespace

// ---------------------------------------------------------------------------

@interface ProbeView : NSView
@end

@implementation ProbeView {
    CAMetalLayer*                _layer;
    id<MTLDevice>                _device;
    id<MTLCommandQueue>          _queue;
    id<MTLRenderPipelineState>   _pipeline;
    SharedRing                   _ring;
    std::vector<id<MTLTexture>>  _textures;   // one per slot, built once
    std::vector<id<MTLBuffer>>   _buffers;
    uint64_t                     _lastSeen;
    uint64_t                     _framesShown;
    uint64_t                     _lastSeq;
    std::chrono::steady_clock::time_point _statAt;
    NSTextField*                 _hud;
}

- (instancetype)initWithFrame:(NSRect)frame device:(id<MTLDevice>)device {
    self = [super initWithFrame:frame];
    if (!self) return nil;
    _device = device;
    _queue  = [device newCommandQueue];
    _lastSeen = 0; _framesShown = 0; _lastSeq = 0;
    _statAt = std::chrono::steady_clock::now();

    self.wantsLayer = YES;
    _layer = [CAMetalLayer layer];
    _layer.device = device;
    // 16F all the way to the display: the point is not to quantise on the way
    // out either. EDR stays off here — a probe shows values, not a look.
    _layer.pixelFormat = MTLPixelFormatRGBA16Float;
    _layer.framebufferOnly = YES;
    _layer.colorspace = CGColorSpaceCreateWithName(kCGColorSpaceExtendedLinearSRGB);
    self.layer = _layer;

    NSError* err = nil;
    id<MTLLibrary> lib = [device newLibraryWithSource:kShaderSource options:nil error:&err];
    if (lib == nil) { std::fprintf(stderr, "shader: %s\n", err.localizedDescription.UTF8String); return nil; }
    MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction   = [lib newFunctionWithName:@"v_main"];
    pd.fragmentFunction = [lib newFunctionWithName:@"f_main"];
    pd.colorAttachments[0].pixelFormat = _layer.pixelFormat;
    _pipeline = [device newRenderPipelineStateWithDescriptor:pd error:&err];
    if (_pipeline == nil) { std::fprintf(stderr, "pipeline: %s\n", err.localizedDescription.UTF8String); return nil; }

    _hud = [[NSTextField alloc] initWithFrame:NSMakeRect(12, 12, 700, 20)];
    _hud.bezeled = NO; _hud.editable = NO; _hud.drawsBackground = NO;
    _hud.textColor = NSColor.whiteColor;
    _hud.font = [NSFont monospacedSystemFontOfSize:12 weight:NSFontWeightRegular];
    _hud.stringValue = @"waiting for a producer…";
    [self addSubview:_hud];
    return self;
}

- (BOOL)openRing {
    if (_ring.valid()) return YES;
    if (!_ring.open(kRingName)) return NO;

    // One MTLBuffer + texture per slot, created once. Rebuilding them per
    // frame would be the copy we went to shared memory to avoid.
    const RingHeader* h = _ring.header();
    _buffers.clear(); _textures.clear();
    for (uint32_t i = 0; i < h->slot_count; ++i) {
        _buffers.push_back(nil);
        _textures.push_back(nil);
    }
    std::printf("ring open: %u slots, %llu KiB each\n",
                h->slot_count, (unsigned long long)(h->pixels_capacity / 1024));
    return YES;
}

- (id<MTLTexture>)textureForSlot:(uint32_t)slot pixels:(const void*)pixels desc:(const FrameDesc&)d {
    if (slot < _textures.size() && _textures[slot] != nil) {
        id<MTLTexture> t = _textures[slot];
        if (t.width == d.width && t.height == d.height) return t;
    }
    id<MTLBuffer> buf = [_device newBufferWithBytesNoCopy:const_cast<void*>(pixels)
                                                   length:_ring.header()->pixels_capacity
                                                  options:MTLResourceStorageModeShared
                                              deallocator:nil];
    if (buf == nil) return nil;
    MTLTextureDescriptor* td =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
                                                           width:d.width height:d.height mipmapped:NO];
    td.usage = MTLTextureUsageShaderRead;
    td.storageMode = MTLStorageModeShared;
    id<MTLTexture> tex = [buf newTextureWithDescriptor:td offset:0 bytesPerRow:d.bytes_per_row];
    if (slot < _textures.size()) { _buffers[slot] = buf; _textures[slot] = tex; }
    return tex;
}

- (void)tick {
    if (![self openRing]) return;

    FrameDesc d{};
    const void* pixels = nullptr;
    if (!_ring.acquire_latest(&_lastSeen, &d, &pixels)) return;

    const uint32_t slot = _ring.held_slot();
    id<MTLTexture> tex = [self textureForSlot:slot pixels:pixels desc:d];
    if (tex == nil) { _ring.release(); return; }

    _layer.drawableSize = CGSizeMake(self.bounds.size.width * self.window.backingScaleFactor,
                                     self.bounds.size.height * self.window.backingScaleFactor);
    id<CAMetalDrawable> drawable = [_layer nextDrawable];
    if (drawable != nil) {
        MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = drawable.texture;
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
        id<MTLCommandBuffer> cb = [_queue commandBuffer];
        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
        [enc setRenderPipelineState:_pipeline];
        [enc setFragmentTexture:tex atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [enc endEncoding];
        [cb presentDrawable:drawable];
        [cb commit];
        [cb waitUntilCompleted];     // hold the slot until the GPU is done with it
    }
    _ring.release();

    ++_framesShown;
    const uint64_t skipped = (_lastSeen > _lastSeq + 1) ? _lastSeen - _lastSeq - 1 : 0;
    _lastSeq = _lastSeen;

    const auto now = std::chrono::steady_clock::now();
    const double since = std::chrono::duration<double>(now - _statAt).count();
    if (since >= 0.5) {
        NSString* s = [NSString stringWithFormat:
            @"%ux%u  %s  seq %llu  shown %llu  skipped(last) %llu  %.1f fps  \"%s\"",
            d.width, d.height,
            d.source_tier == SourceTier::Float32 ? "32f->16f"
              : d.source_tier == SourceTier::Int16 ? "16i->16f" : "8i->16f",
            (unsigned long long)_lastSeen, (unsigned long long)_framesShown,
            (unsigned long long)skipped, _framesShown / since, d.comp_name];
        _hud.stringValue = s;
        _framesShown = 0;
        _statAt = now;
    }
}
@end

@interface ProbeDelegate : NSObject <NSApplicationDelegate>
@end

@implementation ProbeDelegate {
    NSWindow*   _window;
    ProbeView*  _view;
    NSTimer*    _timer;
}
- (void)applicationDidFinishLaunching:(NSNotification*)n {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    NSRect frame = NSMakeRect(0, 0, 960, 540);
    _window = [[NSWindow alloc] initWithContentRect:frame
                                          styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                                    | NSWindowStyleMaskResizable
                                            backing:NSBackingStoreBuffered defer:NO];
    _window.title = @"QCBridgeAE probe — no transform applied";
    _view = [[ProbeView alloc] initWithFrame:frame device:device];
    _window.contentView = _view;
    [_window center];
    [_window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];

    _timer = [NSTimer scheduledTimerWithTimeInterval:1.0 / 120.0 repeats:YES
                                               block:^(NSTimer*){ [self->_view tick]; }];
}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)a { return YES; }
@end

int main(int argc, const char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "";
    if (mode == "produce") {
        uint32_t w = 1280, h = 720; double fps = 24.0;
        for (int i = 2; i + 1 < argc; i += 2) {
            const std::string k = argv[i];
            if (k == "--width")  w = static_cast<uint32_t>(std::atoi(argv[i + 1]));
            if (k == "--height") h = static_cast<uint32_t>(std::atoi(argv[i + 1]));
            if (k == "--fps")    fps = std::atof(argv[i + 1]);
        }
        return run_producer(w, h, fps);
    }
    if (mode == "view") {
        @autoreleasepool {
            [NSApplication sharedApplication];
            [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
            ProbeDelegate* d = [ProbeDelegate new];
            [NSApp setDelegate:d];
            [NSApp run];
        }
        return 0;
    }
    std::fprintf(stderr,
        "usage:\n"
        "  qcbae-probe produce [--width W] [--height H] [--fps N]\n"
        "  qcbae-probe view\n");
    return 2;
}
