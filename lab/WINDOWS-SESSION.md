# Instructions for the Windows session

*Written 2026-09-23 on the macOS side, for whoever — person or agent — sits
down at the Windows machine. Everything you need is in three repositories'
`main` branches and in this folder. Read this file first, then the three it
points to, in that order.*

## Read these, in this order

1. **This file** — how to work, what to do on day one, what not to do.
2. **`PLAN-windows.md`** — the sequence across the three repos, six phases,
   each with an entry and an exit condition. It says *when* to do what.
3. **`HANDOFF-windows.md`** — the standing instructions: the Adobe SDKs,
   the ring on Windows, F16C, install paths, how to report.
4. **`TRACKING-windows.md`** — the checklist. Every item is something the
   Mac side landed that Windows must build, port or verify, with what
   counts as evidence. You tick items; nobody else does.
5. **`RUNSHEET-triangle.md`** — once both sides are caught up (they are,
   as of 2026-09-23 afternoon): the paired session, step by step, for
   Phase 4 and the two-machine items.

Then, per repo, the documents the checklist cites when you get there:
QCView's `scripts/RELEASE.md`; QCBridgeAE's `PLAN.md`; QCBridge's
`SYNC-AUDIT.md`, `COVERAGE.md`, `CACHES.md`, `smokes/README.md` and
`ARCHITECTURE.md` (read the banner at its top: it predates the rest).

## The three repositories

| repo | what it is | branch | what Windows owes |
|---|---|---|---|
| **QCView** (`QCView-Player`) | the viewer | `main` | a build (known compile fix to confirm), verification of five merged branches, a package |
| **QCBridgeAE** | After Effects / Premiere → QCView | `main` | phase A5 (the ring, F16C, the Transmit device), phase A7 packaging |
| **QCBridge** | Blender host → replica → QCView | `main` | build the agent, run 102 tests, six smoke suites, the survey, the bench; the two-machine items; the Windows-only gaps; packaging |

All three `main` branches were brought up to date on 2026-09-23. QCBridge's
`main` now carries the agent line that used to live on `spike/quinn`; do
not check out that branch, it is behind `main`. Pull `main` in each repo
before anything else — if a pull shows nothing new, ask the Mac side
whether it pushed.

## How to work

- **A green build is not done.** Run the thing, measure it, write the
  numbers down. Two items on the checklist were merged with "test on
  Windows" unfulfilled and one would not even compile. Assume nothing has
  run on Windows until you ran it.
- **Evidence goes in the repo, in public folders.** QCView and QCBridgeAE
  results: `lab/results/<YYYY-MM-DD>-<slug>/notes.md` in QCBridgeAE (this
  repo), `runs.jsonl` when you measured. QCBridge results:
  `spikes/parity/results/<YYYY-MM-DD>-<slug>/` in that repo, next to the
  Mac's. These folders are public: **no job names, no client names, no
  user paths, no client pixels, no machine names.** Re-read `lab/README.md`
  once before your first notes file.
- **Tick the checklist with a pointer** to the evidence (a results folder, a
  commit). An item that behaves *differently* on Windows is not a failure;
  it is a line in the notes and, if it matters, a flag to the Mac side.
- **Commit your work on `main` in each repo; push only if you were told
  you may.** The Mac side pushes its own commits; ask before pushing yours
  the first time. Never force-push, never rewrite history.
- **Pull before you edit `TRACKING-windows.md`, every time.** Both sides
  write to it during the day. A merge that keeps one side's copy silently
  drops the other's sections (it happened on 2026-09-23; the Mac restored
  them). If a pull conflicts in that file, keep both sides' sections — the
  headings are dated and never overlap.
- **The Adobe SDKs never enter a repo.** `private/sdk/` is gitignored; keep
  them there.
- **Secrets stay in the credential store.** Signing keys, notarization or
  code-signing passwords are entered by the owner, never pasted into a
  chat, a file or a commit.
- **Do not delete build directories or results without asking.** `git
  clean`, `rm -rf` on a `build/` or `target/`, or on anything under
  `results/`, needs an explicit yes.
- **When something contradicts a document, say so in the notes** rather
  than working around it. A wrong plan everyone follows is worse than a
  right one nobody wrote down.

## Day one, concretely

Follow `PLAN-windows.md` Phase 0 and the start of Phase 1. In practice:

1. Toolchains: MSVC Build Tools with the C++ workload and CMake; rustup
   with the `x86_64-pc-windows-msvc` toolchain (**the agent links
   `zstd-sys`, which compiles C — the Build Tools' `cl.exe` must be on the
   path when `cargo build` runs**); Python 3.13 with `pytest` and `pyzmq`;
   Blender 5.2 LTS; an NVIDIA driver current enough for NVENC.
2. Clone all three, `main` each. Put the two Adobe SDKs in QCBridgeAE's
   `private/sdk/`.
3. **QCView first**: configure and build `main`. The first thing to learn
   is whether the `read_ahead.cpp` NOMINMAX fix from the Mac actually
   compiles here (`TRACKING-windows.md` → *the Windows build is broken on
   main*). If it does not, that is the first results folder of the day,
   with the compiler output.
