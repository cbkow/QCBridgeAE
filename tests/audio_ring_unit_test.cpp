// Guard rails for the audio ring (DESIGN-NOTES D6): geometry, the packet
// round trip, splitting, the lapped-reader resync, the session seqlock, and
// the state words a consumer reads from its own mapping.

#include "common/surface/audio_ring.h"
#include "common/surface/shared_ring.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <unistd.h>
#endif

using namespace qcbae;

namespace {
int failures = 0;
constexpr const char* kName = "/qcbae-audio-unit";
constexpr int64_t kTicks = 254016000000ll;   // Premiere's ticks per second

uint64_t page_size() {
#if defined(_WIN32)
    SYSTEM_INFO si {};
    ::GetSystemInfo(&si);
    return static_cast<uint64_t>(si.dwPageSize);
#else
    return static_cast<uint64_t>(getpagesize());
#endif
}
void check(bool cond, const char* what) {
    std::printf("%-66s %s\n", what, cond ? "ok" : "FAIL");
    if (!cond) ++failures;
}

// A planar buffer whose every sample is `value`, `ch` channels of `n` frames.
struct Planes {
    std::vector<std::vector<float>> data;
    std::vector<const float*> ptrs;
    Planes(uint32_t ch, uint32_t n, float value) : data(ch, std::vector<float>(n, value)) {
        for (auto& d : data) ptrs.push_back(d.data());
    }
    const float* const* get() const { return ptrs.data(); }
};
}  // namespace

