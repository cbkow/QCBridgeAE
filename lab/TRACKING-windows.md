# Tracking — what Windows still owes

The running list of Windows work across all three repos: QCView, QCBridgeAE
and QCBridge. The macOS side adds an item whenever it lands something that
Windows has to build, port or verify. The Windows side ticks it off with a
pointer to its evidence (a `lab/results/` folder, a commit).
`WINDOWS-SESSION.md` is where a Windows session starts; `PLAN-windows.md`
is the sequence across the three repos; `HANDOFF-windows.md` is the standing
instructions; this file is the checklist.

Same rule as the rest of `lab/`: public. No job names, no user paths, no
client pixels.

Status: `[ ]` open, `[~]` in progress, `[x]` done (with evidence), `[-]`
dropped (with the reason).

> **To the Windows session, 2026-09-23 (Mac side):** the merge that brought
> your tracker edits in kept your copy of this file, which dropped five
> sections the Mac added the same day. They are back (commit `28b439d`),
> your ticks and notes untouched. New for you since your last pull, all
> QCView unless said: **Alt+Scroll timeline pan**, **side-aware drop in dual
> view** (with the empty-side dual view under it), **drag the viewport to
> move the window**, **image sequences with non-ASCII names** (a user's
> bug; the Windows half is the fix), and under QCBridge **the ffmpeg capture
> path on Windows** (`ddagrab`). Each says what to build and what to look
> at. To keep this from happening again: **pull before every edit of this
> file**, on both machines.

## The goal: one coordinated release (decided 2026-09-22)

**Nothing ships until all three are built and released together.** QCView
goes out as 2.4.0 (rebuilt at the gate; the unpublished 2.3.4 DMG is
superseded) and is *waiting for Windows*; the two bridges go out with it
as 0.2.0 each. So there is no time pressure — the point is that a user never
has a QCView that speaks to a bridge they cannot install, or a bridge built
against a QCView that has not shipped.

Practical consequence for the Windows side: **a green build is not the bar.
Run the thing.** Two items already on this list were written on the Mac and
merged with "test on Windows before merging" unfulfilled, and one of them
(`read_ahead.cpp`) would not even compile. Assume nothing here has been
exercised on Windows unless an item says who exercised it.

### Suggested order

*(The full sequence, with entry and exit conditions per phase, is
`PLAN-windows.md`. This is the one-line version.)*

1. **QCView first, because it is the blocker with a known compile error.**
   Get it building, then `9c3874f6`'s dual matrix. Everything else in QCView
   is verification of already-merged work.
2. **QCBridgeAE A5** next — the producer side, and QCView's live path has
   nothing to read on Windows until the ring is ported.
3. **QCBridge (Blender) agent** last of the three. It is the newest and the
   most self-contained: a Rust build plus a test suite that spawns its own
   agents, so it can be judged without the other two.

### What the machine needs

- **MSVC toolchain** and CMake, for QCView and QCBridgeAE.
- **Rust** (rustup, MSVC toolchain) for QCBridge's agent. The old 1.89 pin
  came from Kyber and is gone; the Mac builds on 1.98.1. There is no external
  clone to fetch any more.
- **Python with pytest** for QCBridge's suite (`pyzmq` too, for the frozen
  zmq transport tests).
- **Blender 5.2** for the QCBridge smoke suites.
- An **NVIDIA GPU** for the NVENC paths (A5, and QCBridge S7 later).

## Getting the code

- [x] **QCView** `main`: everything below (`qcbae-live`, `metal-source-race`,
  `dual-network-read`, `rotating-upload-textures`, stacked) was
  fast-forwarded into `main` on 2026-09-22. Branch names below say where each
  item was developed. Pull `main`; one fix commit may still be local to the
  Mac.
  *Windows 2026-09-23:* pulled; the Mac's fix commit was in (`49d189c2`). Two commits on top from this side (`e30bf137`, `01577fc2`), unpushed.
- [x] **QCBridgeAE** `main`: this repo. A1–A4, A6 and A3 are done; A5 and A7
  are what Windows owes.
  *Windows 2026-09-23:* pulled; A5 spine landed as `5057e96`, unpushed.
- [x] **QCBridge** (the Blender bridge): `main`. The agent line
  (`spike/quinn`) was merged into `main` on 2026-09-23; the zmq transport
  remains in it as the frozen fallback. `spike/quinn` is now behind `main`
  — do not use it.
  *Windows 2026-09-23:* pulled `main` at `1d6e8ec`; commits `a4e2c6c`, `682da7f`, `c1f1e56` on top, unpushed.

## QCBridgeAE — phase A5 (PLAN.md)

The producer side, so this comes first.

- [x] **Ring as a named file mapping.** `src/common/surface/shared_ring.cpp`
  is POSIX (`shm_open` / `mmap` / `ftruncate`). Port it to
  `CreateFileMapping` / `MapViewOfFile` with the same geometry: header padded
  to a page, page-aligned slots, `slots_offset` in the header. Decide the
  object name (`Local\` + the ring name?). AE and QCView share a session, so
  `Global\` shouldn't be needed. Say what you chose in the notes.
  *Windows 2026-09-23:* `Local\` + name without the slash, page-file backed, same geometry; replace-while-held retires the old mapping and retries. Unit, tearing and ctest pass. `lab/results/2026-09-23-a5-windows-spine/`, commit `5057e96`.
- [x] **Liveness without `kill(pid, 0)`.** A dead producer's ring persists
  (measured on macOS: neither host unloads the device on quit). The consumer
  checks `producer_pid`; on Windows that is
  `OpenProcess(SYNCHRONIZE)` + `WaitForSingleObject(h, 0)`. Keep the
  "Retired means reopen" rule.
  *Windows 2026-09-23:* `qcbae::process_alive()` in `shared_ring.h`, vendored into QCView with the ring. Pid-recycling caveat written down. `lab/results/2026-09-23-a5-windows-spine/`.
- [x] **F16C conversion** for `convert_32f_to_rgba16f`: the main path, see
  HANDOFF §F16C. Run `convert_test` (it must match the portable scalar bit
  for bit) and `qcbae-convbench`; results to `lab/results/`.
  *Windows 2026-09-23:* runtime CPUID, bit-exact against VCVTPS2PH over 1,048,532 patterns; 4K A6 pass 6.5 ms F16C vs 73.7 ms portable (11.4x), memcpy floor ~2.4x the Mac's. `lab/results/2026-09-23-a5-windows-spine/`.
- [x] **The Transmit device on Windows.** Build `qcbae-transmit` as the
  Premiere SDK's Transmitter sample does, and install it in MediaCore
  (HANDOFF's table).
  *Windows 2026-09-23:* the Premiere Pro SDK 26.0 arrived at 13:00; `qcbae-transmit` builds as `QCBridgeAE-Transmit.prm` (`764a696`: PRWIN_ENV, dllexport entry, %TEMP% log, GetModuleFileName host detection), installed into MediaCore by `packaging/windows/install-transmit.ps1` (UAC), and **After Effects 2026 loads it**: `--- module load, host interface v4 --- / startup in After Effects: ring /qcbae-ae`. With the device enabled under Video Preview AE published 1920x1080 32f frames and QCView on Windows went LIVE on `qcbae://ae` (240 frames, ring copy 1.2 ms mean). Notes: the A5 folder's addendum.
- [~] **Re-measure host behaviour; don't inherit it.** macOS findings
  (`lab/results/2026-09-21-a4-transmit-probe/`): the host picks 32f when
  offered; frames arrive bottom-up; AE flattens alpha over the comp colour,
  while Premiere sends straight alpha; AE's inTime is -1 and always
  "scrubbing"; the colour request is ignored under OCIO; focus loss stops the
  stream unless the background preference is unticked; the device is never
  unloaded on quit; each viewer change pushes 2 frames. Any of these can
  differ on Windows.
  *Windows 2026-09-23:* AE picks 32f but hands **BGRA**, not ARGB (the device converts both); focus loss pauses the stream the same way (activation 3 → video off → PausedFocus, back on return). Premiere Pro 2026 loads the same `.prm`, hands BGRA 32f, 63 fps peak into QCView; both hosts ran into QCView at once; an AE viewer resize to 9216x3164 retired and re-created the ring (228 MB slots) and QCView followed. Unload-on-quit unmeasured (the session ended in a GPU driver reset). A deliberate alpha look still owed. A5 notes addendum.
- [-] **`qcbae-probe`**: `produce` and `dump` should port with the ring;
  `view` is Metal and needs a D3D11 twin, or skip it and use QCView as the
  viewer.
  *Windows 2026-09-23:* dropped for Windows: QCView is the viewer; `produce`/`dump` are not needed without the device.

## QCView — live sources (branch `qcbae-live`)

- [x] **Enable the host bridge on Windows.** `src/decode/CMakeLists.txt`
  builds `HostBridgeSource` and defines `QCV_HAS_HOST_BRIDGE` on APPLE only.
  The vendored `src/decode/qcbae/shared_ring.*` gets the same Windows port as
  QCBridgeAE's (keep the two identical; `kFrameDescVersion` guards a
  mismatch). Then enable it for Windows.
  *Windows 2026-09-23:* vendored ring re-copied from `5057e96`, `QCV_HAS_HOST_BRIDGE` on every platform, builds and runs (QCView `01577fc2`). Behaviour waits for a producer. `lab/results/2026-09-23-qcview-windows/`.
