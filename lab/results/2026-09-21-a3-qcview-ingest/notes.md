# 2026-09-21 — A3: QCView ingest

QCView-Player branch `qcbae-live` (13 commits on `main` b582d25f, local,
unpushed). macOS 26.7, Apple Silicon. Plan: `PLAN-A3.md`.

## What was built (QCView side, GPL)

- **`qcbae://` live items.** `MediaType::LiveStream` with path `qcbae://ae`,
  `qcbae://premiere` (and `qcbae://probe` for `qcbae-probe produce`). Every
  existing live behaviour keys off `LiveStream` / `"://"` and carried over
  unchanged. One mapping header, `src/decode/qcbae/host_bridge_url.h`: ring
  name, item name ("QCBridge After Effects"), host app, and the Inspector's
  source facts.
- **`LiveSource` base** under the SRT decoder and the new ring reader, so
  LiveStrip / LeftRail bind once. Adds `Paused`, `statusDetail`,
  `nonFinite`, `sharedMemory`.
- **`HostBridgeSource`** — consumer of this repo's ring (protocol vendored at
  e979480 into `src/decode/qcbae/`, macOS-only until A5). States: waiting
  (no ring), producer dead (pid check — AE never unlinks), retired
  (re-open), paused (with the preference hint), live. Frames **copied** into
  a pool of reused QImages: the ring grants one reader claim and QCView
  uploads later on its render thread, so a view would be overwritten.
- **Half-float CPU upload** in both renderers (it had silently converted
  RGBA16F to 8-bit). D3D11 half is a code-along, unverified on Windows.
- **UI:** File ▸ Connect to After Effects / Premiere Pro; LiveStrip WAITING /
  PAUSED / inf-NaN badge, `Mercury Transmit · shared memory · RGBA16F`, no
  Mb/s; Inspector Source block (every claim from A4/A6 measurements).

## Verified

- **Bit-exact against the synthetic producer.** QCView logs the first
  frame's ring sequence and centre pixel; for seq 7 it read
  `34bd 35c1 2d8a 3c00`, exactly what `qcbae-probe produce` computes for that
  frame. (A 1-ULP difference against Python's rounding was the *producer's*
  truncating `to_half` — see Open.)
- **Recovery:** killing the producer shows WAITING "not running"; a new
  producer is picked up within a second, "1 restart".
- **SRT regression:** the documented HEVC Main10 sender still goes LIVE,
  VideoToolbox zero-copy, through the new base class.
- **Media matrix in the real app** (chris watching): Test Signal → video →
  image sequence → Test Signal → SRT → Test Signal → tone.wav → New Project
  while live → annotated video ↔ playlist. Every switch clean; SRT's socket
  released on switch; New Project unmaps the ring.
- **After Effects and Premiere through QCView:** confirmed by eye by chris
  ("both work wonderfully").

## Measured with the real hosts (2026-09-22)

AE 2026 scratch project (OCIO/ACEScg), QCView branch build connected via
`qcbae://ae` / File ▸ Connect to Premiere Pro.

| Check | Result |
| --- | --- |
| AE values | Static solids comp, centre (640,360) = the 50% (0.8,0.4,0.2) solid flattened over black at 8 bpc. **QCView's logged sample, the `/qcbae-ae` ring, and the expected nearest halves of 102/255, 51/255, 26/255 all equal (0.399902, 0.199951, 0.101990, 1.0).** |
| inf / NaN | White square, Exposure +20 stops with linear-light conversion bypassed, **32 bpc** project (8 bpc clamps at 1.0 before any inf can form). Device logged `has inf`; ring holds **R=G=B=inf**, not 65504; frame flagged; the rest of the frame untouched (blue now 0.099976 = half of an exact 0.1). **QCView shows the "inf/NaN in frame" badge**; the square draws white (SDR, OCIO off). |
| Paused hint | AE's "Disable video output when in the background" ticked: focus loss sent `ApplicationLostFocus` / video off → ring PausedFocus → **QCView PAUSED with the preference hint**, last frame held. |
| 4K playback | 8 bpc 4K comp, cached preview: **24.00 fps into QCView**; ring copy **2.81 ms mean / 6.7 ms max** (66 MB/frame, ~24 GB/s — twice the plan's 1.5 ms estimate); QCView process **0.39 cores** total. |
| Premiere | Straight alpha arrives and blends: QCView centre = ring = (0.799805, 0.399902, 0.199951, **0.501953**). Scrubbing arrives at fractional resolution (640×360, copy 0.2 ms). chris: "tight with timing … plays down great". |
| Stale ring, for real | QCView first opened a `/qcbae-premiere` left by the **previous day's** Premiere (pid dead — Premiere doesn't unlink on quit either), recognised it, and attached to the new Premiere the moment it published. |

Open from this: with OCIO engaged, some transforms may turn inf into NaN
(inf−inf, 0×inf in a matrix) — worth checking when QCView handles
non-finite values deliberately.

## Pre-existing QCView issues found and fixed on the branch

Stale final live frame after stop; sticky range override applied to live;
dual controls clickable during live; `file://srt://…` drags and URL drops
refused; no model guard keeping live out of B-source / playlists; New
Project leaving media running; annotation sync reading the previous item's
mode; live worker outliving the renderer on quit; **audio-only files silent
with an "unsupported" notice** (regression from QCView c8949562, confirmed
before and after).

## Open

- **Zero-copy into QCView** (texture over the ring slot) — the copy is v1.
- **Metal source-state race** (pre-existing, not live-specific): media-switch
  setters write render-thread state unlocked. Parked for a ThreadSanitizer
  pass; recorded in QCView's memory.
- NotesPanel reads a note thumbnail ~50 ms before its write lands
  (pre-existing, cosmetic).
- `qcbae-probe produce` truncates to half; it should use
  `half_convert`'s `float_to_half` so the test signal rounds like the device.
- Windows: the ring (A5) and the D3D11 upload branch.
