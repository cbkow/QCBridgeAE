# QCBridgeAE: an After Effects beauty window for QCView

Committed 2026-09-20 (chris). Repo copy of the plan; running findings live in
`lab/`, which is also the channel to the Windows machine.

## What this is

An After Effects comp, live in QCView, as **untransformed pixel data** — so
QCView's OCIO chain, A/B wipes and EDR paths operate on the signal AE actually
computed, not on a Rec.709 rendering of it.

**This is not QCBridge for AE.** The only parity is what the user sees at the
far end: a live item in QCView, correct bit depth, OCIO engaged, A/B working.
Everything upstream is different:

| | QCBridge (Blender) | QCBridgeAE |
| --- | --- | --- |
| Machines | Two — host + GPU replica | **One.** AE and QCView side by side |
| Transport | SRT / QUIC over LAN or VPN | **Shared GPU surface.** No network code at all |
| Signal | HEVC 10-bit 4:2:0, BT.709 tagged | RGBA16F, untransformed, working space tagged |
| Source cadence | Continuous 30–60 fps viewport | Event-driven: a frame exists when AE renders one |
| Purpose | Offload the render to a bigger GPU | Give AE a viewer it doesn't have |

## Decisions

**D1 — Native wire format per AE tier; only the float tier converts.**
*(Revised 2026-09-20 after measuring. The original decision was RGBA16F for
everything.)*

| AE tier | Wire | Why |
| --- | --- | --- |
| 8 bpc | `RGBA8Unorm` | memcpy, exact, **half the wire bytes** (31.6 vs 63.3 MB at 4K) |
| 16 bpc | `RGBA16Unorm` | memcpy, and keeps all 15 bits instead of ~11 |
| 32 bpc | `RGBA16F` | converted; QCView has no 32f flow (D2) |

Measured at 4K (`tests/convert_bench.cpp`, `lab/results/2026-09-20-a1c-wire-format/`):
converting the integer tiers to half costs **4.4× for 8 bpc** (0.66 → 2.90 ms)
and **1.2× for 16 bpc** (1.31 → 1.60 ms) against carrying them natively — and
8 bpc is already exact in half, so that cost buys literally nothing.

The original argument said matching tiers would buy "three texture formats and
three shader paths". The first half was true and irrelevant; the second half
was wrong. All three sample identically through one `texture2d<float>` binding,
because the texture unit normalizes integer formats in hardware — verified in
`tests/texture_format_test.mm`. Per-tier costs a `pixelFormat` switch and one
uniform, not a code path.

**The 32 bpc path needs hardware half conversion**, not a portable fallback:
12.55 ms scalar vs 2.67 ms with NEON `vcvt_f16_f32` at 4K (~80 vs ~374 fps).
The x86 equivalent is F16C `_mm256_cvtps_ph`, available since 2012. The integer
tiers sidestep the question entirely by never converting.

**D2 — Accepted precision losses, written down on purpose.**
- 32f → 16f: 24-bit significand to 11-bit. Accepted (chris, 2026-09-20) —
  QCView has no 32F flow today. **This is the only remaining conversion loss.**
- 16bpc: ~~up to 16× coarser just under white~~ **no longer lost on the wire**
  (D1 revision) — `RGBA16Unorm` carries all 15 bits. Whether they survive
  *inside* QCView depends on where it quantizes to 16F: if OCIO runs in the
  same pass as the sample, fp32 registers carry them and only the output
  quantizes; if it writes a 16F intermediate first, they die there. **Open,
  A3.** The speed and byte-count wins hold either way.
- 8bpc: exact, and now exact without touching a pixel.

**D3 — Normalize 16bpc by 32768, not 65535.** With D1's revision this moves to
the consumer: `RGBA16Unorm` hands the GPU AE's 0..32768 inside a 0..65535
container, so hardware normalization lands on ~0.5 and the image is
half-bright. `FrameDesc::value_scale` carries the correction (65535/32768,
exact in fp32) and a consumer applies it **unconditionally** rather than
keeping a table of per-format special cases. Pinned by
`tests/texture_format_test.mm`, which asserts both that the raw sample is wrong
and that the scale fixes it.

**D4 — Clamp at 65504 on the 32f path.** Half overflows to `inf` above that and
AE scene-linear specular hits legitimately go there; `inf` does ugly things
downstream in OCIO.