- [~] **Build check: `c7fef457`** (D3D11 uploads `Format_RGBA16FPx4` as
  `R16G16B16A16_FLOAT`), written on macOS and not compiled there. Verify
  over-range and negative values reach the canvas unclamped: sample a pixel,
  don't eyeball it.
  *Windows 2026-09-23:* compiles on Windows; the over-range/negative pixel sample needs a producer (Transmit or SRT).
- [ ] **Range override on D3D11's CPU path.** On Metal the range override
  applies only to YUV frames, never to CPU RGBA, so live frames are
  unaffected. Check that D3D11's CPU slot behaves the same.
- [ ] Shared QML and model changes on this branch (LiveStrip, Inspector facts,
  drag and drop of URLs, New Project, the live guards): run the media matrix
  from `lab/results/2026-09-21-a3-qcview-ingest/` on Windows.

## QCView — the Windows build is broken on main (found 2026-09-22)

A pre-release audit of QCView `main` found the Windows half of the 2.3.4 diff
has never been compiled — there is no CI, and both of these went in from the
Mac.

- [x] **Confirm the `NOMINMAX` fix.** `src/decode/read_ahead.cpp` included
  `<windows.h>` with no guard while calling `std::min`/`std::max` with plain
  arguments in four places, so MSVC could not compile it: windows.h defines
  those as function-like macros. Fixed on the Mac by the same
  `WIN32_LEAN_AND_MEAN` + `NOMINMAX` idiom every other file here uses — but
  fixed blind. **A Windows build is the only thing that proves it**, and it is
  the first thing to try, because nothing else in the file has ever been
  compiled either.
  *Windows 2026-09-23:* confirmed — and `video_decoder.cpp` needed the same guard (`907f18ec` added a plain `std::max` under `hwcontext_vulkan.h`'s windows.h). QCView `e30bf137`. `lab/results/2026-09-23-qcview-windows/`.
- [~] **The read-ahead may be inert on Windows.** `read_ahead.cpp` uses
  `SetThreadPriority(THREAD_MODE_BACKGROUND_BEGIN)`, which throttles I/O far
  harder than macOS's `IOPOL_UTILITY`. Check the `ReadAhead: … window warm`
  log actually reports a useful fetch rate rather than crawling.
  *Windows 2026-09-23:* builds and logs `window warm` on every open, but the media here is local NVMe (cache hits); the LucidLink/background-priority question needs the studio filespace mounted on this box. Open.
- [x] **Live in dual freezes a D3D11-decoded side.** `DualFrame` has `Cpu`,
  `Metal` and `Vulkan` kinds but no D3D11 one, so
  `dual_live_source.cpp:186-190` falls through to `default:` and the side
  holds its previous frame for ever, silently. An SRT stream decoded through
  D3D11VA on one side of dual is the case. Needs either a D3D11 `DualFrame`
  kind or an honest status instead of a frozen picture.
  *Windows 2026-09-23:* fixed: `DualLiveSource` now brings a D3D11VA live frame to the CPU and converts it the way `DualVideoDecoder` does a D3D11VA file side (RGBA8/RGBA64 by the depth rule), so the compositor sees a Cpu frame — one readback per frame, the zero-copy D3D11 kind is the next step. Proven with a QCBridge SRT stream on side A and a file on B: `DualLiveSource: D3D11VA live frames brought to the CPU for dual (3840x2160, RGBA64)`, no errors. `--simulate-user` now accepts a live URL for A.

## QCView — threading fixes (branch `metal-source-race`)

Found with a ThreadSanitizer build on macOS. MSVC has no TSan, so on Windows
the check is a long harness run plus the real-app dual matrix.
`qcview --switch-test SECONDS LISTFILE` (one path or URL per line) randomly
switches media, enters and leaves dual and trims the timeline, then quits.
`QCV_SWITCH_SEED` replays a sequence; macOS used seed 3222196691.

- [x] **Build and test `9c3874f6`: the D3D11 teardown handshake.** RELEASE
  BLOCKER for a cross-platform 2.3.4 — its own commit message says "Build and
  run the dual matrix on Windows before merging", and it was merged anyway.
  The Metal original was validated under TSan with a 321-step replay; this
  port has zero minutes of runtime, and it holds `sourceMutex` across a whole
  draw with a lock order (`d3d11_player_renderer.cpp:294`) that has never been
  exercised. A deadlock here hangs the app. Written on
  macOS and never compiled. `Impl::sourceMutex` is held around
  `consumeLatestVideoFrame` and each draw, released just before `Present`;
  the source setters take it. Check: no deadlock under `--switch-test`, no
  hitch when switching media, and the wipe drag is as smooth as before (its
  parameters are atomics, not locked).
  *Windows 2026-09-23:* `--switch-test 600`: 1417 steps, 443 dual entries/exits across D3D11VA, Vulkan and CPU sources, done logged, exit 0, no deadlock, 0 non-QML warnings. Wipe-drag smoothness owed by hand. `lab/results/2026-09-23-qcview-windows/`.
- [x] **Shared-code commits, verify on Windows:**
  - `bde75934`: a B change in dual rebuilds the island. This goes through the
    Windows-only `m_dualSourceAdapter` teardown and cold entry. Clearing B
    returns to single view.
  - `45f10658`: the timeline snapshot. Slip / trim / slide in dual still
    shows every drag delta.
  - `f482da50`: atomic fetch counter. (Its screenshot half is Metal-only;
    D3D11's capture reads the ComPtr-held slot and needs no change.)
  - `eb323d05`: no B chip or dual controls for a stream item.
  - `d28dc540`, `aaeb1ade`: the harness itself should build.
  *Windows 2026-09-23:* exercised by the same switch run (harness builds; B set/clear through `m_dualSourceAdapter` 443 times; head-trim bursts). Visual deltas owed by hand.
- [x] Long run: `--switch-test 600` on a Release build with the Windows media
  set. Pass = no crash, no hang, and "done" logged.
  *Windows 2026-09-23:* pass — no crash, no hang, "done" logged.

## QCView — network read-ahead (branch `dual-network-read`)

Branched from `metal-source-race`. On a network volume an uncached frame
arrives slowly (measured on macOS: ~60–70 MB/s through LucidLink's local
SMB gateway, shared by every reader). `ReadAhead` (`src/decode/read_ahead.*`)
warms the byte ranges of the next ~2 s of frames from the container index.

- [x] **Build check: `e7e7995f` and the touch rewrite after it.** Written on
  macOS. The read-ahead now touches one byte per 1 MiB page (LucidLink's
  cache page) rather than copying frames. The Windows-specific part is
  `SetThreadPriority(THREAD_MODE_BACKGROUND_BEGIN)` for the worker; reads go
  through `QFile` (unbuffered). macOS also sets `F_NOCACHE`; Windows has no
  equivalent for a 1-byte read (`FILE_FLAG_NO_BUFFERING` needs aligned
  sector-sized reads), so there the OS may keep the touched page, which is
  harmless.
  *Windows 2026-09-23:* builds; the worker runs under `THREAD_MODE_BACKGROUND_BEGIN`.
- [~] **Does LucidLink on Windows hydrate from a touch?** On macOS
  `lucid3 cache` showed the on-disk cache grow by the file's size after a
  touch of an uncached file. Repeat on Windows; the client may mount the
  filespace differently.
  *Windows 2026-09-23:* needs the filespace on this box; not mounted today.
- [~] **Does it help on Windows?** Open an uncached large file on a network
  share with `QCV_READAHEAD_SECONDS=0` (off) and at the default. Compare how
  quickly a paused seek and the first seconds of playback fill; the
  `ReadAhead: … window warm` log lines give the fetch rate. Background mode
  also lowers I/O priority on Windows; check that it doesn't starve the
  read-ahead on an idle machine.
  *Windows 2026-09-23:* same — needs a network share. Local NVMe only says the code runs.
- [~] **Loop-range hydration** (commit after `b116e372`): loop on + in/out
  set touches the whole range, in single and dual. Shared code; verify the
  `ReadAhead: … loop range … touched` log line appears and the range plays
  smoothly on the next pass.
  *Windows 2026-09-23:* shared code, builds; the `loop range … touched` line needs a loop set by hand. Owed.
- [x] `bc8ca1c4` only adds dual-decoder diagnostics (slow read, read EOF,
  empty-ring stall); nothing to port.
  *Windows 2026-09-23:* nothing to port; builds.

## QCView — upload ring (branch `rotating-upload-textures`)

- [x] **Does D3D11 need a twin?** macOS landed `MetalUploadRing`: CPU frames
  go into a ring of textures so an upload never overwrites one a frame in
  flight samples. On macOS the old single-texture path overwrote an
  in-flight texture 1 in ~1000 uploads at 4K60 with OCIO, and 0 at 1080p60.
  D3D11's `UpdateSubresource` on a default-usage texture is ordered by the
  runtime (it copies or waits as needed), so the hazard may not exist there;
  confirm that before porting anything. The dual compositor's other change,
  skipping the re-upload of an unchanged frame every vsync, is worth
  checking on D3D11 regardless (`d3d11_dual_compositor.cpp`).
  *Windows 2026-09-23:* no: the CPU path is `UpdateSubresource` on `D3D11_USAGE_DEFAULT` textures (player renderer, texture pool, dual compositor), which the runtime orders against in-flight draws. No hazard by construction. `lab/results/2026-09-23-qcview-windows/`.

## QCView — live sources in dual view (branch `dual-live`)

Merged into QCView `main` on 2026-09-22 (verified on macOS with the real
hosts). A live side is a `DualLiveSource`
(`src/dual/dual_live_source.*`): it owns its own receiver and hands its latest
frame back for any master frame. Live can be either side or both; two live
sides have no clock (`dualSeekable` false) and the transport hides.

- [x] **Build check.** New file in `qcv_dual`; `LiveSource::setSink` now takes
  a `LiveFrameSink*` (`decode/live_source.h`), which `VideoDecoder`
  implements. `DualLiveSource` has a `Q_OS_WIN` branch that clones a Vulkan
  AVFrame the way `DualVideoDecoder` does; that path has never been compiled.
  *Windows 2026-09-23:* compiles, including the `Q_OS_WIN` Vulkan-clone branch that never had.
- [x] **srt:// in dual on Windows.** `qcbae://` stays macOS-only until the
  ring port above, but SRT works on both: put a stream on one side and a file
  on the other, check the file side still drives the transport and the live
  side updates. D3D11 renders on demand, so the frame-available callback is
  what wakes it — if the live side only repaints when you move the mouse,
  that callback is not reaching the renderer.
  *Windows 2026-09-23:* single-view `srt://` from a Windows Blender replica works: `LiveStreamDecoder: LIVE (d3d11va zero-copy, 3840x2160)`, 10-bit, through the native helper. Dual with a file on the other side: done (the item above); the file side drives the transport, the live side updates through the CPU path.
- [ ] **Live + live**: transport and timeline hidden, both sides updating.

## QCView — Alt+Scroll timeline pan (fixed on the Mac 2026-09-23, unverified on Windows)

Reported from Windows: Alt+Scroll pans the timeline on macOS and does
nothing on Windows. Cause, from the Qt 6.11.1 source: the Windows (and X11)
platform plugin reports Alt+wheel as a *horizontal* rotation
(`qwindowspointerhandler.cpp`, `keyModifiers & Qt::AltModifier` →
`QPoint(delta, 0)`); Cocoa keeps it vertical. Both timeline wheel handlers
read only `angleDelta.y`, so on Windows the pan delta was always zero.
`TimelinePanel.qml` now folds x into y for the Alt case only
(`wheelPanDelta`). The macOS build is green and the Mac path is unchanged.

- [ ] **Alt+Scroll pans on Windows** with a mouse wheel, in the track area
  and over the overview bar, and in the same direction as macOS (the folded
  delta keeps Windows' sign; `WM_MOUSEHWHEEL` is the one Qt negates, and
  Alt+vertical is not that message). If the direction is reversed, say so;
  do not flip it locally.
- [ ] **Plain Scroll still zooms** and a trackpad horizontal swipe with no
  modifier does nothing (the fold is gated on Alt).

## QCView — side-aware drop in dual view (landed on the Mac 2026-09-23, Windows half unbuilt)

A file dropped onto the viewport in Side-by-Side or Split-Wipe now loads the
side it lands on (left → A via the ordinary open path, right → B via
`setBSource`), and the target side is lifted toward white while the drag
hovers. Routing is in `WindowManager::dropMediaAt`; the split rule is
half for side-by-side, `splitPos` for wipe. The Windows half was written
blind against the D3D11 code and has not been compiled:

- [ ] **It builds.** `D3D11DropTarget` grew hover and leave callbacks and a
  `POINTL` on drop; `D3D11PlayerRenderer::init` maps the point with
  `ScreenToClient` + `GetClientRect` on the child HWND and normalizes it.
  `d3d11_dual_compositor.cpp` gained an `int dropSide` at cbuffer offset 56
  (it took one of the two pad floats; `sizeof(DualCB)` is still 64 and the
  static_asserts hold) and the matching `dropSide` in the HLSL cbuffer after
  `diffGain`.
- [ ] **A drag over the right half of a side-by-side lights the right half,
  the left lights the left**, and the highlight clears on leave and on drop.
  Wipe follows the seam. Difference and single light the whole canvas.
- [ ] **The drop lands on the side it showed**, on a real dual session, with
  an mp4 on each side. Dropping on B rebuilds the island (expected: playhead
  and track edits reset; that is the `setBSource` contract, not a bug).
- [ ] **The QML `DropArea` path still works** while the surface is hidden (a
  modal open) — it now calls `dropMediaAt` with `drop.x / width`.
- [ ] **Dual view with an empty side** (landed with it, same day). The
  "no A" gate in `setCompositorMode` is gone; `clearBSource` keeps the mode;
  loading a new A keeps dual even with no B. On D3D11 nothing changed — the
  compositor already draws with null views — so this is verify only: run
  `qcview --empty-dual-test A.mp4 B.mp4` (logs each step's mode and sides;
  the mode must stay 1 until the explicit single step) and look at the
  window during it: blank half, divider or seam, other side's picture, and
  the transport plus two timeline lanes present in every dual state
  (`dualSeekable` used to read two empty sides as two live sides and hid
  both bands; the first step now dwells 2.5 s so you can see it). Then
  `--switch-test 40 list.txt` and `--simulate-user` as before (the Mac ran
  111 steps, 36 entries, 12 exits, clean).

## QCView — drag the viewport to move the window (landed 2026-09-23, Windows half unbuilt)

A left press on the viewport that neither the wipe seam nor a drawing tool
claims, dragged past the threshold, calls `QWindow::startSystemMove()` on
the UI window (`WindowManager::startWindowMove`, gated by the
`ui/dragViewportMovesWindow` setting, off in fullscreen and under a modal).
On Windows the press arrives in the centerStage MouseArea in `Main.qml`
(the D3D11 child is HTTRANSPARENT) and the QML calls the invokable.

- [ ] **It moves.** With no drawing tool selected, drag the viewport: the
  window follows, snaps at edges like a title-bar drag. A plain click does
  nothing. Qt's Win32 `startSystemMove` sends `SC_MOVE` with the button
  held — confirm it takes over cleanly from the QML press.
- [ ] **It yields.** Select a pen: dragging draws, the window stays. Wipe
  mode: dragging the seam moves the seam, not the window. Borderless
  fullscreen (F): nothing moves. Settings → "Drag viewport to move
  window" off: nothing moves.
- [ ] **After the move** the next click still reaches the annotator (the
  QML MouseArea disarms on release; if Windows swallows the release inside
  the move loop, the next press re-arms anyway — check the first stroke
  after a move is not lost).

## QCView — image sequences with non-ASCII names (fixed blind 2026-09-23; this is the Windows bug)

A Chinese user reported image sequences with Chinese file names not
loading. Detection was never the problem (the regexes capture the base
with `.+`); the loaders were. Paths leave Qt as UTF-8 and the PNG, JPEG
and TIFF loaders opened them with narrow `fopen` / `TIFFOpen`, which on
Windows reads the ANSI code page — so a CJK name never resolved and the
sequence cache's frame-0 probe refused the sequence. EXR was already fine
(`MemoryMappedIStream` → `CreateFileW`). Now `src/decode/utf8_file.h`
converts UTF-8 → UTF-16 and uses `_wfopen` / `TIFFOpenW` on Windows;
exiftool gets `-charset filename=utf8` there too. macOS verified with
JPEG and PNG sequences named `镜头_0000.jpg` / `画面_0000.png` in a
`测试序列` folder — but macOS never had the bug, so:

- [ ] **It builds.** `utf8_file.h` includes `<windows.h>` with
  `WIN32_LEAN_AND_MEAN` + `NOMINMAX` (the same NOMINMAX story as
  `read_ahead.cpp`); `TIFFOpenW` must exist in the vcpkg libtiff (it is
  Windows-only API, `#ifdef _WIN32`).
- [ ] **A CJK-named sequence loads**, PNG, JPEG and TIFF each (make one
  with `ffmpeg -i x.mp4 测试序列/镜头_%04d.png`), from a CJK-named folder,
  on a machine whose system locale is *not* Chinese (that is the case that
  fails: on a Chinese-locale box the ANSI page happens to cover it). EXR
  as the control, which worked before.
- [ ] **The Inspector shows metadata** for a CJK-named file (the exiftool
  charset flag) — compare against an ASCII copy of the same file.
- [ ] **Thumbnails and scrub** on that sequence (same loaders, via the
  thumbnail cache and the dual scrub path).

## QCView — packaging changes that touch Windows (2026-09-22)

The macOS release flow was rebuilt (`scripts/`, committed now — it used to be
gitignored, which is how the originals were lost with the old Mac). Three of
those changes are not macOS-only:

- [x] **The log moved out of the app bundle.** `installFileLogger` wrote next
  to the executable, which on Windows is `Program Files` — never writable, so
  released builds almost certainly had no log at all. It is now
  `%LOCALAPPDATA%/QCView/logs/` (`QCV_LOG_DIR` overrides). Confirm a packaged
  Windows build actually writes there.
  *Windows 2026-09-23:* a built binary writes `%LOCALAPPDATA%\QCView\logs\qcview-log.txt`. `lab/results/2026-09-23-qcview-windows/`. The MSIX check is Phase 5.
- [x] **Qt Multimedia was dropped** from `find_package` (nothing used it).
  windeployqt should stop shipping Qt's media plugin and its FFmpeg; check the
  MSIX shrinks and nothing breaks.
  *Windows 2026-09-23:* no `Qt6Multimedia` anywhere in `build-release`; MSIX size is Phase 5.
- [~] **Pruning.** `scripts/prune_bundle.sh` is macOS-shaped (frameworks,
  otool). If windeployqt is as generous as macdeployqt was — it deployed Qt3D,
  PDF, the virtual keyboard and a second FFmpeg — the Windows package may
  deserve the same treatment.

  *Windows 2026-09-23:* measured on the 2.3.4 MSIX: 466 MB uncompressed. FFmpeg 180 MB is one copy (avcodec 118, avfilter 36, avformat 22) — no second FFmpeg, no Qt3D, no virtual keyboard, no Qt Multimedia. Prunable: `opengl32sw.dll` 21 MB (software GL; the renderer is D3D11), `dxcompiler.dll` 22 MB (check whether Qt RHI needs it at run time), `Qt6Pdf.dll` 4.6 MB, the unused Quick Controls styles (Imagine, Material, Universal, Fluent, ~10 MB), `qmltooling/` 1 MB, and exiftool's library tree shipped twice (`assets/exiftool/exiftool_files/lib` 34 MB and `assets/exiftool/lib` 20 MB). Done the same afternoon: `installer/msix/build_msix.ps1` prunes after staging (`--no-opengl-sw` plus a list; `-NoPrune` keeps it all), QCView `13dbb081`: **62 MB out, 141 MB packed against 165 for 2.3.3**. `d3dcompiler_47` stays (the D3D11 renderer's HLSL compile).
## QCBridge — transport, after the mux-tax bench (2026-09-22)

The Blender sister repo is dropping Kyber for quinn on the control lanes and
keeping SRT for video. The bench that settled the video question
(`spikes/parity/mux-tax/`, results in
`spikes/parity/results/2026-09-22-mux-tax/`) ran on the Mac only, and two of
its conclusions need a Windows twin before they are safe to build on.

- [ ] **Re-run the ladder on Windows.** `qcb-stamp` is VideoToolbox, so the
  Windows side needs an NVENC equivalent feeding the same unmodified
  `probe_reader.py`. The prize is the pairwise deltas, not the absolutes:
  does a process hop cost ~18 ms there too, and is SRT's latency setting 1:1?
  The 2026-09-17 `win-loopback` runs already say yes to the second
  (91 → 191 for 20 → 120); the first is untested off the Mac.
- [ ] **In-process mux on Windows.** `muxsend.c` is portable C against
  libavformat and should build as-is, but the Windows agent has to link an
  FFmpeg with SRT — confirm the one it ships has the protocol, the way
  QCView's vendored build does.
- [x] ~~No host-side demux leg.~~ Done 2026-09-22 on the Mac: the video lane
  is out of the transport entirely and `video_listen` is deleted. Nothing for
  Windows to inherit.

## QCBridge — the bootstrap count the Windows survey flagged (answered 2026-09-23)

- [-] **"8 bootstraps on Windows against 1 on the Mac."** Not a difference:
  the 1 is the Mac's first survey table, before the day's fixes; the Mac's
  final run (`spikes/parity/results/2026-09-23-sync-coverage/full-agent-final.json`)
  has the same replica stats as Windows, 8 bootstraps included. Seven are
  `Scene`-level structural changes escalating to a tier-2 that `Scene`
  cannot express as a blob, so the host resends a bootstrap — a cost item
  on both platforms (a per-Scene resend would be the fix), tracked as a
  QCBridge improvement, not here.

## QCBridge — the replica agent under the logon task dies with a console-control exit (found 2026-09-23, paired run)

Twice in the paired session the replica agent registered by
`agent/windows/logon-task.ps1` ended with task result `0xC000013A`
(`STATUS_CONTROL_C_EXIT`): once after about an hour up (the host saw
"replica lost"), and then *every* `Start-ScheduledTask` restart died within
a second with the same code, no crash event in the Application log. The
same binary started through a one-line batch wrapper
(`cd` to the release folder, `qcbridge-agent.exe --role replica > log 2>&1`)
by an equivalent interactive one-shot task ran fine, took the host, launched
Blender and streamed. Launched directly by the scheduler the process has a
console of its own with nothing on stdout/stderr; behind `cmd` it has
redirected handles. That is the only difference found.

- [ ] **Find why the direct launch dies.** Reproduce with the task as
  registered; try `-Argument` with a redirect via `cmd /c`, or build the
  agent with `#![windows_subsystem = "windows"]` when `tray = true` (no
  console at all; log to a file beside `agent.toml`). The fix should make the
  logon task the reliable path; until then the wrapper is.
- [ ] **A log file for the agent on Windows.** Its stdout is the only
  diagnostic and the task swallows it. Write `agent.log` next to
  `agent.toml` (rotating) from the binary, not the launcher.
- [ ] **The visible console window is a hazard.** Started by the scheduler
  the agent shows a console on the desktop that a user can close (that ends
  it with this exact code). Same fix as the first item.

## QCBridge — the ffmpeg capture path on Windows (added 2026-09-23)

Native capture (S7) is out of scope for this release *because the ffmpeg
capture path works* — and that has only ever been shown on the Mac
(avfoundation → VideoToolbox). The Windows run that exists
(`spikes/parity/results/2026-09-17-win-loopback/`) fed NVENC a synthetic
`testsrc`, not the screen. The replica's real Windows argv is in
`qcbridge/ring0/pixel_path.py`: `ddagrab=output_idx=0:framerate=<fps>:draw_mouse=0`
→ `hevc_nvenc` (main10, `-tune ull -delay 0 -bf 0`), GPU-resident, the display
*is* the viewport in kiosk mode. Phase 4's first item silently depends on it.

- [ ] **The bundled ffmpeg has `ddagrab`.** QCView's Windows ffmpeg
  (`%LOCALAPPDATA%\QCView\bin\ffmpeg.exe`) needs a build with D3D11VA and the
  `ddagrab` filter (`ffmpeg -filters | findstr ddagrab`). The 09-17 notes
  confirm libsrt and hevc_nvenc; they do not mention ddagrab.
- [ ] **A replica Blender in kiosk mode streams its screen.** Start the replica
  with streaming on, receive in QCView (or `probe_reader.py`), and confirm the
  picture is the Blender viewport at the configured fps, with no cursor. Note
  `output_idx=0`: on a multi-monitor box the capture is the *first* output,
  which may not be the one Blender is on — say which monitor you used.
- [ ] **The 4:4:4 rung** (`hevc_10_444_50`) takes the CPU path
  (`hwdownload,format=bgra,format=yuv444p10le`, profile `rext`). Confirm it
  actually produces 4:4:4 at the receiver — the code comment says NVENC
  silently downgrades 4:4:4 on GPU frames, so verify receiver-side.
- [ ] **Glass-to-glass with ddagrab**, the way Phase 4 asks: the synthetic
  pipe number is 43 ms at 1080p60; the screen-capture number is the one a
  user gets. Write both.

## QCBridge — the agent after the quinn port (2026-09-22, then branch `spike/quinn`, now `main`)

Kyber is gone; the transport is plain `quinn`. The Mac side builds, passes
10/10 transport contract tests and 87/87 pytest. None of it has been compiled
on Windows.

- [x] **Build the agent on Windows.** `cargo build` in `agent/`. The old
  Rust 1.89 pin came from Kyber and can relax; the Mac builds on 1.98.1.
  There is no `build-win.*` script and no external clone to make any more —
  `HANDOFF-windows.md` task 4 is superseded.
  *Windows 2026-09-23:* `cargo build --release` clean, 45 s, `zstd-sys` included. QCBridge `spikes/parity/results/2026-09-23-windows-agent/`.
- [x] **Run the contract tests there.** `python -m pytest -q` → expect 87
  passed, 0 skipped. `tests/test_transport_agent.py` spawns two
  `qcbridge-agent` processes itself, so it needs only the built binary. If it
  *skips*, the binary was not found — that is a fail, not a pass.
  *Windows 2026-09-23:* 100 passed, 2 skipped — the skips are `skipif(darwin)` marks in `test_pathmap.py`, not a missing binary (== the Mac's 102).
- [x] **Check the per-instance config change.** Cert and `agent.json` now
  derive from the config file's directory, so `--config` isolates an
  instance. Confirm the default path still resolves to
  `%APPDATA%\QCBridge\` and that two agents with different configs do not
  share a certificate.
  *Windows 2026-09-23:* `--config` isolates cert and agent.json beside the TOML; the default resolves to `%APPDATA%\QCBridge\` (Roaming). Both run.
- [x] **21-check smoke over the agent transport**, now committed as
  `smokes/`: `QCB_TRANSPORT=agent QCB_AGENT=spawn`. The scripts are zsh and
  macOS-shaped (`${0:a:h}`, `/Applications/...`); a Windows equivalent is
  its own task. `$BLENDER` overrides the binary.
  *Windows 2026-09-23:* 21/21 via `smokes/run_smokes.py` (Python port of the runners, `a4e2c6c`). QCBridge `spikes/parity/results/2026-09-23-windows-agent/`.
- [~] **Re-run the transport A/B.** `bootstrap_bench.sh <w> agent 8` and
  `... 40` against `zmq` as control. The Kyber baselines this replaced are
  quoted in `results/2026-09-22-quinn-port/notes.md`; the directories that
  held them were deleted.
  *Windows 2026-09-23:* the latency bench ran for both transports (see the sync section below); `bootstrap_bench.sh` itself not yet ported.
- [ ] **Cross-machine rungs.** Everything measured so far is loopback, where
  SRT's latency buffer looks like pure overhead. mac↔win with real loss is
  what decides how low that setting can actually go.

## QCBridge — discovery and runtime settings (2026-09-22, then branch `spike/quinn`, now `main`)

The agent now finds replicas three ways — a unicast probe to `host:4246`
(the VPN path, primary), a shared-storage phonebook, and multicast on
`239.42.0.4:4246` — and takes settings at runtime (`set_config`, a tray
Network submenu). All of it built and proven on the Mac only. The group and
port continue the studio family (MinRender `.1:4243`, UFB `.2:4244`/`.3:4245`)
so the beacons read as siblings; do not move them.

- [~] **The beacon socket.** `agent/src/discovery.rs::bind_udp` mirrors UFB's
  Rust `udp_notify.rs` (socket2: `SO_REUSEADDR`, bind `0.0.0.0:4246`, join
  the group, TTL 1, multicast loop on) plus `SO_REUSEPORT`, which is
  `#[cfg(unix)]` — Windows has no such option and `SO_REUSEADDR` alone means
  something different there. Confirm a replica can bind 4246 while UFB or
  MinRender are running on the same machine, and that a unicast probe from
  another box gets an answer. **Windows Firewall will block inbound
  UDP/4246 until a rule exists** — MinRender's installer adds one for its
  own port (`installer/minrender_installer.iss`); the agent needs the same.
  *Windows 2026-09-23:* binds `0.0.0.0:4246` and answers direct probes on one box (pytest discovery test, and a tray run). Coexistence with UFB/MinRender and a probe from another box: two-machine. Firewall rule: `agent/windows/firewall-rule.ps1` (`c1f1e56`), needs an installer/elevation to apply.
  *Paired 2026-09-23, from the Mac seat:* a direct probe from the Mac **over the VPN** answered with the replica beacon (role, port, version, fingerprint, `paired`), and the host agent's QUIC attach to 19990 followed once the tokens matched — no firewall rule was needed for either, the agent process having been allowed when it bound. That is the direct-address item done; multicast stays unexercised (different networks).
- [ ] **Only replicas answer on 4246 — by design, keep it that way.** Two
  sockets sharing the port with reuse both get multicast but a unicast
  probe reaches only one; a host on the port silently ate probes meant for
  the replica beside it on the Mac. If Windows delivery differs, that is
  worth a line in the notes, not a reason to let hosts bind.
- [~] **The machine name.** `config::machine_name()` reads `COMPUTERNAME`
  on Windows (libc `gethostname` elsewhere). Confirm it is set in the
  interactive session the agent will run in — it is, normally — and that a
  name with spaces or Unicode survives into the phonebook filename
  (`entry_name` replaces anything non-alphanumeric with `_`).
  *Windows 2026-09-23:* `COMPUTERNAME` is set in the interactive session here; the spaces/Unicode-in-phonebook case not exercised.
- [ ] **The phonebook on a share.** `phonebook = "<dir>"` writes
  `<dir>/qcbridge/<name>.json` by temp-file-then-rename. Check that rename
  is atomic enough on the SMB path the studio uses (it is what MinRender
  relies on already), that a UNC path and a mapped drive both work, and
  that a stale entry from a machine that crashed is dropped after 60 s
  rather than offered.
  *Windows 2026-09-23:* not exercised (two-machine / the studio share). One thing learned on this box applies: a rename over a file another process holds open fails on Windows (`PermissionError`) where macOS always succeeds — the coverage survey's own JSON dump hit it. The phonebook writer should retry the rename briefly; the agent's reader is open-read-close, so the window is small but real.
- [x] **`pid_alive` returns `None` on Windows**, so the single-instance guard
  cannot refuse a second agent there: it needs `OpenProcess(SYNCHRONIZE)` +
  `WaitForSingleObject(h, 0)` via `windows-sys`. Same answer as the
  QCBridgeAE ring's liveness item above; do them together.
  *Windows 2026-09-23:* ported: `OpenProcess(SYNCHRONIZE)` + `WaitForSingleObject(0)` (`a4e2c6c`); unit test asserts a nonsense pid is dead on Windows. QCBridge `spikes/parity/results/2026-09-23-windows-agent/`.
- [~] **The tray on Windows.** Never run there. Checkable items, a submenu
  and `set_tooltip` are all supported by muda/tray-icon on Windows, and
  muda's premature check-toggle is in its Windows backend too (the handler
  already sets all three by id). Click through Off / Direct / Discoverable
  and confirm the TOML changes and the addon panel follows.
  *Windows 2026-09-23:* starts and runs (replica, 6 s, no crash); the Off/Direct/Discoverable click-through is a hand check, owed.
- [ ] **`find_agent` picks the newest cargo build by mtime.** A stale
  `target/release` once shadowed a fresh `debug` and the contract tests
  passed against an agent that did not know `set_config`. If a Windows run
  ever shows `unknown cmd` in `<base>/host-agent.log`, check which binary
  was spawned before anything else.
- [x] **Run it:** `cargo build --release` in `agent/`, then
  `python -m pytest -q` → 89 passed, 0 skipped on 2026-09-22 (102 by the
  end of 2026-09-23 — see the next section), including
  `test_set_config_round_trip_and_needs_restart` and
  `test_discover_by_direct_probe_finds_the_replica`.
  *Windows 2026-09-23:* 100 passed + 2 darwin-only skips, both named tests included.

## QCBridge — the sync after the audit (2026-09-23, then branch `spike/quinn`, now `main`)

One day of audit-driven work on the host↔replica sync, all of it proven on
the Mac only: `SYNC-AUDIT.md`, `COVERAGE.md` and `CACHES.md` in the QCBridge
repo are the record, `smokes/` the proof. Sixteen commits,
`e816205..e25040f`. The shape of what changed, for the Windows side to know
what to exercise: a fast QUIC lane for tier-1 deltas with a merge rule,
byte-based credits, reconnect/resync recovery, ~50 detection holes closed, a
shared cache root for simulations, local-edit detection on the replica, path
mapping from any host OS, a blob-digest gate that skips resends the replica
already holds, zstd moved from Blender to the agent (pipelined, level 1),
and a copy-free local link (`memoryview` chunks, `recv_into`). Nothing in it
is `#[cfg(unix)]`-gated, so the risk is behavioural, not build.

- [x] **The agent now links zstd (`zstd-sys`, C).** `cargo build` needs a C
  compiler in the MSVC toolchain (the Build Tools' `cl.exe`, which rustc's
  msvc target already wants for linking) — until now the agent's C was
  only ring's, which ships prebuilt objects. If the build fails in
  `zstd-sys`, that is the reason. Cold-lane payloads are compressed on the
  agent's blocking threads (four chunks in flight, sent in order, level 1)
  so Blender writes and loads `.blend` partials uncompressed; the contract
  test `test_cold_payloads_round_trip_byte_identical` is the proof it
  survives the wire, and the zmq transport keeps Blender-side compression.
  Watch the agent's CPU while a big blob crosses: the pipeline is what put
  the loopback blob time back where it was, and a Windows box with fewer
  cores will show it differently.
  *Windows 2026-09-23:* compiled first try with the Build Tools' `cl.exe` (found via vswhere, nothing on PATH). CPU during a blob not watched separately; see the bench's heavy row.
- [x] **Build and unit suite.** `cargo build --release` in `agent/`, then
  `python -m pytest -q` → **102 passed**, including
  `test_fast_lane_is_not_behind_a_cold_blob`,
  `test_agent_advertises_byte_credits` and `test_localize_any_…`. The
  discovery test binds UDP/4246 and fails while any replica agent is
  running on the machine — that is the port, not the code.
  *Windows 2026-09-23:* 100 passed, 2 darwin-only skips; the three named tests pass.
- [x] **The smoke runners are zsh.** `run_smoke*.sh`, `bench_latency.sh`,
  `coverage/run_coverage.sh` all assume zsh, `mktemp -d /tmp/…`, `kill -9`
  and (mapping) `ln -s`. The Python halves are portable; the runners are
  not. Either port them to PowerShell or run the two Blender halves by hand
  with the same arguments — but run them: the 21-check, `reconnect` (6
  checks: kill and restart the replica mid-session, drop a frame, edit the
  replica by hand), `cache` (5), `mapping` (5), and the coverage survey
  (123 actions, 120 of 122 surveyed cross on the Mac; its `t2`/`t1` columns are the blob
  and delta cost per action — read them, not just the status).
  *Windows 2026-09-23:* ported to CPython (`smokes/run_smokes.py`): 21/21, reconnect 6/6, cache 5/5, mapping 4/5 (the 5th is not expressible on a same-OS pair — notes), bench both transports, coverage survey 120 crossed / 2 not / 2 piggybacked — the Mac's rows exactly, median 249 ms; replica counted 8 bootstraps vs the Mac's 1 (Scene-level actions escalate to an unsupported Scene blob → auto bootstrap; cost, not correctness — flagged). The survey host's `os.replace` of its JSON hit a Windows rename-over-open-file error once; retried now (`smokes/_atomic.py`, `3a38b52`). QCBridge `spikes/parity/results/2026-09-23-windows-agent/`.
- [x] **The shared cache root on Windows paths.** `cache_root` (addon
  preference, `subtype='DIR_PATH'`) is joined with `os.path.join` and
  `os.makedirs`; the host writes `<root>/<file>/<uuid>/<sim>` and Blender
  writes `.bphys` frames there. Confirm a UNC path and a mapped drive both
  work as the root, that `bpy.path.abspath` on the preference resolves a
  `//`-relative root sensibly, and that the replica's "frames exist" test
  (`any(f.endswith('.bphys') …)`) sees files the host just wrote on the SMB
  share without a delay that makes it keep the cache in memory.
  *Windows 2026-09-23:* the cache smoke passes on a local root (5/5). UNC and mapped-drive roots, and the SMB timing: two-machine / the studio share.
  *Paired 2026-09-23, driven from the Mac seat (`ONE-SEAT.md`):* a Mac host's cloth cache externalized into a root on the studio SMB share and baked (24 `.bphys`); the Windows replica read it through the mapping row as a UNC root and again as a mapped drive — `frozen 0`, `errors 0`, no resync. SMB timing showed no stall on a 12×12 grid; a heavier sim is still worth a look. `spikes/parity/results/2026-09-23-triangle/`.
- [x] **The two cache hazards are Blender behaviour — confirm they hold on
  Windows.** (7) An unbaked external cache on the replica writes into the
  shared directory and poisons the host's bake; (8) re-setting
  `use_disk_cache`/`use_external` on an already-external evaluated cache
  and seeking wipes the directory. `probes/caches/shared_dir_*.py` reproduce
  both headlessly; run them with `S=<scratch>` set. If either differs on
  Windows, `bootstrap.localize_object_paths` is the code that relies on it.
  *Windows 2026-09-23:* both hold on Windows: poisoning 2 vs 24 files, z 2.994 vs 1.5408; wipe 24 → 2 → 0. QCBridge `spikes/parity/results/2026-09-23-windows-agent/`.
- [x] **`use_disk_cache` is ignored on an unsaved file** (probed on the Mac
  2026-09-18; the host now refuses to externalize with a panel note). Verify
  the same on Windows so the note is not a false alarm there.
  *Windows 2026-09-23:* holds: reads back `False`, bake writes 0 files.
- [x] **Path mapping from a mac host — the real cross-OS case.** A bootstrap
  carries the host's native paths; a Windows replica must translate
  `/Volumes/…` absolute paths through the table (`pathmap.localize_any`,
  unit-tested only) and count what it cannot resolve by *existence*
  (`os.path.exists` on the mapped path). This was the mapping smoke's
  finding and it cannot be exercised on one machine. Two machines, a mac
  host with an absolute texture path under the mapped root and one outside
  it: the first must load, the second must show as "1 unmapped" on both
  panels.
  *Paired 2026-09-23, driven from the Mac seat (`ONE-SEAT.md`):* exactly that, over the VPN: absolute image under the root loaded, the stray outside every mapping counted `unmapped 1` on the host panel. The finding on the way there: the row must be on **both** machines (the replica localizes with its own table); with it missing on the replica the count was 4 and Blender read `/Volumes/…` as `C:\Volumes\…`. User doc updated. `spikes/parity/results/2026-09-23-triangle/`.
- [x] **The reconnect smoke's expectations.** After the replica is killed and
  restarted, the host re-handshakes within ~1.5 s (QUIC idle timeout 3 s,
  keepalive 500 ms) and re-bootstraps; a dropped tier-1 frame is detected
  as a gap on the fast lane and the host ships a bootstrap unasked. On
  Windows, kill the replica's Blender *and* its agent (they are separate
  processes; `--exit-with-addon` takes the agent down with Blender when it
  exits cleanly, `taskkill /F` does not give it the chance).
  *Windows 2026-09-23:* reconnect 6/6; the replica's Blender is killed with its process tree (`taskkill /T`), so its agent goes too.
