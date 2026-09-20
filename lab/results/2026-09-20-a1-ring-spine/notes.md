# 2026-09-20 — A1a: the shared frame ring

Machine: 18-core Apple Silicon, macOS 27, Apple clang 21, arm64.

Built the producer/consumer spine (`src/common/surface/shared_ring.{h,cpp}`,
`src/common/protocol/frame_desc.h`) plus two tests. No AE, no QCView, no Metal
yet — the point was to make the boundary provably correct before anything
interesting sits on either side of it.

## Departure from the plan: shared memory, not IOSurface

`PLAN.md` names IOSurface with a Mach-port handoff. Implementing it surfaced a
problem the plan hadn't reckoned with: `IOSurfaceCreateMachPort` /
`IOSurfaceLookupFromMachPort` are the easy half. The hard half is the
*rendezvous* — getting that port from one process to an unrelated one. Mach
ports cannot travel over a Unix socket (`SCM_RIGHTS` carries file descriptors,
and an IOSurface has no fd form), and XPC needs a registered Mach service,
which means a LaunchAgent. A plugin living inside After Effects is in no
position to register one.

The remaining options were global surfaces by `IOSurfaceID` (deprecated, and
IDs are guessable) or a LaunchAgent (heavy, and an install-time dependency for
what is meant to be a plugin).

So: a page-aligned POSIX shared mapping instead. Rendezvous is by path, the
same shape works on Windows (`CreateFileMapping`), and it keeps the zero-copy
property that motivated IOSurface in the first place —
`MTLDevice.makeBuffer(bytesNoCopy:)` over these pages hands the GPU the exact
memory the CPU wrote, which is why the pixel region is page-aligned and the
unit test asserts it. A1b will confirm that against a real Metal texture; if it
doesn't hold, this decision has to be revisited rather than worked around.

**PLAN.md wants updating** once A1b confirms. Leaving it as written for now, so
the plan and this note disagree visibly rather than quietly.

## Two bugs worth recording

**1. Sequence did not imply slot.** The producer steps over whatever slot the
consumer is holding, so a frame's sequence number and its slot index diverge
the first time that happens. The consumer was deriving the slot arithmetically
(`(latest - 1) % slot_count`) and therefore read the wrong one.

What makes this worth writing down is the failure mode: the consumer got a
*stale but internally consistent* frame — pixels and sidecar from the same old
slot, agreeing with each other perfectly. The first version of the tearing test
compared pixels against their own sidecar and passed clean, 20000 frames, zero
errors. The bug was invisible to it.

The fix is to stop deriving: sequence and slot now travel together in one
atomic word (`pack_latest`). The test now compares both against the sequence it
was *told* it acquired, which catches it — 69 stale frames out of 169.

Lesson for A3: any check of the form "does this frame agree with itself" is
worthless. Correctness claims have to be anchored to something outside the
frame.

**2. A test race that only appeared under load.** First `ctest` run after a
build failed; every subsequent run passed. Not noise — the parent signalled
"done" by storing the final frame count, and on a loaded machine the forked
consumer was scheduled late enough to see that flag on its very first loop
check and exit having read nothing. Fixed with a rendezvous in both directions:
the consumer announces the ring is open before the producer publishes, and the
producer's completion flag is separate from progress, with a drain pass so the
last frames aren't lost to the shutdown edge.

Verified 20 consecutive clean runs, then 10 more with 12 spinners saturating
the machine.

## Results

`ring_unit` — 28 checks, all green: name/geometry validation, page alignment,
sidecar and pixels surviving the crossing, `abandon()` publishing nothing, and
the ICC region's seqlock (round-trip, generation bump, no stale tail when a
profile is replaced by a shorter one, oversize rejection).

`ring_tearing` — two processes, 20000 frames, every 8-byte word carrying its
frame's sequence, consumer deliberately dawdling 80 µs per frame so the
producer laps it ~120×:

| | |
| --- | --- |
| published | 20000 |
| consumer saw | ~160–210 |
| skipped | ~19800 (latest-wins, as intended) |
| torn frames | 0 |
| sidecar/pixel mismatch | 0 |
| out of order | 0 |

Publish cost, RGBA16F, memcpy into the slot (`runs.jsonl`):

| Frame | Size | Per publish | Ceiling |
| --- | ---: | ---: | ---: |
| 1080p | 15.8 MB | 0.24 ms | ~4150 fps |
| UHD 4K | 63.3 MB | 1.03 ms | ~967 fps |
| 6K | 151.9 MB | 2.62 ms | ~382 fps |

~64 GB/s sustained — memory-bandwidth bound, so the ring costs essentially
nothing on top of touching the pixels at all. Note this measures a memcpy from
a separate source buffer; the real plugin converts *directly into* the slot, so
one pass rather than two. Treat these as a floor, not a ceiling.

The headroom matters for a reason worth stating plainly: AE is not a realtime
source, so a ~1 ms publish at 4K was never going to be the constraint. What
these numbers actually establish is that the transport can be ruled out as a
suspect when something downstream feels slow.

## Open

- A1b: Metal probe viewer; confirm `makeBuffer(bytesNoCopy:)` over these pages
  really is zero-copy, and that a texture over it displays without a blit.
- Nothing here has been run on Windows. The spine is deliberately free of the
  Adobe SDKs so it can be — see `lab/HANDOFF-windows.md`.
- Ring lifetime on producer crash: the mapping is unlinked by the owner's
  destructor, which a crash skips. A stale segment is detected by magic and
  replaced on next create, but a consumer could sit on a dead ring. Needs a
  liveness signal; deferred to A2 when there's a real producer to kill.
