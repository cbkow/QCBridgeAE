# QCBridgeAE

**Experimental.** An After Effects beauty window for
[QCView](https://github.com/cbkow/QCView-Player): your comp, live in QCView, as
untransformed pixel data — so QCView's OCIO chain, A/B wipes and EDR display
paths operate on the signal After Effects actually computed rather than on a
Rec.709 rendering of it.

AE and QCView run on the **same machine**. Frames cross through a shared GPU
surface (`RGBA16F`), with the project's working colorspace travelling alongside
them so QCView's OCIO input is driven by fact rather than guesswork. There is no
network code anywhere in this project, by design.

It's a sibling of [QCBridge](https://github.com/cbkow/QCBridge), the Blender
remote-viewport tool, but shares no machinery with it — only the feel at the far
end. See [`PLAN.md`](PLAN.md) for the architecture, the decisions and what's
been settled versus what hasn't.

Because the tap is a Mercury Transmit device, **Premiere Pro gets the same
viewer for free** — that's intentional.

---

**Status:** nothing works yet. Phase A1 of seven.

**Requirements (dev):** the Adobe After Effects SDK, and the Premiere Pro SDK
from phase A4 onward. Neither is included — they're Adobe Confidential. Put
your own copies in `private/`.

---

`lab/` holds findings and measurements, and is the channel between the macOS and
Windows machines. It is public: read [`lab/README.md`](lab/README.md) before
writing there.
