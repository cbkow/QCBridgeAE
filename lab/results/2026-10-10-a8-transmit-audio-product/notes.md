# 2026-10-10 — A8: audio over the bridge, the product

Same machine and hosts as the probe run (`../2026-10-10-a8-transmit-audio/`).
Product: `src/transmit/qcbae_transmit.cpp` at the commit that adds this
folder, declaring push audio and writing into the audio segment
(`src/common/surface/audio_ring.*`, DESIGN-NOTES D6). Consumer: the new
`qcbae-probe audio --ring premiere`, reading the segment the way QCView
will. Project: the probe run's `a7-audio.prproj` (the 20 s 440 Hz tone
clip), "Audio Stream" ticked for the device.

## The segment behaves

`probe-audio-forward.txt`, nine seconds of play from 0:

| | |
|---|---|
| packets | 429 in the window, 1024 frames each, 2 ch 48 kHz |
| resyncs / drops | 0 / 0 |
| frame gaps | 0 — `first_frame` contiguous throughout |
| time gaps | 0 — every `time_value` within one sample of the previous packet's end |
| cadence | mean 21.34 ms, max 23.70 ms |
| peak | 0.092 both channels (the tone) |
| plugin summary | `429 pushes, 429 packets, 439296 frames x 2 ch, 0.000 s .. 9.131 s` — matches |

`probe-audio-reverse.txt`: J-K reverse, `speed -1.00` in the session,
194 packets with timeline running 9.080 → 4.963 s, 0 resyncs, cadence
unchanged. (The probe's contiguity check was made sign-aware after this
run; the 193 "time gaps" in the file are that, not the data.)

## Two things the probe run had not shown

1. **A device that declares push audio must wire `QueryAudioMode`.**
   With `outPushAudioAvailable` and the entry left null, Premiere
   created each instance and disposed it at once — no video-mode query,
   no activation, no frames, so the picture path was dead too (log,
   09:43–09:47). The A8 probe never hit this because it wired the entry.
   The product now answers it with the instance's own format.
2. **Premiere disposes placeholder instances while opening a project**,
   which retires the audio segment the way the last instance retires the
   frame ring. The segment is therefore re-created when absent at
   `CreateInstance` and `StartPushAudio` (host thread), not only at
   Startup.

## The "Audio Stream" hint

`probe-audio-unticked.txt` + `transmit.log` 09:50:04: box unticked,
play → no session, no packets, and after twelve pushes in Playing mode the
device sets `host_audio = Off` and logs it. `probe-audio-reticked.txt`:
box ticked again, next play → session 4, `host_audio = On`. A viewer can
turn Off into "tick Audio Stream for QCBridgeAE → QCView in Premiere ▸
Preferences ▸ Playback".

## Not measured here

Speeds beyond ±1 and loop wrap (same keystroke limits as the probe run);
Windows (tracked in `lab/TRACKING-windows.md`).