- [x] **Latency bench numbers to compare.** Mac loopback, end of day
  (`spikes/parity/results/2026-09-23-sync-latency/agent-after-pipeline/`):
  tier-1 ~105 ms p50, hot ~30 ms, sweep-path ~200 ms, a delta 150 ms
  behind a 640k-vertex blob 100–130 ms (no longer waits for it), the blob
  itself ~330 ms toggle→visible. The Windows numbers go next to the Mac
  ones; a tier-1 above ~150 ms or a sweep above ~300 ms means something in
  the tick/sweep path behaves differently there (timer resolution is the
  usual suspect), and a delta-behind-blob far above tier-1 means the fast
  lane or the merge rule is not doing its job.
  *Windows 2026-09-23:* t1 107/115, hot 29/44, sweep 187/273, hol150 112 — all inside the thresholds. Heavy blob 613 ms vs 327 on the Mac (~1.9x), while the zmq control on the same box does it in 369: the difference is the agent path's uncompressed 61 MB partial crossing Windows' loopback TCP twice (no stall, 16/16 crossed), not Blender. hol0 170–176 on both transports (write cost). Numbers and reading in the notes. QCBridge `spikes/parity/results/2026-09-23-windows-agent/`.
- [x] **The local link reads with `recv_into` and writes buffer lists.**
  Windows sockets support both; the one thing to confirm is that a 61 MB
  uncompressed partial crossing the loopback TCP link twice (host→agent,
  agent→Blender) does not stall on Windows' default socket buffers the way
  it does not on macOS — the `heavy` bench row is the measurement.
  *Windows 2026-09-23:* the heavy row crossed 16/16 with 0 lost; slower than the Mac for the reason above, no stall.
