# Tracking — what Windows still owes

The running list of Windows work, in both QCBridgeAE and QCView. The macOS
side adds an item whenever it lands something that Windows has to build,
port or verify. The Windows side ticks it off with a pointer to its evidence
(a `lab/results/` folder, a commit). `HANDOFF-windows.md` is the standing
instructions; this file is the checklist.

Same rule as the rest of `lab/`: public. No job names, no user paths, no
client pixels.

Status: `[ ]` open, `[~]` in progress, `[x]` done (with evidence), `[-]`
dropped (with the reason).

## Getting the code

- [ ] QCView: everything below (the `qcbae-live`, `metal-source-race` and
  `dual-network-read` branches, stacked) was fast-forwarded into QCView
  `main` on 2026-09-22 (now at `597559df`, with the upload ring). It is local to the macOS machine
  until chris pushes. Branch names below say where each item was developed.

## QCBridgeAE — phase A5 (PLAN.md)

The producer side, so this comes first.

- [ ] **Ring as a named file mapping.** `src/common/surface/shared_ring.cpp`
  is POSIX (`shm_open` / `mmap` / `ftruncate`). Port it to
  `CreateFileMapping` / `MapViewOfFile` with the same geometry: header padded
  to a page, page-aligned slots, `slots_offset` in the header. Decide the
  object name (`Local\` + the ring name?). AE and QCView share a session, so
  `Global\` shouldn't be needed. Say what you chose in the notes.
- [ ] **Liveness without `kill(pid, 0)`.** A dead producer's ring persists
  (measured on macOS: neither host unloads the device on quit). The consumer
  checks `producer_pid`; on Windows that is
  `OpenProcess(SYNCHRONIZE)` + `WaitForSingleObject(h, 0)`. Keep the
  "Retired means reopen" rule.
- [ ] **F16C conversion** for `convert_32f_to_rgba16f`: the main path, see
  HANDOFF §F16C. Run `convert_test` (it must match the portable scalar bit
  for bit) and `qcbae-convbench`; results to `lab/results/`.
- [ ] **The Transmit device on Windows.** Build `qcbae-transmit` as the
  Premiere SDK's Transmitter sample does, and install it in MediaCore
  (HANDOFF's table).
- [ ] **Re-measure host behaviour; don't inherit it.** macOS findings
  (`lab/results/2026-09-21-a4-transmit-probe/`): the host picks 32f when
  offered; frames arrive bottom-up; AE flattens alpha over the comp colour,
  while Premiere sends straight alpha; AE's inTime is -1 and always
  "scrubbing"; the colour request is ignored under OCIO; focus loss stops the
  stream unless the background preference is unticked; the device is never
  unloaded on quit; each viewer change pushes 2 frames. Any of these can
  differ on Windows.
- [ ] **`qcbae-probe`**: `produce` and `dump` should port with the ring;
  `view` is Metal and needs a D3D11 twin, or skip it and use QCView as the
  viewer.

## QCView — live sources (branch `qcbae-live`)

- [ ] **Enable the host bridge on Windows.** `src/decode/CMakeLists.txt`
  builds `HostBridgeSource` and defines `QCV_HAS_HOST_BRIDGE` on APPLE only.
  The vendored `src/decode/qcbae/shared_ring.*` gets the same Windows port as
  QCBridgeAE's (keep the two identical; `kFrameDescVersion` guards a
  mismatch). Then enable it for Windows.
- [ ] **Build check: `c7fef457`** (D3D11 uploads `Format_RGBA16FPx4` as
  `R16G16B16A16_FLOAT`), written on macOS and not compiled there. Verify
  over-range and negative values reach the canvas unclamped: sample a pixel,
  don't eyeball it.
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

- [ ] **Confirm the `NOMINMAX` fix.** `src/decode/read_ahead.cpp` included
  `<windows.h>` with no guard while calling `std::min`/`std::max` with plain
  arguments in four places, so MSVC could not compile it: windows.h defines
  those as function-like macros. Fixed on the Mac by the same
  `WIN32_LEAN_AND_MEAN` + `NOMINMAX` idiom every other file here uses — but
  fixed blind. **A Windows build is the only thing that proves it**, and it is
  the first thing to try, because nothing else in the file has ever been
  compiled either.
- [ ] **The read-ahead may be inert on Windows.** `read_ahead.cpp` uses
  `SetThreadPriority(THREAD_MODE_BACKGROUND_BEGIN)`, which throttles I/O far
  harder than macOS's `IOPOL_UTILITY`. Check the `ReadAhead: … window warm`
  log actually reports a useful fetch rate rather than crawling.
- [ ] **Live in dual freezes a D3D11-decoded side.** `DualFrame` has `Cpu`,
  `Metal` and `Vulkan` kinds but no D3D11 one, so
  `dual_live_source.cpp:186-190` falls through to `default:` and the side
  holds its previous frame for ever, silently. An SRT stream decoded through
  D3D11VA on one side of dual is the case. Needs either a D3D11 `DualFrame`
  kind or an honest status instead of a frozen picture.

## QCView — threading fixes (branch `metal-source-race`)

Found with a ThreadSanitizer build on macOS. MSVC has no TSan, so on Windows
the check is a long harness run plus the real-app dual matrix.
`qcview --switch-test SECONDS LISTFILE` (one path or URL per line) randomly
switches media, enters and leaves dual and trims the timeline, then quits.
`QCV_SWITCH_SEED` replays a sequence; macOS used seed 3222196691.

- [ ] **Build and test `9c3874f6`: the D3D11 teardown handshake.** RELEASE
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
- [ ] **Shared-code commits, verify on Windows:**
  - `bde75934`: a B change in dual rebuilds the island. This goes through the
    Windows-only `m_dualSourceAdapter` teardown and cold entry. Clearing B
    returns to single view.
  - `45f10658`: the timeline snapshot. Slip / trim / slide in dual still
    shows every drag delta.
  - `f482da50`: atomic fetch counter. (Its screenshot half is Metal-only;
    D3D11's capture reads the ComPtr-held slot and needs no change.)
  - `eb323d05`: no B chip or dual controls for a stream item.
  - `d28dc540`, `aaeb1ade`: the harness itself should build.
- [ ] Long run: `--switch-test 600` on a Release build with the Windows media
  set. Pass = no crash, no hang, and "done" logged.

## QCView — network read-ahead (branch `dual-network-read`)

Branched from `metal-source-race`. On a network volume an uncached frame
arrives slowly (measured on macOS: ~60–70 MB/s through LucidLink's local
SMB gateway, shared by every reader). `ReadAhead` (`src/decode/read_ahead.*`)
warms the byte ranges of the next ~2 s of frames from the container index.

- [ ] **Build check: `e7e7995f` and the touch rewrite after it.** Written on
  macOS. The read-ahead now touches one byte per 1 MiB page (LucidLink's
  cache page) rather than copying frames. The Windows-specific part is
  `SetThreadPriority(THREAD_MODE_BACKGROUND_BEGIN)` for the worker; reads go
  through `QFile` (unbuffered). macOS also sets `F_NOCACHE`; Windows has no
  equivalent for a 1-byte read (`FILE_FLAG_NO_BUFFERING` needs aligned
  sector-sized reads), so there the OS may keep the touched page, which is
  harmless.
- [ ] **Does LucidLink on Windows hydrate from a touch?** On macOS
  `lucid3 cache` showed the on-disk cache grow by the file's size after a
  touch of an uncached file. Repeat on Windows; the client may mount the
  filespace differently.
- [ ] **Does it help on Windows?** Open an uncached large file on a network
  share with `QCV_READAHEAD_SECONDS=0` (off) and at the default. Compare how
  quickly a paused seek and the first seconds of playback fill; the
  `ReadAhead: … window warm` log lines give the fetch rate. Background mode
  also lowers I/O priority on Windows; check that it doesn't starve the
  read-ahead on an idle machine.
- [ ] **Loop-range hydration** (commit after `b116e372`): loop on + in/out
  set touches the whole range, in single and dual. Shared code; verify the
  `ReadAhead: … loop range … touched` log line appears and the range plays
  smoothly on the next pass.
- [ ] `bc8ca1c4` only adds dual-decoder diagnostics (slow read, read EOF,
  empty-ring stall); nothing to port.

## QCView — upload ring (branch `rotating-upload-textures`)

- [ ] **Does D3D11 need a twin?** macOS landed `MetalUploadRing`: CPU frames
  go into a ring of textures so an upload never overwrites one a frame in
  flight samples. On macOS the old single-texture path overwrote an
  in-flight texture 1 in ~1000 uploads at 4K60 with OCIO, and 0 at 1080p60.
  D3D11's `UpdateSubresource` on a default-usage texture is ordered by the
  runtime (it copies or waits as needed), so the hazard may not exist there;
  confirm that before porting anything. The dual compositor's other change,
  skipping the re-upload of an unchanged frame every vsync, is worth
  checking on D3D11 regardless (`d3d11_dual_compositor.cpp`).

## QCView — live sources in dual view (branch `dual-live`)

Merged into QCView `main` on 2026-09-22 (verified on macOS with the real
hosts). A live side is a `DualLiveSource`
(`src/dual/dual_live_source.*`): it owns its own receiver and hands its latest
frame back for any master frame. Live can be either side or both; two live
sides have no clock (`dualSeekable` false) and the transport hides.

- [ ] **Build check.** New file in `qcv_dual`; `LiveSource::setSink` now takes
  a `LiveFrameSink*` (`decode/live_source.h`), which `VideoDecoder`
  implements. `DualLiveSource` has a `Q_OS_WIN` branch that clones a Vulkan
  AVFrame the way `DualVideoDecoder` does; that path has never been compiled.
- [ ] **srt:// in dual on Windows.** `qcbae://` stays macOS-only until the
  ring port above, but SRT works on both: put a stream on one side and a file
  on the other, check the file side still drives the transport and the live
  side updates. D3D11 renders on demand, so the frame-available callback is
  what wakes it — if the live side only repaints when you move the mouse,
  that callback is not reaching the renderer.
- [ ] **Live + live**: transport and timeline hidden, both sides updating.

## QCView — packaging changes that touch Windows (2026-09-22)

The macOS release flow was rebuilt (`scripts/`, committed now — it used to be
gitignored, which is how the originals were lost with the old Mac). Three of
those changes are not macOS-only:

- [ ] **The log moved out of the app bundle.** `installFileLogger` wrote next
  to the executable, which on Windows is `Program Files` — never writable, so
  released builds almost certainly had no log at all. It is now
  `%LOCALAPPDATA%/QCView/logs/` (`QCV_LOG_DIR` overrides). Confirm a packaged
  Windows build actually writes there.
- [ ] **Qt Multimedia was dropped** from `find_package` (nothing used it).
  windeployqt should stop shipping Qt's media plugin and its FFmpeg; check the
  MSIX shrinks and nothing breaks.
- [ ] **Pruning.** `scripts/prune_bundle.sh` is macOS-shaped (frameworks,
  otool). If windeployqt is as generous as macdeployqt was — it deployed Qt3D,
  PDF, the virtual keyboard and a second FFmpeg — the Windows package may
  deserve the same treatment.

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

## QCBridge — the agent after the quinn port (2026-09-22, branch `spike/quinn`)

Kyber is gone; the transport is plain `quinn`. The Mac side builds, passes
10/10 transport contract tests and 87/87 pytest. None of it has been compiled
on Windows.

- [ ] **Build the agent on Windows.** `cargo build` in `agent/`. The old
  Rust 1.89 pin came from Kyber and can relax; the Mac builds on 1.98.1.
  There is no `build-win.*` script and no external clone to make any more —
  `HANDOFF-windows.md` task 4 is superseded.
- [ ] **Run the contract tests there.** `python -m pytest -q` → expect 87
  passed, 0 skipped. `tests/test_transport_agent.py` spawns two
  `qcbridge-agent` processes itself, so it needs only the built binary. If it
  *skips*, the binary was not found — that is a fail, not a pass.
- [ ] **Check the per-instance config change.** Cert and `agent.json` now
  derive from the config file's directory, so `--config` isolates an
  instance. Confirm the default path still resolves to
  `%APPDATA%\QCBridge\` and that two agents with different configs do not
  share a certificate.
- [ ] **21-check smoke over the agent transport**, now committed as
  `smokes/`: `QCB_TRANSPORT=agent QCB_AGENT=spawn`. The scripts are zsh and
  macOS-shaped (`${0:a:h}`, `/Applications/...`); a Windows equivalent is
  its own task. `$BLENDER` overrides the binary.
- [ ] **Re-run the transport A/B.** `bootstrap_bench.sh <w> agent 8` and
  `... 40` against `zmq` as control. The Kyber baselines this replaced are
  quoted in `results/2026-09-22-quinn-port/notes.md`; the directories that
  held them were deleted.
- [ ] **Cross-machine rungs.** Everything measured so far is loopback, where
  SRT's latency buffer looks like pure overhead. mac↔win with real loss is
  what decides how low that setting can actually go.

## Coming, not landed yet

Each will need a D3D11 twin when it lands on macOS:

- **Dual live (live A, clocked B).** It also removes the stream gating from
  `eb323d05` for A.

## Done

(nothing yet)
