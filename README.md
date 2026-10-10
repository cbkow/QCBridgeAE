# QCBridgeAE

**QCBridgeAE for After Effects and Premiere Pro** is a Mercury Transmit plugin that streams live viewport output from the the AE or Premiere to [QCView](https://qcview.app/). The output is unaltered and full float, converted to 1/2 float in QCView so the image quality is preserved under all circumstances, including HDR scenarios. See the [add-on](https://qcview.app/qcbridge/adobe/) page for more info.

**Audio (Premiere Pro).** The device can carry the sequence's audio to
QCView alongside the picture. Premiere keeps playing through its own audio
device; the plugin receives a copy. Switch it on per device: Preferences →
Playback → Transmit Device Playback → tick **Audio Stream** next to
"QCBridgeAE → QCView". After Effects does not send audio to Transmit
devices (its own limitation, measured in
`lab/results/2026-10-10-a8-transmit-audio/`).

**Large comps.** A comp that fits inside 3840×2160 arrives pixel for pixel.
A larger one is scaled by the host to fit inside 3840×2160 (aspect kept)
before it is handed over: asked for at full size, an 8000×8000 comp is a
977 MiB float frame per push built inside After Effects' playback loop, and
playback drops to a few frames per second. In a **32 bpc project** After
Effects spends ~65 ms per frame on any Transmit device whatever it asks for,
so preview playback with the device enabled tops out around 9–12 fps there;
16 bpc projects play in real time. Measurements:
`lab/results/2026-10-06-transmit-size-cap/`.

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

---

`lab/` holds findings and measurements, and is the channel between the macOS and
Windows machines. It is public: read [`lab/README.md`](lab/README.md) before
writing there.

---

Licensed GPL-3.0-or-later ([LICENSE](LICENSE)), like QCBridge. The Adobe After
Effects and Premiere Pro SDKs it builds against are Adobe's, are not included,
and are not covered by that licence.