- [~] **The blob-digest gate assumes `libraries.write` is deterministic** for
  unchanged data — probed on the Mac (same bytes twice, after `update()`,
  after a move and back). If Windows builds write differently (pointer
  fields, padding), the gate never skips and the only symptom is cost: an
  undo costs ~34 blobs instead of ~6 in the survey's cost column. Run
  `QCB_COV_UNDO=1 QCB_COV_ONLY=obj_color,mesh_vertex_move,undo_after_move`
  and read the `t2` column for the undo row.
  *Windows 2026-09-23:* the gate skips on Windows — 27 of 102 tier-2 sends in the full survey were `unchanged blob`, so the writes are deterministic enough. The undo row itself could not be measured: `bpy.ops.ed.undo()` from the survey's timer crashes Blender 5.2.0 here (access violation in `deg_update_eval_copy_datablock`; the Mac's GUI only rewinds). Hand check: Ctrl-Z in a `QCB_DEBUG=1` host and count the skips. Flagged to the Mac side as a Blender-on-Windows difference. QCBridge `spikes/parity/results/2026-09-23-windows-agent/`.
- [x] **Linked libraries cross by path.** A `link` message names the
  library file in the host's native form and the linked object/collection
  names; the replica maps the path (`pathmap.localize_any`), links the
  names, and counts a file it cannot find as unmapped. Two machines: link
  an object from a `.blend` under the mapped root on the host and confirm
  it appears on the Windows replica; link one from outside every mapping
  and confirm "1 unmapped" on both panels. A library the replica had to
  repoint is `reload()`ed — watch for a Windows-specific stall there on a
  large library.
  *Paired 2026-09-23, driven from the Mac seat (`ONE-SEAT.md`):* a collection linked from a `lib.blend` under the mapped root appeared on the Windows replica through the UNC row and through the drive-letter row, `last_error` empty, no stall on a one-object library. The outside-every-mapping case was covered by the stray image in the same run. `spikes/parity/results/2026-09-23-triangle/`.
