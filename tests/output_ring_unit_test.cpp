// The output ring (QCBridgeN/PLAN.md, P1): names, the request/reply
// handshake with generations, the consumer heartbeat and presence, the
// frame path through the shared slot protocol, the status line, and the
// lifecycle transitions of O13 as the ring sees them:
//   producer retires   -> consumer reads Retired
//   producer dies      -> consumer finds its pid dead (a forked child here)
//   consumer leaves    -> its pid is cleared on close, presence drops at once
//   consumer dies      -> heartbeat ages out, presence drops
// Everything is checked on what the OTHER side reads from the mapping, not
// on the writer's own bookkeeping.

#include "common/surface/output_ring.h"
#include "common/surface/audio_ring.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <chrono>
#if !defined(_WIN32)
#  include <sys/wait.h>
#  include <unistd.h>
#endif

using namespace qcbae;

namespace {
int failures = 0;
void check(bool cond, const char* what) {
    std::printf("%-62s %s\n", what, cond ? "ok" : "FAIL");
    if (!cond) ++failures;
}

OutputRequest request_1080p() {
    OutputRequest r {};
    r.protocol_version = kOutputDescVersion;
    r.width = 1920; r.height = 1080;
    r.pixel_format = static_cast<uint32_t>(OutputPixelFormat::BGRA8);
    r.transfer     = static_cast<uint32_t>(OutputTransfer::SdrRec709);
    r.fit          = static_cast<uint32_t>(OutputFit::Letterbox);
    r.content      = static_cast<uint32_t>(OutputContent::Composite);
    r.layers       = kOutputLayerGuides | kOutputLayerAnnotations;
    return r;
}
}  // namespace