4. While QCView builds, **QCBridge can start in parallel** (Phase 3 is
   self-contained): `cargo build --release` in `agent/`, then
   `python -m pytest -q` from the repo root → **102 passed** is the bar.
   Know two things about that suite: the transport tests spawn the agent
   binary themselves (`find_agent` picks the newest build by mtime — if a
   test ever says `unknown cmd`, check which binary it spawned); and the
   discovery test binds UDP/4246 and fails while any replica agent is
   running on the machine — that is the port, not the code.
5. Write the day's notes even if the day was "it built" or "it did not".

## What to expect from QCBridge specifically

- **The smoke runners are zsh.** `run_smoke*.sh`, `bench_latency.sh`,
  `coverage/run_coverage.sh` assume zsh, `mktemp -d /tmp/…`, `kill -9`,
  and the mapping smoke uses `ln -s`. The Python halves are portable. Port
  the runners to PowerShell, or run the two Blender halves by hand with the
  same arguments (each runner shows exactly how it launches them). Either
  way, run all of them — the 21-check, `reconnect` (6 checks), `cache` (5),
  `mapping` (5), the bench, and the survey (123 actions, 120 of 122
  surveyed cross on the Mac; read its `t2`/`t1` cost columns, not just the
  status).
- **The Mac's numbers, so you know what "different" looks like** (loopback,
  640k-vertex blob): tier-1 delta ~105 ms p50, hot frame ~30 ms, sweep-path
  edit ~200 ms, a delta 150 ms behind the big blob 100–130 ms, the blob
  itself ~330 ms. A tier-1 above ~150 or a sweep above ~300 means the
  tick/sweep path behaves differently (timer resolution first); a
  delta-behind-blob far above tier-1 means the fast lane or the merge rule
  is not doing its job.
- **Two-machine items cannot be proven on one box** and are listed as such:
  cross-OS path mapping, discovery over a real LAN and by direct IP over
  the VPN, the shared cache root on an SMB share, linked libraries by
  mapped path. Pair with the Mac side for those; do not fake them.
- **Real Windows work, not verification**: `pid_alive` returns `None` on
  Windows so the single-instance guard cannot refuse a second agent
  (`OpenProcess` + `WaitForSingleObject`); a firewall rule for UDP/4246
  (MinRender's installer shows the pattern); the logon task that
  autostarts the agent in the interactive session (not a service — it
  launches Blender).
- **Blender behaviours the code relies on, probed on the Mac, to
  re-confirm here**: `use_disk_cache` is ignored on an unsaved file; an
  unbaked external cache on the replica writes into the shared directory
  (finding 7); re-setting the disk/external flags on an evaluated cache
  wipes it (finding 8); `libraries.write` is deterministic for unchanged
  data (the blob-digest gate assumes it). `probes/caches/` has the scripts;
  each is a few seconds headless.

## What to expect from QCView and QCBridgeAE

- QCView's live path has nothing to show until QCBridgeAE's Transmit device
  exists on Windows (Phase 2). Verify the file-based paths first (threading
  fixes, network read-ahead, upload ring, dual view) and come back to live
  and dual-live once there is a producer.
- QCBridgeAE A5 is the largest single port: the ring as a named file
  mapping, liveness without `kill(pid, 0)`, F16C conversion (the scalar
  fallback is 4.7× slower — measured; `_mm256_cvtps_ph` is not optional),
  the Transmit device from the Premiere SDK's Transmitter sample into
  MediaCore. `HANDOFF-windows.md` has the specifics and the fallback if a
  mapping cannot back a texture. **Expect one PCIe upload on a discrete
  GPU**; do not chase it, write both numbers and why they differ.
- Re-measure the host behaviours the Mac found (32f pick, bottom-up
  frames, alpha handling, focus loss, "never unloaded on quit") rather
  than inheriting them. Any of these can differ on Windows.

## Decisions that are not yours

Two things will come up that the Mac owner decides, not the Windows
session. Ask, do not guess:

- **Versions are decided (2026-09-23):** QCView 2.4.0; QCBridgeAE 0.2.0
  and QCBridge 0.2.0 (extension and agent alike). The numbers are in each
  repo now; what remains a release act is tagging and publishing.
- **Signing.** Whether QCView's Windows package, the Transmit bundle and
  the agent are signed, and with what. Keys are not something you will be
  handed in a chat.

## When to stop and ask

An SDK is missing. A Blender or Adobe behaviour differs in a way that
changes what the code assumes. A measured number is off by more than the
thresholds above and you cannot see why. A checklist item would need you
to delete or rewrite something. Any of the two decisions above.

## What is deliberately out of scope

Native capture for the agent on Windows (DDA/WGC → NVENC; the hook exists,
the binary does not) is the largest piece of Windows work still ahead and
**not a release blocker** — the ffmpeg capture path works. A/B follow mode
in QCView and zero-copy ingest in QCBridgeAE are parked on the Mac side.
