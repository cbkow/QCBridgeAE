# 2026-09-21 — A4: the Transmit probe

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

## 7. Under Adobe CMS the request matters — and only one field, one way, works

Project switched by hand to **Adobe CMS, 16 bpc, working space sRGB
IEC61966-2.1**. Same comp.

| Run | Offered | Encoding | Picked | Result |
| --- | --- | --- | --- | --- |
| R9 / R9b | argb32f | both | argb32f | **untouched** (0.149994 = 4915/32768, red 1,0,0) |
| R10 | argb32f | unset | argb32f | **converted**: 0.15 → 0.194301, red → (1.000002, 0.003617, −0.005072) |
| R11 | all six | both | bgra32f | converted (same values as R10) |
| R12 | argb16 | both | argb16 | untouched: 4915 / 11469 / 31130 / 3277, red 32768 — A2's table exactly |
| R13 | bgra32f | both | bgra32f | untouched |
| R14 | argb32f, bgra32f | both | bgra32f | converted |
| R15 | argb8, argb16, argb32f | both | argb32f | converted |
| R16 | argb32f, bgra32f | buffer | bgra32f | converted |
| R17 | argb32f, bgra32f | name | bgra32f | converted |
| R18 | argb32f | **buffer** | argb32f | **converted** |
| R19 | argb32f | **name** | argb32f | **untouched** |

**Under Adobe CMS the default is a conversion.** Unset means Adobe's
"BT.709 full range 32f", and the pixels change. D5's "always request working
space, never leave it unset" is doing real work here, not just insurance.

**AE reads `ioProfileRec.outName`** (a `PrSDKString`), not the raw
`inDestinationBuffer`: buffer-only converts even with a single mode (R18),
name-only is untouched (R19). This is the encoding the SDK never shows.

**With more than one mode, the request was lost** — whichever mode AE picked,
whatever the encoding (R11, R14–R17). Hypothesis: the probe handed the *same*
`PrSDKString` to every mode, and the host takes ownership of what it reads,
so every mode after the first carried a spent handle and AE fell back to its
default. `PrSDKTransmit.h` states that contract for the audio output names
("allocated by the plug-in and NOT be disposed by the plug-in"). The probe
now allocates a fresh string per mode and never disposes them; **to be
confirmed after the next AE restart.** If it does not fix multi-mode, the
host honours the colour space only for a single offered mode, and the device
must offer one mode.

**Format choice at 16 bpc:** offered all six, AE picked bgra32f (R11) — the
third project depth with the same answer. Offered argb16 alone, it delivered
argb16, bit-exact.

**Under OCIO (section 6) none of this showed**: there, every request was
ignored and the pixels were untouched. Under Adobe CMS the request is live.
The device has to be right for both.

## 8. Confirmed: one `PrSDKString` per mode, and the request holds

AE restarted with the fixed probe (fresh string allocated per
`QueryVideoMode` call, never disposed by the plugin). Fresh untitled project,
**Adobe CMS, 16 bpc, sRGB**; comp rebuilt by script.

| Run | Offered | Colour space | Picked | Result |
| --- | --- | --- | --- | --- |
| R14b | argb32f, bgra32f | working | bgra32f | **untouched** (was converted as R14) |
| R15b | argb8, argb16, argb32f | working | argb32f | **untouched** (was converted as R15) |
| R11b | all six | working | bgra32f | **untouched** (was converted as R11) |
| R10b | argb32f | unset | argb32f | converted — the control still moves |

The hypothesis holds: the host takes ownership of each `outName` it reads.
**Rule for A6: allocate a new `PrSDKString` for every mode, never dispose
it.**

R15b also repeats the format finding at a fourth combination: offered
8u/16u/32f in ARGB order only, a 16 bpc project gets **32f**. The host prefers
32f whatever the project depth and whatever the order; the only way observed
so far to get an integer tier is to offer it alone.

## 9. OCIO/ACES re-check, fixed build

ACES 2.0 Studio config, ACEScg working space, 16 bpc. Offered all six with
working space (R20), argb32f unset (R21), argb32f sRGB by name (R22): all
**untouched**, all identical. Under OCIO the request is ignored in both
configs tried; under Adobe CMS it is honoured (sections 7, 8).

## 10. What the host's tier choice costs — measured

Scratch project, **8 bpc**, OCIO/ACEScg. Comp 3840×2160, 24 fps, 10 s, one
solid moving across a background so every frame differs. Viewer locked to
Full resolution. AE's preview playback, looping, frames from AE's cache
(host render time 0 ms throughout). Each condition offers **one** format, so
the host must deliver it. AE CPU is cumulative CPU-seconds over a fixed 12 s
window of steady playback (`ps cputime`), divided by 12.

