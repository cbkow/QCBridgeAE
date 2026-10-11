# Driving Premiere Pro from a shell (macOS)

Premiere has no AppleScript dictionary and its windows are invisible to the
accessibility tree, so System Events `click at` does nothing inside them.
These three tools post real CGEvents instead; the lab runs drive Premiere
with them plus `open -a` and System Events keystrokes.

    cc -o click click.c -framework ApplicationServices
    cc -o drag  drag.c  -framework ApplicationServices
    cc -o wins  wins.c  -framework ApplicationServices

- `click X Y` — left click at logical point (X, Y).
- `drag X0 Y0 X1 Y1 MS` — press, move over MS milliseconds, release.
- `wins` — on-screen windows as `owner | title | x y w h` (logical points);
  the way to find Premiere's and QCView's window frames.

Logical points = screenshot pixels × (display width in points / screenshot
width); with `screencapture -x` + `sips -Z 1600` on a 3360-pt display the
factor is 2.1.
