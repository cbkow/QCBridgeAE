// QCBridgeAE — the process-local guard around an AudioRing's producer side.
//
// The Transmit device receives audio on the host's own high-priority thread,
// concurrently with every other entry point (PrSDKTransmit.h: "only a single
// thread will enter a module at a time, with the exception of PushAudio()").
// Everything else — session start/stop, instance disposal, module unload —
// runs on the host thread and is what creates and destroys the mapping. So
// the mapping must never go away under a push, and a push must never touch
// anything but the ring.
//
// This header is the whole of that rule, kept free of the SDK so a test can
// hammer it under a sanitizer:
//   * `owner` is the instance id whose session is current. A module reset
//     creates the new instance BEFORE StopPushAudio of the old one (A8 log),
//     and the old one keeps pushing until then; pushes from a non-owner are
//     dropped, and a stop from a non-owner is ignored.
//   * `in_push` counts pushes in flight. quiesce() takes the ring away first
//     and then waits for the count to reach zero before unmapping. Both sides
//     are seq_cst: the pusher increments then loads the ring, the host
//     exchanges the ring then loads the count — a Dekker pattern that
//     acquire/release alone would not make safe.
//   * The wait is bounded. If a push is somehow stuck, the mapping is leaked
//     rather than unmapped under it; the caller logs that.

#pragma once

#include "common/surface/audio_ring.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>

namespace qcbae {

class AudioPublisher {
public:
    static constexpr int64_t kNoOwner = -1;

    AudioPublisher() = default;
    ~AudioPublisher() { quiesce(50u); }
    AudioPublisher(const AudioPublisher&) = delete;
    AudioPublisher& operator=(const AudioPublisher&) = delete;

    // --- Host thread -----------------------------------------------------------

    // Takes ownership of a created ring. Refused (and the ring destroyed)
    // if one is already adopted: the segment is created once per host
    // session and reused across every start/stop.
    bool adopt(std::unique_ptr<AudioRing> ring) {
        if (!ring || !ring->valid()) return false;
        AudioRing* expected = nullptr;
        AudioRing* raw = ring.get();
        if (!ring_.compare_exchange_strong(expected, raw, std::memory_order_seq_cst)) return false;
        ring.release();
        return true;
    }

    bool has_ring() const { return ring_.load(std::memory_order_seq_cst) != nullptr; }
    int64_t owner() const { return owner_.load(std::memory_order_seq_cst); }

    // Host-thread view of the ring for state writes. Never cached across
    // a call that may quiesce.
    AudioRing* ring_for_host() { return ring_.load(std::memory_order_seq_cst); }

    void start(int64_t owner, const AudioSession& s) {
        AudioRing* r = ring_.load(std::memory_order_seq_cst);
        if (r == nullptr) return;
        r->begin_session(s);
        r->set_host_audio(HostAudio::On);
        owner_.store(owner, std::memory_order_seq_cst);
    }

    // Only the owner ends its session: a stop that arrives for an instance
    // that already lost ownership (the old instance of a module reset) must
    // not stop the new one's.
    void stop(int64_t owner) {
        if (owner_.load(std::memory_order_seq_cst) != owner) return;
        owner_.store(kNoOwner, std::memory_order_seq_cst);
        if (AudioRing* r = ring_.load(std::memory_order_seq_cst)) r->end_session();
    }

    // Retire and unmap, once no push is in flight. Returns false when the
    // wait timed out and the mapping was leaked instead.
    bool quiesce(unsigned timeout_ms) {
        owner_.store(kNoOwner, std::memory_order_seq_cst);
        AudioRing* r = ring_.exchange(nullptr, std::memory_order_seq_cst);
        if (r == nullptr) return true;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (in_push_.load(std::memory_order_seq_cst) != 0u) {
            if (std::chrono::steady_clock::now() > deadline) return false;   // leak r on purpose
            std::this_thread::yield();
        }
        r->set_state(AudioState::Retired);
        delete r;
        return true;
    }

    // --- Any thread (the host's audio thread) -----------------------------------

    // Returns packets written; 0 when there is no ring or `owner` is not the
    // session's owner.
    uint32_t push(int64_t owner, const float* const* planes, uint32_t channels, uint32_t sample_rate,
                  uint32_t frames, int64_t time_value, uint32_t flags) {
        in_push_.fetch_add(1u, std::memory_order_seq_cst);
        uint32_t written = 0u;
        AudioRing* r = ring_.load(std::memory_order_seq_cst);
        if (r != nullptr && owner_.load(std::memory_order_seq_cst) == owner)
            written = r->push(planes, channels, sample_rate, frames, time_value, flags);
        in_push_.fetch_sub(1u, std::memory_order_seq_cst);
        return written;
    }

private:
    std::atomic<AudioRing*> ring_{nullptr};
    std::atomic<int64_t>    owner_{kNoOwner};
    std::atomic<uint32_t>   in_push_{0u};
};

}  // namespace qcbae
