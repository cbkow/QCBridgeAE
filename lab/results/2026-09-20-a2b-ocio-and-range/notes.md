# 2026-09-20 — A2b: HDR range, OCIO mode, and what the sidecar can honestly claim

After Effects 2026 (26.5), macOS 27, Apple Silicon. Follows
`2026-09-20-a2-aegp-tap`.

Two questions, both answered by measurement: does the tap clamp HDR, and what
happens to the working space when AE's colour management is switched to OCIO.

## Over-range survives — nothing clamps at 1.0

A white solid with AE's Exposure effect, read back through the ring at 32 bpc:

| Exposure | Linear multiplier | Received |
| --- | ---: | ---: |
| +2 stops | ×4 | 1.78125 |
| +6 stops | ×64 | 5.65625 |
| +20 stops | ×1,048,576 | 322.5 |
| +3 stops, linear-light conversion **bypassed** | ×8 | **8.00000** |

Unclamped, straight through. The half ceiling (65504, PLAN.md D4) was never
reached even at +20 stops, so that clamp remains untested against real data.

**A confound worth recording.** The first three rows are not `×multiplier` —
they are `multiplier^(1/2.4)`. I briefly read that as proof the render was
stuck in a gamma-2.4 space and concluded the tap was ignoring OCIO. Wrong:
AE's Exposure effect does its own linear-light conversion using
`app.project.workingGamma`, which still reports 2.4 **even under OCIO**.
Bypassing that conversion gives exactly 8.0 and the apparent gamma vanishes.

The probe was measuring the effect, not the working space. When a measurement
implies something structural, check whether the instrument is in the path.

## Range is a property of the container, not the colour space

Worth stating plainly because it came up as "does working space assume 0–255":
it does not, and the question resolves one level down. A 32f buffer has nothing
to clamp into; an 8u or 16u buffer is bounded by construction whatever the
colour space is called.

Supporting evidence from AE's own Transmit layer, which names its colour spaces
explicitly (`MediaFoundation`):

```
WorkingColorSpace        = Working Color Space
Overranged709 / Overranged2020 / Overranged2020Linear
Overranged2100PQ / Overranged2100HLG / Overranged2100PQScene
Rec709RGBScene / Rec2020RGBScene / Rec2100HLGRGBScene
Rec601525 / Rec601625 / sRGB / DCDMXYZ
```

Adobe needed distinct identifiers for "Rec 709 **but values may exceed 1.0**".
You only need that concept if the plain variants are bounded — so bounding is
real and explicit, and the unbounded path is a named first-class thing rather
than an accident. The descriptor table in the same binary lists
`Full,RGB,32f,Scene-Referred` as a supported combination.

## Under OCIO, AE's reported working space is wrong

Project switched (by hand, in Project Settings) to:

```
colorManagementSystem = 1                       (OCIO)
ocioConfigurationFile = ACES 2.0 Studio v4.0
workingSpace          = ACEScg
```

`AEGP_ColorSettingsSuite6 → AEGP_GetNewWorkingSpaceColorProfile` still returns
a profile whose description reads **"Rec.709 Gamma 2.4"**.

That is not merely uninformative, it is misleading. Had we built the original
D5 plan — auto-configuring QCView's OCIO input node from this profile — an
ACEScg project would have been silently interpreted as Rec.709. Bit-perfect
numbers, wrong picture, nothing visibly broken.

Chris had already ruled that design out on other grounds (QCView's OCIO is more
reliable than any ICC round-trip, and the user should choose explicitly). This
is independent confirmation that it was the right call.

**D5 revised**: the sidecar carries *container* facts, not colour semantics.
`channel_order` and `value_scale` stay essential — mechanical, always true, and
the image is obviously broken without them. The ICC drops to an optional hint
that must not be presented as authoritative, and under OCIO it appears not to
track the working space at all, so it is not dependable for drift detection
either.

## Fixed along the way: the profile was published once and never again

It was written only at ring creation, so switching colour management mid-session
left the sidecar describing a space the project no longer used. Now re-checked
on a 1 s interval and republished only when the blob actually changes, bumping
the generation. Cheap, and it is the mechanism a consumer needs to notice drift
— in Adobe CMS mode, where the profile is trustworthy.

## Imported media is a separate variable

A tagged sRGB pure-red PNG imported into the ACEScg project arrives as
`R=1.45117 G=0.0 B=0.00832` — converted by AE's own Interpret Footage on the
way in, and not by any transform we apply. Not the ACEScg conversion of sRGB
red (≈0.61/0.07/0.02) either; the exact path was not identified and was not
worth identifying, because it is AE's business, not the pipe's.

The practical answer (chris): set **Preserve RGB** in Interpret Footage when
the media should not be converted. A manual step, and the right one — it keeps
the decision with the person who knows what the footage is.

## What this changes

Nothing about the transport. The pipe's requirement is fidelity, and fidelity
is measured: bit-exact at 8 and 16 bpc, nearest-half at 32, over-range intact.
What AE *reports* about its own colour setup turned out not to matter, because
QCView decides the transform.
