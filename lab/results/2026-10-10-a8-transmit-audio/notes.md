# 2026-10-10 — A8: audio over the bridge, the Transmit probe

Premiere Pro 2026 (26.0) and After Effects 2026 on macOS 26, Apple Silicon,
Premiere Pro SDK 26.0 (Transmit interface v4). Probe:
`src/transmit/qcbae_transmit_probe.cpp`, installed under the product's
bundle path (so both hosts keep their per-path "enabled" state), with the
audio keys added to its config. `probe.log` is the whole session, Premiere
first, After Effects after the `=== AE LAUNCH` marker.

Test material: a synthetic 20 s 1280x720 25 fps H.264 clip with a 440 Hz
sine at 48 kHz, made with the vendored ffmpeg:

    ffmpeg -f lavfi -i "testsrc2=size=1280x720:rate=25" \
           -f lavfi -i "sine=frequency=440:sample_rate=48000" -t 20 \
           -c:v libx264 -pix_fmt yuv420p -c:a aac -b:a 192k probe-tone-720p25.mp4

The questions, from the discussion the same morning: does Premiere push
audio to a device that declares *only* push audio (no primary device, no
clock of ours), what does it look like, and does After Effects touch the
audio API at all.

## 1. Premiere pushes audio to a push-only device — once the user ticks a box

Declared: `outPushAudioAvailable` true, `outAudioAvailable` false,
`outClockAvailable` false, `outHasStreaming` false.

- Preferences → Playback shows the device in the **Transmit Device
  Playback** table with an **Audio Stream** checkbox next to its Video
  Stream checkbox. **Primary Audio Device stays "Adobe Desktop Audio".**
  Adobe's own SRT output has the same checkbox; the NDI plugin (which
  acquires the play-module audio suite, i.e. the pull path) does not.
- The state is a pref keyed by bundle path, like the video one:
  `TransmitSerializedData<escaped path>0EnabledAudio`.
- Unticked (run 1): playback pushes video at 25 fps, **no** `StartPushAudio`.
- Ticked (run 2): `StartPushAudio` at every play start, `PushAudio` for the
  whole run, `StopPushAudio` at stop. Streaming was never enabled and never
  needed.
- `QueryAudioMode` is called once per instance even with audio declared
  off (harmless; index 0, answered with 2 ch 48 kHz).
- `ActivateDeactivate` reports `audio 0` throughout, even with the box
  ticked. The audio flag there is about the primary device.

## 2. What the pushes look like

From `StopPushAudio` summaries and the first pushes of each run:

| | |
|---|---|
| format | `float**` planar, 2 ch (the sequence's), `tmInstance` says 32-bit float 48 kHz |
| size | **exactly `outSamplesPerFrame`**: 1024 when asked 1024, 2048 when asked 2048 |
| cadence | real time: 1024 every 21.33 ms, 2048 every 42.67 ms; max jitter 1.1 ms over 382 pushes |
| timestamps | `inTime` = timeline position of the first sample, contiguous push to push (the per-push delta is 1024 samples less ~0.4 µs of rounding; the probe's exact-match "GAP" flag fired on every push, so treat < 1 sample as contiguous) |
| vs video | same timeline; audio `inTime` within −27…+27 ms of the last `PushVideo` `inTime` (one frame either side); wall-clock 0–44 ms after it |
| thread | arrives while `PushVideo` is running (as the header says) |
| level | peak 0.088–0.092 throughout: real samples, not silence; a handful of silent pushes at the sequence end and in reverse |
| speed −1 | `StartPushAudio` speed −1.00, timestamps run backwards, cadence unchanged, samples still present |
| scrubbing | `StartPushAudio` with `scrubbing 1`, in/out = one frame, one push for that frame |
| play start | `inStartTime` = playhead; in/out = sequence bounds; loop 0 |
| pause | `StopPushAudio` within the same stop |

Not measured: speeds other than ±1 (the J-K-L double-tap did not take by
synthetic keystroke) and loop wrap (Ctrl+L did not take either). Both are
host matters that would show as a `StartPushAudio` speed value and a time
jump between pushes; nothing in the design depends on them.

Module reset during playback: the host created the new instance, then
called `StopPushAudio` on the old one, then disposed it. Push state has to
live per instance.

## 3. After Effects does not do Transmit audio

Declared everything: pull audio, push audio, clock. A 1280x720 comp from the
same clip (`hasAudio true`), preview played for 240 frames at 25 fps.

- `tmInstance`: `audio 0, 0 ch, sample type 0, 0 Hz`, no play ID, no timeline ID.
- No `QueryAudioMode`, no `StartPlaybackClock`, no `StartPushAudio`, no
  `PushAudio`. Only video, as before.

So AE's Transmit is video-only, by measurement, not recollection. AE audio
over the bridge is shelved; if it ever matters, the driver-free route is
process-scoped OS capture in the viewer (WASAPI process loopback, Core
Audio process taps), with measured rather than sample-tied sync.

## What this settles for the design

- The Premiere path is the **mirror** mode: push audio only, no primary
  device, no clock of ours, Adobe Desktop Audio keeps playing. The editor
  ticks one box in Playback preferences.
- The device chooses the push granularity. Something near a video frame
  (1024–2048 at 48 kHz) is right; the host honours it exactly.
- Timestamps are the sync: every push carries its timeline position, and
  video frames carry theirs. A consumer aligns by those; nothing needs a
  clock in between.
- The layout is planar float in, which is what Core Audio (non-interleaved)
  and NDI's audio frame take as-is. Only a WASAPI shared-mode render would
  interleave, at the very end.
- `PushAudio` runs on the host's high-priority audio thread concurrently
  with `PushVideo`: the ring write for audio must not take the video lock
  and must not block.

## Windows

The probe builds as a `.prm` with the same source; the push path should be
re-run on the box when Premiere is available there (tracked in
`lab/TRACKING-windows.md`).
