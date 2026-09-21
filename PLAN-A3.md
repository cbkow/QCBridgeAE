# QCView: After Effects / Premiere live sources (QCBridgeAE phase A3)

## Context

QCBridgeAE's Transmit device (A6, done) streams After Effects and Premiere
frames — top-down RGBA16F in the working colour space, straight alpha, inf/NaN
flagged, never clamped — into shared-memory rings `/qcbae-ae` and
`/qcbae-premiere` (`QCBridgeAE/src/common/surface/shared_ring.h`,
`protocol/frame_desc.h` v3; host state Active / PausedFocus / Paused / Retired
in the ring header; liveness = producer_pid alive). A3 makes those rings a live
source in QCView, single view only (A/B is out of v1; OCIO interpretation is
the user's, by hand).

Decided with chris (2026-09-21):
- **Model:** `MediaType::LiveStream` with a new scheme — `qcbae://ae`,
  `qcbae://premiere` (+ dev-only `qcbae://probe`). Every existing live
  behaviour keys off `LiveStream` / `"://"` and carries over.
- **Entry:** menu actions *Connect to After Effects* / *Connect to Premiere
  Pro* add the fixed item on demand (deduped) and activate it; it waits until
  the ring appears.
- **Scope:** also fix every pre-existing issue the exploration found.

Work happens on a new branch `qcbae-live` in QCView-Player (GPL); chris
pushes. Build: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
-DCMAKE_PREFIX_PATH=/Users/chris/Qt/6.11.1/macos
-DCMAKE_OSX_SYSROOT=/Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk`.

## Design

### 1. Protocol headers, vendored
Copy `shared_ring.{h,cpp}` + `frame_desc.h` from QCBridgeAE (MIT → GPL is
compatible) into `src/live/qcbae/`, with a header noting source commit and
"keep in sync; kFrameDescVersion guards mismatch" (`SharedRing::open` already
rejects a version ≠ 3). Only the consumer half is used.

### 2. A common live-source base (fixes the concrete-type coupling)
New `src/decode/live_source.h`: `class LiveSource : public QObject` carrying
exactly the surface QML reads today (from `live_stream_decoder.h`): `status`
(Q_ENUM, values unchanged 0–3 **plus `Paused = 4`**), `width`, `height`,
`codecName`, `pixelFormatName`, `hasAudio`, `reconnectCount`, `url`; the
invokables `statFramesReceived`, `statBytesReceived`, `statLiveSeconds`,
`statFramesConflated`; signals `statusChanged`, `metadataChanged`; virtual
`setSink`, `setFrameCallback`, `open`, `close`. Add a `statusDetail()` string
property (e.g. "Waiting for After Effects", "Paused: host lost focus —
untick 'Disable video output when in the background'") and a
`nonFinite` property (frame carried inf/NaN).
`LiveStreamDecoder` derives from it with no behaviour change.
`WindowManager`'s `liveDecoder` Q_PROPERTY / member are retyped to
`LiveSource*` / `std::unique_ptr<LiveSource>` (`window_manager.h:287,672,1647`).

### 3. `HostBridgeSource : LiveSource` (new, `src/decode/host_bridge_source.{h,cpp}`)
Worker thread, ~2 ms poll (no cross-process wakeup exists; cheap at this
rate — measure CPU), per iteration:
- No ring / `open()` fails → status Connecting, detail "Waiting for <host>".
- `producer_pid` dead (`kill(pid,0)`, EPERM = alive) or `host_state ==
  Retired` → close mapping, status Reconnecting, re-open by name.
- `host_state` PausedFocus/Paused → status Paused + detail; keep last frame.
- `acquire_latest` → validate FrameDesc (RGBA16F, RGBA order, bytes_per_row ≥
  w*8) → **copy** into a `QImage(w,h,Format_RGBA16FPx4)` (row by row honouring
  `bytes_per_row`) → `release()` → `sink->publishExternalFrame(
  FrameHandle::cpu(img, pts), pts)` → frame callback. Set `nonFinite` from
  `kFlagHasInf|kFlagHasNaN`; `width/height`, `codecName` = "Transmit
  (After Effects|Premiere Pro)", `pixelFormatName` = "RGBA16F".
- Why copy, not `cpuShared`: the ring grants one reader claim; the renderer
  uploads the handle later on its own thread, so a zero-copy view would be
  overwritten once the reader moves on. Copy costs ~1.5 ms/4K frame on the
  reader thread. True zero-copy (Metal texture over the slot, claim held by
  the renderer) is a follow-up once measured.
- Stats: frames, bytes (LiveStrip's Mb/s becomes MB/s-ish; see §6),
  live-seconds.

### 4. Routing and naming
- `WindowManager::startLiveStream` (`window_manager.cpp:3891`): construct
  `HostBridgeSource` when `path` starts with `qcbae://`, else
  `LiveStreamDecoder`. Map `qcbae://ae|premiere|probe` → ring name via a
  single helper (only place the mapping lives).
- `ProjectManager::addLiveStream` (`project_manager.cpp:687`): default name
  "After Effects" / "Premiere Pro" / "QCBridgeAE probe" for `qcbae://` hosts
  (QUrl host would give "ae").
- New `Q_INVOKABLE WindowManager::connectHostBridge(QString host)` →
  `addLiveStream("qcbae://"+host, name)` + `setActiveItem`. File menu (Main.qml
  ~`:403`, beside *Open Stream…*): *Connect to After Effects*, *Connect to
  Premiere Pro*. Deep link `qcview://stream?url=qcbae://ae` and CLI already
  route via `"://"`.

### 5. Renderer: 16F CPU frames (both platforms)
- Metal `uploadCpuFrameRgba` (`render/metal/metal_player_renderer.mm:70-110`):
  `Format_RGBA16FPx4` → `MTLPixelFormatRGBA16Float` (today silently converted
  to 8-bit).
- D3D11 CPU slot (`render/d3d11/d3d11_player_renderer.cpp:~949-955`): →
  `DXGI_FORMAT_R16G16B16A16_FLOAT`.
- Per QCView memory (verify each media path): grep the siblings
  (`Format_RGBA16FPx4`, `uploadCpuFrameRgba`, `replaceRegion`) across
  `src/render/metal/` and `src/dual/metal/`; the dual compositor already maps
  RGBA16FPx4 (live is blocked from dual anyway).
- Confirmed by reading: Metal's range override applies only to
  `Kind::Metal` YUV frames, not CPU RGBA — our frames are unaffected on Metal.
  D3D11 CPU path to be checked the same way.

### 6. UI
- LiveStrip (`ui/qml/LiveStrip.qml`): Paused (4) state label + detail text;
  "Waiting for …" for Connecting on qcbae; hide Mb/s for qcbae or show MB/s;
  a NON-FINITE badge when `nonFinite` (D4 surfacing; false-colour overlay is
  a later feature).
- LeftRail dot (`LeftRail.qml:538-548`): amber for Paused too.
- InspectorPanel (`InspectorPanel.qml:105-128, 275-276`): `typeLabel` "Live",
  `typeIconName` "broadcast" for 6 (and "Dual view" for 5, missing too);
  `hasOnDiskPath` false for 6 → no Reveal, no size row, "Copy URL" instead.

### 7. Pre-existing fixes (all in this branch, one commit each)
1. **Stale last live frame** — `stopLiveStream` (`window_manager.cpp:3928`)
   clears the sink's publish slot (add `VideoDecoder::clearPublishedFrame()`
   or close the sink unconditionally); today `closeActiveMedia` skips
   `close()` because the sink's path is empty (verified).
