# Run sheet — the three together (Phase 4), two machines

*Written 2026-09-23 on the Mac side, the day both sides caught up. This is
the order for the paired session. `PLAN-windows.md` Phase 4 says what
must be true at the end; this says what to do, in what order, and where
each result goes. Public folder: no addresses, no machine names, no job
material in what you write down.*

## What is already true

- All three repos are on `main` on both machines with the same versions:
  QCView 2.4.0, QCBridgeAE 0.2.0, QCBridge 0.2.0 (extension and agent).
- Phase 4 leg 1 has run **on one box**: a Windows Blender replica streamed
  HEVC over SRT into QCView on the same Windows machine (QCView notes,
  *srt:// live on Windows*). AE on Windows into QCView on Windows through
  the Transmit device has also run (A5 notes, the 13:00 addendum). What
  nobody has seen is the **two-machine** shape, and that is what this
  session is for.
- The Windows side listed exactly what it could not do alone: cross-OS
  path mapping, discovery over the LAN and by direct address over the
  VPN, the shared cache root on the SMB share (UNC and mapped drive),
  linked libraries by mapped path.
- The Mac reaches the tunnel (WireGuard is up; the Mac has its tunnel
  address; the gateway answers ping). No QCBridge agent answers on the
  gateway, which is expected — the Windows replica agent must be running
  before discovery has anything to find.

## 0. Prepare, both machines (15 min)

1. **The extension.** Build one package and install the *same file* on
   both: `blender --command extension build --source-dir qcbridge
   --output-dir <dir>` in the QCBridge checkout, then Blender → Preferences
   → Get Extensions → Install from Disk. Remove any older QCBridge first.
   Check the panel shows 0.2.0.
2. **The agent.** `cargo build --release` in `agent/` on each box (the Mac
   has done this; `qcbridge-agent --version` prints 0.2.0). Each box runs
   its own role: Windows starts as **replica** (`agent/windows/logon-task.ps1`
   or by hand with `--role replica`), the Mac as **host**. The firewall rule
   (`agent/windows/firewall-rule.ps1`, elevated) must be in before the Mac
   can probe UDP/4246 on Windows.
3. **The token** is typed into the replica's `agent.toml` and the host's
   panel by the owner, never pasted into a chat or a file in a repo.
4. **QCView** running on both machines from the current build.
5. **Results folders**, created up front so nothing is written elsewhere:
   `lab/results/2026-09-23-triangle/notes.md` in this repo for QCView and
   QCBridgeAE observations; `spikes/parity/results/2026-09-23-triangle/`
   in QCBridge for the sync and stream numbers.

## 1. Find each other (10 min)

1. Windows: replica agent running, `discovery = "direct"` (the default).
2. Mac: from the host panel, **Discover…** with the replica's tunnel
   address (the VPN path is direct probe, not multicast). Expect the
   replica's name and certificate fingerprint back. Pair; the host pins
   the fingerprint. Write down that the probe worked and how long the
   first attach took.
3. If both machines are ever on the same LAN, flip the replica to
   **Discoverable** and run **Discover…** with no address; that is the
   multicast item. If they are not, write "VPN only, multicast not
   exercised" and move on. Do not fake it.

## 2. Leg 1 — Blender host on the Mac, replica on Windows (45 min)

1. Open a small scene on the Mac host. Force Resync. The replica's Blender
   should launch on Windows (kiosk) and show the scene.
2. Spot-check the sync by hand, one of each kind: move an object (tier 1),
   add a modifier (tier 2), change a material colour, change the frame,
   rename an object, delete one, undo. Each must appear on the replica.
   The survey already ran on both boxes in loopback; this is the wire.
3. **Cross-OS path mapping.** Set one row in the host's path-mapping table
   (mac root ↔ win root of the same share). Open a scene on the Mac whose
   images and caches live under that root. On the replica: relative and
   absolute images resolve, the missing one is counted on the host's
   panel. This is the item the Windows notes flagged as unit-tested only.
4. **Shared cache root on SMB.** `cache_root` pointed at the share on both
   sides — once as a UNC path on Windows, once as a mapped drive. Bake a
   cloth or particle cache on the host; the replica reads the same frames
   with no resync (the cache smoke's five checks, done by hand). Watch for
   the two hazards written in `CACHES.md`: the replica must never write
   into the shared directory, and re-setting the disk flags must not wipe
   it. Note SMB timing if frames arrive late.
5. **Linked libraries by mapped path.** A scene that links a library
   `.blend` under the mapped root. The replica resolves the link through
   the mapping; a change in the library on the host side crosses.
6. **The stream.** QCView on **Windows** opens the replica's `srt://` (the
   replica listens; the URL form is in the QCView bridge doc). Then QCView
   on the **Mac** opens the same stream across the tunnel. Both must show
   the replica's viewport. Note the encoder rung and whether the native
   Windows capture helper or the ffmpeg path produced it (the replica's
   panel says).

## 3. Leg 1 reversed — host on Windows, replica on the Mac (20 min)

Same checks, fewer of them: pair, resync, three spot edits, the stream
into QCView on Windows from the Mac replica (the Mac uses its native
ScreenCaptureKit helper). This proves the direction assumption is not
baked in anywhere. Path mapping runs the other way round for free.

## 4. Leg 2 — AE on Windows into QCView on Windows, with the Blender stream up (20 min)

1. Leg 1's stream still running into QCView on Windows.
2. AE with the Transmit device enabled publishes into the ring; QCView
   opens the AE source as the second side (`qcbae://ae`) — dual view, one
   live side from each bridge. Side-by-side, then wipe, then difference.
3. The dual-live item in the tracker says what to look for (the D3D11
   side that used to freeze was fixed on 2026-09-23; confirm it stays
   live while the other side reconnects).

## 5. Numbers (20 min)

Glass-to-glass for both paths, the way the Mac measured them: the method
and the reference numbers are in `spikes/parity/results/2026-09-17-mac-vt-latency/notes.md`
(SRT latency is a ~1:1 additive; the encoder holds a fixed budget on
top) and the Windows loopback twin in `2026-09-23-windows-agent/notes.md`.
Two rows per path: same box, and across the tunnel. Write the encoder,
the rung, the SRT latency setting and the measured delay.

## 6. The by-hand checks each side still owes (10 min each, while there)

- **Windows, from its own notes:** wipe-drag smoothness while switching;
  slip / trim / slide deltas in dual; no B chip on a stream item; the
  media matrix from `2026-09-21-a3-qcview-ingest`.
- **Windows, from the Mac's items landed today:** Alt+Scroll pans the
  timeline; a drop on each viewport half and each lane lands on that
  side and lights it; dual view with nothing loaded shows the transport
  and two lanes; drag the viewport to move the window; a Chinese-named
  PNG sequence loads. Each has a tracker section saying what to look at.
- **Mac, from the Windows session's flags:** the two "flagged for the Mac
  side" lines in the Windows notes (a replica frame count that differed;
  a conversion edge in the A5 spine). Read them before the session.

## When to stop

A pairing that never attaches, a sync that silently diverges, a stream
that connects but shows the wrong picture, or a cache written into the
share by the replica. Any of these is the session's finding; write it up
with the log lines and stop that leg rather than working around it.

## Exit

Phase 4's exit line, written: the triangle working on Windows, with
numbers, in the two results folders; the two-machine items ticked in
`TRACKING-windows.md` with pointers; whatever differed, flagged.
