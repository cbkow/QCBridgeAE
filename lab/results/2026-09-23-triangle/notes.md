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
