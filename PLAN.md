# QCBridgeAE: an After Effects beauty window for QCView

Committed 2026-09-20 (chris); reframed 2026-09-21. Repo copy of the plan;
running findings live in `lab/`, which is also the channel to the Windows
machine.

## What this is

**A Mercury Transmit plugin that streams to QCView.** An After Effects comp,
live in QCView, as **untransformed pixel data** — so QCView's OCIO chain and
EDR paths operate on the signal AE actually computed, not on a Rec.709
rendering of it. Premiere gets the same device for free (D6).

The AEGP tap built in A2 (Route B) was scaffolding: it proved the ring, the
pixel fidelity and the colour behaviour against a live AE comp. It stays useful
as an instrument; it is not the product. See D7.

**v1 scope (2026-09-21):** live AE → QCView in QCView's single view, OCIO set
by hand in QCView. **A/B is out of v1** — it needs a deliberate build-out on
the QCView side (§QCView ingest), and will get one later.

**This is not QCBridge for AE.** The only parity is what the user sees at the
far end: a live item in QCView, OCIO engaged. Everything upstream is different:

| | QCBridge (Blender) | QCBridgeAE |
| --- | --- | --- |
| Machines | Two — host + GPU replica | **One.** AE and QCView side by side |
| Transport | SRT / QUIC over LAN or VPN | **Shared GPU surface.** No network code at all |
| Signal | HEVC 10-bit 4:2:0, BT.709 tagged | RGBA, untransformed, in AE's working space (D1, D5) |
| Source cadence | Continuous 30–60 fps viewport | Event-driven: AE pushes a frame when it renders one |
| Purpose | Offload the render to a bigger GPU | Give AE a viewer it doesn't have |

## Decisions

**D1 — Native wire format per AE tier; only the float tier converts.**
*(Revised 2026-09-20 after measuring. The original decision was RGBA16F for
everything.)*

| AE tier | Wire | Why |
| --- | --- | --- |
| 8 bpc | `RGBA8Unorm` | memcpy, exact, **half the wire bytes** (31.6 vs 63.3 MB at 4K) |
| 16 bpc | `RGBA16Unorm` | memcpy, and keeps all 15 bits instead of ~11 |
| 32 bpc | `RGBA16F` | converted — **not for speed**; see below |

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

**Why 32 bpc converts, when native is nearly free.** Measured at 4K, native
`RGBA32Float` costs ~8% more CPU (2.60 vs 2.40 ms) and 0.16 ms more GPU (0.56
vs 0.40) — noise either way, and a ~385 fps ceiling fully native. The reason is
footprint against benefit: **QCView quantizes to 16F at ingest today**, so a
native wire pays 2× the memory to carry bits discarded at the door.

| 3-slot ring | 1080p | 4K | 6K | 8K |
| --- | ---: | ---: | ---: | ---: |
| `RGBA32Float` | 95 MB | 380 MB | 911 MB | 1519 MB |
| `RGBA16F` | 47 MB | 190 MB | 456 MB | 759 MB |

`wire_format_for(tier, native_float32=true)` keeps the lossless path built and
tested. **The trigger to flip it: QCView gaining a 32f pipeline.** At that
point the bits stop being discarded, 2× memory buys 13 bits of significand, and
it is a flag rather than a rewrite.

**The 32 bpc path needs hardware half conversion**, not a portable fallback:
12.55 ms scalar vs 2.67 ms with NEON `vcvt_f16_f32` at 4K (~80 vs ~374 fps).
The x86 equivalent is F16C `_mm256_cvtps_ph`, available since 2012. The integer
tiers sidestep the question entirely by never converting.

**Under Transmit: the host picks the tier, the copy does the work.**
*(2026-09-21, pending A4. Briefly "v1 carries RGBA16F only" earlier the same
day — withdrawn: requesting 32f for an 8 bpc project makes AE up-convert,
costs us ~4× the CPU (2.67 vs 0.66 ms at 4K) and doubles the wire bytes, all
to carry values that were exact in 8 bits.)* Speed and efficiency of the data
path are the priority.

