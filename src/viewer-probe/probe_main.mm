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
int run_producer(uint32_t w, uint32_t h, double fps, SourceTier tier) {
    const PixelFormat fmt = wire_format_for(tier);
    const uint32_t bpp = bytes_per_pixel(fmt);
    const uint32_t bpr = aligned_bytes_per_row(w, bpp);
    const uint64_t frame_bytes = static_cast<uint64_t>(bpr) * h;

    SharedRing ring;
    // Sized for the worst tier so a format change needs no rebuild.
    if (!ring.create(kRingName, max_frame_bytes(w, h))) {
        std::fprintf(stderr, "producer: %s\n", ring.error().c_str());
        return 1;
    }
    const char* fmt_name = fmt == PixelFormat::RGBA8Unorm  ? "RGBA8Unorm (AE 8bpc, native)"
                         : fmt == PixelFormat::RGBA16Unorm ? "RGBA16Unorm (AE 16bpc, native)"
                                                           : "RGBA16F (AE 32bpc, converted)";
    std::printf("producing %ux%u %s at %.1f fps into %s (ctrl-C to stop)\n",
                w, h, fmt_name, fps, kRingName);

    // A stand-in ICC blob so the consumer's profile path is exercised before
    // A2 supplies a real one from AEGP_ColorSettingsSuite6.
    const std::string placeholder(512, '\0');
    ring.set_icc_profile(placeholder.data(), static_cast<uint32_t>(placeholder.size()));

    const auto period = std::chrono::duration<double>(1.0 / fps);
    auto next = std::chrono::steady_clock::now();
    uint64_t frame = 0;

    while (true) {
        auto* px = static_cast<uint8_t*>(ring.begin_write(frame_bytes));
        if (px == nullptr) { std::fprintf(stderr, "begin_write: %s\n", ring.error().c_str()); return 1; }

        // Flat background, new colour every frame — a seam means a tear.
        const float phase = static_cast<float>(frame) * 0.05f;
        const float br = 0.15f + 0.10f * std::sin(phase);
        const float bg = 0.15f + 0.10f * std::sin(phase + 2.094f);
        const float bb = 0.15f + 0.10f * std::sin(phase + 4.189f);
        const uint32_t bar = static_cast<uint32_t>(frame * 7u) % w;

        for (uint32_t y = 0; y < h; ++y) {
            uint8_t* row = px + y * bpr;
            // Vertical ramp that runs past 1.0, so clipping is visible and the
            // float path has something above diffuse white to carry. The
            // integer tiers clip at 1.0 by definition — which is itself worth
            // seeing side by side.
            const float lift = 2.5f * static_cast<float>(y) / static_cast<float>(h);
            for (uint32_t x = 0; x < w; ++x) {
                const bool on_bar = (x >= bar && x < bar + 24u);
                const float v[4] = {
                    on_bar ? 3.0f : br * (0.4f + lift),
                    on_bar ? 3.0f : bg * (0.4f + lift),
                    on_bar ? 3.0f : bb * (0.4f + lift),
                    1.0f,
                };
                switch (fmt) {
                    case PixelFormat::RGBA8Unorm: {
                        uint8_t* o = row + x * 4;
                        for (int c = 0; c < 4; ++c)
                            o[c] = static_cast<uint8_t>((v[c] < 0 ? 0 : v[c] > 1 ? 1 : v[c]) * 255.0f + 0.5f);
                        break;
                    }
                    case PixelFormat::RGBA16Unorm: {
                        // AE's native 0..32768. The container is 0..65535, so
                        // the consumer's value_scale puts it back.
                        auto* o = reinterpret_cast<uint16_t*>(row) + x * 4;
                        for (int c = 0; c < 4; ++c)
                            o[c] = static_cast<uint16_t>((v[c] < 0 ? 0 : v[c] > 1 ? 1 : v[c]) * 32768.0f + 0.5f);
                        break;
                    }
                    default: {
                        auto* o = reinterpret_cast<uint16_t*>(row) + x * 4;
                        for (int c = 0; c < 4; ++c) o[c] = to_half(v[c]);
                        break;
                    }
                }
            }
        }

        FrameDesc d{};
        d.width = w; d.height = h; d.bytes_per_row = bpr;
        d.pixel_format = fmt;
        d.source_tier  = tier;
        d.value_scale  = (tier == SourceTier::Int16) ? kAE16ValueScale : 1.0f;
        d.channel_order = ChannelOrder::RGBA;   // the synthetic producer is RGBA
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
// dump — read the ring and print what is actually in it.
//
// The viewer proves frames arrive; it cannot prove they arrived unaltered,
// because a window shows the display pipeline's opinion of the pixels. This
// prints the bytes, which is what "bit-exact" has to mean.
// ---------------------------------------------------------------------------
// Minimal ICC reader: enough to name the profile, which is the whole job the
// sidecar profile has under PLAN.md D5 — tell the user what AE says the
// working space is, so a mismatch with their OCIO input choice is visible.
// Deliberately not a colour engine.
std::string icc_description(const std::string& icc) {
    auto be32 = [&](size_t o) -> uint32_t {
        if (o + 4 > icc.size()) return 0;
        const auto* b = reinterpret_cast<const uint8_t*>(icc.data()) + o;
        return (uint32_t(b[0]) << 24) | (uint32_t(b[1]) << 16) | (uint32_t(b[2]) << 8) | b[3];
    };
    if (icc.size() < 132) return {};
    const uint32_t count = be32(128);
    if (count > 256) return {};
    for (uint32_t i = 0; i < count; ++i) {
        const size_t e = 132 + i * 12;
        if (e + 12 > icc.size()) break;
        if (icc.compare(e, 4, "desc") != 0) continue;
        const uint32_t off = be32(e + 4), len = be32(e + 8);
        if (off + len > icc.size() || len < 12) return {};
        if (icc.compare(off, 4, "desc") == 0) {          // ICC v2 textDescription
            const uint32_t n = be32(off + 8);
            if (n == 0 || off + 12 + n > icc.size()) return {};
            std::string s = icc.substr(off + 12, n);
            while (!s.empty() && s.back() == '\0') s.pop_back();
            return s;
        }
        if (icc.compare(off, 4, "mluc") == 0) {          // ICC v4 multiLocalizedUnicode
            const uint32_t recs = be32(off + 8);
            if (recs == 0) return {};
            const uint32_t slen = be32(off + 20), soff = be32(off + 24);
            if (off + soff + slen > icc.size()) return {};
            std::string s;                                // UTF-16BE -> ASCII subset
            for (uint32_t k = 0; k + 1 < slen; k += 2) {
                const auto hi = static_cast<uint8_t>(icc[off + soff + k]);
                const auto lo = static_cast<uint8_t>(icc[off + soff + k + 1]);
                const uint16_t c = static_cast<uint16_t>((hi << 8) | lo);
                if (c == 0) break;
                s.push_back(c < 128 ? static_cast<char>(c) : '?');
            }
            return s;
        }
    }
    return {};
}

int run_dump(int argc, const char** argv) {
    SharedRing ring;
    if (!ring.open(kRingName)) {
        std::fprintf(stderr, "dump: %s\n", ring.error().c_str());
        return 1;
    }
    uint64_t last_seen = 0;
    FrameDesc d{};
    const void* pixels = nullptr;
    for (int attempt = 0; attempt < 400; ++attempt) {
        if (ring.acquire_latest(&last_seen, &d, &pixels)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (pixels == nullptr) { std::fprintf(stderr, "dump: no frame published\n"); return 1; }

    const char* fmt = d.pixel_format == PixelFormat::RGBA8Unorm  ? "RGBA8Unorm"
                    : d.pixel_format == PixelFormat::RGBA16Unorm ? "RGBA16Unorm"
                    : d.pixel_format == PixelFormat::RGBA32Float ? "RGBA32Float"
                    : d.pixel_format == PixelFormat::RGBA16F     ? "RGBA16F" : "?";
    const char* ord = d.channel_order == ChannelOrder::ARGB ? "ARGB"
                    : d.channel_order == ChannelOrder::BGRA ? "BGRA" : "RGBA";
    std::printf("seq %llu  %ux%u  %s  order %s  tier %u  scale %.6f  row %u\n",
                (unsigned long long)last_seen, d.width, d.height, fmt, ord,
                (unsigned)d.source_tier, d.value_scale, d.bytes_per_row);
    std::printf("time %lld/%lld  premult %d  comp \"%s\"\n",
                (long long)d.time_value, (long long)d.time_scale,
                (d.flags & kFlagPremultiplied) ? 1 : 0, d.comp_name);

    std::string icc;
    const uint64_t gen = ring.read_icc_profile(0, &icc);
    const std::string desc = icc_description(icc);
    std::printf("working space: \"%s\"\n", desc.empty() ? "(no description)" : desc.c_str());
    std::printf("  ICC %zu bytes, generation %llu, data space '%s', sidecar generation %llu\n",
                icc.size(), (unsigned long long)gen,
                icc.size() >= 20 ? icc.substr(16, 4).c_str() : "?",
                (unsigned long long)d.icc_generation);

    // Sample points given as x,y pairs on the command line.
    std::printf("\nsamples (as stored, AE channel order):\n");
    for (int i = 2; i + 1 < argc; i += 2) {
        const uint32_t x = (uint32_t)std::atoi(argv[i]), y = (uint32_t)std::atoi(argv[i + 1]);
        if (x >= d.width || y >= d.height) { std::printf("  (%u,%u) out of range\n", x, y); continue; }
        const auto* row = static_cast<const uint8_t*>(pixels) + (size_t)y * d.bytes_per_row;
        if (d.pixel_format == PixelFormat::RGBA8Unorm) {
            const uint8_t* p = row + x * 4;
            std::printf("  (%4u,%4u)  A=%3u R=%3u G=%3u B=%3u\n", x, y, p[0], p[1], p[2], p[3]);
        } else if (d.pixel_format == PixelFormat::RGBA16Unorm) {
            const auto* p = reinterpret_cast<const uint16_t*>(row) + x * 4;
            std::printf("  (%4u,%4u)  A=%5u R=%5u G=%5u B=%5u   (AE white = 32768)\n",
                        x, y, p[0], p[1], p[2], p[3]);
        } else {
            const auto* p = reinterpret_cast<const uint16_t*>(row) + x * 4;
            auto h2f = [](uint16_t h) {
                const int e = ((h >> 10) & 0x1F) - 15 + 127;
                const uint32_t b = ((uint32_t)(h & 0x8000u) << 16) | ((uint32_t)e << 23)
                                 | ((uint32_t)(h & 0x3FFu) << 13);
                float f; std::memcpy(&f, &b, 4); return (h & 0x7FFFu) ? f : 0.0f; };
            std::printf("  (%4u,%4u)  A=%.5f R=%.5f G=%.5f B=%.5f\n",
                        x, y, h2f(p[0]), h2f(p[1]), h2f(p[2]), h2f(p[3]));
        }
    }
    ring.release();
    return 0;
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

struct Sidecar { float value_scale; uint channel_order; };

fragment float4 f_main(VSOut in [[stage_in]],
                       texture2d<float> tex [[texture(0)]],
                       constant Sidecar& sc [[buffer(0)]]) {
    constexpr sampler s(filter::nearest, address::clamp_to_edge);
    float4 c = tex.sample(s, in.uv);

    // After Effects stores ARGB, so a texture read lands every channel one
    // slot over. Reordering here is free; doing it on the CPU would undo the
    // memcpy that makes the integer tiers cheap.
    if (sc.channel_order == 2u) c = c.gbar;        // ARGB -> RGBA
    else if (sc.channel_order == 3u) c = c.bgra;   // BGRA -> RGBA

    // value_scale is a container correction, not a look: AE 16bpc white is
    // 32768 in a 0..65535 unorm. Applied unconditionally, never per-format.
    // Beyond these two, deliberately no transform. See the file header.
    return float4(c.rgb * sc.value_scale, c.a);
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
    std::vector<PixelFormat>     _slotFormats;
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
        _slotFormats.push_back(PixelFormat::Unknown);
    }
    std::printf("ring open: %u slots, %llu KiB each\n",
                h->slot_count, (unsigned long long)(h->pixels_capacity / 1024));
    return YES;
}

- (id<MTLTexture>)textureForSlot:(uint32_t)slot pixels:(const void*)pixels desc:(const FrameDesc&)d {
    // Cache is keyed on geometry AND format: the user changing project bit
    // depth mid-session changes the format under a slot that is otherwise the
    // same size, and reusing the texture would reinterpret the bytes.
    if (slot < _textures.size() && _textures[slot] != nil
        && _slotFormats[slot] == d.pixel_format) {
        id<MTLTexture> t = _textures[slot];
        if (t.width == d.width && t.height == d.height) return t;
    }
    MTLPixelFormat mpf;
    switch (d.pixel_format) {
        case PixelFormat::RGBA8Unorm:  mpf = MTLPixelFormatRGBA8Unorm;  break;
        case PixelFormat::RGBA16Unorm: mpf = MTLPixelFormatRGBA16Unorm; break;
        case PixelFormat::RGBA16F:     mpf = MTLPixelFormatRGBA16Float; break;
        default: return nil;
    }
    id<MTLBuffer> buf = [_device newBufferWithBytesNoCopy:const_cast<void*>(pixels)
                                                   length:_ring.header()->pixels_capacity
                                                  options:MTLResourceStorageModeShared
                                              deallocator:nil];
    if (buf == nil) return nil;
    MTLTextureDescriptor* td =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:mpf
                                                           width:d.width height:d.height mipmapped:NO];
    td.usage = MTLTextureUsageShaderRead;
    td.storageMode = MTLStorageModeShared;
    id<MTLTexture> tex = [buf newTextureWithDescriptor:td offset:0 bytesPerRow:d.bytes_per_row];
    if (slot < _textures.size()) {
        _buffers[slot] = buf; _textures[slot] = tex; _slotFormats[slot] = d.pixel_format;
    }
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
        struct { float value_scale; uint32_t channel_order; } sc {
            d.value_scale > 0.0f ? d.value_scale : 1.0f,
            static_cast<uint32_t>(d.channel_order),
        };
        [enc setFragmentBytes:&sc length:sizeof(sc) atIndex:0];
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
            d.pixel_format == PixelFormat::RGBA8Unorm  ? "8i native"
              : d.pixel_format == PixelFormat::RGBA16Unorm ? "16i native"
              : "32f->16f",
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
        SourceTier tier = SourceTier::Float32;
        for (int i = 2; i + 1 < argc; i += 2) {
            const std::string k = argv[i], v = argv[i + 1];
            if (k == "--width")  w = static_cast<uint32_t>(std::atoi(v.c_str()));
            if (k == "--height") h = static_cast<uint32_t>(std::atoi(v.c_str()));
            if (k == "--fps")    fps = std::atof(v.c_str());
            if (k == "--tier")   tier = v == "8"  ? SourceTier::Int8
                                      : v == "16" ? SourceTier::Int16
                                                  : SourceTier::Float32;
        }
        return run_producer(w, h, fps, tier);
    }
    if (mode == "dump") return run_dump(argc, argv);
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
        "  qcbae-probe view\n"
        "  qcbae-probe dump [x y]...\n");
    return 2;
}
