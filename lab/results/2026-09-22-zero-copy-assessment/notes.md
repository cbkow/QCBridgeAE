# 2026-09-22 — Zero-copy ingest: measured, and parked

Apple M5 Max, macOS 26.7. Bench: `tests/ingest_bench.mm` (`qcbae-ingestbench`).

A1b proved zero-copy works: the GPU reads a linear texture over the ring's
pages with no transfer. That doesn't say whether QCView *needs* it. The
question came up because of dual view: a live source on one side of A/B needs
a frame handoff, and whether the ring hands over a copy or a borrowed slot
decides the shape of that handoff. Before designing either, we measured what
zero-copy would buy.

## What QCView does today

Two copies per live frame:

1. `HostBridgeSource` copies the ring slot into a `QImage` on its reader thread.
2. The Metal renderer `replaceRegion`s that `QImage` into one cached
   shared-storage texture, **synchronously on the render thread**, inside
   `drawFrame`.

## Numbers

Real POSIX shm with the ring's slot geometry. Each slot is rewritten before
it is read, as the producer does, and the GPU pass is a bilinear resample into
an RGBA16F canvas, the shape of QCView's composite. Stable across three runs.

| per frame | 1080p | 4K | 8K → 4K canvas |
| --- | ---: | ---: | ---: |
| copy 1, ring → `QImage` (reader thread) | 0.23 ms | 0.95 ms | 3.8 ms |
| copy 2, `replaceRegion` (render thread) | 0.50 ms | 2.3 ms | **8.2 ms** |
| GPU resample, copied texture | 0.07 ms | 0.54 ms | 1.3 ms |
| GPU resample, linear texture over the slot | 0.09 ms | 0.21 ms | 0.8 ms |
| zero-copy setup, once per ring | 0.01 ms | 0.03 ms | 0.05 ms |

In the app (A3, AE 4K preview), copy 1 measured 2.81 ms mean, 6.7 ms max,
roughly 3× the bench. That is the copy competing with AE for memory
bandwidth. It was not investigated further.

The GPU samples the linear texture **faster** than the `replaceRegion`
texture, so zero-copy has no sampling penalty to set against it.

## What zero-copy would buy

- **CPU:** 3–5 ms per 4K frame, about 0.12 core at 24 fps out of QCView's
  measured 0.39. Nice to have, not needed.
- **Latency:** a few ms, against a host that renders for tens to hundreds.
  Irrelevant to QC.
- **Render-thread stall:** the only real effect. 2.3 ms at 4K is harmless.
  8.2 ms at 8K is a full 120 Hz refresh, so ProMotion would drop one redraw
  per new frame (a redraw, not a source frame).
- **Windows:** nothing. D3D11 can't sample a file mapping, so there's always
  an upload there.

## What it would cost

The ring gives the reader **one** claim. That is enough to copy a slot out,
or to look at one frame in place. It is not enough for a renderer that keeps
frames in flight. QCView runs up to three drawables deep, and moving the
claim to frame N+1 releases N while command buffers may still be sampling
it. So in-place sampling needs a set of claims, more slots, and claims
released when the GPU finishes, from a completion handler on another thread.
Dual view would add per-side holds and a handover between renderers. That is
a ring protocol v4, before the Windows port duplicates the ring. A1b already
flagged the start of this ("a stall a real consumer wants to replace with a
completion handler that releases the claim"); it just doesn't stop at one
frame.

## Decision

**Parked.** The copy stays and frames stay self-contained. That removes the
dependency between zero-copy and dual view: a consumer can hold a copied frame
indefinitely and hand it between renderers without touching the ring.

It changes shape, though. There will be **one copy, from the ring straight
into one of a few rotating textures, off the render thread.** That removes
the render-thread stall at every size with no protocol change, and closes a
probable hazard found while reading. Today `replaceRegion` writes into the
single cached texture with no wait for earlier command buffers, so an upload
can overwrite pixels that an earlier frame's GPU work is still reading. It has
never been observed, and the window is short, since each frame's GPU work
takes about 0.5 ms. It applies to every CPU-decoded format in QCView, not
just live. It belongs to the Metal source-state race dig.

Revisit zero-copy for 8K or high-frame-rate sources.
