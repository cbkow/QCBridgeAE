# 2026-10-11 — A8: the app against Premiere, 5.1 and dual

Same machine and plugin as `../2026-10-10-a8-transmit-audio-product/`
(0.3.0 dev bundle, `db8bf2a`; QCView main `8b42c3ec`). Scratch project
`private/lab-scratch/a8-audio/prproj/a8-51.prproj` (the 2026-10-10
project with the clip path relinked to the durable folder).

## 5.1 material

`clip/probe-51-720p25.mov`: 20 s testsrc2 720p25 with six sines in a
5.1 layout, each channel a different frequency *and* level so the
order survives any fold —

| ch | label | Hz | amplitude |
|---|---|---|---|
| 1 | L | 440 | 0.10 |
| 2 | R | 554 | 0.20 |
| 3 | C | 659 | 0.30 |
| 4 | LFE | 60 | 0.40 |
| 5 | Ls | 880 | 0.50 |
| 6 | Rs | 1108 | 0.60 |

    ffmpeg -f lavfi -i "testsrc2=size=1280x720:rate=25" \
      -f lavfi -i sine=frequency=440:sample_rate=48000 ... (six sines) \
      -filter_complex "[1:a]volume=0.10[L];...;[L][R][C][LFE][Ls][Rs]join=inputs=6:channel_layout=5.1:map=0.0-FL|1.0-FR|2.0-FC|3.0-LFE|4.0-BL|5.0-BR[a]" \
      -map 0:v -map "[a]" -t 20 -c:v libx264 -pix_fmt yuv420p -c:a pcm_s24le probe-51-720p25.mov

Imported with Cmd+I, then File ▸ New ▸ Sequence From Clip: Premiere made
a 5.1 master sequence from it without a dialog
(`premiere-51-sequence.png`).

## The segment carries 5.1 in Premiere's order

`probe-51-forward.txt`, nine seconds of play from 0, the probe beside
QCView on the same segment:

| | |
|---|---|
| session | `6 ch 48000 Hz, push 1024 frames` |
| packets | 421, 0 resyncs, 0 frame gaps, 0 time gaps |
| cadence | 21.34 ms mean, 22.76 max |
| peaks | ch1 0.012, ch2 0.025, ch3 0.037, ch4 0.050, ch5 0.062, ch6 0.075 |
| plugin summary | `490 pushes, 490 packets, 501760 frames x 6 ch` — 1024 × 490 |

The peaks climb 1:2:3:4:5:6 with the clip's levels, so the planes arrive
as L R C LFE Ls Rs — the order `PrSDKAudioSuite.h` documents and the
order `LiveAudioSource::rebuildFold` and `sourceChannelNames` assume.
(Premiere's levels are ~0.125 × the file's, the same scale the stereo
run showed: 0.092 for a full-scale sine. Not ours.)

## QCView, single view (`inspector-51-single.png`)

Audio card during play: State Pushing, 48000 Hz, Channels 6, Listen on,
Mix **Auto 5.1 → 2**, six meters labelled L R C LFE Ls Rs rising left
to right with the clip. Log: `LiveAudioSource: LIVE — 6 ch 48000 Hz,
depth target 60 ms`.

## QCView, dual view with a live side (`dual-live-a-playing.png`)

A = `qcbae://premiere`, B = the 5.1 file, side-by-side (the file was
dragged from the Project panel onto the right half; in single mode a
drop always lands on A — `dropSideAt` returns A without a dual
controller — so side-by-side was entered first, with an empty B). Log:
`DualAudioMixer: opened — A=yes, B=yes`, `DualPlaybackController: opened
— A=premiere (1x1 …), B=probe-51-720p25.mov (1280x720, 500 frames)`.
Play in Premiere: A shows the pushed frames, B holds its frame, the six
live meters move; the plugin's session summary matches the pushes.
Per-side mutes are the speaker icons on the A/B chips.

**Gap seen:** in dual the Audio card hides State (by design: "the facts
belong to the side's own source") but still draws empty Sample rate and
Channels rows, because `liveSrc` binds `WindowManager.liveDecoder`,
which is null in dual. Either hide the two rows in dual or bind them to
`dualLiveA` / `dualLiveB`.

## A/V sync (Live) and Mute scrub audio (14:05–14:12, QCView rebuilt)

QCView got two log lines first so both are readable without ears: the
offset setter says what it set, and the reader says when it silences
scrub packets (counted in the periodic line too). Then, with the machine
free (`qcview-offset-and-scrub-lines.txt`):

- **Offset.** Settings ▸ Audio ▸ A/V sync (Live) is a read-only spin
  box (arrows only; press-and-hold auto-repeats). Held up to 110 ms:
  `live A/V sync offset 110 ms for qcbae://premiere (audio depth +110
  ms, video hold 0 ms)`, pref `audio.liveSyncOffsetMs.premiere` = 110,
  and the next play's reader line read `depth target 170 ms` — the 60 ms
  buffer plus the offset, consumed. Reverted to 0 with the row's ↺.
- **Scrub.** Mute scrub audio on; a press on Premiere's timeline ruler
  (`probe-51-scrub.txt`): the plugin opened `session 8: start 0.880 s …
  scrubbing` with two flagged packets (`audio session ended: 2 pushes, 2
  packets, 0.880 s .. 0.901 s`), and QCView logged `scrub packets
  silenced (Mute scrub audio is on)`. The chain holds end to end. A
  synthetic ruler *drag* moves the playhead on release but produces no
  scrub audio in Premiere (same class of limit as the J-K-L double taps),
  so a sustained scrub stays a by-hand check. Setting restored to off.
- **Dual Audio card fixed** (`inspector-51-dual-side-b.png`): with the
  file on A and Premiere on B, the Inspector's B side now shows State
  Pushing, 48000 Hz, 6 channels, Mix 5.1 → 2 and the meters — the card
  binds the inspected side's own source in dual.

## Harness notes

- `lab/tools/mac/scroll.c` added: `scroll X Y LINES` posts wheel events
  at a point (negative = down).
- QCView's Project panel rows are OS-level drags (`Drag.Automatic`,
  threshold 6 px); the `drag` tool's press → 10 ms moves → release
  works on them.
- Raise QCView's dev binary with System Events by pid
  (`set frontmost of (first process whose unix id is N) to true`);
  `open -a` would start a second instance.
- The Chrome profile picker floats above everything after Chrome is
  launched with no profile; a click meant for a window beneath it opens
  a profile and restores its session.
