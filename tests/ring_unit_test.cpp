// Guard rails and the ICC region. The ICC blob is the piece that makes the
// signal interpretable (PLAN.md D5), and it changes on project settings
// rather than per frame — so it gets its own seqlock and a generation counter
// the consumer can cache against.

#include "common/surface/shared_ring.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>

using namespace qcbae;

namespace {
int failures = 0;
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
    check(producer.header()->pixels_offset % static_cast<uint64_t>(getpagesize()) == 0,
          "pixel region is page-aligned (MTLBuffer bytesNoCopy needs it)");

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

    std::printf("\n%s (%d failure%s)\n", failures == 0 ? "PASS" : "FAIL",
                failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
