# Handoff — Windows machine

Standing instructions for the Windows side. Keep this file current; it is the
contract between the two machines. The checklist of open Windows work, in both
QCBridgeAE and QCView, is `TRACKING-windows.md`.

## Before anything builds

**Get your own copy of the Adobe SDKs.** They are not in this repo and never
will be — the headers carry an ADOBE CONFIDENTIAL notice (`PLAN.md` §Privacy).

1. After Effects SDK (25.6 or newer) from Adobe's developer site.
2. Premiere Pro SDK — only needed for phase A4 onward (the Transmit route).

Unpack both into `private/sdk/` on the Windows machine. That path is
gitignored, so it stays local. The macOS side has the same layout, mirroring
the `minColor` repo's convention.

## What's yours

Phase **A5** (`PLAN.md`): Windows parity for the ring, the Transmit device
(the product since 2026-09-21, `PLAN.md` D7) and, as a test instrument, the
AEGP tap.

The macOS side does **not** use IOSurface any more — it is a page-aligned
POSIX shared mapping (`PLAN.md` D8, measured zero-copy on Apple Silicon). The
same shape on Windows is a **named file mapping**, and that is the first thing
to try: `src/common` is written for it. The fallback, only if a mapping cannot
back a texture without a staging copy:

- `ID3D11Texture2D`, `DXGI_FORMAT_R16G16B16A16_FLOAT`
- created with `D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX`
- handed to the consumer as an NT handle; opened via `OpenSharedResource1`
- `IDXGIKeyedMutex` for producer/consumer sync — an alternative to the ring's
  seqlock + reader claim, not an addition

Expect one PCIe upload on a discrete GPU. That is understood and accepted; do
not chase it. The macOS number will look better because Apple Silicon has
unified memory, not because the Windows code is wrong. Record both, note why
they differ.

## The 32 bpc conversion needs F16C

Under Transmit, v1 asks the host for 32f and converts everything to half in
the plugin (`PLAN.md` D1, Transmit note), so on Windows this is the main path,
not an edge case. It must use **hardware** conversion: `_mm256_cvtps_ph` (F16C, on every x86-64 part
since 2012). The portable scalar fallback measured **12.55 ms vs 2.67 ms** at
4K on the macOS side — 4.7x, or ~80 fps versus ~374. The same gap will show up
on x86 if the intrinsic isn't used.

Texture formats for the three tiers: `DXGI_FORMAT_R8G8B8A8_UNORM`,
`DXGI_FORMAT_R16G16B16A16_UNORM`, `DXGI_FORMAT_R16G16B16A16_FLOAT`. All three
sample as `float4` through one shader, the same way they do in Metal — but
**verify it rather than assuming**, the way `tests/texture_format_test.mm`
does on the macOS side. That equivalence is what makes per-tier formats cheap,
and if D3D11 doesn't give it to you for free, say so before working around it.

Run `qcbae-convbench` on your hardware and put the numbers in `lab/results/`.
The ratios may not survive the trip off unified memory, and if they don't, D1
deserves revisiting for Windows specifically.

## Plugin install locations

| | Path |
| --- | --- |
| AEGP / effect | `%PROGRAMFILES%\Adobe\Adobe After Effects <ver>\Support Files\Plug-ins\` — on macOS AE also loads AEGPs from MediaCore (A2); untested here |
| Transmit (shared — also loads in Premiere) | `%PROGRAMFILES%\Adobe\Common\Plug-ins\7.0\MediaCore\` |

## Reporting back

New folder under `lab/results/<YYYY-MM-DD>-<slug>/` with `notes.md` and, if you
measured anything, `runs.jsonl`. Re-read `lab/README.md` first — that folder is
public, and "the client's 4K spot" is not a thing that can appear in it.

If you hit something that contradicts `PLAN.md`, say so in your notes and flag
it rather than quietly working around it. A wrong plan that everyone follows is
worse than a right one nobody has written down yet.
