# One seat drives both machines

*Set up 2026-09-23, the afternoon the two-seat flow was paused. The Mac
session drives the Windows box over SSH; the Windows Claude session is
for Windows-only build work in its own time, not for paired tests.*

## Why

The paired tests had a person in the loop for every hop: two sessions
that only talked through git and pasted messages, a merge that dropped a
day of tracker sections, and every "look at the Windows screen" step was
itself a remote-desktop hop over the VPN. With SSH from the Mac, leg 1
of Phase 4 ran end to end in twenty minutes with no hands on the Windows
side.

## The pieces

- **SSH.** Win32-OpenSSH on the Windows box (the Windows-feature route
  failed to download there; the GitHub MSI via `winget` worked), key
  auth only, the Mac's key in `administrators_authorized_keys` with the
  ACL that file needs. Shell is Windows PowerShell 5.1. An admin login
  gets the full token, so `New-SmbShare` and `schtasks` work without a
  UAC prompt.
- **`C:\qcb-lab`** on the Windows box holds the helper scripts; the
  copies of record are `lab/tools/win/` here.
  - `run.ps1 -Name x -Command "…"` runs a command **on the logged-on
    desktop**. A process started straight from SSH has no desktop and is
    killed when the SSH session ends; a one-shot scheduled task with `/it`
    runs in the interactive console session and survives. Use it for
    anything with a window (QCView, Blender) and for long builds.
  - `shot.ps1 [out.png]` captures the whole Windows virtual desktop to a
    PNG; run it through `run.ps1`, then `scp` the file back and read it.
    The D3D11 viewport composites through DirectComposition and may not
    appear in a GDI capture; the log lines are the evidence for pixels.
  - `open-stream.ps1` starts the Release build of QCView on the URL in
    `C:\qcb-lab\stream-url.txt`.
  - `build-qcview.cmd` rebuilds `build-release` with the MSVC environment;
    run it through a task (`qcb-lab-build`). The viewer must be closed
    first or the link fails on the open `qcview.exe`.
- **The replica agent** is a logon task ("QCBridge Agent"); `Start-ScheduledTask`
  from SSH restarts it after a config change. Its beacon on UDP/4246
  answers a direct probe from the Mac (`{"t":"q"}`), which is the quickest
  "is it up, is it paired" check.
- **`qcb-lab` share** on the Windows box (`C:\qcb-lab\share`) for the
  shared-root tests when no studio share is reachable from both networks.

## How a paired leg runs from the Mac

1. Probe the replica's beacon; restart its task if silent.
2. Start the host agent here (`~/Library/Application Support/QCBridge/agent.toml`,
   role host, peer and pinned fingerprint set from the probe, token typed
   by the owner). Silence in its log after the socket line means attached;
   the beacon's `paired` flips to true.
3. Run a scripted host Blender (`--python`) that starts the session in
   agent mode, makes the edits on a timer and dumps state to a JSON file;
   read the replica's stats off the pong (`peer_status`) and take a
   Windows screenshot for the picture.
4. Open QCView on Windows on the stream through `open-stream.ps1`, read
   its log for `LiveStreamDecoder: connected` and `LIVE`; then QCView here
   on the same URL across the VPN. **One viewer at a time:** the replica's
   SRT listener serves a single caller by design (`pixel_path.py`); the
   second connects when the first leaves.

## Rules that still hold

No addresses, machine names, tokens or passphrases in anything under
`lab/`. The stream URL carries a passphrase derived from the token: it
lives in `C:\qcb-lab\stream-url.txt` on the private box and nowhere else.
Pull before editing `TRACKING-windows.md`, on both machines.
