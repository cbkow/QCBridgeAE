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

## Pre-existing QCView issues found and fixed on the branch

Stale final live frame after stop; sticky range override applied to live;
dual controls clickable during live; `file://srt://…` drags and URL drops
refused; no model guard keeping live out of B-source / playlists; New
Project leaving media running; annotation sync reading the previous item's
mode; live worker outliving the renderer on quit; **audio-only files silent
with an "unsupported" notice** (regression from QCView c8949562, confirmed
before and after).

## Open

- **Measured checks with the real hosts** (only visual so far): ring-vs-
  QCView centre-sample comparison for AE and Premiere; the Paused hint with
  AE's background preference ticked; the inf/NaN badge from a comp that
  produces one; 4K AE playback through QCView (fps, reader copy cost).
- **Zero-copy into QCView** (texture over the ring slot) — the copy is v1.
- **Metal source-state race** (pre-existing, not live-specific): media-switch
  setters write render-thread state unlocked. Parked for a ThreadSanitizer
  pass; recorded in QCView's memory.
- NotesPanel reads a note thumbnail ~50 ms before its write lands
  (pre-existing, cosmetic).
- `qcbae-probe produce` truncates to half; it should use
  `half_convert`'s `float_to_half` so the test signal rounds like the device.
- Windows: the ring (A5) and the D3D11 upload branch.