- [ ] **The replica's local-edit detector** is a `depsgraph_update_post`
  handler with time-based attribution (1.5 s touch grace, 3 s after a blob,
  0.5 s after a frame change). It has no platform code, but its false
  positives depend on how fast Blender's depsgraph reports after a blob on
  the machine; if the Windows replica shows "edited here" without anyone
  touching it, widen `_BLOB_GRACE` in `replica_apply.py` and say so.

## Packaging and shipping — the actual release gate (2026-09-22)

Building is not releasing. Each of the three has to produce something a user
can install on Windows, and none of that exists yet. This is the section that
decides when the coordinated release can happen.

### QCView

- [~] **A Windows package that installs on a clean machine.** The macOS half
  was rebuilt and proven this session (signed, notarized, stapled, 124 MB —
  `scripts/RELEASE.md` in that repo). There is no Windows equivalent of
  `sign-and-notarize.sh`. Decide MSIX or a plain installer, and whether it is
  signed.
  *Windows 2026-09-23:* `cmake --build build-release --target qcview_msix` works on this box (MakeAppx + SignTool from the 26100 SDK, windeployqt 6.11.1): `installer/msix/dist/QCView-2.3.4-win64.msix`, 173 MB, **unsigned** — the sideload signature needs `QCV_SIGNING_PFX` set, and signing is the owner's call. Installed and run here as a dev registration of the pruned 2.3.4 (the signed 2.3.3 removed and put back after): D3D11 + D3D11VA + Vulkan decode, 4K HEVC at 30 fps, exiftool through `exiftool_files/`, `--switch-test 60` with 56 dual entries, no missing-module warnings. Two things learned: the packaged 2.3.3 wrote **no log at all** (the pre-move location was Program Files — the tracker's guess, confirmed), and the packaged 2.3.4 writes `%LOCALAPPDATA%\QCView\logs\` un-virtualized. A clean machine is still owed.
- [~] **Does Sparkle's Windows story matter?** The macOS build auto-updates
  through the appcast at `https://qcview.app/appcast.xml`. If Windows has no
  update channel, say so in the release rather than leaving users to discover
  it.

