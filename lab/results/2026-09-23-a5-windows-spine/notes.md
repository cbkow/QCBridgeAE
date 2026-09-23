# 2026-09-23 — A5 on Windows: the ring, liveness, F16C, and the spine's tests

Machine: AMD Ryzen Threadripper PRO 7955WX (16 cores), 256 GB DDR5-4800,
NVIDIA GeForce RTX 5090 (driver 32.0.16.1088), Windows 11 Pro 26200, MSVC
19.44 (VS 2022 17.14), CMake 3.31 (VS-bundled), Ninja. Release, `/O2`.

The first Windows day for this repo. Before it, nothing in `src/common` had
been compiled here: `shared_ring.cpp` is POSIX (`shm_open`/`mmap`) and the
unit test included `<unistd.h>`, so `cmake --build` stopped at the second
object. This folder is the port and its evidence. Commit `5057e96`.

## What was decided

**Object name.** `Local\` + the ring name without its leading slash:
`/qcbae-ae` is `Local\qcbae-ae`. Session-local, page-file backed. AE and
QCView run in the same logon session, so `Global\` (and the privilege it
needs) is not required. The 31-byte name cap is kept on Windows even though
it has no such limit, so a name that works on one platform works on the
other. Validation is the same on both.

**Geometry.** Unchanged: header padded to a page, ICC region, page-aligned
slots, `slots_offset` in the header. `page_size()` is `GetSystemInfo().
dwPageSize` (4096 here). Nothing maps at a non-zero offset, so the 64 KiB
allocation granularity never enters.

**Replace semantics — the one real difference.** POSIX `create()` unlinks a
stale name and makes a fresh one; a consumer holding the old mapping keeps
it until it notices the producer's pid is gone. Windows cannot unlink a
section: `CreateFileMapping` on a name that still exists returns the *old*
object with its old geometry and `ERROR_ALREADY_EXISTS`, and the name lives
until the last handle closes. So on Windows `create()`:

1. creates (or gets the old object) — if `ERROR_ALREADY_EXISTS`, maps the
   old header, sets `host_state = Retired`, closes;
2. retries every 25 ms for ~1.5 s (longer than the consumer's 250 ms wait
   poll) while the consumers honour Retired and close;
3. if the name is still held after that, fails with "ring name is still
   held by another process".

In practice the holder is QCView, which on a dead producer already does
`ring = SharedRing()` and re-opens, and on Retired does the same; the retry
covers it. The case that fails is a consumer that ignores Retired, which is
a consumer bug and now a loud one. **A producer that re-creates its own
ring in the same process (module reset, larger frame) is fine:** `create()`
closes its own handle first, so it is not its own blocker.

The unit test exercises exactly this sequence and it passes on both models
(the POSIX branch is `#else`; the test asserts the POSIX outcome there).

**Liveness.** New `qcbae::process_alive(pid)` in `shared_ring.h`, so the
check is vendored into QCView with the ring rather than re-implemented
there: `kill(pid, 0)` with EPERM as alive on POSIX;
`OpenProcess(SYNCHRONIZE)` + `WaitForSingleObject(h, 0)` on Windows, with
`ERROR_ACCESS_DENIED` (another user, elevated host) as alive the way EPERM
is. **Caveat, written down and not solved:** Windows recycles pids far
faster than macOS. A consumer polling every 250 ms can in principle see an
unrelated new process under the dead producer's pid and read the ring as
alive-but-silent until that process exits. Closing it would need a process
start time or a producer token in the header, which is FrameDesc v4. Not
for v1; the window is small and the symptom is a stale "connected" rather
than wrong pixels. Flagged for the Mac side.

**The tearing test without fork().** On Windows the test re-runs its own
executable with `--consumer`; the tally the two processes report into is a
second named mapping (`Local\qcbae-ringtest-tally`) instead of an anonymous
shared one. Same producer, same consumer, same checks. It is a CTest test on
Windows now (`ring_tearing`), which the `if(NOT WIN32)` used to skip.

## F16C

`convert_32f_to_rgba16f` on x86 now has the twin of the NEON pass: four host
pixels per step, two 256-bit loads, `VCVTPS2PH` each (`_mm256_cvtps_ph`,
round-to-nearest-even), one `PSHUFB` per vector for the channel order, the
same cheap per-row non-finite OR with exact classification only for a row
that trips it. Chosen at run time from CPUID (SSSE3 + OSXSAVE + AVX + F16C
and XCR0 bits 1|2); the portable path remains the fallback, and
`convert_has_hardware_path()` tells a host which it will get. MSVC needs no
`/arch` flag for the intrinsics; GCC/Clang get a `target("avx,f16c,ssse3")`
attribute on the one function.

