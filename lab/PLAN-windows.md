# The Windows plan — three repos, one release

*Written 2026-09-23 on the macOS side. A Windows session starts with
`WINDOWS-SESSION.md`; this is the sequence; the detail per item is in
`TRACKING-windows.md` (the checklist, ticked with evidence) and the standing
instructions are `HANDOFF-windows.md`.*

Three repositories ship together: **QCView** (the viewer), **QCBridgeAE**
(After Effects / Premiere → QCView) and **QCBridge** (Blender host → Blender
replica → QCView). Nothing goes out until all three are built, exercised and
packaged on Windows. There is no time pressure; there is a bar: **a green
build is not done — run the thing, measure it, write the numbers down.**
Two items on the checklist were merged on the Mac with "test on Windows"
unfulfilled and one would not even compile. Assume nothing has run on
Windows until you ran it.

## How the three fit

```
After Effects / Premiere ──(QCBridgeAE Transmit + shared ring)──▶ QCView
Blender host ──(QCBridge agent, QUIC)──▶ Blender replica ──(SRT, HEVC)──▶ QCView
```

QCView is the consumer of both. QCBridgeAE produces frames into a shared
ring QCView reads directly. QCBridge produces a video stream QCView reads
over SRT — and the sync between the two Blenders is a separate system that
must be right before the stream is worth looking at. So the order is: the
viewer, then the AE producer (the viewer's live path has nothing to show
without it), then the Blender bridge (self-contained: its own build, its own
tests, its own smokes), then the three together, then packaging.

## Phase 0 — the machine, the SDKs, the code

Entry: a Windows box with an NVIDIA GPU.

- Toolchains: MSVC Build Tools + CMake (QCView, QCBridgeAE); rustup with the
  MSVC toolchain **and a C compiler on the path** — the agent links
  `zstd-sys`, which compiles C (QCBridge); Python 3.13 with `pytest` and
  `pyzmq` (QCBridge's suite); Blender 5.2 LTS (QCBridge's smokes).
- The Adobe SDKs into `private/sdk/` (HANDOFF §Before anything builds). They
  are never in a repo.
- Clones: QCView `main`; QCBridgeAE `main`; QCBridge `main` (the agent line
  was merged into it on 2026-09-23; `spike/quinn` is behind it now).

Exit: all three configure. Nothing built yet is evidence of anything.

## Phase 1 — QCView builds and runs

Why first: it is the only one with a *known* compile error on Windows
(`read_ahead.cpp`, fixed on the Mac 2026-09-22, unconfirmed there), and the
other two are meaningless without a viewer to look at.

1. Build `main`. Confirm the NOMINMAX guard fixes the compile, then the
   `9c3874f6` dual-matrix path (`TRACKING-windows.md` → *the Windows build is
   broken on main*).
2. Verify the merged branches that never ran on Windows, in this order:
   threading fixes, network read-ahead, upload ring, dual live. Each has a
   section in the tracker saying what to look at and what "wrong" looks
   like. The live-source sections need a producer, which is Phase 2 — do
   the file-based verification now and come back.
