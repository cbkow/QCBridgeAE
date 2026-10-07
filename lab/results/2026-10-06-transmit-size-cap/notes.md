# 2026-10-06 — Large comps: where the time goes, and the UHD cap

Mac: Apple M5 Max, 128 GB, macOS 26.7, After Effects 2026 (26.5).
Windows: the A5 box (Threadripper PRO 7955WX, 256 GB, RTX 5090), AE 2026.
Instruments: the device's log (push time and cadence per `published` line,
new in this release), the A4 probe (`BURST` lines: arrival interval, copy
time; a `size` key added today), and on the Mac a screen-capture loop
timer that reads the comp's own playback rate with no device at all.

Report: an 8000×8000 comp brings AE to ~0.6 fps on Windows through the
device, while a hardware Transmit device holds 24 fps on the same comp.

## 1. Our pass is not the cost

Benchmarked alone at 8000×8000 (977 MiB of 32f in, 488 MiB of 16f out):
48 ms on Windows, 18 ms on the Mac; ring creation 376 / 55 ms once. Against
1.6 s per frame observed that is 3 %. Threading the pass (row bands) gets
21–28 ms on Windows and 5–8 ms on the Mac — not pursued, see §4.

## 2. What the device asks for is the cost

`tmVideoMode.outWidth/outHeight = 0` asks for the comp's own size; the host
then builds a full-size 32f frame for every push, inside its playback loop.
Asking for a size that fits inside 3840×2160 (aspect kept) makes the host
scale first. Measured with the real device, same comp, 16 bpc OCIO:

| | Mac | Windows (a 16 bpc 8000×8000 comp) |
| --- | ---: | ---: |
| 0.2.1, full size | 12.6–15 fps, 18 ms/push | ~0.6–1 fps, 430 ms first push |
| capped to 2160×2160 | 24.0 fps, 1.8 ms/push | 12.0 fps, 5 ms/push |

**The instance is not the frame.** `tmInstance` said 400×400 while the host
pushed 8000×8000 through it (and 6–10 % of logged frames disagree with their
instance). The cap is learned from the first frame over the limit, and the
host is made to ask again with `NeedsReset`, which re-queries modes at once,
mid-session, and honours a non-standard raster such as 2160×2160.

**It must not stick.** Comps rarely share an aspect, so a kept raster would
letterbox or stretch the next one. The request is dropped whenever a new
instance goes live after frames have flowed (instances the host re-creates
after a reset go live *before* the new module's first frame; focus return
re-activates an instance already seen; neither counts). Verified on the Mac:
8000² → 2160² at 24 fps; a 1920×1080 comp arrives 1920×1080; back to 8000²
re-caps; 6720×3780 → 3840×2160. A switch costs one full-size frame and two
or three resets, inside a second on the Mac.

## 3. The cap's size does not matter, nor the format, nor latency

Probe, same comp, request varied while it looped:

| request | Mac 16 bpc (Adobe CMS) | Mac 32 bpc | Windows 16 bpc |
| --- | ---: | ---: | ---: |
| 2160×2160 32f | 19.1 | 10.6 | 12.06 |
| 1440×1440 32f | 19.5 | 10.6 | 12.03 |
| 1080×1080 32f | 18.7 | 11.0 | 12.07 |
| 64×64 | 19.6 | 10.4 | 12.02 |
| full size 32f | 12.6 (probe copy 60 ms) | 7–8 | — |

Windows sits at exactly two frame periods (83 ms interval) even for a 64×64
frame the probe does nothing with: AE's own work per frame with a device
attached is over 42 ms there, so no cap size and no conversion speed can
reach 24. Earlier Windows runs: bgra32f, argb32f, argb8, bgra8 at 2160²
all 3–5.5 fps on the same comp — the pixel format is not a lever either.
Latency 1 or 5 frames: no change at either size; every frame AE pushes is
`playmode_Scrubbing` with `inTime` −1. The Mac 16 bpc OCIO figure for the
cap was 24.0 (§2); under Adobe CMS the same comp gave 19 — loop length also
differed, so that is a hint about the colour-management mode, not a finding.

## 4. 32 bpc is the host's

Mac, 8000×8000, 24-frame cached loop, 32 bpc: with **no** Transmit device
the comp plays at 23.95 fps (screen timer); with the probe asking for 64×64
8-bit, 9.3 fps; capped 32f, 8.5–9.6; full size, 7–8. AE spends ~65 ms a
frame on any Transmit device in a float project, whatever it asks for. The
hardware device chris compared against was measured in a 16 bpc project and
asks for integer YUV; we are not chasing that.

## Decisions

- One cap, 3840×2160 fit, no setting: smaller buys nothing (§3) and costs
  QC pixels. No 8/16/32 output setting: the format buys nothing (§3) and a
  wrong setting would clamp a float project.
- The device logs push time and cadence on every `published` line, and the
  probe keeps the `size` key for the next question of this kind.
- Windows at 12 fps and 32 bpc at ~9 fps are AE's; to be said in the README.
- Threading the pass and dropping the ring memset: not worth a change at
  the capped sizes (1.8–5 ms, 9–50 ms once).

## Traps met today

- `tmInstance` dimensions (above). The first cap build sized from them and
  asked for nothing.
- AE remembers the last project's settings for new projects: a scripted
  32 bpc scratch project made new projects 32 bpc until set back.
  `app.project.colorManagementSystem = 0` (Adobe CMS) allows
  `bitsPerChannel = 16`, which OCIO mode refuses from script.
- `Play Current Preview` toggles; `comp.workAreaDuration` does not shorten
  the preview loop, `comp.duration` does.
- A 240-frame log cadence is one line every four minutes at 1 fps: the
  device now also writes a line after ten seconds without one.
