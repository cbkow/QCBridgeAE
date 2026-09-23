// A1 exit criteria: frames cross the boundary and slots recycle without
// tearing. Two real processes, not two threads — the whole point is the
// cross-process mapping, and a thread test would prove nothing about it.
//
// Every 8-byte word of a frame's pixel payload carries that frame's sequence
// number, and the sidecar's time_value carries it too. So a torn read is
// detectable three ways: words disagreeing with each other, words disagreeing
// with the sidecar, or a sequence going backwards.
//
// The consumer deliberately dawdles so the producer laps it. That is the
// interesting case: it forces slot reuse while a reader holds one, which is
// exactly what reader_claim exists to survive.
//
// The second process is a fork() on POSIX. Windows has no fork, so there the
// test re-runs its own executable with `--consumer` (A5); the tally the two
// report into is a second named mapping instead of an anonymous shared one.
// The producer/consumer code is the same on both.

#include "common/surface/shared_ring.h"

#include <atomic>
#include <chrono>
#include <new>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

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

constexpr const char* kName   = "/qcbae-ringtest";
constexpr uint64_t kWords     = 8192;              // 64 KiB of payload
constexpr uint64_t kBytes     = kWords * sizeof(uint64_t);
constexpr uint64_t kFrames    = 20000;

struct Tally {
    std::atomic<uint64_t> seen{0};
    std::atomic<uint64_t> torn{0};
    std::atomic<uint64_t> desc_mismatch{0};
    std::atomic<uint64_t> went_backwards{0};
    // Rendezvous, both directions. The consumer announces it has the ring
    // open before the producer publishes anything, and the producer announces
    // it has finished so the consumer knows to drain and stop. Without the
    // first of these the test is flaky exactly when the machine is busy — the
    // child gets scheduled after the producer has already finished, sees the
    // done flag on its first loop check, and exits having seen nothing.
    std::atomic<uint32_t> consumer_ready{0};
    std::atomic<uint32_t> producer_done{0};
};

int run_consumer(Tally* tally) {
    SharedRing ring;
    for (int attempt = 0; attempt < 2000 && !ring.open(kName); ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!ring.valid()) { std::fprintf(stderr, "consumer: %s\n", ring.error().c_str()); return 2; }

    uint64_t last_seen = 0, previous = 0;
    FrameDesc desc{};
    const void* pixels = nullptr;

    tally->consumer_ready.store(1);

    // Keep going until the producer is done AND one final sweep finds nothing
    // new, so the last published frames are not missed by the shutdown edge.
    bool draining = false;
    while (true) {
        if (!ring.acquire_latest(&last_seen, &desc, &pixels)) {
            if (draining) break;
            if (tally->producer_done.load()) draining = true;
            std::this_thread::yield();
            continue;
        }
        draining = false;
        const auto* words = static_cast<const uint64_t*>(pixels);
        const uint64_t first = words[0];
        bool torn = false;
        for (uint64_t i = 1; i < kWords; ++i) {
            if (words[i] != first) { torn = true; break; }
        }
        // Hold the slot a beat so the producer genuinely laps us.
        std::this_thread::sleep_for(std::chrono::microseconds(80));
        // Re-check after the dawdle: if reader_claim were not honoured, the
        // producer would have overwritten these pixels underneath us.
        for (uint64_t i = 0; i < kWords && !torn; i += 512) {
            if (words[i] != first) torn = true;
        }

        if (torn) tally->torn.fetch_add(1);
        // Compare against the sequence we were TOLD we acquired, not merely
        // pixels against sidecar. Those two agree even when the slot lookup
        // hands back a stale frame, which is precisely the bug this catches.
        if (first != last_seen || static_cast<uint64_t>(desc.time_value) != last_seen) {
            tally->desc_mismatch.fetch_add(1);
        }
        if (last_seen < previous) tally->went_backwards.fetch_add(1);
        previous = last_seen;
        tally->seen.fetch_add(1);
        ring.release();
    }
    return 0;
}

// --- the second process, per platform ---------------------------------------

#if defined(_WIN32)
constexpr const wchar_t* kTallyName = L"Local\\qcbae-ringtest-tally";

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
    // Tally lives in its own shared mapping so both processes report into it.
    void* shared = ::mmap(nullptr, sizeof(Tally), PROT_READ | PROT_WRITE,
                          MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (shared == MAP_FAILED) { std::perror("mmap tally"); return 1; }
    auto* tally = new (shared) Tally();
#endif

    SharedRing ring;
    if (!ring.create(kName, kBytes, 3)) {
        std::fprintf(stderr, "producer: %s\n", ring.error().c_str());
        return 1;
    }

    Child child;
#if !defined(_WIN32)
    child.tally = tally;
#endif
    if (!child.start(argv[0])) return 1;

    // Wait for the consumer to have the ring open before publishing.
    for (int i = 0; i < 5000 && tally->consumer_ready.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (tally->consumer_ready.load() == 0) {
        std::fprintf(stderr, "consumer never became ready\n");
        child.kill();
        return 1;
    }

    uint64_t published = 0;
    for (uint64_t seq = 1; seq <= kFrames; ++seq) {
        void* dst = ring.begin_write(kBytes);
        if (dst == nullptr) { std::fprintf(stderr, "begin_write failed: %s\n", ring.error().c_str()); break; }
        auto* words = static_cast<uint64_t*>(dst);
        for (uint64_t i = 0; i < kWords; ++i) words[i] = seq;

        FrameDesc desc{};
        desc.width = 128; desc.height = 64; desc.bytes_per_row = 128 * 8;
        desc.pixel_format = PixelFormat::RGBA16F;
        desc.source_tier  = SourceTier::Float32;
        desc.flags        = kFlagPremultiplied;
        desc.time_value   = static_cast<int64_t>(seq);
        desc.time_scale   = 24;
        std::snprintf(desc.comp_name, kMaxCompName, "synthetic");
        ring.commit(desc);
        ++published;
    }
    tally->producer_done.store(1);

    const int consumer_rc = child.wait();

    const uint64_t seen = tally->seen.load();
    const uint64_t torn = tally->torn.load();
    const uint64_t mism = tally->desc_mismatch.load();
    const uint64_t back = tally->went_backwards.load();
    const uint64_t skipped = published > seen ? published - seen : 0;

    std::printf("published      %" PRIu64 "\n", published);
    std::printf("consumer saw   %" PRIu64 "\n", seen);
    std::printf("skipped        %" PRIu64 "  (latest-wins; the consumer is meant to fall behind)\n", skipped);
    std::printf("torn frames    %" PRIu64 "\n", torn);
    std::printf("desc mismatch  %" PRIu64 "\n", mism);
    std::printf("out of order   %" PRIu64 "\n", back);

    bool ok = true;
    if (consumer_rc != 0) { std::printf("FAIL: consumer exited %d\n", consumer_rc); ok = false; }
    if (torn != 0)  { std::printf("FAIL: torn frames\n"); ok = false; }
    if (mism != 0)  { std::printf("FAIL: sidecar disagreed with pixels\n"); ok = false; }
    if (back != 0)  { std::printf("FAIL: sequence went backwards\n"); ok = false; }
    if (seen == 0)  { std::printf("FAIL: consumer saw nothing\n"); ok = false; }
    if (skipped == 0) {
        std::printf("FAIL: consumer kept up — the reuse path was never exercised\n");
        ok = false;
    }
    std::printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
