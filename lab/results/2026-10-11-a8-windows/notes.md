# A8 on Windows — audio over the bridge, built and verified on the box (2026-10-11)

Closes both A8 items in `lab/TRACKING-windows.md`. Everything ran from the
Mac seat over SSH; desktop steps through `C:\qcb-lab\run.ps1`.

## QCBridgeAE (main `b75a204`)

- `build-ae.cmd` (VS 18, MSVC 14.51, Ninja, Release): configure, build and
  **6/6 ctest pass** — `convert`, `ring_unit`, `audio_ring_unit`,
  `audio_tearing`, `audio_quiesce` and the rest (`windows-log-lines.txt`,
  "ctest"). The Windows create/replace path of the audio ring
  (CreateFileMapping + Retired retry) had never run before this.
- Inno produced `dist/QCBridgeAE-0.3.0-Setup-x64.exe` (2,133,168 bytes);
  copied to the Mac's gitignored `dist/`, SHA-256
  `9838cad8…314b83` identical on both machines. 0.3.0 is not tagged yet
  (chris's).
- The built `.prm` (61,440 bytes) was copied over the 0.2.2 release plugin in
  `C:\Program Files\Adobe\Common\Plug-ins\7.0\MediaCore\` (folder was
  writable from the SSH session, no UAC); the 0.2.2 file is parked as
  `C:\qcb-lab\QCBridgeAE-Transmit.prm.0.2.2-release`.
- Premiere Pro 2026 on the box, with the device's audio ticked by adding the
  pref `TransmitSerializedData…QCBridgeAE_45_Transmit.prm0EnabledSecondaryAudio`
  = true next to the existing `…0EnabledVideo` (Premiere not running; it
  reads the file at launch; a backup is `C:\qcb-lab\Premiere-Prefs-26.before-a8`).
  `EnabledSecondaryAudio` is the key the Mac's prefs carry for a third-party
  device's Audio Stream checkbox; Adobe's own Desktop Audio uses
  `EnabledAudio`.
- Project: the Mac's `a8-51` autosave (which has the 5.1 sequence) gunzipped,
  both clip paths rewritten to `C:\qcb-lab\media\…` with sed, gzipped back;
  the 5.1 clip regenerated on the box with the same ffmpeg line (winget
  ffmpeg n8.1). Premiere opened it without a relink dialog.
- Log on launch: `startup in Premiere Pro: ring /qcbae-premiere` then
  `audio ring /qcbae-premiere-audio created: 128 slots x 1024 frames x 16 ch
  (8204 KiB)`.
- Six plays of the 20 s 5.1 sequence across two Premiere launches, every one:
  `audio session: instance 18068 at 0.000 s, speed 1.00, 6 ch 48000 Hz` →
  `audio session ended: … 938 pushes, 938 packets, 960512 frames x 6 ch,
  0.000 s .. 19.989 s`. packets == pushes, frames == 1024 × pushes, as the
  tracker asked. Video alongside at 25.00 fps, push mean ~1.0 ms. A second
  launch (instance 47250) behaved the same as the first (18068).
- One oddity worth knowing: the very first space (16:16:08) opened an audio
  session and then Premiere toggled the device's video off/on ~20 times in a
  second and never called StopPushAudio for that session — a focus fight with
  the scheduled task's console window, which was in front at the time. The
  next StartPushAudio (16:18:39) started a clean session with correct counts,
  so a start-over-start is handled. Launching the desktop scripts with
  `-WindowStyle Hidden` removed the console and the flapping with it.

## QCView (main `ab4f0f67`)

- `cmake --build build-release` (Qt 6.11.1 msvc2022_64, VS 18): builds.
- `tests/audio/live_audio_source_test.cpp` compiled with `cl` against
  Qt6Core (`C:\qcb-lab\live-audio-test.cmd`, `/std:c++20 /Zc:__cplusplus
  /DNOMINMAX`): **PASS (0 failures)**, all 25 checks — the file-mapping
  open/retire path of the vendored audio ring on Windows.
- `qcview.exe qcbae://premiere` with `audio/livePlaydown` = true in the
  registry (`HKCU\Software\QCView\QCView\audio`): `WasapiAudioDevice:
  initialized (48000 Hz, 2 ch, buffer=1056 frames, 10 ms target)`,
  `LiveAudioSource: segment /qcbae-premiere-audio open (producer pid …, 128
  slots x 1024 frames)`, `AudioPlayer: live source playing, device buffer
  latency 22.0 ms`. The picture went LIVE at 1280x720 RGBA16F, 25 fps.
- Three back-to-back plays: `LiveAudioSource: 2000 packets, resync-dropped
  0, depth-dropped 18, scrub-silenced 0, queued 77 ms`. No resyncs, no gap
  signature; the 18 depth drops are the queue trimming to its target at
  the three session starts.
- Inspector, Audio card during the push (`qcview-windows-live-51.png`):
  State **Pushing**, 48000 Hz, 6 channels, Listen in QCView on, Mix Auto
  **5.1 → 2**, six meters labelled L R C LFE Ls Rs — the same card as the
  Mac's, on the WASAPI build.
- **Device latency, both OSes** (the new log line, QCView `ab4f0f67`):
  WASAPI 22.0 ms (1056-frame buffer) vs CoreAudio 10.7 ms on the Mac (512
  frames, `mac-latency-lines.txt`). Windows plays ~11 ms deeper before the
  Live offset; worth folding into the per-OS default when the offset gets
  one.
- Not done here: hearing the tone. The session has no ears on the box; the
  meters and the packet counts are the consumer-side evidence.

## Harness notes (Windows)

- `run.ps1` tasks show a console window that steals the foreground; launch
  with `powershell -WindowStyle Hidden -File …` or SendKeys and
  `AppActivate` go to the console.
- A non-DPI-aware PowerShell gets virtualised coordinates: `GetWindowRect`
  and `CopyFromScreen` then capture only the top-left part of a window at
  125 % scaling. Call `SetProcessDPIAware()` first and work in physical pixels
  (the desktop is 3840x2160 physical, 3072x1728 logical).
- Premiere: `Shift+3` focuses the Timeline panel, then Space plays; the
  sequence stops at its end. `Ctrl+Q` then Enter saves and quits. Launch to
  project-open is 60–90 s; wait for the window title to carry the project
  name before sending keys. A launch that starts while the previous
  Premiere is still quitting drops the project argument and lands on the
  Home screen; the project is then a click on its row under Recent.
- Window captures of QCView and Premiere only (`CopyFromScreen` of the
  window rect), never the whole desktop.

## Files

- `windows-log-lines.txt` — plugin log, QCView log and ctest excerpts.
- `mac-latency-lines.txt` — the Mac's CoreAudio counterpart.
- `premiere-windows-51-sequence.png` — Premiere playing the 5.1 sequence.
- `qcview-windows-live-51.png` — QCView LIVE on the feed during the push.