### QCBridgeAE — phase A7

  *Windows 2026-09-23:* no update channel exists in the Windows package (Sparkle is macOS-only; the Store handles updates for Store installs, sideloads get nothing). To be said in the release notes.
- [ ] **Sign the Transmit bundle and build an installer.** Today, on both
  platforms, the plugin is copied into MediaCore by hand and is unsigned.
  A7's exit criterion is "installs clean on a machine that has never seen the
  SDK". macOS is untouched too, so this is a shared piece of work, not a
  Windows-only one — but Adobe's plugin loading is stricter on Windows and
  worth checking early.
- [x] **Where the plugin goes on Windows.** The macOS path is
  `/Library/Application Support/Adobe/Common/Plug-ins/7.0/MediaCore/`. Confirm
  the Windows equivalent and whether it needs admin.

### QCBridge (Blender)

  *Windows 2026-09-23:* `%PROGRAMFILES%\Adobe\Common\Plug-ins.0\MediaCore\`, confirmed — AE 2026 loads a `.prm` from there. Needs admin to copy (`packaging/windows/install-transmit.ps1` self-elevates), not to run.
- [ ] **Bundle the agent binary with the extension.** `find_agent` in
  `qcbridge/ring1/transport_agent.py` already looks in `qcbridge/bin/` first,
  before the cargo build dirs — that is the shipping path and nothing puts a
  binary there yet. One per platform.
- [ ] **Autostart the agent at login.** The design calls for a Windows logon
  task **in the interactive user session, not a service** — it launches
  Blender, which needs a desktop. macOS gets a login item. Neither exists.
- [ ] **Sign the agent.** Unsigned binaries that open listening sockets and
  launch other programs are exactly what endpoint protection objects to.
- [x] **Which addon version ships.** Decided 2026-09-23: the agent line is
  merged into `main`; the zmq transport stays as the fallback. The manifest
  says 0.2.0, matching the agent's Cargo version; the addon panel now warns
  when the two differ. Tagging is the macOS owner's release act.
- [ ] **The pyzmq wheel matrix.** The frozen zmq transport still needs a
  wheel per Python version in `qcbridge/wheels/`. The agent path is
  stdlib-only and needs none — so if the agent becomes the default, most of
  that matrix can go.

## Coming, not landed yet

Each will need a D3D11 twin when it lands on macOS. Nothing is queued here
right now — dual live landed on 2026-09-22 and has its own section above.

- *(empty)*

## Parked on the macOS side — not Windows work, but do not be surprised by it

- **Zero-copy ingest** (QCBridgeAE). Measured 2026-09-22 and parked: saves
  ~3–5 ms of CPU per 4K frame, nothing visible, and would need ring protocol
  v4. If Windows measures something very different, that is worth saying.
- **A/B follow mode** (QCView) — a B reference that follows the host
  playhead. Its own project, after the release.
- ~~**QCBridge native capture, S7**~~ — **done on 2026-09-23 after all, at
  the owner's call at the box** (this contradicts `PLAN-windows.md` §What
  is deliberately not here; the decision, not the plan, is recorded here).
  `agent/capture-win` (`0e942e9`): Desktop Duplication → D3D11 video
  processor → Media Foundation hardware HEVC, one system API like the Mac's
  SCK + VideoToolbox, not NVENC direct; same stdout/stdin contract as
  `qcb-capture-mac`. 4K60 at 3.5 ms/frame (3.0 Main10), 58 of 60 fps. Wired
  into the pixel path as *helper → ffmpeg mux → SRT* (`2927a38`), which the
  Mac inherits — flagged. Replica stream into QCView on Windows: d3d11va
  zero-copy, capture+mux ~0.05 cores. QCBridge
  `spikes/parity/results/2026-09-23-windows-agent/` §S7.

## Done

- [x] **QCBridgeAE A1–A4, A6, A3** — macOS. Listed for orientation, not as
  Windows work.
- [x] **QCBridge: Kyber removed, transport on plain quinn** (2026-09-22,
  macOS). 10/10 transport contract tests, 21/21 sync smoke, bootstrap A/B at
  parity with Kyber. The Windows verification of it is open, above.