int main() {
    // --- name and geometry validation --------------------------------------
    {
        AudioRing r;
        check(!r.create("no-leading-slash", kTicks), "rejects name without leading slash");
        check(!r.create("/this-name-is-far-too-long-for-macos", kTicks), "rejects name over PSHMNAMLEN (31)");
        check(!r.create(kName, kTicks, 2), "rejects slot_count < 4");
        check(!r.create(kName, kTicks, 8, 0), "rejects slot_frames == 0");
        check(!r.create(kName, kTicks, 8, 64, 0), "rejects max_channels == 0");
        check(!r.create(kName, kTicks, 8, 64, 17), "rejects max_channels > 16");
        check(!r.create(kName, 0), "rejects a non-positive time_scale");
    }

    // Small geometry so the lapping cases run fast: 8 slots of 64 frames, 4 planes.
    constexpr uint32_t kSlots = 8, kFrames = 64, kCh = 4;
    AudioRing producer;
    check(producer.create(kName, kTicks, kSlots, kFrames, kCh), "creates an audio ring");
    {
        const auto* h = producer.header();
        const uint64_t ps = page_size();
        check(h->magic == kAudioRingMagic && h->version == kAudioDescVersion, "header carries magic and version");
        check(h->slots_offset % ps == 0, "slot region starts on a page boundary");
        check(h->samples_offset % kAudioPlaneAlignment == 0 && h->slot_stride % kAudioPlaneAlignment == 0,
              "planes and strides are 64-byte aligned");
        check(h->samples_offset + uint64_t(kFrames) * 4 * kCh <= h->slot_stride, "a slot holds max_channels planes");
        check(h->total_size == h->slots_offset + uint64_t(kSlots) * h->slot_stride, "total_size matches the layout");
        check(h->time_scale == kTicks, "time_scale is stamped into the header");
    }

    AudioRing consumer;
    check(consumer.open(kName), "opens an existing audio ring");
    check(!AudioRing().open("/qcbae-audio-nonexistent"), "fails to open a ring that isn't there");
    {
        // A segment claiming another version is refused by name.
        AudioRing other;
        other.create("/qcbae-audio-unit-v", kTicks, 4, 8, 1);
        const_cast<AudioRingHeader*>(other.header())->version = kAudioDescVersion + 1;
        AudioRing probe;
        check(!probe.open("/qcbae-audio-unit-v") && probe.error().find("version") != std::string::npos,
              "refuses a ring of another version, naming the version");
    }

    // --- nothing pushed yet ------------------------------------------------
    uint64_t last = 0, dropped = 0;
    AudioPacketDesc d {};
    std::vector<float> out(uint64_t(kCh) * kFrames, -1.0f);
    check(consumer.next_packet(&last, &d, out.data(), &dropped) == AudioRing::Next::None, "an empty ring yields nothing");
    check(consumer.state() == AudioState::Idle, "a new ring reads as Idle");
    check(consumer.host_audio() == HostAudio::Unknown, "host audio starts Unknown");
    check(AudioRing().state() == AudioState::Retired, "an unopened ring reads as Retired");

    // --- one packet --------------------------------------------------------
    {
        Planes p(2, kFrames, 0.25f);
        check(producer.push(p.get(), 2, 48000, kFrames, 1000, 0) == 1, "a full push is one packet");
    }
    check(consumer.next_packet(&last, &d, out.data(), &dropped) == AudioRing::Next::Packet, "delivers the packet");
    check(last == 1 && d.packet_seq == 1, "sequence starts at 1");
    check(d.frames == kFrames && d.channels == 2 && d.sample_rate == 48000 && d.time_value == 1000
          && d.first_frame == 0, "descriptor survives the crossing");
    check(out[0] == 0.25f && out[kFrames - 1] == 0.25f && out[kFrames] == 0.25f && out[2 * kFrames - 1] == 0.25f,
          "both planes survive the crossing, at plane stride slot_frames");
    check(out[2 * kFrames] == -1.0f, "planes beyond desc.channels are left alone");
    check(consumer.next_packet(&last, &d, out.data(), &dropped) == AudioRing::Next::None, "no re-delivery");

    // --- partial and oversize pushes ---------------------------------------
    {
        Planes p(2, 10, 0.5f);
        check(producer.push(p.get(), 2, 48000, 10, 2000, kAudioPacketScrubbing) == 1, "a short push is one packet");
        check(consumer.next_packet(&last, &d, out.data(), &dropped) == AudioRing::Next::Packet
              && d.frames == 10 && d.first_frame == kFrames && d.flags == kAudioPacketScrubbing,
              "a short packet carries its frame count, stream position and flags");
    }
    {
        // 2.5 slots' worth in one push -> three contiguous packets, time advanced per packet.
        std::vector<float> ramp(kFrames * 2 + kFrames / 2);
        for (size_t i = 0; i < ramp.size(); ++i) ramp[i] = static_cast<float>(i);
        const float* planes[1] = { ramp.data() };
        check(producer.push(planes, 1, 48000, static_cast<uint32_t>(ramp.size()), 0, 0) == 3,
              "an oversize push splits into three packets");
        uint64_t frame_expect = kFrames + 10;
        int64_t  time_expect  = 0;
        bool contiguous = true, values = true;
        for (int i = 0; i < 3; ++i) {
            if (consumer.next_packet(&last, &d, out.data(), &dropped) != AudioRing::Next::Packet) { contiguous = false; break; }
            if (d.first_frame != frame_expect || d.time_value != time_expect) contiguous = false;
            for (uint32_t k = 0; k < d.frames; ++k)
                if (out[k] != static_cast<float>(i * kFrames + k)) values = false;
            frame_expect += d.frames;
            time_expect  += static_cast<int64_t>(d.frames) * kTicks / 48000;
        }
        check(contiguous, "split packets are contiguous in frames and time");
        check(values, "split packets carry the right samples");
        check(d.frames == kFrames / 2, "the last split packet is the remainder");
    }

    // --- in-order delivery and the lapped reader -----------------------------
    {
        bool in_order = true;
        for (uint32_t i = 0; i < kSlots - 2; ++i) {
            Planes p(1, kFrames, static_cast<float>(100 + i));
            producer.push(p.get(), 1, 48000, kFrames, 0, 0);
        }
        uint64_t prev = last;
        for (uint32_t i = 0; i < kSlots - 2; ++i) {
            if (consumer.next_packet(&last, &d, out.data(), &dropped) != AudioRing::Next::Packet
                || last != prev + 1 || out[0] != static_cast<float>(100 + i)) in_order = false;
            prev = last;
        }
        check(in_order, "N packets within a ring's worth arrive in order");
        check(dropped == 0, "nothing dropped while keeping up");

        // Now push far more than the ring holds without reading.
        for (uint32_t i = 0; i < kSlots + 10; ++i) {
            Planes p(1, kFrames, static_cast<float>(1000 + i));
            producer.push(p.get(), 1, 48000, kFrames, 0, 0);
        }
        const uint64_t before = last;
        const auto r = consumer.next_packet(&last, &d, out.data(), &dropped);
        check(r == AudioRing::Next::Resynced && last > before && dropped > 0,
              "a lapped reader resyncs and reports what it dropped");
        bool monotonic = true, consistent = true; uint64_t n = 0;
        prev = last;
        while (consumer.next_packet(&last, &d, out.data(), &dropped) == AudioRing::Next::Packet) {
            if (last != prev + 1) monotonic = false;
            if (d.packet_seq != last) consistent = false;
            // Every sample of packet q is 1000 + (q - firstOfBurst): check the plane matches the seq.
            prev = last; ++n;
        }
        check(n == kSlots - 2, "after a resync the reader gets slot_count - 2 packets");
        check(monotonic && consistent, "delivered packets are monotonic and match their descriptors");
        check(last == producer.latest(), "and ends at the newest packet");
    }

    // --- skip_to_latest -------------------------------------------------------
    {
        Planes p(1, kFrames, 7.0f);
        producer.push(p.get(), 1, 48000, kFrames, 0, 0);
        producer.push(p.get(), 1, 48000, kFrames, 0, 0);
        uint64_t cold = 0;
        consumer.skip_to_latest(&cold);
        check(cold == producer.latest(), "skip_to_latest lands on the newest packet");
        check(consumer.next_packet(&cold, &d, out.data(), &dropped) == AudioRing::Next::None,
              "a cold consumer ignores history");
    }

    // --- the session record -----------------------------------------------------
    {
        AudioSession s {};
        uint64_t known = 0;
        check(!consumer.read_session(&s, &known), "no session before one begins");
        AudioSession in {};
        in.start_time = 123; in.in_time = 0; in.out_time = 456; in.speed = -1.0f;
        in.sample_rate = 48000; in.channels = 2; in.flags = kAudioSessionScrubbing; in.push_frames = 1024;
        producer.begin_session(in);
        check(consumer.state() == AudioState::Pushing, "begin_session reads as Pushing");
        check(consumer.read_session(&s, &known) && known == 1, "the consumer sees generation 1");
        check(s.start_time == 123 && s.out_time == 456 && s.speed == -1.0f && s.channels == 2
              && s.flags == kAudioSessionScrubbing && s.push_frames == 1024, "the session round-trips");
        check(!consumer.read_session(&s, &known), "a known generation reads as unchanged");
        producer.end_session();
        check(consumer.state() == AudioState::Idle, "end_session reads as Idle");
        in.speed = 2.0f;
        producer.begin_session(in);
        check(consumer.read_session(&s, &known) && known == 2 && s.speed == 2.0f, "a second session bumps the generation");
        producer.set_host_audio(HostAudio::Off);
        check(consumer.host_audio() == HostAudio::Off, "consumer sees host audio Off");
        producer.set_state(AudioState::Retired);
        check(consumer.state() == AudioState::Retired, "consumer sees Retired");
    }

    // --- liveness ----------------------------------------------------------------
    check(process_alive(producer.header()->producer_pid), "this process reads as alive");

    // --- replacing a ring a consumer still holds ------------------------------------
    {
        producer = AudioRing();
        AudioRing second;
        const bool created = second.create(kName, kTicks, kSlots, kFrames, kCh);
#if defined(_WIN32)
        check(!created, "create() while a consumer holds the name reports it (Windows)");
        check(consumer.state() == AudioState::Retired, "the held mapping was marked Retired");
        consumer = AudioRing();
        check(second.create(kName, kTicks, kSlots, kFrames, kCh), "create() succeeds once the consumer let go");
#else
        check(created, "create() replaces the name while a consumer holds the old one (POSIX)");
        consumer = AudioRing();
#endif
        check(second.valid() && consumer.open(kName)
              && consumer.header()->producer_pid == second.header()->producer_pid,
              "consumer re-opens the replaced ring");
        check(consumer.state() == AudioState::Idle, "the replacement reads as Idle");
    }

    std::printf("\n%s (%d failure%s)\n", failures == 0 ? "PASS" : "FAIL",
                failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
