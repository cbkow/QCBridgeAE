# 2026-09-23 — QCView on Windows: the build, the harness, the merged branches

Machine: AMD Ryzen Threadripper PRO 7955WX, 256 GB, NVIDIA GeForce RTX 5090
(driver 32.0.16.1088, Vulkan 1.4.341, D3D11 feature level 11.1), Windows 11
Pro 26200. MSVC 19.44, Qt 6.11.1 msvc2022_64, vcpkg, the repo's vendored
FFmpeg (`external/ffmpeg-win64`), Ninja, Release. QCView `main` at
`49d189c2` plus the two commits below.

Test material: synthetic `testsrc2` files made with the vendored ffmpeg —
1080p60 H.264 60 s with audio, 2160p30 HEVC 10-bit 30 s, 1080p24 ProRes
422 HQ 20 s, 720p30 H.264 20 s, and a 96-frame 1080p PNG sequence. No
client material anywhere near this.

## The build (tracker: *the Windows build is broken on main*)

- **`read_ahead.cpp`'s NOMINMAX guard is confirmed**: the file compiles as
  merged from the Mac.
- **It was not the only one.** `video_decoder.cpp` failed the same way
  (C2589 on `std::max`, line 1506): `907f18ec` (the read-wait stats, also
  from the Mac) added a plain `std::max` to a file that reaches
  `<windows.h>` through `libavutil/hwcontext_vulkan.h` → `vulkan_win32.h`,
  with no guard. Same `WIN32_LEAN_AND_MEAN` + `NOMINMAX` idiom, added before
  that include. QCView `e30bf137`. After it, `main` links; three C4858
  warnings in `window_manager.cpp` (discarded `QThreadPool::start` QFuture)
  are the only compiler noise.
- The host bridge is enabled on Windows in `01577fc2` (see the A5 folder in
  this directory); it builds and the app starts with it in.

## Runs

### First run: open a file, hold, close

D3D11 + DirectComposition swapchain up, D3D11VA attached to H.264
(codec-routed, shared device, zero-copy NV12 → RGBA16F), Vulkan device with
video-decode queues for ProRes, the frame at 0. Closed from the window:
clean exit. **The log lands in `%LOCALAPPDATA%\QCView\logs\qcview-log.txt`**
— the tracker's *log moved out of the app bundle* item, confirmed for a
built binary (the MSIX is Phase 5; the code path is the same).

On close the log fills with QML `TypeError: Cannot read property … of null`
from `Main.qml`, `StatusStrip.qml`, `ColorPanel.qml`, `NotesPanel.qml`,
`TimelinePanel.qml`: bindings evaluating after the context object is gone.
Teardown noise, not Windows-specific by the look of it, and nothing else in
the log is a warning. Mentioned so nobody chases it as a Windows bug.

### `--switch-test 600` (tracker: threading fixes, `9c3874f6`)

`qcview --switch-test 600 <list>` on the five items above, seed
`2160967491`, Release build.

| | |
| --- | --- |
| steps | **1417** in 600 s (999 activate, 209 set B, 101 single view, 108 clear B) |
| dual entries / exits | 443 / 443 (`dual entry — single-flow vulkanBridge torn down` / `single entry — vulkanBridge restored`) |
| result | `--switch-test: done, 1417 steps`, exit 0 |
| crashes, hangs | none |
| non-QML warnings or errors in 32,630 log lines | **0** |

So the D3D11 teardown handshake — `sourceMutex` held across
`consumeLatestVideoFrame` and each draw, the source setters taking it, the
lock order at `d3d11_player_renderer.cpp:294` that had never run — survived
1417 random switches with 443 dual entries and exits, cold and warm,
across D3D11VA (H.264/HEVC), Vulkan (ProRes) and CPU (PNG) sources, with
head-trim bursts while dual was open. No deadlock, no stall, "done"
logged. The wipe drag is a human check and still owed by hand.

The shared-code commits the tracker lists (`bde75934` B change rebuilds the
island; `45f10658` timeline snapshot; `f482da50` atomic fetch counter;
`eb323d05` no B chip for a stream; `d28dc540`/`aaeb1ade` the harness) are
exercised by the same run: the harness itself builds and runs, B changes
and clears go through the Windows-only `m_dualSourceAdapter` teardown 443
times, and the trims are the head-trim bursts. Slip/trim/slide *display*
and the stream chip are visual checks, owed by hand.