One MSVC gotcha: `_mm256_cvtps_ph`'s immediate is the 3-bit rounding field
only, so `_MM_FROUND_NO_EXC` (bit 3) is out of range (warning C4556, and it
would have silently changed the instruction). `_MM_FROUND_TO_NEAREST_INT`
alone is the correct argument.

**Bit-exactness.** `convert_test` on x86 now uses `VCVTPS2PH` itself
(`_mm_cvtps_ph`, one lane) as the hardware reference, the way it uses FCVT
on AArch64: 1,048,532 bit patterns plus the specials, **0 mismatches**
against the portable `float_to_half` — NaN payloads, signed zeros,
subnormals (F16C produces them regardless of FTZ) and the inf boundary
included. The 40 frame cases then hold the vector path to the portable one.
84 checks, 0 failures.

## Numbers (`qcbae-convbench`, 4K UHD RGBA, 8.3 Mpx)

| | Windows (this box) | macOS (A1c, M5 Max) |
| --- | ---: | ---: |
| 8 bpc → `RGBA8Unorm` native memcpy | 1.21 ms (55 GB/s) | 0.66 ms (101 GB/s) |
| 16 bpc → `RGBA16Unorm` native memcpy | 2.47 ms | 1.31 ms |
| 32 bpc → `RGBA16F` portable scalar | 44.79 ms (22 fps) | 12.55 ms (80 fps) |
| 32 bpc → `RGBA16F` hardware (clamp only) | 4.75 ms F16C (210 fps) | 2.67 ms NEON (374 fps) |
| A6 pass, ARGB bottom-up → RGBA16F | **6.49 ms (154 fps)** | 2.40 ms (~417 fps) |
| A6 pass, BGRA bottom-up → RGBA16F | 6.26 ms (160 fps) | — |
| A6 pass, portable fallback | 73.73 ms (13.6 fps) | — |

Read across, not down:

- **The hardware path is not optional here either, and by more.** Portable
  vs F16C on the A6 pass is **11.4×** on this machine (73.7 vs 6.5 ms),
  against 4.7× on the Mac. MSVC's scalar `float_to_half` is worse than
  clang's, not better. Without F16C a 4K 32f AE viewer would top out at
  ~13 fps in the conversion alone.
- **The absolute numbers are ~2.4× the Mac's across the board, including the
  plain memcpy rows.** That is memory bandwidth, not the code: DDR5-4800 on
  a Threadripper reads and writes at ~55 GB/s single-threaded here against
  the M5 Max's ~100 GB/s unified memory. The conversion rows scale with the
  memcpy rows, so the F16C pass is bandwidth-bound too, which is the best
  it can be. The unified-memory caveat HANDOFF predicted for the GPU upload
  shows up one layer earlier, in the CPU pass.
- **154 fps at 4K for the full Transmit pass** is still 2.5× anything AE
  will push through Transmit (the Mac measured 2 frames per viewer change).
  D1 stands on Windows; no revisit needed.
- The integer-tier ratios (8 bpc converted ÷ native = 35×, 16 bpc = 18×)
  are far worse than the Mac's 4.4× and 1.2× because the bench's integer
  conversion rows are the plain scalar `to_half` loop and MSVC does not
  vectorize it. They are not the product path (the integer tiers never
  convert, D1), so they change nothing; noted so nobody reads them as new
  information.

`runs.jsonl` has every row.

## The tests, on Windows

| Test | Result |
| --- | --- |
| `ring_unit_test` | 43 checks, PASS (34 original + liveness + replace-while-held) |
| `convert_test` | 84 checks, 0 failures; fast path: hardware |
| `ring_tearing_test` | 20000 published, 12 seen, 19988 skipped, 0 torn, 0 mismatch, 0 out of order — PASS |
| `ctest` | 3/3 (`convert`, `ring_unit`, `ring_tearing`) |

The tearing consumer saw 12 of 20000 — the producer laps it far harder than
on the Mac, since the consumer's 80 µs dawdle is a smaller fraction of a
publish here. Reuse under a held claim is what the test exists to
exercise, and it did.

## QCView side (`QCView-Player` `01577fc2`)

