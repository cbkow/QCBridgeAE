// The audio ring's crossing, two real processes (the frame ring's tearing
// test, ported). Every float of every plane of packet p is float(p), and the
// descriptor's time_value is p too, so a torn copy is visible three ways:
// samples disagreeing with each other, samples disagreeing with the
// descriptor, or the sequence going backwards. The consumer dawdles before
// its copy so the producer laps it, which exercises the resync path — the
// only thing this ring has in place of the frame ring's reader claim.

#include "common/surface/audio_ring.h"

#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <thread>
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
#  include <csignal>
#  include <sys/mman.h>
#  include <sys/wait.h>
#  include <unistd.h>
#endif

using namespace qcbae;

namespace {

constexpr const char* kName  = "/qcbae-audiotest";
constexpr uint32_t kSlots    = 16;
constexpr uint32_t kFrames   = 1024;
constexpr uint32_t kCh       = 2;
constexpr uint64_t kPackets  = 20000;
constexpr int64_t  kTicks    = 254016000000ll;

struct Tally {
    std::atomic<uint64_t> seen{0};
    std::atomic<uint64_t> torn{0};
    std::atomic<uint64_t> desc_mismatch{0};
    std::atomic<uint64_t> went_backwards{0};
    std::atomic<uint64_t> resyncs{0};
    std::atomic<uint64_t> dropped{0};
    std::atomic<uint32_t> consumer_ready{0};
    std::atomic<uint32_t> producer_done{0};
};

int run_consumer(Tally* tally) {
    AudioRing ring;
    for (int attempt = 0; attempt < 2000 && !ring.open(kName); ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (!ring.valid()) { std::fprintf(stderr, "consumer: %s\n", ring.error().c_str()); return 2; }

    std::vector<float> planes(uint64_t(kCh) * kFrames);
    uint64_t last = 0, previous = 0, dropped = 0;
    AudioPacketDesc d {};
    tally->consumer_ready.store(1);

    bool draining = false;
    while (true) {
        // Dawdle BEFORE the copy: with no claim, the interesting case is the
        // writer reaching our slot while we are about to read it.
        std::this_thread::sleep_for(std::chrono::microseconds(30));
        const auto r = ring.next_packet(&last, &d, planes.data(), &dropped);
        if (r == AudioRing::Next::Resynced) { tally->resyncs.fetch_add(1); continue; }
        if (r == AudioRing::Next::None) {
            if (draining) break;
            if (tally->producer_done.load()) draining = true;
            std::this_thread::yield();
            continue;
        }
        draining = false;
        const float first = planes[0];
        bool torn = false;
        for (uint32_t c = 0; c < d.channels && !torn; ++c)
            for (uint32_t i = 0; i < d.frames; ++i)
                if (planes[uint64_t(c) * kFrames + i] != first) { torn = true; break; }
        if (torn) tally->torn.fetch_add(1);
        if (first != static_cast<float>(last) || static_cast<uint64_t>(d.time_value) != last
            || d.packet_seq != last)
            tally->desc_mismatch.fetch_add(1);
        if (last < previous) tally->went_backwards.fetch_add(1);
        previous = last;
        tally->seen.fetch_add(1);
    }
    tally->dropped.store(dropped);
    return 0;
}

#if defined(_WIN32)
constexpr const wchar_t* kTallyName = L"Local\\qcbae-audiotest-tally";

struct Child {
    HANDLE process = nullptr;
    bool start(const char* exe_path) {
        std::string cmd = std::string("\"") + exe_path + "\" --consumer";
        STARTUPINFOA si {}; si.cb = sizeof si;
        PROCESS_INFORMATION pi {};
        if (!::CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
            std::fprintf(stderr, "CreateProcess failed (%lu)\n", ::GetLastError()); return false;
        }
        ::CloseHandle(pi.hThread);
        process = pi.hProcess;
        return true;
    }
    void kill() { ::TerminateProcess(process, 9); wait(); }
    int  wait() {
        ::WaitForSingleObject(process, INFINITE);
        DWORD code = 1; ::GetExitCodeProcess(process, &code);
        ::CloseHandle(process); process = nullptr;
        return static_cast<int>(code);
    }
};

Tally* map_tally(bool create, HANDLE* out) {
    HANDLE h = create
        ? ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Tally), kTallyName)
        : ::OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, kTallyName);
    if (h == nullptr) { std::fprintf(stderr, "tally mapping failed (%lu)\n", ::GetLastError()); return nullptr; }
    void* p = ::MapViewOfFile(h, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(Tally));
    if (p == nullptr) { std::fprintf(stderr, "tally view failed (%lu)\n", ::GetLastError()); ::CloseHandle(h); return nullptr; }
    *out = h;
    return create ? new (p) Tally() : static_cast<Tally*>(p);
}
#else
struct Child {
    pid_t pid = -1;
    Tally* tally = nullptr;
    bool start(const char*) {
        pid = ::fork();
        if (pid < 0) { std::perror("fork"); return false; }
        if (pid == 0) { _exit(run_consumer(tally)); }
        return true;
    }
    void kill() { ::kill(pid, SIGKILL); ::waitpid(pid, nullptr, 0); }
    int  wait() { int status = 0; ::waitpid(pid, &status, 0); return WIFEXITED(status) ? WEXITSTATUS(status) : 1; }
};
#endif

}  // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
    if (argc > 1 && std::strcmp(argv[1], "--consumer") == 0) {
        HANDLE th = nullptr;
        Tally* tally = map_tally(false, &th);
        if (tally == nullptr) return 2;
        return run_consumer(tally);
    }
    HANDLE th = nullptr;
    Tally* tally = map_tally(true, &th);
    if (tally == nullptr) return 1;
