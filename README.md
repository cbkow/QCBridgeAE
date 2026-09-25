# QCBridgeAE

**QCBridgeAE for After Effects and Premiere Pro** is a Mercury Transmit plugin that streams live viewport output from the the AE or Premiere to [QCView](https://qcview.app/). The output is unaltered and full float, converted to 1/2 float in QCView so the image quality is preserved under all circumstances, including HDR scenarios. See the [add-on](https://qcview.app/qcbridge/adobe/) page for more info.

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