2. **Sticky range override** — reset `m_videoDecoder`'s range override on
   entering live (read by Metal every frame; D3D11 to be checked).
3. **Inspector** — live label/icon, Reveal/size on a URL (§6).
4. **Dual buttons enabled during live** — gate in
   `ViewportOverlay.qml:452-489` on `!WindowManager.liveActive`.
5. **LeftRail drag builds `file://srt://…`** — `LeftRail.qml:630-687`: pass
   URLs through untouched.
6. **Viewport / A-chip drops call `addMediaFile` directly** —
   `PlayerWindow.qml:57-75`, `ViewportOverlay.qml:240-246` → route through
   `addMediaPaths` (which already splits on `"://"`).
7. **No live guard in `setBSourceMediaId` / `replacePlaylistItems` /
   `createPlaylist`** — `project_manager.cpp:559, 446` reject LiveStream like
   Audio.
8. **New Project leaves live (and video) running** — `projectReplaced`
   handler (`window_manager.cpp:285-293`) calls `closeActiveMedia()`
   unconditionally when no item is active.
9. **Stale mode flags on `activeItemIdChanged`** (F6: `m_playlistActive` has
   no NOTIFY; annotation sync reads the old state) — make annotation re-sync
   run after load completes; new live state emits its own signal.
10. **Metal unlocked GUI-thread writes** (`clearSourceAState`,
    `setImageSeqCache`, `lastSourceTexture` not cleared) — investigate first
    (code read + `MTL_DEBUG_LAYER=1` switch test); fix only if a real race is
    shown, else document why it is safe.
11. **Audio items fall into the unsupported-video branch** (F8, unverified) —
    reproduce with an audio file first; fix if real.

## Order of work (each step builds and is checked before the next)
1. Branch `qcbae-live`; §7.1–7.2 (small, on the live path).
2. §5 renderer 16F branch (Metal; D3D11 code-along, compiled by chris on Windows).
3. §1 + §2 base class + retype; LiveStreamDecoder unchanged in behaviour —
   SRT regression check with the documented ffmpeg test sender.
4. §3 HostBridgeSource + §4 routing/naming/menu.
5. §6 UI.
6. §7.3–7.9 fixes; §7.10–7.11 investigations.
7. QCBridgeAE side: `lab/results/<date>-a3-qcview-ingest/notes.md`, PLAN.md
   A3 ✅, memory updates in both projects.

## Verification
- **Synthetic, no Adobe apps:** QCBridgeAE `qcbae-probe produce --tier 32`
  writes `/qcbae-probe` RGBA16F → QCView `qcbae://probe` shows the moving
  pattern; kill the producer → Reconnecting/"waiting", restart → recovers
  (Retired + dead-pid paths). Dev log line per session: first frame format,
  size, and a sampled pixel value, compared with `qcbae-probe dump` on the
  same ring (values must match — the consumed property, not "it shows").
- **After Effects / Premiere:** the A4 solids comp and test PNG; with OCIO
  disengaged, sampled values in QCView's log equal the ring's; Premiere's
  straight alpha shows over the background; focus loss with the preference
  ticked shows the Paused hint; a comp with an inf/NaN (e.g. an expression
  dividing by zero) raises the NON-FINITE badge.
- **Media matrix in the real app** (QCView memory rule): video → AE live →
  image sequence → Premiere live → playlist → video → SRT live → AE live →
  New Project; after each, correct picture, no stale frame, LiveStrip/timeline
  swap correct, dual buttons disabled only in live. chris eyeballs
  (screencapture is unavailable to Claude on this Mac).
- **Performance:** 4K AE playback through QCView — reader copy time and
  reader-thread CPU logged; playback holds 24 fps.
- `MTL_DEBUG_LAYER=1` run of the switch sequence: no validation aborts.
- Windows/D3D11: compiled and checked by chris (not buildable here).