- **Offer every tier; let the host choose.** `QueryVideoMode` offers
  `ARGB_4444_{8u,16u,32f}` (AE's native order) then the same in BGRA
  (Premiere's), all in the working colour space (D5). Per Adobe's guide the
  host "chooses the best format to use on a per-segment basis" from that list,
  so a project should arrive at its own depth with no host conversion. We read
  `GetPixelFormat` per frame; the ring already switches tier mid-session (A2).
  We cannot detect the depth ourselves anyway — a Transmit plugin has no AE
  project suites, runs in Premiere too, and `tmInstance` carries no depth.
  **A4 verifies "best" means what we need**; if AE always picks the deepest,
  fall back to one tier per instance, re-queried via `NeedsReset`.
- **One unavoidable copy, and all per-pixel work folded into it.** The host
  owns the PPix, so host → ring is the floor. It is memory-bound, so a channel
  shuffle (ARGB/BGRA → RGBA) inside it should be near-free — to be measured
  in `qcbae-convbench`.

| Host tier | In the copy | Ring | QCView CPU slot today |
| --- | --- | --- | --- |
| 8u | shuffle | `RGBA8Unorm` | accepted (`Format_RGBA8888`) |
| 16u | shuffle; **maybe** a saturating ×2 (0..32768 → 0..65535) | `RGBA16Unorm` | accepted (`Format_RGBA64`), but no `value_scale` |
| 32f | shuffle + hardware half conversion | `RGBA16F` | needs the new 16F branch |

  The 16u shift is a **suspicion to test, not a decision**: it is injective
  and lands within 1.5×10⁻⁵ of exact — ~60× finer than QCView's 16F floor —
  and would spare QCView a scale uniform. Alternatives: keep `value_scale` and
  add the uniform in QCView, or convert 16u to half (1.2× a memcpy, A1c).
  Adobe's guide confirms Transmit 16u is 0..32768, like AE.
- **Zero-copy where the hardware allows is QCView's half.** Its CPU slot
  copies into its own texture (`replaceRegion`); on Apple Silicon a ring
  reader can instead wrap the slot as a texture (A1b), making the whole path
  host render → one write → GPU reads. Windows on a discrete GPU pays one
  upload regardless.

**D2 — Accepted precision losses, written down on purpose.**
- 32f → 16f: 24-bit significand to 11-bit. Accepted (chris, 2026-09-20),
  re-examined against measurements the same day and **confirmed**: native is
  nearly free in time but doubles ring memory to feed bits QCView discards at
  ingest. **This is the only remaining conversion loss on the wire** — 8 and
  16 bpc are now bit-exact end to end.
- 16bpc: ~~up to 16× coarser just under white~~ **no longer lost on the wire**
  (D1 revision) — `RGBA16Unorm` carries all 15 bits. Whether they survive
  *inside* QCView depends on where it quantizes to 16F: if OCIO runs in the
  same pass as the sample, fp32 registers carry them and only the output
  quantizes; if it writes a 16F intermediate first, they die there.
  **Answered 2026-09-21, by reading QCView's renderers:** it writes a 16F
  intermediate first — the source is bilinear-resampled into a viewport-sized
  `RGBA16F` canvas and OCIO runs on that. The extra bits die inside QCView.
  The speed and byte-count wins hold either way.
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

**D5 — The sidecar carries container facts, not colour semantics.**
*(Revised 2026-09-20 after measuring; the original had it driving QCView's OCIO
input node.)*

Per frame: surface index, dimensions, source tier, `channel_order`,
`value_scale`, premultiplied-alpha flag, comp name, frame time. These are
**mechanical** — how the bytes are laid out — always true, exactly reversible,
and an image is visibly broken without them.

Colour *meaning* is QCView's to decide, explicitly, in its OCIO panel. Two
reasons. First, deriving an input transform from an ICC profile re-imports the
ICC-round-trip pitfalls this project exists to escape, and does so invisibly: a
mislabelled image doesn't look broken, it looks like a different grade.
Second, it demonstrably would not work — under OCIO colour management
`AEGP_GetNewWorkingSpaceColorProfile` reports "Rec.709 Gamma 2.4" for an
ACEScg project (`lab/results/2026-09-20-a2b-ocio-and-range/`).

The working-space ICC is still published — it costs 624 bytes, refreshes on
change, and is correct under Adobe colour management — but as an **optional
hint that must never be presented as authoritative**. Under OCIO it does not
track the working space, so it is not dependable for drift detection either.

**Under Transmit the device, not the host, names the colour space** — and the
default is wrong for us. `tmVideoMode.outColorSpaceRec` is filled by the
plugin in `QueryVideoMode`, the host renders into whatever it names, and left
alone it means BT.709 full-range 32f. The Premiere SDK defines a token for
exactly our case, `kPrWorkingColorSpace` ("Working Color Space",
`PrSDKColorSpaces.h`): passed back, it tells the host to render in its current
working space. That is the generic "working space" QCView should receive.
Documented, **not yet verified under AE** — A4 checks it with known-value
solids under both Adobe CMS and OCIO/ACEScg, as A2b did for the AEGP.

In QCView the feed is labelled only as working space; the user sets the input
transform in its OCIO panel, which is a single global input — there is no
per-item colour space to fill in, and v1 does not add one.

Imported media is AE's own business: Interpret Footage converts on import, and
**Preserve RGB** is the escape hatch when it shouldn't. A manual step, kept
with the person who knows what the footage is.

**D6 — Premiere support is a feature, not a side effect.** A Transmit plugin
lives in the shared `/Library/Application Support/Adobe/Common/Plug-ins/7.0/
MediaCore/` (`%PROGRAMFILES%\Adobe\Common\Plug-ins\7.0\MediaCore\` on Windows),
so it loads into Premiere too. We take that: Premiere → QCView for free.

**D7 — Route A is the product; Route B was the scaffold.** *(Revised
2026-09-21; originally "Route B first, Route A as the goal".)* Route B did its
job — it proved the ring and the pixel fidelity against a live comp — and its
UI-thread ceiling (below) rules it out as the thing we ship. It stays in the
tree as an instrument: a known-good producer to compare Transmit's output
against. See below.

**D8 — Shared memory rather than IOSurface.** See Transport, below.

## The tap: two routes

| | **A — Mercury Transmit** | **B — AEGP + RenderSuite** |
| --- | --- | --- |
| SDK | Premiere Pro SDK (26.0, in `private/sdk/`) | AE SDK (26.5, in `private/sdk/`) |
| Delivery | Push; rides the preview AE already rendered, with its `PrTime` | Pull; we request the render |
| Color purity | **Unverified.** The device names the colour space it wants; `kPrWorkingColorSpace` should mean "no transform" (D5) | **By construction** — we choose the world type |
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

**Route B's ceiling, measured in A2.** `AEGP_RenderAndCheckoutFrame` is
synchronous on AE's UI thread, and the AsyncManager that would fix it is
reachable only through `PF_GetContextAsyncManager` in
`PF_EffectCustomUISuite2` — DRAW-event specific, belonging to effects with
custom UI. An AEGP cannot get one. A trivial 1280×720 comp costs ~30 ms of
render per frame; a heavy comp blocks AE for as long as its frame takes.
Throttling bounds how often we pay that, not how much. This is what settled
D7: Route A pushes frames AE has already rendered and never asks the host for
work.

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

## QCView ingest

Read from QCView-Player 2.3.3 on 2026-09-21 (native Metal and D3D11
renderers). This is what v1 plugs into, and what it has to change there.

**The seam already exists.** `MediaType::LiveStream` is a media item kind, fed
today by QCBridge over SRT. Its decoder owns no frame slot: it publishes into
the main video decoder's latest-wins slot through
`VideoDecoder::publishExternalFrame(FrameHandle, pts)`, and both renderers pull
from there. A ring reader is a sibling of `LiveStreamDecoder` publishing the
same way — `FrameHandle::cpuShared` can wrap a ring slot as a `QImage` view
with our padded `bytesPerLine`, and both renderers' CPU paths honour the
stride. Live status, reconnect and hold-last-frame-on-dropout come with it.

**What has to change in QCView for v1:**
- **A 16F branch in the CPU upload path**, both renderers. Today only
  `RGBA8888` and `RGBA64` are accepted; anything else — including
  `Format_RGBA16FPx4` — is silently converted to **8-bit**. Half-float reaches
  the GPU today only via the image-sequence upload threads.
- **Routing.** Anything containing `://` is treated as an SRT URL and handed to
  the FFmpeg decoder, so the ring needs its own scheme or MediaType branch.
  `WindowManager::m_liveDecoder` is typed as the SRT class, so a small live-source
  base class comes out of `LiveStreamDecoder`.
- Nothing downstream swizzles channels or applies a scale, so the plugin
  delivers RGBA, and the 16u range question is settled in A4 (D1's Transmit
  note). 8u and 16u already have CPU-slot formats; only 16F is new.
- **For zero-copy on Apple Silicon**, the ring reader should wrap the slot as a
  texture (A1b) rather than hand `replaceRegion` a `QImage` view. The
  `cpuShared` route above is the quick first cut.

**Facts that bound the design:**
- **16F is QCView's floor.** The source is bilinear-resampled into a
  viewport-sized `RGBA16F` canvas (capped 3840×2160 on Metal), and OCIO runs
  on that canvas, not the source. Nothing in QCView is fp32 except OCIO's LUTs;
  EXRs are read as half.
- **OCIO input is one global setting**, off by default, chosen in the
  ColorPanel. No per-item input, no "working space" concept — the user picks.
- **Straight alpha** everywhere; pixels with alpha 0 are discarded to the
  background. If AE's Transmit frames are premultiplied, that has to be
  handled (A4 finds out).
- Over-range survives to an EDR / scRGB swapchain from a 16F source; SDR
  swapchains clamp.
- QCView's CPU slot uploads with `replaceRegion` / `UpdateSubresource` into its
  own texture, so the A1b zero-copy does not carry through this path. At ~1 ms
  a 4K frame it isn't worth chasing in v1.

**Why A/B is out of v1.** Live is blocked from A/B deliberately, at three
guards (`setCompositorMode`, `setBSource`, the dual-capable check). A/B runs on
a master clock pumping two seekable, frame-addressed sources with fps and
duration; a free-running live source breaks that model, and QCView's own
comment names the fix as a dedicated live-A + scrubbed-B pairing mode. OCIO
also runs once *after* compositing, so both sides must share an encoding. That
is a deliberate QCView build-out, not something to back into. When it happens,
Transmit's `PrTime` on every frame is what would let B follow AE's playhead.

## Phases

Phase IDs are stable — `lab/` refers to them — so the table is in **execution
order**, not numeric order. *(Reordered 2026-09-21 when Transmit became the
product: A4 moved first because QCView's ingest format depends on what
Transmit delivers.)*

| Phase | Deliverable | Exit criteria |
| --- | --- | --- |
| **A1 Spine** ✅ | Shared-memory ring + sidecar + throwaway Metal viewer. No AE involved | ~~Synthetic frames land in the viewer; surfaces recycle without tearing~~ **Done 2026-09-20** — `lab/results/2026-09-20-a1-ring-spine/`, `-a1b-metal-probe/` |
| **A2 AEGP tap** ✅ | AEGP plugin: render the active comp on idle, convert, publish. macOS | ~~A live AE comp appears in the probe viewer, bit-exact for 8bpc~~ **Done 2026-09-20** — bit-exact for 8 **and** 16 bpc, ICC sidecar live. `lab/results/2026-09-20-a2-aegp-tap/` |
| **A4 Transmit probe** | Minimal Transmit device from the Premiere SDK sample, publishing into the existing ring. Logs every `QueryVideoMode` negotiation and every pushed frame's format, colour space, alpha and time | Recorded in `lab/results/`, under **both** AE and Premiere: **which offered format the host picks at each project depth** (8/16/32 bpc); whether `kPrWorkingColorSpace` delivers A2b's known-value solids untransformed under Adobe CMS **and** OCIO/ACEScg; premultiplied or straight; whether the stream survives AE losing focus; the 16u range suspicion (D1); shuffle-in-copy cost in `qcbae-convbench`. Go / no-go on Route A |
| **A6 Transmit plugin** | The real device, if A4 is green: every tier offered in working space, host's pick → RGBA into the ring in one copy (D1). Ships Premiere support (D6) | Appears in Preferences → Video Preview in both apps; `qcbae-probe dump` matches the AEGP tap on the same comp |
| **A3 QCView ingest** | Ring-reader live source in QCView (GPL, that repo), the 16F upload branch in both renderers, routing split from SRT. Single view only | Comp is live in QCView as a media item and follows AE; OCIO engaged by hand gives the expected picture; dropouts hold the last frame; bytes match `qcbae-probe dump` |
| **A5 Windows parity** | Named file mapping (or D3D11 shared texture), MSVC build, F16C conversion, Transmit on Windows | Same A4/A6 checks pass on Windows |
| **A7 Packaging** | Signing, notarization, installers both platforms | Installs clean on a machine that has never seen the SDK |

**Later, not v1:** live in QCView's A/B (§QCView ingest).

Everything through A6 builds on macOS with what is in `private/sdk/`. A3 needs
QCView-Player checked out alongside.

**The A3 cold-pickup plan (`PLAN-A3.md`) was lost with the original machine**
before it was committed. This table and §QCView ingest replace it. Plans get
committed.

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
3. **Installing needs no admin.** AE loads AEGPs from
   `/Library/Application Support/Adobe/Common/Plug-ins/7.0/MediaCore/`, which
   is group-admin writable, provided `CFBundlePackageType` is `AEgx`. The same
   folder serves a Transmit plugin (D6), so one install location covers both
   routes and neither needs privilege escalation.
4. **The product never writes pixels to disk.** Debug frame dumps are an
   explicit opt-in, land in the OS temp dir, and never touch the project tree
   or this repo.
5. **No network code, at all.** Same-machine IPC only — shared memory by path,
   no sockets, no listeners. Nothing to firewall, nothing to pair, no TOFU
   design like QCBridge needed. This is a deliberate invariant: if a change
   wants a socket, the change is wrong.
6. **Logs carry no project paths or comp names by default.** The comp name
   travels in the sidecar (in memory, to QCView) — not to a log file.
7. **License: MIT**, matching minColor, because a proprietary-SDK plugin under
   GPL is a fight nobody needs. QCView is GPL-3.0, so the QCView-side ingest
   code lives in **that** repo under **its** license. The boundary is the
   shared surface. *(Open: confirm before first public push.)*

## Open questions

- Does AE honour `kPrWorkingColorSpace` for a Transmit device, and offer a 32f
  format with it — or does it hand over a display-transformed frame? (A4 —
  gates Route A entirely)
- Does "Disable video output when in the background" kill the stream when AE
  loses focus? (A4)
- Are Transmit frames premultiplied? If so, un-premultiply in the plugin or
  flag it for QCView, which assumes straight alpha? (A4 measures, then decide)
- What does AE push while idle — one frame per change, or nothing until
  playback? QCView holds the last frame either way, but a comp edit must
  arrive. (A4)
- Does "closest format" pick the project's own depth, or always the deepest
  offered? (A4 — decides whether D1's per-tier Transmit design holds)
- 16u: saturating shift in the copy, `value_scale` uniform in QCView, or
  convert to half? (A4 measures; may prove unnecessary)
- ~~Route B cadence~~ and ~~ICC → OCIO name~~: retired with D5 and D7.
