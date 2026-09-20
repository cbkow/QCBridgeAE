# lab/ — findings, measurements, and the Windows channel

This folder is how the macOS and Windows machines talk to each other. Both
write here; both read here. It is the QCBridgeAE equivalent of QCBridge's
`spikes/parity/`.

## Lifecycle

`lab/` is scaffolding, not a permanent fixture. It exists to carry the project
from A1 to shipping and to be the channel between the two machines; once
QCBridgeAE reaches release it can be retired, with anything still worth keeping
promoted into `docs/`.

That does **not** relax the rule below, for one reason: deleting the folder at
release does not unpublish it. Git keeps every version of every file that was
ever committed, so a `git rm` in 2027 does nothing about a client name written
here in 2026 — and this repo will very likely be public well before release,
the way its siblings are. The discipline has to hold from the first commit.

## The rule

**Everything in `lab/` is committed, and this repo is public. Write
accordingly.**

Not in here, ever:

- client or job names, project codes, comp names
- project file paths, or any path under a job root
- machine hostnames, user names, non-RFC1918 addresses
- frame captures, thumbnails, EXR/DPX dumps — these are client pixels

Describe test material in neutral terms: "a 4K comp, ~30 layers, heavy
gaussians" carries every fact a reader needs and none they shouldn't have.

Anything that can't be written that way goes in `private/` instead, which is
gitignored. That includes the Adobe SDKs — see `PLAN.md` §Privacy.

## Layout

```
lab/
├── README.md             this file
├── HANDOFF-windows.md    standing instructions for the Windows machine
└── results/
    └── <YYYY-MM-DD>-<slug>/
        ├── notes.md      what was tried, what happened, what it means
        └── runs.jsonl    raw measurements, one object per run
```

One folder per investigation, dated. `notes.md` is the artifact that matters —
a number without the conditions that produced it is noise. Record the negative
results too; QCBridge's most useful note turned out to be the one explaining
that an unreachable replica was a closed app, not a bug.

## Working across machines

Both machines commit to the same branch and pull before writing, the way
QCBridge's parity spike ran. Conflicts in `results/` are rare because folders
are dated and machine-specific; conflicts in `HANDOFF-windows.md` mean two
people are editing the contract at once — talk first.
