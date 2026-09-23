// Guard rails and the ICC region. The ICC blob is the piece that makes the
// signal interpretable (PLAN.md D5), and it changes on project settings
// rather than per frame — so it gets its own seqlock and a generation counter
// the consumer can cache against.

#include "common/surface/shared_ring.h"

#include <cstdio>
#include <cstring>
#include <string>
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
    std::printf("%-58s %s\n", what, cond ? "ok" : "FAIL");
    if (!cond) ++failures;
}
}  // namespace

int main() {
    // --- name and geometry validation ------------------------------------
    {
        SharedRing r;
        check(!r.create("no-leading-slash", 4096), "rejects name without leading slash");
        check(!r.create("/this-name-is-far-too-long-for-macos", 4096),
              "rejects name over PSHMNAMLEN (31)");
        check(!r.create("/qcbae-unit", 4096, 2), "rejects slot_count < 3");
        check(!r.create("/qcbae-unit", 4096, 300), "rejects slot_count > 255");
    }

    SharedRing producer;
    check(producer.create("/qcbae-unit", 64 * 1024), "creates a ring");
    check(producer.header()->slot_count == kDefaultSlots, "defaults to 3 slots");
    // The intra-slot offset being aligned proves nothing; what matters is the
    // ADDRESS the GPU is handed. Checking the proxy instead of the real thing
    // is how a 16-byte misalignment reached Metal in the first place.
    {
        const uint64_t ps = page_size();
        check(producer.header()->pixels_offset % ps == 0, "pixel offset within a slot is page-aligned");
        check(producer.header()->slots_offset % ps == 0, "slot region starts on a page boundary");
        bool every_slot_aligned = true;
        for (uint32_t i = 0; i < producer.header()->slot_count; ++i) {
            void* p = producer.begin_write(256);
            if (p == nullptr || reinterpret_cast<uintptr_t>(p) % ps != 0) every_slot_aligned = false;
            producer.abandon();
        }
        check(every_slot_aligned,
              "every slot's pixel ADDRESS is page-aligned (MTLBuffer bytesNoCopy)");
    }

    check(producer.begin_write(1024 * 1024) == nullptr, "rejects a frame larger than a slot");

    SharedRing consumer;
    check(consumer.open("/qcbae-unit"), "opens an existing ring");
    check(!SharedRing().open("/qcbae-nonexistent"), "fails to open a ring that isn't there");

    // --- nothing published yet -------------------------------------------
    uint64_t last_seen = 0;
    FrameDesc desc{};
    const void* pixels = nullptr;
    check(!consumer.acquire_latest(&last_seen, &desc, &pixels), "acquire on an empty ring yields nothing");

    // --- one frame --------------------------------------------------------
    {
        void* dst = producer.begin_write(256);
        check(dst != nullptr, "begin_write returns a slot");
        std::memset(dst, 0xAB, 256);
        FrameDesc d{};
        d.width = 8; d.height = 8; d.bytes_per_row = 64;
        d.pixel_format = PixelFormat::RGBA16F;
        d.source_tier  = SourceTier::Int16;
        d.time_value = 7; d.time_scale = 24;
        std::snprintf(d.comp_name, kMaxCompName, "unit");
        producer.commit(d);
    }
    check(consumer.acquire_latest(&last_seen, &desc, &pixels), "acquires the published frame");
    check(last_seen == 1, "sequence starts at 1");
    check(desc.source_tier == SourceTier::Int16, "sidecar survives the crossing");
    check(std::string(desc.comp_name) == "unit", "comp name survives the crossing");
    check(static_cast<const uint8_t*>(pixels)[255] == 0xAB, "pixels survive the crossing");
    check(!consumer.acquire_latest(&last_seen, &desc, &pixels), "no re-delivery of the same frame");
    consumer.release();

    // --- abandon must not publish ----------------------------------------
    {
        void* dst = producer.begin_write(256);
        check(dst != nullptr, "begin_write after commit");
        producer.abandon();
    }
    check(!consumer.acquire_latest(&last_seen, &desc, &pixels), "abandon() publishes nothing");

    // --- ICC region -------------------------------------------------------
    std::string blob;
    check(consumer.read_icc_profile(0, &blob) == 0, "no profile before one is set");

    const std::string fake_icc(3000, '\x7f');
    check(producer.set_icc_profile(fake_icc.data(), static_cast<uint32_t>(fake_icc.size())),
          "sets an ICC profile");
    const uint64_t gen = consumer.read_icc_profile(0, &blob);
    check(gen != 0, "consumer sees a new generation");
    check(blob == fake_icc, "ICC blob round-trips byte for byte");
    check(consumer.read_icc_profile(gen, &blob) == 0, "cached generation reads as unchanged");

    const std::string second(10, '\x01');
    producer.set_icc_profile(second.data(), static_cast<uint32_t>(second.size()));
    const uint64_t gen2 = consumer.read_icc_profile(gen, &blob);
    check(gen2 != 0 && gen2 != gen, "a replaced profile bumps the generation");
    check(blob == second, "the shorter replacement round-trips without stale tail");

    check(!producer.set_icc_profile(fake_icc.data(), 128 * 1024), "rejects a profile larger than the region");

    // --- Host state ---------------------------------------------------------
    // Read from the consumer's own mapping: the state exists to reach a
    // process that is receiving no frames, so a producer-side read proves
    // nothing about it.
    check(consumer.host_state() == HostState::Active, "a new ring reads as Active");
    producer.set_host_state(HostState::PausedFocus);
    check(consumer.host_state() == HostState::PausedFocus, "consumer sees PausedFocus");
    producer.set_host_state(HostState::Retired);
    check(consumer.host_state() == HostState::Retired, "consumer sees Retired");
    check(SharedRing().host_state() == HostState::Retired, "an unopened ring reads as Retired");

    // --- Liveness -----------------------------------------------------------
    // The consumer's only truth about a producer that quit without unlinking
    // (A5: kill(pid, 0) on POSIX, OpenProcess + WaitForSingleObject on Windows).
    check(process_alive(producer.header()->producer_pid), "this process reads as alive");
    check(!process_alive(0), "pid 0 reads as dead");
    check(!process_alive(0x7FFFFFF0u), "an absurd pid reads as dead");

    // --- Replacing a ring a consumer still holds -----------------------------
    // The producer goes away (its host quit, or reset the module) and comes
    // back under the same name while the consumer still has the old mapping
    // open. POSIX: the old name is unlinked, the new create() succeeds at
    // once, the consumer keeps reading the stale mapping until it notices the
    // pid is gone. Windows cannot unlink: the name lives while the consumer
    // holds it, so create() marks the old mapping Retired and reports the
    // held name; the consumer honours Retired, and the next create() wins.
    {
        producer = SharedRing();   // the old producer's own handle is gone
        SharedRing second;
        const bool created = second.create("/qcbae-unit", 64 * 1024);
#if defined(_WIN32)
        check(!created, "create() while a consumer holds the name reports it (Windows)");
        check(consumer.host_state() == HostState::Retired, "the held mapping was marked Retired");
        consumer = SharedRing();   // the consumer honours Retired: close and re-open
        check(second.create("/qcbae-unit", 64 * 1024), "create() succeeds once the consumer let go");
#else
        check(created, "create() replaces the name while a consumer holds the old one (POSIX)");
        consumer = SharedRing();
#endif
        check(second.valid() && consumer.open("/qcbae-unit"), "consumer re-opens the replaced ring");
        check(second.valid() && consumer.valid()
              && consumer.header()->producer_pid == second.header()->producer_pid,
              "the re-opened ring is the new producer's");
        check(consumer.host_state() == HostState::Active, "the replacement reads as Active");
    }

    std::printf("\n%s (%d failure%s)\n", failures == 0 ? "PASS" : "FAIL",
                failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
