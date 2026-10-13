# Notes on live streams, B2: LIVE / REVIEW (2026-10-13, early morning)

The second step of the stream-notes plan. B1 stored notes for a stream in
the project and minted their identity from the capture stamp; B2 makes a
note's frame something the viewport can show again, and gives the feed a
freeze so a note can be drawn on a still picture. QCView 2.5.6 dev, macOS.

## What was built (QCView cfef02aa)

- **Review state** on WindowManager: `liveReview` (bool) and `reviewLabel`
  ("HH:mm:ss" of the capture), `enterReview(key)`, `freezeLive()`,
  `exitReview()`. Review holds either a stream note (its key) or a
  transient freeze (no note yet; the key is minted at the freeze and the
  first note takes it, together with the frozen pixels as its clean PNG).
- **Publish gate** on both live sources (`LiveSource::setPublishEnabled`):
  frames are still taken from the ring / decoded, nothing stalls upstream,
  but none reaches the sink while Review is on. The Review picture goes
  into the single view's publish slot like any external frame.
- **View-transform bypass** on both renderers
  (`IPlayerRenderer::setViewTransformBypass`): the single draw, the
  capture, the output pass and the scope tap treat OCIO as disengaged while
  it is set, because the picture is QCView's own display-referred capture.
  The compositors encode that picture for a linear or PQ swapchain the way
  they encode their fill (Metal `setLinearizeSource`, D3D11 `encodeSource`
  in the constant buffer), the gap the plan review had found.
- **Input.** Space on a live item freezes the feed, or in Review returns
  to live (the guard added yesterday sits in the same place). Escape
  returns to live, after the stroke and selection rungs. Clicking a note
  card enters Review of that note; ← / → walk the notes in capture order
  (from a transient freeze, ← goes to the latest note). "Add note here"
  and switching the annotation tools on freeze the feed first.
- **Transitions.** `stopLiveStream()` and `setCompositorMode()` exit
  Review, so an item switch, a mode change, closing the media and a
  project replace all return the feed to live and clear the bypass.
- **UI.** The Notes panel gets a LIVE / REVIEW pill above "Add note here"
  (red dot LIVE, amber dot "REVIEW · HH:mm:ss"; click = freeze / back to
  live), and the Add tile reads "freezes the feed" or "on the frozen
  frame". The LiveStrip's state chip reads "REVIEW · HH:mm:ss" with a
  steady dot.

## Method

The B1 rig: an ffmpeg SRT listener looping the 25p burn-in clip on port
9100, a saved project with the stream item and two notes from B1, the dev
build driven by `QCV_QML_EVAL` with `QCV_CAPTURE_PNG` for one capture per
run, logs under `QCV_LOG_DIR`. Pixel comparisons decode both PNGs with
ffmpeg to raw RGB and compare byte for byte. The output check streams the
viewport over SRT to a local listener (`QCV_STREAM_SPIKE`) and measures
the consecutive-frame difference of the received video per second
(`tblend=difference,signalstats` YAVG).

## Results

| check | result |
|---|---|
| freeze → note | `freezeLive()` logs the frozen stamp; `addNoteAtCurrentFrame()` creates the note under that stamp and writes its clean PNG from the frozen pixels |
| capture in Review vs the note's PNG | 1280x720, **max channel difference 0** on every byte (fresh freeze) |
| Review of an old note vs its PNG | **max channel difference 0** on every byte (the PNG → slot → viewport → capture path is lossless) |
| exit | "back to live", the renderer logs the transform restored; the next decoded frame replaces the picture |
| ← / → | from note 1: → note 2 → note 3 → no-op at the end |
| Space, Space | back to live, then a new freeze |
| mode change in Review | `compositorMode = 1` exits Review first, then enters dual |
| close media in Review | exits through `stopLiveStream()` |
| outputs follow | SRT stream of the viewport: consecutive-frame difference 2.1–3.1 while live, **0.001 for the 6 s between freeze and exit**, 2.3–3.0 after |
| quit | clean (texture pools empty); the QML "property of null" warnings at quit are the same pre-existing teardown noise every binding prints |

## Not verified here

- The Premiere / After Effects feed (`HostBridgeSource` gate) — the same
  shape as the SRT decoder's gate, but it needs the bridge on the Mac or
  the box.
- The EDR / PQ encode of the Review picture (`setLinearizeSource`,
  `encodeSource`): the capture chain is SDR, so this needs eyes on an EDR
  display, or the box with an HDR swapchain.
- Windows / D3D11: written to the same shape, builds untested here; a
  tracker item in `lab/TRACKING-windows.md`.
- Escape itself (a key event, not driven headless); the handler is the
  one-line rung after the selection rung.

## Next

B3 (stream-aware exports, "Open in minNotes") follows A2 (the Windows
stream encoder) in the plan's order.