**D5 — The sidecar carries meaning; the surface carries only numbers.**
Preserving values while mislabeling them is still a broken picture. Per frame:
surface index, dimensions, source tier, premultiplied-alpha flag, comp name,
frame time, and the **working-space ICC profile**. AE hands us the real one —
`AEGP_ColorSettingsSuite6` → `AEGP_GetNewWorkingSpaceColorProfile` →
`AEGP_GetNewICCProfileFromColorProfile` (v6 also exposes graphics white and the
color-space-aware flag; frozen in AE 25.1). QCView's OCIO Input node is driven
by that, never guessed.

**D6 — Premiere support is a feature, not a side effect.** A Transmit plugin
lives in the shared `/Library/Application Support/Adobe/Common/Plug-ins/7.0/
MediaCore/` (`%PROGRAMFILES%\Adobe\Common\Plug-ins\7.0\MediaCore\` on Windows),
so it loads into Premiere too. We take that: Premiere → QCView for free.

**D7 — Route B first, Route A as the goal.** See below.

**D8 — Shared memory rather than IOSurface.** See Transport, below.

## The tap: two routes

| | **A — Mercury Transmit** | **B — AEGP + RenderSuite** |
| --- | --- | --- |
| SDK | Premiere Pro SDK — **not on this machine** | AE SDK — already vendored |
| Delivery | Push; rides the preview AE already rendered | Pull; we request the render |
| Color purity | **Unverified.** Depends on whether AE applies its display transform before the device sees the frame | **By construction** — we choose the world type |
| Gets Premiere | Yes (D6) | No |

Route A's entire ABI is one exported symbol, `xTransmitEntry` — confirmed
against Adobe's own `TransmitFullScreen.bundle` and AJA's `TransmitAJA.bundle`,
both installed on this machine. AE 2026 is a confirmed host
(`TransmitHost.framework`, `ML::ITransmitHost` / `ML::ITransmitPlugin`).
`PrPixelFormat_BGRA_4444_32f_Linear` exists in `PrSDKPixelFormat.h` — but an
enum value in a shared Premiere header does **not** prove AE offers it to a
Transmit device. That is the open question A4 settles.

Route B is fully specified by headers already on disk:
`AEGP_RenderAndCheckoutFrame` → `AEGP_FrameReceiptH` → `AEGP_WorldH`, with
`AEGP_SetWorldType(AEGP_WorldType_32)`.

Known Route A risk: the Transmit host has a **"Disable video output when in the
background"** preference. The whole premise here is that the user is looking at
QCView, not at AE. If that default stops the stream on focus loss, Route A has
a usability problem Route B doesn't.

## Transport: same machine, one write

Mercury Transmit hands over a CPU buffer, so nothing is GPU-resident to begin
with. The goal is *one* write landing where the GPU can read it — not literally
zero touches.

**D8 — Page-aligned shared memory, not IOSurface.** *(Revised 2026-09-20 in
A1; the original plan said IOSurface.)* `IOSurfaceCreateMachPort` /
`LookupFromMachPort` are the easy half; the rendezvous is the hard one. Mach
ports cannot travel over a Unix socket — `SCM_RIGHTS` carries file descriptors
and an IOSurface has no fd form — and XPC needs a registered Mach service,
meaning a LaunchAgent. A plugin living inside After Effects is in no position
to register one. The alternatives were deprecated global surfaces addressed by
guessable `IOSurfaceID`, or an install-time LaunchAgent dependency for what is
meant to be a plugin.

A page-aligned POSIX mapping is rendezvous-by-path, carries to Windows
unchanged, and keeps the property that made IOSurface attractive.
**Confirmed, not assumed**: `tests/metal_zerocopy_test.mm` builds a linear
`MTLTexture` over the ring, mutates those pages from the CPU with no Metal call
of any kind, and shows the GPU reading the new values. Apple M5 Max, macOS 27.

| | Mechanism | Real cost |
| --- | --- | --- |
| macOS | Page-aligned `shm_open` + `mmap`; consumer wraps a slot with `newBufferWithBytesNoCopy:` and `newTextureWithDescriptor:offset:bytesPerRow:`. | **Zero transfer on Apple Silicon** — measured: the GPU reads bare CPU writes |
| Windows | Named file mapping; or a D3D11 shared texture (`DXGI_FORMAT_R16G16B16A16_FLOAT`, `SHARED_NTHANDLE \| SHARED_KEYEDMUTEX`) if a mapping can't back a texture without a staging copy. A5 decides. | One PCIe upload on discrete GPUs. Unavoidable |

Rows are padded to `kRowAlignment` (256 B) because a linear texture has a
device minimum for `bytes_per_row`. The producer owns that padding and reports
it in the sidecar; a consumer computing `width * 8` instead will shear any
frame whose width isn't a multiple of 32 pixels.

A **ring of 3 slots**, so AE writes N+1 while QCView samples N. Sync is a
seqlock per slot plus a reader claim the producer honours — no mutex, no
ready-signal, and nothing for a crashed consumer to hold hostage. The Windows
keyed-mutex option in A5 is an alternative to this, not an addition.

## Phases

| Phase | Deliverable | Exit criteria |
| --- | --- | --- |
| **A1 Spine** ✅ | Shared-memory ring + sidecar + throwaway Metal viewer. No AE involved | ~~Synthetic frames land in the viewer; surfaces recycle without tearing~~ **Done 2026-09-20** — `lab/results/2026-09-20-a1-ring-spine/`, `-a1b-metal-probe/` |
| **A2 AEGP tap** | AEGP plugin: render active comp to a 32f world on idle, convert, publish. macOS | A live AE comp appears in the probe viewer, bit-exact for 8bpc |
| **A3 QCView ingest** | Float inlet in QCView (new construction — everything there arrives via libavcodec today); OCIO Input driven by the sidecar ICC | Comp is live in QCView as a media item; OCIO engaged; A/B against an approved render works |
| **A4 Transmit probe** | Premiere SDK; build its Transmit sample; log the `PrPixelFormat` list the host actually offers, under **both** AE and Premiere | Go / no-go on Route A, recorded in `lab/results/` |
| **A5 Windows parity** | DXGI shared texture + keyed mutex; MSVC build of A1–A2 | Windows probe viewer matches macOS behaviour |
| **A6 Transmit plugin** | Route A implementation, if A4 is green. Ships Premiere support (D6) | Device appears in Preferences → Video Preview in both apps |
| **A7 Packaging** | Signing, notarization, installers both platforms | Installs clean on a machine that has never seen the SDK |

A1–A2 are buildable today with what's on disk. A4 is independent of them and
can run in parallel — it only needs a download.

## Privacy and licensing

These are constraints, not aspirations. See `lab/README.md` for the rule that
governs the shared notes folder.

1. **The Adobe SDKs never enter this repo.** AE and Premiere headers carry an
   ADOBE CONFIDENTIAL notice. They live in `private/` (gitignored). The Windows
   machine obtains its own copy from Adobe — **the SDK does not travel through
   this repo**.
2. **`lab/` is committed, therefore public.** No client names, job codes, comp
   names, project paths, hostnames or frame captures. Findings are written in
   neutral terms ("a 4K comp, ~30 layers").
3. **The product never writes pixels to disk.** Debug frame dumps are an
   explicit opt-in, land in the OS temp dir, and never touch the project tree
   or this repo.
4. **No network code, at all.** Same-machine IPC only — Mach port / named pipe,
   no sockets, no listeners. Nothing to firewall, nothing to pair, no TOFU
   design like QCBridge needed. This is a deliberate invariant: if a change
   wants a socket, the change is wrong.
5. **Logs carry no project paths or comp names by default.** The comp name
   travels in the sidecar (in memory, to QCView) — not to a log file.
6. **License: MIT**, matching minColor, because a proprietary-SDK plugin under
   GPL is a fight nobody needs. QCView is GPL-3.0, so the QCView-side ingest
   code lives in **that** repo under **its** license. The boundary is the
   shared surface. *(Open: confirm before first public push.)*

## Open questions

- Does AE offer `32f_Linear` to a Transmit device, or a display-transformed
  frame? (A4 — gates Route A entirely)
- Does "Disable video output when in the background" kill the stream when AE
  loses focus? (A4)
- Route B cadence: which AEGP hook drives the pull — idle hook plus change
  detection? What does it cost when AE's frame cache misses?
- Does the sidecar ICC need to become an OCIO colorspace *name* for QCView's
  Input node, or can QCView consume the ICC directly?
- Alpha: is straight vs premultiplied a QC concern worth surfacing in the UI,
  or just a correctly-set flag?
