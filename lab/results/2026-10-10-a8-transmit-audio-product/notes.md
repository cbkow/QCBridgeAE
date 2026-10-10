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

## QCView reading the same segment (12:36–12:43)

QCView main at 4854dc0e + 8b42c3ec (`LiveAudioSource`), opened on
`qcbae://premiere` while the probe tool read the segment alongside it —
two consumers, no reader claim, neither disturbs the other.

- Play: the Inspector's Audio card shows State Pushing, 48000 Hz,
  2 (stereo), meters moving; the probe beside it saw 282 packets in 6 s,
  0 resyncs, 0 frame gaps, 0 time gaps, cadence 21.34 ms mean / 22.72 max,
  peak 0.089 both channels. Stop: State Idle. QCView's own counter after
  ~43 s of play: 2000 packets, resync-dropped 0, depth-dropped 0.
- Listen off → on (the Inspector switch; pref `audio.livePlaydown` = 1):
  meters identical either way, as designed — the output is zeroed after
  the drain, not before. Hearing the tone is Chris's check (Premiere's
  own output was moved to the MacBook speakers to keep the two apart).
- Reverse (J): session at speed −1.00, 240 packets in 5 s.
- Quit Premiere mid-play (Cmd+Q): Premiere never reaches the plugin's
  DisposeInstance / module unload (no "retired" or "unload" line in the
  log, same as the frame ring has always behaved), so both segments are
  left behind; QCView removes them as a dead producer's
  ("removed the segment a dead producer left"), stays up, and on relaunch
  reopens both against the new pid within a poll. Play then flows again
  (187 packets / 4 s, 0 resyncs).
- Found and fixed (8b42c3ec): the live meters held the last packet's
  level after a stop; the reader now clears them after 100 ms without a
  packet.

## Not measured here

Speeds beyond ±1 and loop wrap (same keystroke limits as the probe run);
a 5.1 sequence (routing pills and the fold), dual view with a live side,
the A/V sync (Live) offset and scrub mute in the app — all built and
unit-tested, not yet exercised against Premiere; Windows (tracked in
`lab/TRACKING-windows.md`).
