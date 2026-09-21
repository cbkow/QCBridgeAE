# QCBridgeAE

**Experimental.** An After Effects beauty window for
[QCView](https://github.com/cbkow/QCView-Player): your comp, live in QCView, as
untransformed pixel data — so QCView's OCIO chain and EDR display paths
operate on the signal After Effects actually computed rather than on a
Rec.709 rendering of it.

It is a **Mercury Transmit device**: After Effects pushes the frames it has
already rendered, and they cross to QCView on the **same machine** through
shared memory, in AE's working colour space. QCView's OCIO panel decides how to
interpret them — nothing upstream guesses. There is no network code anywhere in
this project, by design.

It's a sibling of [QCBridge](https://github.com/cbkow/QCBridge), the Blender
remote-viewport tool, but shares no machinery with it — only the feel at the far
end. See [`PLAN.md`](PLAN.md) for the architecture, the decisions and what's
been settled versus what hasn't.

Because it is a Transmit device, **Premiere Pro gets the same viewer for
free** — that's intentional.

---

**Status:** experimental, not usable yet. The shared-memory transport and an
AE test tap are done and measured (bit-exact at 8 and 16 bpc, over-range
intact); the Transmit device and QCView's side are next. See the phase table in
`PLAN.md`.

**Requirements (dev):** the Adobe After Effects SDK, and the Premiere Pro SDK
(for the Transmit device). Neither is included — they're Adobe Confidential. Put
your own copies in `private/`.

---

`lab/` holds findings and measurements, and is the channel between the macOS and
Windows machines. It is public: read [`lab/README.md`](lab/README.md) before
writing there.
