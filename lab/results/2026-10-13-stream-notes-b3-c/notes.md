# Notes on live streams, B3 and C: exports, and the hand-off to minNotes (2026-10-13)

The last two steps of the stream-notes plan. B3 makes QCView's exports
and its "Open in minNotes" button stream-aware; C teaches minNotes to open
a stream's notes folder as a document.

## B3 (QCView 593ed862)

- The four exporters (Markdown, HTML, PDF, DOCX) head a stream note with
  its capture time (`yyyy-MM-dd HH:mm:ss.zzz`) and the host's timecode when
  the feed carries one (`TC 00:01:02:03`), instead of a file timecode and
  frame number; the title is the stream's name and the Source line its URL.
- The export panel gains **Open in minNotes** for a stream item: the
  folder as a `minnotes:///<folder>` link (minNotes resolves the path),
  with a process-launch fallback and a greyed button when minNotes is not
  installed (`/Applications/minNotes.app` or `~/Applications`, the
  Program Files install or the registered scheme on Windows).

Verified on the Mac with the B1 project's stream (5 notes): the Markdown,
HTML and DOCX headings carry the capture times and the Source line the SRT
URL; the PDF goes through the same heading code (its text is glyph-encoded,
so it was not grepped).

## C (minNotes 205da7d)

- `annotation_io` / `annotation_note` re-ported verbatim from QCView, so
  minNotes reads and preserves the stream fields and the viewer / OCIO
  tags (it dropped them on every save before).
- `VideoNotesModel` gains a stream mode (`sidecarDir`): the notes of a
  folder with no media behind them, the `notes.json` header round-tripped
  on save, keys sorted as capture stamps, no frame minting or thumbnail
  extraction. Cards get the capture time and host timecode.
- `insertNotesAsBlocks()`: every note into the document as an image block
  of the clean PNG with QCView's strokes as frame-space ink (the two
  stroke spaces are the same normalised frame), then a caption paragraph
  with the capture time or timecode, the host timecode and the text. One
  undo group.
- Opening a folder that holds `notes.json` (or the file, or QCView's
  `minnotes:///` link) opens `<folder>/<name>.mnd`, created with every note
  inside on first open; the tab remembers the folder and the bottom rail
  shows the stream's name and note count with **Insert notes** for what
  QCView adds later.
- The player's `abuffersink` setup was replaced with QCView's (alloc,
  array-typed options, init), so minNotes builds against FFmpeg 7.1
  through 9 — needed to link the Mac dev build against QCView's FFmpeg.

Verified headless on the Mac (`--shot`): a copy of the B2 stream folder
(5 notes) opened into a new `stream.mnd` with an image block and a caption
per note that has its PNG, and a caption alone for the one note whose
capture never reached disk; the bottom rail reads the stream's name,
"5 notes" and Insert notes. The only drawn note was that PNG-less one, so
for the ink check its strokes were copied onto a note with a PNG and the
folder reopened: the media block then carries one `block_ink` row with
the strokes in block-local (frame) space, the points unchanged.

## Not verified

- The Windows build of minNotes with these changes (the box has the
  checkout, KF6 and FFmpeg 8).
- A click on **Insert notes** and **Open in minNotes** in the GUI (both
  are the same code paths the headless run exercised, minus the click).
