# 2026-09-20 — A2: the AEGP tap

macOS 27, Apple Silicon, After Effects 2026 (26.5), AE SDK 25.6.

A live AE comp now reaches the probe viewer through the shared ring, and the
sidecar carries the project's real working-space ICC.

## Exit criteria: met for all three tiers, not just 8 bpc

Comp with three solids at known values, read back from the ring with
`qcbae-probe dump` — the bytes, not a screenshot, because a window shows the
display pipeline's opinion of the pixels rather than the pixels.

| AE solid | 8 bpc expected | got | 16 bpc expected | got |
| --- | ---: | ---: | ---: | ---: |
| 0.15 | 38 | **38** | 4915 | **4915** |
| 0.35 | 89 | **89** | 11469 | **11469** |
| 0.60 | 153 | **153** | 19661 | **19661** |
| 0.95 | 242 | **242** | 31130 | **31130** |
| 0.75 | 191 | **191** | 24576 | **24576** |
| 0.10 | 26 | **26** | 3277 | **3277** |

Alpha reads 32768 at 16 bpc — AE white, as `PF_MAX_CHAN16` says and as
`value_scale` (1.999969) corrects for. 32 bpc lands on 0.14990 / 0.34985 /
0.59961: exactly the nearest halves, which is the D2 loss showing up where it
was predicted rather than anywhere else.

Two of three tiers are bit-exact from AE to the wire.

## Four things that cost time, all worth recording

**1. `CFBundlePackageType` must be `AEgx`.** With `eFKT` (the effect type) AE
loads nothing and says nothing. This was masked as a path problem for a while
— see below.

**2. AE loads AEGPs from MediaCore, so installing needs no admin.**
`/Library/Application Support/Adobe/Common/Plug-ins/7.0/MediaCore/` is
group-admin writable; `<AE.app>/Plug-ins/` is root-owned. The first MediaCore
attempt failed and looked like proof that MediaCore is not an AEGP search
path. It was the package type. **This matters for distribution**: no installer
privilege escalation, and it is the same folder a Transmit plugin needs for
Route A.

**3. `AEGP_GetNewWorkingSpaceColorProfile`'s second parameter is an
`AEGP_CompH`, not null.** Passing null returns an error and leaves the sidecar
with no profile — the one field D5 exists for. With the comp handle it returns
a 624-byte ICC whose colour-space field reads `RGB `.

**4. A stray `AEGP_GetItemName(id, item, NULL)` bricked the host.** Dead code
left in by mistake. AE raises a modal *"internal verification failure
{unicode_namePH cannot be NULL} (5027 :: 81)"* — and because the idle hook
fires again behind the dialog, it raises it forever. AE becomes unusable.

That last one is a design lesson, not just a bug. **Any** error from an idle
hook repeats at the hook's frequency, so the tap now:

- never returns an error to AE (errors go to the log; AE surfaces them as
  modal dialogs, which is the trap above), and
- disables itself after 5 consecutive failures and says so, leaving AE usable.

A plugin that runs on idle can take the host down with it. The circuit breaker
is not defensive decoration.

## Route B's ceiling, confirmed

`AEGP_RenderAndCheckoutFrame` is synchronous on AE's UI thread. The
AsyncManager that would fix this is reachable only through
`PF_GetContextAsyncManager` in `PF_EffectCustomUISuite2` — it is DRAW-event
specific and belongs to effects with custom UI. **An AEGP cannot get one.**

Measured on a trivial 1280×720 comp with three solids: 100 frames in 13 s
against a 100 ms throttle, so ~130 ms per cycle, i.e. **~30 ms of render and
publish per frame**. A1a measured the ring publishing 4K in 1.03 ms, so
essentially all of that is AE's render. On a trivial comp.

A heavy comp will block AE for as long as its frame takes. Throttling bounds
how often we pay it; nothing here bounds how much. **This is the strongest
argument yet for Route A**, which pushes frames AE has already rendered rather
than asking for new ones — and it is an argument A4 should now weigh more
heavily than the plan currently does.

## Also working

- Ring rebuilds on a geometry change; slots sized by `max_frame_bytes` so a
  bit-depth change alone does not need one. Verified by switching the project
  between 8, 16 and 32 bpc with the viewer attached — the tier changes and
  the texture cache re-keys without a restart.
- `ChannelOrder::ARGB` carries AE's native order and the consumer swizzles in
  the shader for free (`c.gbar`). Confirmed by colours arriving correct rather
  than rotated.
- Comp name reaches the viewer's HUD; it never reaches the log (privacy).

## Open

- Change detection: the tap re-renders on every tick rather than when
  something changed. AE's cache makes a repeat cheap, but "cheap" is doing
  work to discover nothing happened. Needs a real trigger.
- The 100 ms throttle is a constant, not a policy. It should adapt to how long
  renders are actually taking.
- Nothing reads the ICC yet beyond its length and colour-space field. A3 has
  to turn it into an OCIO input.
- UTF-16 comp names are flattened to their ASCII subset with '?' — fine for a
  probe HUD, wrong for a real label.