The three vendored files re-copied from `5057e96` with the banner updated;
`HostBridgeSource::producerAlive` now calls `qcbae::process_alive`; the
decode target builds the bridge on every platform and defines
`QCV_HAS_HOST_BRIDGE` on Windows too (CoreVideo and `frame_handle.mm` stay
Apple-only). Builds, and the app starts and plays with it in. The live path
itself has nothing to read until the Transmit device exists on Windows —
see below.

## Addendum, 13:00 — the Transmit device on Windows

The Premiere Pro SDK (26.0) arrived at 13:00 and went into
`private/sdk/PremiereProSDK`. `qcbae-transmit` builds as
`QCBridgeAE-Transmit.prm` the way the SDK's Transmitter sample does
(`PRWIN_ENV`, `/MD`, the entry exported with `__declspec(dllexport)`,
no PiPL, no manifest); four portable-isms went (`/tmp` → `%TEMP%`,
`getprogname` → `GetModuleFileName`, `clock_gettime` → `steady_clock`,
the visibility attribute → `QCBAE_EXPORT`). Commit `764a696`. The module
exports `xTransmitEntry` at ordinal 1 and depends only on the VC runtime
the hosts already ship. `packaging/windows/install-transmit.ps1` copies it
into `%PROGRAMFILES%\Adobe\Common\Plug-ins.0\MediaCore\` through a
UAC prompt — the folder is not writable otherwise, which answers the
tracker's "does it need admin" (yes, for the copy; not to run).

**After Effects 2026 loads it and publishes; QCView on Windows goes live
on it.** The device log (`%TEMP%\qcbridgeae-transmit.log`):

```
--- module load, host interface v4 ---
startup in After Effects: ring /qcbae-ae, ticks/s 254016000000
instance 241: 1920x1080
instance 241: activation event 0, video 1 -> state 0
ring /qcbae-ae created, 16200 KiB per slot
published 1 (1920x1080 BGRA)
instance 241: activation event 3, video 0 -> state 1
instance 241: activation event 2, video 1 -> state 0
published 240 (1920x1080 BGRA)
```

and QCView: `ring /qcbae-ae open (producer pid …)`, then `LIVE — first
frame seq 1 1920x1080 RGBA16F row 15360 flags 0x0, centre RGBA half bits
2a44 2e36 31be 3c00`, then `240 frames 1920x1080 in 81.15 s = 2.96 fps;
ring copy mean 1.180 max 15.252 ms` (interactive scrubbing, not playback).
The named file mapping, the liveness check and the F16C pass are all on
that path: this is the first frame ever to cross the Windows ring from a
real host.

Host behaviours, re-measured rather than inherited (`2026-09-21-a4-transmit-probe`):

| | macOS | Windows |
| --- | --- | --- |
| pixel format offered 32f → picked | 32f, **ARGB** | 32f, **BGRA** (`PrPixelFormat_BGRA_4444_32f`) — AE hands Premiere's order on Windows; the device already converts both |
| focus loss | video off unless the background preference is unticked | same: activation event 3 → video 0 → PausedFocus; event 2 → video 1 on return |
| device unloaded on quit | never | *pending — AE is still up; the log will show `module unload` or not* |
| bottom-up rows, alpha flattening | measured | *pending: a person at the box confirms the picture is upright and the alpha is flat — the log cannot see pixels* |
| per-viewer-change frame count, inTime -1 | 2 frames, -1 | not instrumented in the product device; the probe (`QCBridgeAE-Transmit-Probe.prm`, also built) measures it when installed |

The VC runtime dependency (`MSVCP140`, `VCRUNTIME140`) is a packaging
note for A7: the Adobe hosts ship it, a clean machine without them may
not.

## Not done, and why

- ~~`qcbae-transmit` on Windows~~ — done in the addendum above, once the
  SDK arrived. The AE SDK 25.6 for Windows is unpacked into
  `private/sdk/AfterEffectsSDK` as well (the AEGP target stays Apple-only).
- **Host-behaviour re-measurement** — the table above; two rows wait for a
  person at the box and one for AE to quit. Premiere Pro 2026 is installed
  here and not yet tried.
- **`qcbae-probe`** — Metal; `produce`/`dump` would port with the ring,
  `view` needs D3D11. QCView is the viewer on Windows; skipped.
- The **D3D11 texture-format equivalence** check (`texture_format_test.mm`'s
  twin) — belongs with the device, when a real frame reaches a D3D11
  texture.