### Playback (tracker: read-wait stats, the D3D11VA and Vulkan paths)

Neither harness presses play (`--playlist-test` builds and activates a
playlist, nothing more), so each file was opened on the command line and
given the play key from a script, 24 s each. `907f18ec`'s stats line, every
2 s:

| file | decode path | `VideoDecoder: playing` | read waits |
| --- | --- | --- | --- |
| 1080p60 H.264 | D3D11VA, NV12 zero-copy, shared device | **60.0 fps** | 4–5 ms per 2 s (0 %), slowest 0 |
| 2160p30 HEVC 10-bit | D3D11VA, P010 zero-copy | **30.0 fps** | 4 ms (0 %) |
| 1080p24 ProRes 422 HQ | Vulkan video (FFmpeg-managed pool), yuv422p10 zero-copy | **24.0 fps** | 4–5 ms (0 %) |

Codec routing is as the log says it should be: H.264/HEVC → D3D11VA "if the
codec has it", Vulkan reserved for ProRes. No warnings in any of the three
logs. The read-wait numbers are local NVMe and say only that the stats
path works; on a network share they are the number to read.

## The merged branches, item by item

**Network read-ahead (`dual-network-read`).** Builds; the worker runs with
`THREAD_MODE_BACKGROUND_BEGIN`; every open logs `ReadAhead: … window warm`
(1393 lines in the switch run). On this machine the media is on a local
NVMe, so the numbers are cache hits (3.9 MB in 0.00 s) and say nothing
about the tracker's real questions — **does LucidLink on Windows hydrate
from a 1-byte touch, and does background I/O priority starve the
read-ahead on a network share?** Those need the studio's filespace mounted
on this box, which it is not today. Left open, and flagged: this is the one
Windows-specific behavioural unknown in the QCView list.

**Upload ring (`rotating-upload-textures`).** The question was whether D3D11
needs a twin of `MetalUploadRing`. It does not for the reason the tracker
guessed: the CPU path goes through `UpdateSubresource` on a default-usage
texture, which the D3D11 runtime orders against in-flight draws (it
copies to a staging area or waits), so an upload cannot overwrite a
texture a queued draw samples. No hazard, no port. The dual compositor's
"skip re-upload of an unchanged frame" is shared logic and ran under the
switch test. Not measured for the 1-in-1000 tear the Mac saw because the
D3D11 path cannot exhibit it by construction.

**Live sources / dual live (`qcbae-live`, `dual-live`).** Build check
passed, including the `Q_OS_WIN` branch of `DualLiveSource` that clones a
Vulkan AVFrame — it compiles now, which it never had. `c7fef457`
(`Format_RGBA16FPx4` → `R16G16B16A16_FLOAT`) compiles. Behaviour — over-range
values reaching the canvas unclamped, the range override on the D3D11 CPU
slot, `srt://` in dual, live + live — needs a producer: the Transmit device
(blocked on the Premiere SDK) or a QCBridge SRT stream (Phase 4). Open.

**Live in dual freezes a D3D11-decoded side.** Confirmed by reading:
`dual_live_source.cpp` handles `Cpu`, `Metal`, `Vulkan` and falls to
`default: return` for a D3D11VA frame, so an SRT stream decoded through
D3D11VA on one side of dual holds its last picture silently. Not fixed
today — the honest-status or D3D11-kind change wants the SRT-in-dual run
to test against, which is Phase 4. Open, with the line number.

**Packaging changes (2026-09-22).** Log location: confirmed above. Qt
Multimedia dropped: the build no longer links it (nothing in
`build-release` references `Qt6Multimedia`). Pruning and the MSIX size:
Phase 5.

## Owed by hand (visual)

Wipe-drag smoothness while switching; slip/trim/slide deltas in dual; no B
chip on a stream item; the media matrix from
`2026-09-21-a3-qcview-ingest` (LiveStrip, Inspector facts, drag and drop,
New Project). The harness does not see pixels; a person at the machine
does. Ten minutes with the synthetic set, when someone is at the box.