A first attempt was discarded: the viewer was on Auto resolution, so some
"4K" runs pushed smaller frames (one 32f run's copy time implied >300 GB/s),
and top's instantaneous CPU sampling swung 52–79% for identical conditions.

| Delivered | AE CPU, pass 1 / pass 2 (cores) | Cadence | Our copy, mean |
| --- | --- | --- | --- |
| argb8 | 0.57 / 0.57 | 24.0 fps | 0.71–0.74 ms |
| bgra8 | 0.62 / 0.57 | 24.0 fps | 0.67–0.74 ms |
| argb16 (up-converted) | 0.58 / 0.59 | 24.1 fps | 1.53–1.56 ms |
| argb32f (up-converted) | 0.59 / 0.61 | 24.0 fps | 3.0–3.1 ms |
| bgra32f (up-converted) | 0.70 / 0.60 | 24.0 fps | 3.0 ms |

(argb16 rows are labelled "16bpc" in the raw log; the switch to 16 bpc
failed — AE refuses `bitsPerChannel = 16` from script under OCIO, "Not
supported for OCIO color managed mode" — so they are an 8 bpc project
delivered as 16u.)

**The host's up-convert is lost in the noise.** Pass-to-pass spread is
±0.05 cores; the 8u → 32f difference is +0.02–0.04 cores at 24 fps — at most
~1.7 ms of CPU per 4K frame, spread across AE's threads. Cadence holds 24 fps
in every format.

**The cost is on our side, and it scales with bytes.** Our copy is a plain
memcpy in the probe: 0.7 ms (8u, 33 MB) → 1.5 ms (16u, 66 MB) → 3.0 ms
(32f, 133 MB) — ~44 GB/s throughout, bandwidth-bound as A1c found. In the
product the 32f path converts to half instead (2.67 ms, A1c), writing 66 MB.
So for an 8 bpc project, "always take 32f" costs about **+2 ms of our CPU per
4K frame and 2× the bytes** through the ring and QCView's upload, versus
native 8u. For a 16 bpc project it costs ~+1.2 ms and the same bytes.

## 11. Transparency is flattened over the comp background colour

Solids comp background set to (0, 0, 1) by script. The 50% (0.8, 0.4, 0.2)
solid now reads **(0.4, 0.2, 0.6)**, alpha 1.0 — i.e. composited over the
comp's background colour, not black. QCView receives what AE's viewer shows
with the transparency grid off. A comp with a coloured background delivers
that colour wherever it is transparent; the feed never carries alpha.

## 12. The background preference controls focus loss

With AE's default preferences, every focus loss sent `ActivateDeactivate`
event 3 (`PrActivationEvent_ApplicationLostFocus`) with video off, and frames
stopped (section 5). After unticking **Preferences → Video Preview →
"Disable video output when in the background"** (by hand):

- AE rendered a frame in front; focus moved to Finder (confirmed with
  `lsappinfo front`); three comp changes were then triggered by script with
  Finder still frontmost.
- **No deactivation event at all**, and all three renders arrived while AE
  was in the background.

So the stream survives focus loss, but only with a non-default preference.
The device cannot change it (no API seen). What it can do: it *knows* when
the host deactivates it for focus loss, so it can say so — a sidecar flag
QCView turns into "AE paused the feed: untick 'Disable video output when in
the background'". Otherwise the user sees a frozen frame with no reason.

Each viewer change pushes **two** frames ~3 ms apart (seen as ~300 fps
"bursts" of 2 frames); cheap at this size, but the product should not treat
the pair as two distinct frames to process expensively.

## 13. Premiere Pro 2026 — the same bundle, different behaviour

Same installed bundle, enabled by hand in Premiere's Settings → Playback.
Clip: a generated 1280×720 PNG, untagged, same layout as the AE comp — six
greys 38/89/153/242/191/26 on top, (255, 0, 0) bottom-left, (204, 102, 51)
at 128/255 **straight** alpha bottom-right — on a sequence made from it.
Offered argb32f, bgra32f in working space.

**Loads and pushes (D6 holds).** Module load, one instance, both modes
queried, frames pushed.

**Same as AE:** picks **bgra32f**; values untouched — greys exact n/255
(0.149020 = 38/255 …), red (1, 0, 0); rows **bottom-up** with positive
rowbytes.

**Different from AE:**
- **Alpha is carried, straight.** Bottom-right reads (0.8, 0.4, 0.2),
  **A = 0.501961** (128/255): the clip's own straight alpha, not flattened.
  Matches QCView's straight-alpha assumption. So the device cannot assume
  opaque — AE flattens, Premiere does not.
- **Real timestamps.** Parked frames arrive as `playmode_Stopped` with a true
  `inTime` (e.g. 355978022400 ticks = 1.401 s at 254016000000 ticks/s).
- **Truthful instance.** `tmInstance` reports 1280×720, 29.97 fps, non-zero
  timeline and play IDs — AE reported a 720×480 placeholder with zeros.
- **Fractional resolution while scrubbing.** Scrub frames arrive at 640×360
  (quality 1), parked frames at 1280×720 (quality 3). The probe rebuilt its
  ring on every switch — four times in a few seconds. The product must size
  the ring for full resolution and not rebuild on a size drop.
- **Focus loss deactivated video** (event 3) when the user switched away.
  Premiere's equivalent background preference: not yet checked.

## Next

A4 is done for v1's purposes. Deliberately left: AE playback timestamps
(AE preview arrives as scrubbing with no time; Premiere has real times) —
only matters for A/B, which is out of v1. Premiere's background preference.
The SEI-tag control is not needed: the colour question settled without it.
