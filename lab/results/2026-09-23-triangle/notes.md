# The three together — QCView's side of leg 1 (2026-09-23)

The sync and stream numbers are in QCBridge's
`spikes/parity/results/2026-09-23-triangle/notes.md`. This file is what
QCView did on each machine, driven from the Mac seat (`lab/ONE-SEAT.md`).

- **QCView on Windows** (Release build of the morning's `main`, launched
  on the desktop through the lab runner with the replica's `srt://` URL):
  live stream added, `LiveStreamDecoder: D3D11VA on the shared renderer
  device (zero-copy)`, `connected — hevc 3840x2160 pix_fmt=yuv420p10le`,
  `LIVE — first frame published (d3d11va zero-copy, 3840x2160)`, about five
  seconds after launch. Rebuilt afterwards to the afternoon's `main`
  (2.4.0); the first link failed only because the viewer was still
  running.
- **QCView 2.4.0 on the Mac**, same URL across the VPN: refused with an
  I/O error every six seconds while the Windows viewer held the stream;
  connected and `LIVE (videotoolbox zero-copy, 3840x2160)` within one
  retry once it closed. One viewer per listener is QCBridge's design.
- A GDI screenshot of the Windows desktop shows QCView's chrome with the
  live item but not the picture: the D3D11 viewport composites through
  DirectComposition. The log lines are the pixel evidence on Windows; on
  the Mac the picture was seen directly.

Still owed on this side: the AE leg with the stream up (leg 2), the
glass-to-glass numbers, and the by-hand list in `RUNSHEET-triangle.md` §6.

## Leg 2 — AE into QCView on Windows while the Blender stream is up (2026-09-24)

The triangle: the Mac host driving the Windows replica (leg 1 running),
After Effects 2026 on the Windows box previewing a comp with a moving
element, QCView on Windows opened in dual view with the replica's
`srt://` on A and `qcbae://ae` on B (`--dual-test … --sbs`, driven from
the Mac seat).

| side | log |
|---|---|
| A, Blender stream | `LiveStreamDecoder: connected — hevc 3840x2160 yuv420p10le`, `LIVE (d3d11va zero-copy)`; `DualLiveSource: D3D11VA live frames brought to the CPU for dual (RGBA64)` |
| B, AE Transmit ring | `HostBridgeSource: LIVE — first frame seq 885 1920x1080 RGBA16F`; then `240 frames in 10.13 s = 23.68 fps; ring copy mean 1.03 max 8.0 ms`, `22.93 fps; mean 0.95 ms` |

Both live at once; AE kept previewing throughout (the device's own log:
`published 720 (1920x1080 BGRA)` before QCView attached).

Two things learned driving AE from a script:

- **`AfterFX.exe -r script.jsx` exits when the script's work ends**, the
  preview included — AE went away seconds to minutes after the preview
  stopped, three times. A second `-r` while an instance runs started a
  *second* instance rather than handing the script over. A plain launch
  lives on; so the comp-and-play script went into AE's `Scripts/Startup`
  for the run (removed straight after) and AE was started with no
  arguments.
- QCView's dev entry `--dual-test A B --sbs` had lost its sources since
  the empty-dual change on the 23rd: the mode switch rebuilt the island
  from the empty project (`B=ae (0x0, 0 frames)`, then both closed).
  Guarded in QCView `ebb7b614`: an island that already runs only gets the
  mode pushed.

Still owed: glass-to-glass for both paths (Phase 4 item 3), the wipe and
difference modes on the live pair by hand, and the D3D11 pixel-level look
at the AE side's alpha (the A5 notes' open row).

