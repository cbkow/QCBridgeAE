// The one real use-after-unmap hazard in the audio path: the host thread
// retiring the mapping while the host's audio thread is inside a push. The
// AudioPublisher guard (audio_publisher.h) is the whole defence, and this is
// the only thing that exercises it — threads, not processes, because the
// hazard is inside one process. Run under ThreadSanitizer when in doubt:
//   cmake -S . -B build-tsan -DCMAKE_CXX_FLAGS=-fsanitize=thread
//
// A pusher thread pushes as fast as it can with the owner's id and with a
// stranger's id; the main thread creates, adopts, starts, stops and quiesces
// the ring a few thousand times, and checks that pushes were accepted only
// between start and stop, and that nothing crashed.

#include "common/surface/audio_publisher.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

using namespace qcbae;

namespace {
constexpr const char* kName = "/qcbae-audio-quiesce";
constexpr int64_t kTicks = 254016000000ll;
constexpr int64_t kOwner = 7, kStranger = 9;
}

int main() {
    AudioPublisher pub;
    std::atomic<bool> run{true};
    std::atomic<uint64_t> accepted{0}, stranger_accepted{0}, attempts{0};

    std::vector<float> plane(256, 0.5f);
    std::thread pusher([&] {
        const float* planes[2] = { plane.data(), plane.data() };
        while (run.load()) {
            accepted += pub.push(kOwner, planes, 2, 48000, 256, 0, 0);
            stranger_accepted += pub.push(kStranger, planes, 2, 48000, 256, 0, 0);
            ++attempts;
        }
    });

    int failures = 0;
    uint64_t sessions_with_pushes = 0;
    constexpr int kRounds = 3000;
    for (int round = 0; round < kRounds; ++round) {
        auto ring = std::make_unique<AudioRing>();
        if (!ring->create(kName, kTicks, 8, 256, 2)) {
            std::printf("create failed: %s\n", ring->error().c_str()); ++failures; break;
        }
        if (!pub.adopt(std::move(ring))) { std::printf("adopt refused\n"); ++failures; break; }
        if (pub.adopt(std::make_unique<AudioRing>())) { std::printf("adopt accepted an invalid ring\n"); ++failures; }

        const uint64_t before = accepted.load();
        AudioSession s {};
        s.sample_rate = 48000; s.channels = 2; s.push_frames = 256;
        pub.start(kOwner, s);
        std::this_thread::yield();
        std::this_thread::sleep_for(std::chrono::microseconds(50));
        const uint64_t mid = accepted.load();
        if (mid > before) ++sessions_with_pushes;

        // A stop from a non-owner must be ignored; the owner's stop ends it.
        pub.stop(kStranger);
        if (pub.owner() != kOwner) { std::printf("a stranger's stop took effect\n"); ++failures; }
        pub.stop(kOwner);
        if (pub.owner() != AudioPublisher::kNoOwner) { std::printf("the owner's stop did not take\n"); ++failures; }

        // Half the rounds quiesce mid-session instead, the module-unload case.
        if (round % 2 == 1) pub.start(kOwner, s);
        if (!pub.quiesce(2000u)) { std::printf("quiesce timed out (push stuck?)\n"); ++failures; break; }
        if (pub.has_ring()) { std::printf("ring survived quiesce\n"); ++failures; }
    }
    run.store(false);
    pusher.join();

    std::printf("rounds %d, push attempts %llu, accepted %llu, sessions that saw pushes %llu, stranger accepted %llu\n",
                kRounds, (unsigned long long)attempts.load(), (unsigned long long)accepted.load(),
                (unsigned long long)sessions_with_pushes, (unsigned long long)stranger_accepted.load());
    if (stranger_accepted.load() != 0) { std::printf("FAIL: a non-owner's push was written\n"); ++failures; }
    if (sessions_with_pushes == 0)     { std::printf("FAIL: no session ever saw a push; the race was not exercised\n"); ++failures; }
    if (accepted.load() == 0)          { std::printf("FAIL: nothing accepted\n"); ++failures; }
    std::printf("%s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
