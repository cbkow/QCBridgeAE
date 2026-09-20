# Handoff — Windows machine

Standing instructions for the Windows side. Keep this file current; it is the
contract between the two machines.

## Before anything builds

**Get your own copy of the Adobe SDKs.** They are not in this repo and never
will be — the headers carry an ADOBE CONFIDENTIAL notice (`PLAN.md` §Privacy).

1. After Effects SDK (25.6 or newer) from Adobe's developer site.
2. Premiere Pro SDK — only needed for phase A4 onward (the Transmit route).

Unpack both into `private/sdk/` on the Windows machine. That path is
gitignored, so it stays local. The macOS side has the same layout, mirroring
the `minColor` repo's convention.

## What's yours

Phase **A5** (`PLAN.md`): Windows parity for the surface spine and the AEGP tap.

The macOS side uses IOSurface, which has no Windows equivalent. Yours is:

- `ID3D11Texture2D`, `DXGI_FORMAT_R16G16B16A16_FLOAT`
- created with `D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX`
- handed to the consumer as an NT handle; opened via `OpenSharedResource1`
- `IDXGIKeyedMutex` for producer/consumer sync — this replaces the macOS
  ready-signal, so the sidecar channel carries no sync role on Windows

Expect one PCIe upload on a discrete GPU. That is understood and accepted; do
not chase it. The macOS number will look better because Apple Silicon has
unified memory, not because the Windows code is wrong. Record both, note why
they differ.

## Plugin install locations

| | Path |
| --- | --- |
| AEGP / effect | `%PROGRAMFILES%\Adobe\Adobe After Effects <ver>\Support Files\Plug-ins\` |
| Transmit (shared — also loads in Premiere) | `%PROGRAMFILES%\Adobe\Common\Plug-ins\7.0\MediaCore\` |

## Reporting back

New folder under `lab/results/<YYYY-MM-DD>-<slug>/` with `notes.md` and, if you
measured anything, `runs.jsonl`. Re-read `lab/README.md` first — that folder is
public, and "the client's 4K spot" is not a thing that can appear in it.

If you hit something that contradicts `PLAN.md`, say so in your notes and flag
it rather than quietly working around it. A wrong plan that everyone follows is
worse than a right one nobody has written down yet.