3. Evidence: `lab/results/<date>-qcview-windows/notes.md` in **this** repo
   (the QCView repo's `docs/` is the public site, not a lab).

Exit: QCView opens files, plays, dual view works, and every non-live tracker
item under QCView is ticked or has a written reason.

## Phase 2 — QCBridgeAE A5: the producer

Entry: Phase 1's QCView runs.

`PLAN.md` A5 and HANDOFF §What's yours: the ring as a named file mapping,
liveness without `kill(pid, 0)`, F16C conversion (the main path on Windows —
the scalar fallback is 4.7× slower, measured), the Transmit device built
from the Premiere SDK's Transmitter sample and installed into MediaCore.

1. Ring + liveness + conversion first; `convert_test` must match the scalar
   path bit for bit; `qcbae-convbench` numbers to `lab/results/`.
2. The Transmit device. Then **re-measure host behaviour rather than
   inheriting the macOS findings** (32f pick, bottom-up frames, AE's alpha
   flattening, focus-loss behaviour, "never unloaded on quit") — the tracker
   lists each.
3. Now the QCView live sections from Phase 1: live sources, live in dual
   view, with this producer feeding them.

Exit: AE and Premiere show in QCView on Windows through Transmit; the
numbers are recorded next to the macOS ones with the unified-memory caveat
written down (a PCIe upload on a discrete GPU is expected, not a bug).

## Phase 3 — QCBridge: the agent, the addon, the suites

Entry: none of the above — this phase is self-contained and can run in
parallel with Phase 2 on a second person or a second day.

Two kinds of item here, and the checklist marks which is which: things
that might not *build or bind* on Windows (the beacon socket's
`SO_REUSEPORT`, `pid_alive` returning `None`, the tray, `COMPUTERNAME`,
`zstd-sys`), and things that are pure behaviour and just need to be *run*
(everything from the 2026-09-23 sync work: lanes, credits, recovery, the
cache root, path mapping, linked libraries, the local-edit detector).

1. `cargo build --release` in `agent/`; `python -m pytest -q` → 102 passed.
   The discovery test binds UDP/4246 and fails while any replica agent is
   running — that is the port, not the code.
2. The smoke runners are zsh (`smokes/README.md`). Port them or run the two
   Blender halves by hand with the same arguments — but run all of them:
   the 21-check, `reconnect` (6), `cache` (5), `mapping` (5), the latency
   bench, and the coverage survey (123 actions; 120 of the 122 surveyed cross on the Mac).
   Numbers go in `spikes/parity/results/` in the QCBridge repo, next to the
   Mac's, and the bench thresholds in the tracker say what "different"
   means.
3. The two-machine items, which one box cannot prove: cross-OS path
   mapping (a mac host's absolute paths on a Windows replica), discovery
   over a real LAN and over the VPN by direct IP, the shared cache root on
   an SMB share, linked libraries by mapped path. Pair with the Mac for
   these.
4. The Windows-only gaps that are real work, not verification:
   `pid_alive` (single-instance guard), the firewall rule for UDP/4246, the
   logon task that autostarts the agent in the interactive session.

Exit: every QCBridge item in the tracker ticked with evidence, or written
down as "differs on Windows, here is how".

## Phase 4 — the three together

Entry: Phases 1–3 exited.

The product is the triangle, and nobody has seen it on Windows:

1. Blender replica on Windows streaming SRT into QCView on Windows, with
   the host on the Mac and then on Windows (host and replica can be any
   pairing — never assume a direction).
2. AE on Windows into QCView on Windows while a Blender stream is also up:
   two live sources, dual view.
3. Glass-to-glass numbers for both paths, the way the Mac measured them
   (`spikes/parity/results/…/notes.md` shows the method: SRT latency is a
   ~1:1 additive, the encoder holds a fixed budget on top).

Exit: a written account of the triangle working on Windows, with numbers.

## Phase 5 — packaging, per repo

Entry: Phase 4. Building is not releasing; none of this exists on Windows
yet, and some of it does not exist on macOS either
(`TRACKING-windows.md` → *Packaging and shipping*).

- **QCView**: an installer that works on a clean machine; signed or stated
  unsigned; whether there is an update channel at all (the Mac has Sparkle)
  and, if not, saying so in the release.
- **QCBridgeAE A7**: sign the Transmit bundle, build an installer, confirm
  the MediaCore path and whether it needs admin. Shared work with the Mac.
- **QCBridge**: bundle the agent binary in `qcbridge/bin/` per platform;
  the logon task; sign the agent (it opens sockets and launches Blender —
  endpoint protection will object otherwise); the pyzmq wheel matrix only
  if zmq still ships.

One thing here is **a decision, not a task**, and it is the macOS owner's:
the version QCBridge ships as. The agent line is on `main` now; the
manifest still says 0.1.6, and the bump is a release act. Ask; do not
guess.

Exit: three installers that install on a machine that has never seen a
compiler or an SDK.

## Phase 6 — the release gate

All three checklists in `TRACKING-windows.md` ticked with evidence or with
a written "dropped, because". QCView 2.3.4 is already cut on macOS and is
waiting for this. The three go out on the same day, from the same set of
commits, with release notes that say what a Windows user gets and does not
get (an update channel, native capture) — honestly.

## How to report, and when to stop and ask

- Evidence lives in `lab/results/<date>-<slug>/` (this repo) for QCView and
  QCBridgeAE, and in `spikes/parity/results/` for QCBridge. `notes.md`
  always; `runs.jsonl` when you measured. Both folders are public: no job
  names, no user paths, no client pixels.
- Tick the tracker item with a pointer to the evidence. An item that
  *differs* on Windows is not a failure; it is a line in the notes and,
  if the difference matters, a flag back to the Mac side.
- If something contradicts `PLAN.md`, `HANDOFF-windows.md` or this file,
  say so in the notes rather than working around it. A wrong plan everyone
  follows is worse than a right one nobody wrote down.
- Stop and ask when: an SDK is missing; a Blender or Adobe behaviour
  differs in a way that changes what the code assumes; the branch question
  above comes up; or any measured number is off by more than the tracker's
  stated threshold and you cannot see why.

## What is deliberately not here

- **QCBridge native capture (S7)** — DDA/WGC → NVENC inside the agent. The
  hook exists (`qcb-capture-win.exe` beside the agent); the binary does
  not. Largest single piece of Windows work still ahead and **not a
  release blocker**: the ffmpeg capture path works — on the Mac. Its Windows
  twin (`ddagrab` → NVENC) is unproven and has its own tracker section; Phase 4
  item 1 is what proves it.
- **A/B follow mode** (QCView) and **zero-copy ingest** (QCBridgeAE) — parked
  on the macOS side; not Windows work.
