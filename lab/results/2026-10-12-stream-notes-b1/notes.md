# Notes on live streams, step B1: storage and identity (2026-10-12, evening)

QCView c664ad52. A live stream item (QCBridgeAE's `qcbae://` feed or an
`srt://` receiver) can now take notes like a file, with the storage and
the identity the plan settled on. The Live/Review viewport states come
with B2; this step is only "where do they go and what are they called".

## What was built

- **Storage:** the project's sidecar, one folder per stream item,
  `.qcview/streams/<name>__<id8>/{notes.json,images/}`. The folder is
  found again by the `__<id8>` suffix, so renaming the item changes the
  header's display name and nothing else. The header carries `stream`,
  `stream_id`, `stream_url` in place of `media_file` / `media_type`;
  minNotes reads the same folder (step C).
- **Save first:** stream notes need the project on disk. Until it is,
  the notes panel shows a Save tile where Add note sits; a save re-syncs
  the manager and the tile turns back. Nothing is written before.
- **Identity:** the note key is the capture stamp
  (`YYYYMMDD-HHMMSS-mmm`, sorts chronologically); `frame` is 0,
  `timestamp_seconds` counts from the stream's open. Three optional
  fields say when and from what: `captured_at` (ISO 8601 with offset),
  `host_timecode` (Premiere's sequence time at the host's rate, through
  a new `LiveSource::hostTimecode()`; empty for After Effects and SRT),
  `stream` (the display name).
- **One key helper** in the window manager replaces the frame + fps
  derivation at the stroke, add-note and redo sites; undo entries carry
  what a redo needs; the frame-equality filters (erase, select, mesh)
  compare stream notes against the Review key B2 will set.
- **Same screenshot** as file notes: the renderer's clean capture,
  display-referred, chain-tagged.
- One `fileStemForKey()` shared by the sidecar writer, the exporter and
  the window manager (the three used to disagree on `;`).

## Proof (Mac, an ffmpeg SRT feed of the burn-in clip on a loopback port)

| step | result |
|---|---|
| unsaved project, add note | allowed, `needSave` set, nothing on disk |
| saved project, add note | `streams/127.0.0.1_9100__b5a8942e/notes.json` + `images/note_20261012-171615-599.png` (1280x720 clean capture); `captured_at` `2026-10-12T17:16:15.599-04:00` |
| reopen the project, add another | the folder is found by its id suffix; the second note appends in order with its own PNG; both survive |
| source ordering | by key string: chronological |

Notes for the next steps: the live capture only works while the feed
is up (an ffmpeg SRT listener quits when its caller leaves, so a test
feed needs a loop); `--switch-test` and the positional command line add
a `.qcvproj` as media, `QCV_OPEN_PROJECT` opens it. Undo/redo of stream
strokes needs a pointer and was not exercised here.
