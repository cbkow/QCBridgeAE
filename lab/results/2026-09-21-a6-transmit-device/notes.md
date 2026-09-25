# 2026-09-21 — A6: the Transmit device (in progress)

macOS 26.7, Apple Silicon, After Effects 2026 (26.5), Premiere Pro SDK 26.0.
Device: `src/transmit/qcbae_transmit.cpp`, pass: `src/common/convert/half_convert.*`.

## The conversion pass

One pass from a host frame to the wire: flip (bottom-up), reorder
(ARGB/BGRA → RGBA), IEEE convert to half — never clamp (DESIGN-NOTES D4).
`qcbae-convbench`, 3840×2160, this machine:

| | ms / frame | fps |
| --- | ---: | ---: |
| bare NEON 32f → half (no flip, no reorder, no checks) | 1.75 | 572 |
| **A6 pass, ARGB bottom-up (AE)** | **2.31** | 432 |
| **A6 pass, BGRA bottom-up (Premiere)** | **2.33** | 429 |
| A6 pass, portable fallback | 23.5 | 43 |

**First attempt was 3.5× slower (6.1 ms).** It de-interleaved into channel
planes with LD4/ST4 and classified inf/NaN on every vector. Plain LD1 +
FCVTN/FCVTN2 + one TBL byte shuffle per two pixels, with a cheap per-row
"any exponent all ones" check and exact classification only for rows that
trip it, brought it to 1.3× a bare conversion. The comment claiming the
reorder was free had been written before measuring; the benchmark said
otherwise.

`tests/convert_test.cpp`: the portable conversion matches hardware FCVT bit
for bit over ~1M patterns (NaN payloads, signed zero, subnormals, the inf
boundary). Frames are checked against a true image read back the way QCView
reads the ring. Mutation-checked — every deliberate break caught, after one
that was not: removing half the row check passed, because every test row had
a special value that tripped the check anyway. Test 4 (a lone non-finite at
every position) was added for it.

Note on mutation testing here: make's one-second timestamps let a swapped
source "rebuild" without recompiling; the first round of mutations silently
tested the original code. Delete object, library and binary each time.

## First runs in After Effects

Product installed in place of the probe (same bundle filename, new GUID).

- **AE carried the probe's "enabled" over to the product** despite the new
  GUID and `outVideoDefaultEnabled = false` — possibly keyed by bundle
  filename. Not investigated further.
- Frames arrive and read correctly: RGBA16F, RGBA order, **top-down**
  (the orientation marker now reads at the bottom), values the exact nearest
  halves of the 8 bpc solids (89/255 → 0.349121; 191/255 → 0.749023), the
  50% solid (0.3999, 0.2000, 0.1020, 1).
- **AE churns instances while opening a project**: one created, activated,
  deactivated and disposed for nearly every item it touches — a dozen in a
  second. A last-writer-wins host state could end "paused" with video
  running; the device now derives state from the set of instances with
  video on.
- **AE does not unload the module when it quits.** The instance is disposed
  (after a focus-loss deactivation), but `xTransmitEntry(unload)` never runs
  and neither do static destructors, so the ring is neither marked Retired
  nor unlinked. It survives the quit, readable, its host state frozen at
  "paused: focus". The producer cannot fix this; **a consumer must treat a
  ring whose `producer_pid` is dead as offline, before looking at
  `host_state`.** `qcbae-probe dump` does (it flagged exactly this case).

## Premiere alongside AE, and a frame-dropping optimisation

Premiere opened with the product enabled while AE was running:

- **Two rings, no collision.** `/qcbae-premiere` and `/qcbae-ae` each hold
  their own host's frame. Premiere: orientation correct, straight alpha
  carried (0.7998, 0.3999, 0.2000, **A 0.501953** = nearest half of 128/255),
  real time (1.6016 s), label "Premiere Pro".
- **Premiere has the same background preference.** Before it was unticked,
  every focus loss sent event 3 and the ring read "paused: host lost focus";
  after, Premiere stayed active in the background. Both apps need the same
  setup step.

**AE then went silent** — an active 4K instance, AE busy, nothing pushed into
the ring during playback or scrubbing. Two explanations were on the table:
output ownership between apps (Chris: AJA and Blackmagic behave that way),
or the device's own frame skipping, which dropped any frame whose PPix unique
key matched the last one — silently; skips were logged only every 500 frames.

Discriminating run: the skip removed, AE restarted, **Premiere still open**.
AE pushed every frame — 3 scrubbed 4K frames, then playback. **The cause was
the skip.** In AE the unique key evidently does not identify content, so
every frame after the first was discarded as a "duplicate". It was an
unmeasured saving that could drop frames without a trace, and is gone. (AE
and Premiere *can* both transmit to this device at once — unlike the
hardware devices Chris has seen.)

## 4K playback through the device

AE, the 8 bpc 4K comp, cached preview playback, Premiere also open, two runs:

| | fps into the ring | AE CPU |
| --- | ---: | ---: |
| run 1 | 24.09 | 0.57 cores |
| run 2 | 23.97 | 0.56 cores |

Real time, and AE's CPU matches the A4 baseline for plain 8u delivery
(0.57) — the 2.3 ms conversion on the push thread is lost in the noise.
Values are the exact nearest halves: mover (0.9, 0.6, 0.1) → 0.901855 /
0.600098 / 0.101990; background (0.2, 0.3, 0.4) → 0.199951 / 0.302002 /
0.399902.
