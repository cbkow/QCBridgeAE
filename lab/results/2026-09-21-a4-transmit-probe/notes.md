# 2026-09-21 — A4: the Transmit probe, first contact (in progress)

After Effects 2026 (26.5), macOS 26.7, Apple Silicon, Premiere Pro SDK 26.0.
Probe: `src/transmit/qcbae_transmit.cpp`, installed in MediaCore, enabled by
hand in Preferences → Video Preview. Readings are `qcbae-probe dump` bytes,
never a window.

Test comp, 1280×720, 8 bpc, **Adobe CMS with no working space** (baseline
only — colour conclusions need the later conditions):

- top half: six solids at 0.15 / 0.35 / 0.60 / 0.95 / 0.75 / 0.10 (A2's set)
- bottom-left: (1, 0, 0) — orientation marker
- bottom-right: (0.8, 0.4, 0.2) at 50% opacity — alpha marker

## The device loads

Flat `BNDL` bundle, one export, `_xTransmitEntry`. AE loaded it at launch
(host interface v4), called `QueryVideoMode` once per offered mode with
`ContinueIterate`, and pushed frames. `CFBundlePackageType` was not a
problem here, unlike the AEGP's `AEgx`.

## 1. The host does not pick the project's depth

Offered, in order: argb8, argb16, argb32f, bgra8, bgra16, bgra32f — all in
`kPrWorkingColorSpace`. In an **8 bpc** project AE delivered **`bgra32f`**:
not the first offer, not the project's depth, not AE's native order.

Control — offer `argb8` alone (config change, module reset, no AE restart):
AE delivered **`argb8`**. So 8u is available; given a choice, AE prefers
BGRA 32f. "Best format" is the host's preference, not ours.

**D1's Transmit design ("offer every tier, let the host choose") does not
hold as written.** Offering everything costs an 8 bpc project the up-convert
we were trying to avoid.

## 2. Values arrive exact — at this baseline

argb8, all six greys: 38 / 89 / 153 / 242 / 191 / 26 — **identical to A2's
AEGP table**. bgra32f: the same values as exact n/255 floats (0.149020 =
38/255, 0.949020 = 242/255 …) — AE rendered at 8 bpc and converted up.
No colour conversion is visible, but this project has no working space, so
it would not show one. Not yet evidence about `kPrWorkingColorSpace`.

## 3. Frames are bottom-up, with positive rowbytes

Red, placed in the bottom half, reads at row 180; the greys read at row 540.
`GetRowBytes` returned **+20480** (bgra32f) and **+5120** (argb8), so row 0
in memory is the image's bottom row. Same in both formats. The PPix header's
"may be negative" did not describe this case: the sign is not a reliable
orientation signal here. The product must flip (walk rows from the end).

## 4. Alpha is flattened

The 50% (0.8, 0.4, 0.2) solid reads (0.4, 0.2, 0.1) with alpha **1.0** —
composited over the comp background and made opaque, in both formats
(argb8: A=255 R=102 G=51 B=26). The frames carry no transparency, so the
straight-vs-premultiplied question does not arise for AE; QCView's
alpha-0 discard never triggers. Whether it is the comp background colour or
black: not yet tested.

## 5. Focus loss deactivates the stream

`ActivateDeactivate` event 3 (`PrActivationEvent_ApplicationLostFocus`)
with video 0 each time AE went to the background; event 2
(`PlayerActivated`) with video 1 on return. With default preferences the
feed stops when the user looks at QCView — the known Route A risk, now
observed. The "Disable video output when in the background" preference is
the next thing to test.

## Also observed

- `tmInstance` said 720×480, 30 fps, for a 1280×720 24 fps comp. The pushed
  PPix carried the true size. Trust the frame, not the instance.
- Frames arrive only when the viewer re-renders (scrub mode 2, `inTime` -1 —
  "immediate"). No comp time while scrubbing; playback not yet tested.
- NeedsReset runs `Startup` for the new module **before** `Shutdown` of the
  old. A ring owned per-module could have its name unlinked by the old
  module's destructor after the new one created it. Fixed in the probe by
  owning the ring at file scope; A6 must do the same.

## 6. Under OCIO, AE ignores the colour-space request — and delivers working space anyway

Project switched by hand to **OCIO, 32 bpc**, a custom scene-linear
Rec.2020-primaries config. (`app.project.workingSpace` reads `None` under this
config — the OCIO working space is not visible to scripting here, unlike A2b's
ACES config which reported `ACEScg`.) Same comp, solids typed as before.

| Run | Offered | Colour space requested | Delivered | Values |
| --- | --- | --- | --- | --- |
| R1 | argb32f | `Working Color Space` (predefined) | argb32f | **exact as typed** |
| R2 | argb32f | unset (host default, "BT.709 full 32f") | argb32f | identical to R1 |
| R3 | argb32f_linear | working | argb32f_linear | **every value x^2.4** |
| R4 | all six | working | **bgra32f** | identical to R1 |
| R5 | argb32f | `sRGB`, both fields | argb32f | identical to R1 |
| R6 | argb32f | `BT.2020 RGB Full`, both fields | argb32f | identical to R1 |
| R7 | argb32f | `sRGB`, buffer field only | argb32f | identical to R1 |
| R8 | argb32f | `sRGB`, name field only | argb32f | identical to R1 |

"Exact as typed": 0.150000 / 0.350000 / 0.600000 / 0.950000 / 0.750000 /
0.100000, red (1, 0, 0). The red primary is the telling sample: a working →
Rec.709 conversion from Rec.2020 primaries would have produced roughly
(1.66, −0.12, −0.02).

**What it means.** Every colour-space request — including sRGB, which would
re-encode every value — leaves the pixels untouched. Either AE ignores
`outColorSpaceRec` under OCIO, or our predefined-name encoding is wrong. The
outcome is the one we want (working space, untransformed, as A2b found for
the AEGP), but it is not *because* of what we asked for, so it cannot be
relied on as a mechanism yet. Next control: the SEI-tag form (the only
encoding Adobe's sample shows) requesting PQ/Rec.2020. If that moves the
numbers, our predefined encoding is at fault; if not, AE ignores the record.

**The instrument can see a host transform.** R3's `_Linear` is exactly
x^2.4 (0.15 → 0.010535, 0.95 → 0.884172, 0.4 → 0.110903) — the project's
`workingGamma` of 2.4, not the 2.2 Adobe's guide states — applied even though
this OCIO working space is already linear. `_Linear` is a real, wrong-for-us
transform; never offer it.

**Format preference holds at 32 bpc:** offered everything, AE again chose
bgra32f (R4), matching the 8 bpc result.

**Alpha still flattened at 32f:** the 50% solid is (0.4, 0.2, 0.1, 1.0).

## Next

Formats: argb8+bgra8 only, and depth at 16/32 bpc. Colour: working space set
under Adobe CMS, then OCIO/ACEScg, plus the unset and `_Linear` controls.
Background preference. Comp background colour. Playback timing.
