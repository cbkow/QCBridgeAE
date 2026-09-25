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

**Status:** working on **macOS**, experimental. After Effects and Premiere Pro
frames reach QCView live, measured end to end (values arrive as the host
sent them, rounded only from 32-bit float to half; 4K preview at 24 fps). Needs a QCView build with QCBridge
support (QCView-Player branch `qcbae-live`, not yet released). Windows and
installers are next — see the phase table in `PLAN.md`.

## Setup (macOS)

**1. Install the device.** Quit After Effects and Premiere Pro, then run
`QCBridgeAE-<version>-arm64.pkg` (signed and notarized). It puts
`QCBridgeAE-Transmit.bundle` into

```
/Library/Application Support/Adobe/Common/Plug-ins/7.0/MediaCore/QCBridgeAE/
```

which both After Effects and Premiere Pro load Transmit devices from. On
Windows, `QCBridgeAE-<version>-Setup-x64.exe` (Inno Setup, unsigned —
Windows warns once) does the same into `Program Files\Adobe\Common\Plug-ins\7.0\MediaCore`,
and refuses while a host is running. From a checkout: build
`qcbae-transmit` (below) and `packaging/macos/build-pkg.sh` /
`installer/qcbridgeae_installer.iss` make the installers, or copy the
bundle by hand.

**2. Enable it in After Effects:** *Settings → Video Preview*
- tick **Enable Mercury Transmit**, then tick **QCBridgeAE → QCView**;
- **untick "Disable video output when in the background"**. With it ticked
  (the default), After Effects stops sending the moment you click into
  QCView — QCView shows *PAUSED* and names this setting;
- leave **"Video preview during render queue output"** unticked unless you
  want renders sent to QCView too. That switch is After Effects', not the
  device's: the SDK gives a device no way to tell a render from a preview.

**3. Enable it in Premiere Pro:** *Settings → Playback* — the same two ticks,
and the same background setting to untick.

**4. In QCView:** *File → Connect to After Effects* (or *Connect to Premiere
Pro*). The source appears in the Live bin as **QCBridge After Effects** /
**QCBridge Premiere Pro** and waits until the host is sending.

**5. Set QCView's OCIO input to your project's working space.** The pixels
arrive in the host's working colour space, untransformed — nothing upstream
converts or guesses. With Adobe colour management, that is the project's
working space; with OCIO colour management, the OCIO working space (e.g.
ACEScg).

**What to expect**
- Frames are RGBA16F. The host sends 32-bit float; values above 1.0 and
  negatives are kept, and nothing is clamped — a frame containing inf or NaN
  (usually a broken effect or expression) is flagged in QCView's live strip.
- After Effects flattens transparency over the **comp background colour**, so
  its frames are opaque. Premiere carries **straight alpha**.
- After Effects sends a frame whenever its viewer updates and plays previews
  in real time; it sends no timecode. Premiere sends its sequence time with
  every frame.
- Only one After Effects and one Premiere at a time: each host has one fixed
  feed.

## Building

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
```

If linking fails with `tapi error: unknown architecture`, your Command Line
Tools ship a newer macOS SDK than their linker can read; pin the older one
with `-DCMAKE_OSX_SYSROOT=…/MacOSX26.5.sdk` (details in QCView-Player's
`dependencies.md`, "Toolchain note").

**Requirements (dev):** the Adobe After Effects SDK and the Premiere Pro SDK.
Neither is included — they're Adobe Confidential. Put your own copies in
`private/sdk/` as `AfterEffectsSDK/` and `PremiereProSDK/` (see `PLAN.md`
§Privacy). Without them, the transport and its tests still build.

---

`lab/` holds findings and measurements, and is the channel between the macOS and
Windows machines. It is public: read [`lab/README.md`](lab/README.md) before
writing there.
