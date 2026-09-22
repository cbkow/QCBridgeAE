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
  `main` on 2026-09-22, at `d23bbf05`. It is local to the macOS machine
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

## QCView — threading fixes (branch `metal-source-race`)

Found with a ThreadSanitizer build on macOS. MSVC has no TSan, so on Windows
the check is a long harness run plus the real-app dual matrix.
`qcview --switch-test SECONDS LISTFILE` (one path or URL per line) randomly
switches media, enters and leaves dual and trims the timeline, then quits.
`QCV_SWITCH_SEED` replays a sequence; macOS used seed 3222196691.

- [ ] **Build and test `9c3874f6`: the D3D11 teardown handshake.** Written on
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

## Coming, not landed yet

Each will need a D3D11 twin when it lands on macOS:

- **Rotating upload textures.** The single CPU slot is overwritten by
  `replaceRegion` / `UpdateSubresource` while earlier frames may still sample
  it. D3D11's `UpdateSubresource` on a default-usage texture is ordered by the
  runtime, so check whether the hazard exists there at all before porting.
- **Dual live (live A, clocked B).** It also removes the stream gating from
  `eb323d05` for A.

## Done

(nothing yet)