int main() {
    const std::string id   = "unit-test";
    const std::string name = OutputRing::name_for(id);

    // --- names --------------------------------------------------------------
    check(name == "/qcview-out-unit-test", "name_for builds the prefixed ring name");
    check(OutputRing::audio_name_for(id) == "/qcview-out-unit-test-audio", "audio_name_for appends -audio");
    check(OutputRing::name_for("").empty(), "an empty id is refused");
    check(OutputRing::name_for("too-long-for-the-cap").empty(), "an id past 13 bytes is refused");
    check(OutputRing::name_for("Bad_Id").empty(), "an id outside [a-z0-9-] is refused");
    check(OutputRing::audio_name_for(id).size() <= kMaxShmName, "the audio name fits the shm cap");

    // --- create / open --------------------------------------------------------
    const uint64_t cap = output_frame_bytes(1920, 1080, OutputPixelFormat::BGRA8);
    OutputRing producer;
    check(producer.create(name, cap, 3, kOutputLaunchedByProducer), "producer creates the ring");
    check(producer.state() == OutputState::Active, "a fresh ring is Active");
    check(!producer.consumer_present(OutputRing::now_ns()), "no consumer yet");

    OutputRing consumer;
    check(consumer.open(name), "consumer opens it by name");
    check(consumer.producer_alive(), "consumer sees the producer alive");
    check(consumer.launched_by_producer(), "consumer reads the launched-by flag");
    check(consumer.producer_protocol() == kOutputDescVersion, "consumer reads the producer's protocol");
    check(consumer.header()->pixels_capacity >= cap, "slot capacity covers the 1080p frame");

    // --- attach, heartbeat, presence ------------------------------------------
    const int64_t t0 = OutputRing::now_ns();
    consumer.attach(t0);
    check(producer.consumer_present(t0), "attached consumer is present");
    check(producer.consumer_pid() != 0u, "producer sees the consumer's pid");
    check(!producer.consumer_present(t0 + 2000000000LL), "a 2 s old heartbeat is not present");
    consumer.heartbeat(t0 + 2000000000LL);
    check(producer.consumer_present(t0 + 2000000000LL), "a fresh heartbeat restores presence");

    // --- request / reply ------------------------------------------------------
    uint64_t req_gen = 0;
    OutputRequest got {};
    check(!producer.read_request(&got, &req_gen), "no request before the consumer writes one");
    consumer.write_request(request_1080p());
    check(producer.read_request(&got, &req_gen), "producer reads the request");
    check(req_gen == 1u && got.width == 1920 && got.height == 1080
          && got.pixel_format == static_cast<uint32_t>(OutputPixelFormat::BGRA8),
          "request fields cross intact, generation 1");
    check(!producer.read_request(&got, &req_gen), "the same request is not re-read");

    OutputReply reply {};
    reply.accepted = 1; reply.width = 1920; reply.height = 1080;
    reply.pixel_format = got.pixel_format; reply.transfer = got.transfer;
    reply.content = got.content; reply.layers = got.layers;
    std::strncpy(reply.message, "1080p BGRA8 SDR", sizeof reply.message - 1);
    producer.write_reply(reply, req_gen);

    uint64_t reply_gen = 0, reply_for = 0;
    OutputReply seen {};
    check(consumer.read_reply(&seen, &reply_gen, &reply_for), "consumer reads the reply");
    check(seen.accepted == 1u && reply_for == 1u && std::string(seen.message) == "1080p BGRA8 SDR",
          "reply answers request generation 1 with its message");
    check(!consumer.read_reply(&seen, &reply_gen), "the same reply is not re-read");

    OutputRequest big = request_1080p();
    big.width = 7680; big.height = 4320;
    consumer.write_request(big);
    check(producer.read_request(&got, &req_gen) && req_gen == 2u && got.width == 7680,
          "a second request arrives as generation 2");
    OutputReply refuse {};
    refuse.accepted = 0; refuse.refusal = static_cast<uint32_t>(OutputRefusal::TooLarge);
    std::strncpy(refuse.message, "above the 3840x2160 canvas cap", sizeof refuse.message - 1);
    producer.write_reply(refuse, req_gen);
    check(consumer.read_reply(&seen, &reply_gen, &reply_for) && seen.accepted == 0u
          && seen.refusal == static_cast<uint32_t>(OutputRefusal::TooLarge) && reply_for == 2u,
          "a refusal crosses with its reason and the generation it answers");

    // --- status line ----------------------------------------------------------
    uint64_t status_gen = 0;
    std::string status;
    check(!producer.read_status(&status, &status_gen), "no status before the consumer writes one");
    consumer.write_status("NDI: 2 receivers");
    check(producer.read_status(&status, &status_gen) && status == "NDI: 2 receivers",
          "status line crosses");
    std::string long_status(300, 'x');
    consumer.write_status(long_status.c_str());
    check(producer.read_status(&status, &status_gen) && status.size() == kOutputStatusBytes - 1u,
          "a long status is cut at the buffer, NUL kept");

    // --- frames ---------------------------------------------------------------
    check(producer.begin_write(producer.header()->pixels_capacity + 1u) == nullptr,
          "a frame larger than the slot (page-rounded) is refused");
    uint64_t last_seen = 0;
    OutputFrameDesc d {};
    const void* px = nullptr;
    check(!consumer.acquire_latest(&last_seen, &d, &px), "nothing to acquire before a commit");

    for (uint32_t i = 1; i <= 5; ++i) {
        void* dst = producer.begin_write(cap);
        check(dst != nullptr, "begin_write hands out a slot");
        std::memset(dst, static_cast<int>(i), static_cast<size_t>(cap));
        OutputFrameDesc desc {};
        desc.width = 1920; desc.height = 1080;
        desc.bytes_per_row = output_aligned_bytes_per_row(1920, OutputPixelFormat::BGRA8);
        desc.pixel_format = static_cast<uint32_t>(OutputPixelFormat::BGRA8);
        desc.transfer = static_cast<uint32_t>(OutputTransfer::SdrRec709);
        desc.content = static_cast<uint32_t>(OutputContent::Composite);
        desc.layers = kOutputLayerGuides;
        desc.flags = kOutputFrameHasA | kOutputFrameTimecodeValid;
        desc.presentation_ns = OutputRing::now_ns();
        desc.source_frame_index = i;
        desc.source_fps_num = 25; desc.source_fps_den = 1;
        desc.timecode = pack_timecode(1, 2, 3, i);
        producer.commit(desc);
    }
    check(producer.newest_seq() == 5u, "five commits make sequence 5");
    check(consumer.acquire_latest(&last_seen, &d, &px), "consumer acquires the newest frame");
    check(last_seen == 5u && d.source_frame_index == 5 && d.timecode == pack_timecode(1, 2, 3, 5)
          && static_cast<const uint8_t*>(px)[0] == 5u
          && static_cast<const uint8_t*>(px)[cap - 1u] == 5u,
          "latest wins: frame 5's descriptor and pixels, first and last byte");
    check(reinterpret_cast<uintptr_t>(px) % SharedMapping::page_size() == 0u,
          "the pixel buffer is page-aligned (a GPU can wrap it)");
    check(!consumer.acquire_latest(&last_seen, &d, &px), "no newer frame after it");
    consumer.report_seen(last_seen);
    check(producer.consumer_last_seen() == 5u, "the consumer's seen sequence reaches the producer");

    // The producer must step over the held slot while the consumer holds it.
    const uint32_t held = consumer.held_slot();
    void* dst = producer.begin_write(cap);
    check(dst != nullptr && dst != px, "producer writes elsewhere while a slot is held");
    std::memset(dst, 6, static_cast<size_t>(cap));
    OutputFrameDesc d6 {}; d6.width = 1920; d6.height = 1080; d6.source_frame_index = 6;
    producer.commit(d6);
    check(static_cast<const uint8_t*>(px)[0] == 5u, "the held frame is untouched by the next write");
    consumer.release();
    check(consumer.acquire_latest(&last_seen, &d, &px) && d.source_frame_index == 6 && consumer.held_slot() != held,
          "after release the next frame lands in another slot");
    consumer.release();

    // The audio ring beside it: the same muted flag the bus will set.
    AudioRing audio_p, audio_c;
    check(audio_p.create(OutputRing::audio_name_for(id), 1000000000LL, 8, 1024, 2), "audio ring beside it");
    AudioSession session {}; session.sample_rate = 48000; session.channels = 2; session.speed = 1.0f;
    audio_p.begin_session(session);
    float zeros[1024] {};
    const float* planes[2] = { zeros, zeros };
    check(audio_p.push(planes, 2, 48000, 1024, 0, kAudioPacketMuted) == 1u, "a muted packet is pushed");
    check(audio_c.open(OutputRing::audio_name_for(id)), "the audio ring opens by its derived name");
    uint64_t a_seen = 0, dropped = 0; AudioPacketDesc ad {};
    float planes_out[2 * 1024];
    check(audio_c.next_packet(&a_seen, &ad, planes_out, &dropped) == AudioRing::Next::Packet
          && (ad.flags & kAudioPacketMuted) != 0u, "the muted flag crosses on the packet");

    // --- O13: the consumer leaves cleanly ----------------------------------------
    consumer = OutputRing();   // close(): clears its pid
    check(producer.consumer_pid() == 0u, "a consumer that closes takes its pid with it");
    check(!producer.consumer_present(OutputRing::now_ns()), "...so presence drops at once");

    // --- O13: a consumer dies (pid gone, heartbeat stale) -------------------------
    {
        OutputRing c2;
        check(c2.open(name), "a second consumer opens the same ring");
        c2.attach(OutputRing::now_ns());
        // Pretend it died: forge a pid no process has. pid 0 is "none"; use a
        // high one and an old beat.
        auto* h = const_cast<OutputRingHeader*>(c2.header());
        h->consumer_pid.store(4000000u, std::memory_order_release);
        h->heartbeat_ns.store(OutputRing::now_ns() - 5000000000LL, std::memory_order_release);
        check(!producer.consumer_present(OutputRing::now_ns()), "a dead pid with a stale beat is not present");
        h->heartbeat_ns.store(OutputRing::now_ns(), std::memory_order_release);
        check(!producer.consumer_present(OutputRing::now_ns()), "a dead pid with a fresh beat is still not present");
        h->consumer_pid.store(0u, std::memory_order_release);   // so c2's close() does not fight it
    }

    // --- O13: the producer retires ---------------------------------------------
    OutputRing c3;
    check(c3.open(name), "a consumer opens before the retire");
    producer.set_state(OutputState::Retired);
    check(c3.state() == OutputState::Retired, "the consumer reads Retired");
    producer = OutputRing();   // close(): unlinks on POSIX
#if !defined(_WIN32)
    OutputRing c4;
    check(!c4.open(name), "after the producer closes, the name is gone");
#endif
    check(c3.valid() && c3.state() == OutputState::Retired, "the old mapping stays readable, still Retired");
    c3 = OutputRing();

    // --- O13: the producer dies ---------------------------------------------------
#if !defined(_WIN32)
    {
        const pid_t child = ::fork();
        if (child == 0) {
            OutputRing p;
            if (!p.create(name, cap, 3)) ::_exit(2);
            void* b = p.begin_write(cap);
            if (b == nullptr) ::_exit(3);
            std::memset(b, 9, static_cast<size_t>(cap));
            OutputFrameDesc dd {}; dd.width = 1920; dd.height = 1080; dd.source_frame_index = 9;
            p.commit(dd);
            ::_exit(0);   // no destructor: the mapping outlives its producer, as a crash would leave it
        }
        int status_code = 0;
        ::waitpid(child, &status_code, 0);
        check(WIFEXITED(status_code) && WEXITSTATUS(status_code) == 0, "a child producer published one frame and died");
        OutputRing c5;
        check(c5.open(name), "the orphaned ring still opens");
        check(!c5.producer_alive(), "...and its producer reads as dead");
        uint64_t seen5 = 0; OutputFrameDesc d5 {}; const void* p5 = nullptr;
        check(c5.acquire_latest(&seen5, &d5, &p5) && d5.source_frame_index == 9
              && static_cast<const uint8_t*>(p5)[0] == 9u,
              "the last frame it left is still readable");
        c5.release();
        c5 = OutputRing();
        // Clean the orphan up the way a producer re-creating the name would.
        OutputRing p2;
        check(p2.create(name, cap, 3), "a new producer takes the name over");
        p2 = OutputRing();
    }
#endif

    std::printf("\n%s (%d failures)\n", failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
