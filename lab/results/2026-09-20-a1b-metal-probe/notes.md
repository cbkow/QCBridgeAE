# 2026-09-20 — A1b: Metal probe viewer, and the zero-copy claim settled

Machine: Apple M5 Max, macOS 27, unified memory. Same session as
`2026-09-20-a1-ring-spine`.

A1a left one thing owing: the whole justification for choosing shared memory
over IOSurface rested on `makeBuffer(bytesNoCopy:)` genuinely handing the GPU
the CPU's pages. That was an argument, not a measurement.

## Settling it

`tests/metal_zerocopy_test.mm`. The discriminating step is the third one:

1. Producer writes a known value into a ring slot, commits.
2. Consumer wraps the slot — `newBufferWithBytesNoCopy:` then
   `newTextureWithDescriptor:offset:bytesPerRow:` — and a blit pass has the
   GPU read the texture back. It sees the producer's value.
3. The CPU then writes a *different* value straight into those pages. No Metal
   call. No `didModifyRange`. No re-creating buffer or texture. The same blit
   runs again.

The GPU returns the new value. That is only possible if both are addressing the
same memory, and no amount of looking at a window could have told us that — a
silent upload would render identically. **Zero copy confirmed.**

`MTLBuffer.contents` also comes back byte-identical to the pointer handed in,
so Metal isn't relocating the mapping.

**PLAN.md updated** (D8). A1a's note flagged the plan and the code as
deliberately disagreeing; they agree now, and the reason is recorded rather
than quietly edited away.

## A third bug, same shape as the first two

Metal rejected the first attempt outright:

```
Linear texture: offset (4475371624) must be aligned to 16 bytes
```

The pixel regions were not page-aligned in absolute terms. `create()` rounds
the header up to a page when sizing the mapping, but `slot_at()` and the pixel
pointer math used the raw `sizeof(RingHeader)` — so every slot sat at a
+8-byte offset from where the geometry said it did.

The unit test had an alignment assertion and it passed, because it checked
`pixels_offset` — the offset *within* a slot — rather than the address the GPU
is actually handed. Third time in two days that a test checked a proxy instead
of the real property, and the first two (A1a) were the same mistake. Worth
naming as a pattern: **assert the thing the next layer consumes, not the thing
that is convenient to reach.** The test now walks every slot and checks the
returned pointer.

Fixed by storing `slots_offset` in the header and deriving everything from it.

## The probe

`qcbae-probe produce` / `qcbae-probe view` — two processes over the ring, which
is the shape the real thing has. `produce` is replaced by the AE plugin in A2,
`view` by QCView in A3; neither is meant to survive.

The window applies **no transform**: no sRGB encode, no tone map, `RGBA16F`
straight to a `CAMetalLayer` in extended-linear. Linear content therefore looks
dark, which is correct for a probe on a project about untransformed signal. If
it ever starts looking nice, something has been inserted.

Test pattern designed so tearing is *visible* rather than inferred: flat
background whose colour changes every frame, so a torn frame shows a seam
between two colours; a moving bar for motion; a vertical ramp running past 1.0
so clipping shows and the HDR path carries something above diffuse white.

Observed, 1280×720 at 24 fps: locked to 24.0, `skipped 0`, no seams.
Observed, 3840×2160 requested at 120 fps: ~42 fps, `skipped 0`.

That second number is the **pattern generator**, not the transport. It builds
every pixel with scalar float math and a half conversion; A1a measured the ring
publishing 4K in 1.03 ms (~967 fps). Worth stating because it is exactly the
kind of number that gets misremembered later as "the ring does 42 fps at 4K".
It does not. The synthetic producer does.

## Open

- `waitUntilCompleted` per frame in the viewer holds the slot until the GPU is
  done. Correct, and fine at these rates, but it is a stall a real consumer
  wants to replace with a completion handler that releases the claim.
- Producer crash still leaves a mapping nobody unlinks (carried from A1a).
  Needs a liveness signal; A2, when there is a real producer to kill.
- Nothing here has run on Windows. `src/common` remains SDK-free so it can.
