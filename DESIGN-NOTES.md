# Design notes

The settled decisions the code cites, as `DESIGN-NOTES <label>`. The plan
they came from was a working document and is not part of the repository.

## Decisions

- **D1 — Native wire format per host tier; only the float tier converts.**
  8 bpc goes as RGBA8, 16 bpc as RGBA16 (unsigned, see D3), 32 bpc float is
  converted to half. Converting the integer tiers to half was measured at
  4.5× the cost for bits QCView discards at ingest.
- **D2 — Accepted precision losses, written down.** 32-bit float to half is
  the only conversion loss on the wire: a 24-bit significand becomes 11-bit.
  Nothing else on the path rounds.
- **D3 — 16 bpc is normalised by 32768, not 65535.** After Effects' 16 bpc
  range is 0..32768 inside a 16-bit container; the frame descriptor carries
  `value_scale` (65535/32768) so the consumer's hardware normalisation lands
  on white, not on half-bright.
- **D4 — Never clamp.** Float to half is the IEEE conversion, round to
  nearest even, nothing more: sign kept, subnormals kept, magnitudes past
  half's range become ±inf, NaN stays NaN. A frame containing a non-finite
  value is flagged in the descriptor; clamping would hide exactly what a QC
  viewer exists to show.
- **D5 — The sidecar carries container facts, not colour semantics.** Per
  frame: surface index, dimensions, source tier, channel order, value scale,
  the premultiplied-alpha flag, the host's name, frame time. Colour
  interpretation is the viewer's; nothing upstream guesses.

## The tap

The device is a Mercury Transmit plugin. Its whole ABI is one exported
symbol, `xTransmitEntry`, the same for After Effects and Premiere Pro, so one
bundle serves both hosts. It offers ARGB and BGRA 32-bit float in the host's
working colour space and nothing else, so the host hands over the pixels it
computed without a transform.

A comp that fits inside 3840×2160 is asked for at "any" size and arrives
pixel for pixel. A larger one is asked for at the size that fits inside
3840×2160, aspect kept, learned from its first frame; the host then scales
before building the frame. The request is dropped whenever another instance
goes live, so no comp inherits another's raster. Asked for at full size, an
8000×8000 comp is a 977 MiB float frame per push, built inside the host's
playback loop: 13 fps on the Mac and 0.6 fps on Windows at 16 bpc, 24 fps
capped. The cap's size made no difference to the host's rate between 64×64
and 2160×2160, so there is one cap and no setting. In a 32 bpc project the
host spends ~65 ms a frame on any Transmit device whatever it asks for, which
no device can change (measured 2026-10-06).

## Rings

- **A2.** The first tap was an AEGP that rendered the active comp on idle;
  it proved bit-exact 8 bpc delivery and was superseded by the Transmit
  device, which rides the preview the host already rendered.
- **A6.** One fixed shared-memory ring name per host (After Effects,
  Premiere Pro). A consumer lists those names; there is no discovery and no
  registry.

## Privacy

1. The Adobe SDKs never enter the repository; they live in `private/`,
   gitignored, and each machine obtains its own copy from Adobe.
5. No network code, at all: same-machine shared memory, no sockets, no
   listeners, nothing to pair. If a change wants a socket, the change is
   wrong.
6. Logs carry no project paths and no comp names; the comp name travels only
   in the sidecar, in memory, to the viewer.
