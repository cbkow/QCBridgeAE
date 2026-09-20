# 2026-09-20 — A1c: reassessing one wire format for every tier

Machine: Apple M5 Max, macOS 27, Apple clang 21, arm64.

Prompted by a question with a wrong premise and a right instinct: would 16-bit
integer get 4K closer to 120 fps than 16F?

**The premise doesn't hold.** 16i and 16F are both 8 bytes per pixel. On the
wire they are indistinguishable — same bytes, same traffic, same ring cost.
Swapping one for the other moves the transport number by exactly zero, and the
~42 fps from A1b was the scalar pattern generator, never the format.

**The instinct did hold**, somewhere the question wasn't pointed: the
*conversion*. D1 had us converting every tier to half. Measuring what that
costs overturned it.

## Measurements

4K UHD RGBA, 8.3 Mpx (`tests/convert_bench.cpp`, now in the repo so the
Windows machine can re-run it):

| AE tier | Native | Converted to 16F | Penalty |
| --- | ---: | ---: | ---: |
| 8 bpc → `RGBA8Unorm` | 0.66 ms | 2.90 ms | **4.4×** |
| 16 bpc → `RGBA16Unorm` | 1.31 ms | 1.60 ms | 1.2× |
| 32 bpc → `RGBA16F` | — | 2.67 ms (NEON) | n/a |

Both integer tiers are pure `memcpy` at ~101 GB/s when carried natively — they
are memory-bandwidth bound, which is the floor.

8 bpc is the one that matters. It is **already exact in half**, so the 4.4×
was buying nothing whatsoever, and it was also doubling the wire bytes
(31.6 → 63.3 MB per 4K frame). Pure cost, both ways.

## Hardware half conversion is not optional for the 32f tier

| 32 bpc → 16F | 4K | |
| --- | ---: | ---: |
| portable scalar bit-twiddle | 12.55 ms | ~80 fps |
| NEON `vcvt_f16_f32` | 2.67 ms | ~374 fps |

4.7×. A1b reported "hand-written NEON buys nothing over scalar" — that was
measured against `__fp16`, which clang lowers to the same hardware
instruction. Against a genuinely portable implementation the gap is large.

So: the 32f path must use `vcvt_f16_f32` on ARM and F16C `_mm256_cvtps_ph` on
x86 (available since 2012). **Windows: this is yours in A5.** The integer
tiers sidestep the question by never converting.

## Why the original argument was wrong

D1 claimed per-tier formats would cost "three texture formats and three shader
paths". The first half is true and irrelevant. The second half is false:
`texture2d<float>` samples `RGBA8Unorm`, `RGBA16Unorm` and `RGBA16Float`
identically, because the texture unit normalizes integer formats in hardware.
One pipeline, one binding, verified in `tests/texture_format_test.mm`.

Per-tier costs a `pixelFormat` switch and one uniform. Not a code path.

## The 32768 correction moved, and got safer

Carrying AE 16 bpc natively means the GPU sees 0..32768 inside a 0..65535
unorm container and normalizes to ~0.5 — the image is half-bright, silently.

Rather than a consumer-side table of per-format special cases, `FrameDesc`
now carries **`value_scale`**: the producer states what it did, the consumer
multiplies unconditionally. Self-describing, and it cost nothing — it replaced
the struct's existing `_pad0`, so `sizeof(FrameDesc)` is still 184.

The test asserts both halves: that the raw sample *is* wrong, and that the
scale fixes it exactly (1.000000).

## End-to-end check

Probe captures at all three tiers, same pattern, sampled numerically rather
than eyeballed — brightness comparison by eye is exactly the proxy-checking
habit that produced the last three bugs:

| | top | mid | low |
| --- | ---: | ---: | ---: |
| 8 bpc | 84 | 122 | 159 |
| 16 bpc | 84 | 123 | 160 |
| 32 bpc | 84 | 123 | 160 |

Within 1 LSB. Without `value_scale` the 16 bpc row would read roughly half.
The 8 bpc row landing 1 lower at two points is its 256 levels, as expected.

## Open

- Does the 16 bpc precision survive *inside* QCView? Depends where it
  quantizes to 16F — same pass as the OCIO transform (fp32 registers carry it,
  only the output quantizes) or a 16F intermediate first (dies there). **A3.**
  The speed and byte wins hold regardless.
- Ring slots are now sized by `max_frame_bytes()` for the widest tier, so a
  mid-session bit-depth change needs no rebuild. The texture cache is keyed on
  format as well as geometry for the same reason. Untested against an actual
  tier change — A2, when a real AE project can switch.

## Addendum — native 32 bpc reassessed, and confirmed as stop-down

Question raised after the above: QCView not having a 32f flow *yet* is a weak
reason to quantize permanently. Would native be faster anyway?

Measured, 4K:

| 32 bpc wire | CPU produce | GPU sample | ceiling | bytes/frame |
| --- | ---: | ---: | ---: | ---: |
| `RGBA16F` (converted) | 2.40 ms | 0.40 ms | ~417 fps | 63.3 MB |
| `RGBA32Float` (native) | 2.60 ms | 0.56 ms | ~385 fps | 126.6 MB |

**Full native-32f potential at 4K is ~385 fps**, CPU-bound on the copy. Speed
is not the deciding factor — ~8% CPU and 0.16 ms GPU are noise against
anything AE produces. (Note: the earlier single reading of "2.65 vs 2.67,
statistically identical" was noise-level. Repeated across resolutions,
converting holds a consistent small edge. Same direction, smaller claim.)

Footprint is the deciding factor:

| 3-slot ring | 1080p | 4K | 6K | 8K |
| --- | ---: | ---: | ---: | ---: |
| `RGBA32Float` | 95 MB | 380 MB | 911 MB | **1519 MB** |
| `RGBA16F` | 47 MB | 190 MB | 456 MB | 759 MB |

Decision (chris): stay with 16F for the 32 bpc tier. The reasoning that
settles it is not speed and not memory alone — it is that **QCView quantizes
to 16F at ingest today**, so native would pay 2× memory to carry bits that are
discarded at the door.

`wire_format_for(tier, native_float32=true)` keeps the lossless path built and
tested, including a check that a value half cannot represent survives
bit-exact. The trigger to flip the default is written into the header: QCView
gaining a 32f pipeline.

## Where this leaves the wire

- **8 bpc — bit-exact.** Native, memcpy, no conversion.
- **16 bpc — bit-exact.** Native, all 15 bits, `value_scale` corrects the
  0..32768 container.
- **32 bpc — one conversion**, consciously chosen, reversible by a flag.

Two of three tiers went from lossy to lossless in the course of this
reassessment. The third was re-examined and confirmed rather than assumed,
which is the part that was missing the first time.