#else
    (void)argc;
    void* shared = ::mmap(nullptr, sizeof(Tally), PROT_READ | PROT_WRITE,
                          MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (shared == MAP_FAILED) { std::perror("mmap tally"); return 1; }
    auto* tally = new (shared) Tally();
#endif

    AudioRing ring;
    if (!ring.create(kName, kTicks, kSlots, kFrames, kCh)) {
        std::fprintf(stderr, "producer: %s\n", ring.error().c_str());
        return 1;
    }

    Child child;
#if !defined(_WIN32)
    child.tally = tally;
#endif
    if (!child.start(argv[0])) return 1;

    for (int i = 0; i < 5000 && tally->consumer_ready.load() == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (tally->consumer_ready.load() == 0) {
        std::fprintf(stderr, "consumer never became ready\n");
        child.kill();
        return 1;
    }

    std::vector<float> plane(kFrames);
    const float* planes[kCh] = { plane.data(), plane.data() };
    uint64_t published = 0;
    for (uint64_t seq = 1; seq <= kPackets; ++seq) {
        for (uint32_t i = 0; i < kFrames; ++i) plane[i] = static_cast<float>(seq);
        published += ring.push(planes, kCh, 48000, kFrames, static_cast<int64_t>(seq), 0);
        // Two regimes, alternating: paced, where the consumer keeps up and
        // verifies thousands of packets, and bursts where it is lapped and
        // must resync. Both still far faster than real time (21 ms a packet).
        if ((seq % 2000u) < 1500u) std::this_thread::sleep_for(std::chrono::microseconds(60));
    }
    tally->producer_done.store(1);

    const int consumer_rc = child.wait();

    const uint64_t seen = tally->seen.load();
    const uint64_t torn = tally->torn.load();
    const uint64_t mism = tally->desc_mismatch.load();
    const uint64_t back = tally->went_backwards.load();
    const uint64_t resyncs = tally->resyncs.load();
    const uint64_t dropped = tally->dropped.load();
    const uint64_t skipped = published > seen ? published - seen : 0;

    std::printf("published      %" PRIu64 "\n", published);
    std::printf("consumer saw   %" PRIu64 "\n", seen);
    std::printf("skipped        %" PRIu64 "  (the consumer is meant to fall behind)\n", skipped);
    std::printf("resyncs        %" PRIu64 "  dropped by resync %" PRIu64 "\n", resyncs, dropped);
    std::printf("torn packets   %" PRIu64 "\n", torn);
    std::printf("desc mismatch  %" PRIu64 "\n", mism);
    std::printf("out of order   %" PRIu64 "\n", back);

    bool ok = true;
    if (consumer_rc != 0) { std::printf("FAIL: consumer exited %d\n", consumer_rc); ok = false; }
    if (torn != 0)  { std::printf("FAIL: torn packets\n"); ok = false; }
    if (mism != 0)  { std::printf("FAIL: descriptor disagreed with samples\n"); ok = false; }
    if (back != 0)  { std::printf("FAIL: sequence went backwards\n"); ok = false; }
    if (seen == 0)  { std::printf("FAIL: consumer saw nothing\n"); ok = false; }
    if (skipped == 0 || resyncs == 0) {
        std::printf("FAIL: consumer kept up — the lapped path was never exercised\n");
        ok = false;
    }
    if (skipped != dropped) {
        // Every packet not seen must be accounted for by a resync; anything
        // else was lost silently.
        std::printf("FAIL: %" PRIu64 " packets vanished without a resync accounting for them\n",
                    skipped > dropped ? skipped - dropped : dropped - skipped);
        ok = false;
    }
    std::printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
