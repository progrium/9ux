# 9/UX development plan

This plan turns the phases in [design §10](design.md#10-incremental-implementation)
into concrete work, with an exit check for each phase and the
[v0.1 acceptance criteria](design.md#acceptance-criteria-for-v01) mapped
to where they're met. The environment is h9, described in
[development.md](development.md).

Principles that hold across phases:

- **Never break stock rio.** Every rio change is additive, and the
  Phase 0 regression suite runs on every change from then on.
- **Files first.** Each new interface gets its file protocol written down
  (in `docs/protocols/`) and tested from rc before UI is built on it.
- **Small servers.** Each component can die and restart without taking
  rio down; tests kill them on purpose.
- **Demo each phase.** Every phase ends with something runnable under
  `h9 --gfx`, plus a headless test that proves it.

## Phase 0: environment and baseline

Goal: build and run the rio fork under h9, and lock down stock rio's
behavior before changing it.

Work:

1. Build `rio/` with `mk` inside h9 on macOS arm64 and Linux amd64, and
   run it with `h9 --gfx`. Check nested rio, acme, and sam inside it.
2. Write the test harness under `test/`: a Zig program that depends on h9
   as a package, starts machines with the headless screen, and offers
   helpers to type, click, drag, wait for a window, read `/dev/wsys`, and
   compare screen regions with golden images.
3. Regression suite for stock rio: window creation (`window`, menu New),
   `/dev/wctl` commands, resize/move/hide/delete, `/dev/snarf`, `/dev/kbd`
   and `/dev/mouse`, `/dev/winid` and `/dev/winname`, `/mnt/wsys`
   attaches, nested rio, acme and sam in windows.
4. CI on GitHub Actions: build with `mk`, run the suite headless. Settle
   which h9 backend works there (native on x86_64, or the emulator).
5. Write `docs/protocols/rio.md`: stock rio's files as the fork must
   preserve them, from rio(4) and the source.
6. Decide the fork shape (design open question 1). Recommended: a runtime
   mode of one rio binary, with 9/UX code in new files (`ux*.c`) and
   small hooks in upstream files.
7. Keep a QEMU 9front 11952 VM recipe for the checks h9 can't do
   (`rcpu`, drawterm into a CPU server), and run them once by hand.

Exit: CI is green with the regression suite running against the
unmodified fork, and the steps to reproduce it locally are one command.
Acceptance criterion 1 is guarded from here on.

## Phase 1: desktop layer and menu bar

Goal: rio grows the surfaces 9/UX needs, and legacy windows don't notice.

Work:

1. `/dev/ux/version` and `/dev/ux/events` (focus, window create/delete),
   written up in `docs/protocols/devux.md` first.
2. Desktop layer: a reserved surface behind all windows, claimed through
   `/dev/ux/desktop/ctl`, receiving pointer events only where no window
   is hit, plus resize events.
3. Menu bar: a strip rio draws and owns. Apps register per-app menus
   through `/dev/ux/menubar/ctl` keyed by `winid`; selections come back on
   `/dev/ux/menubar/events`. Unregistered windows get a minimal legacy
   menu. A mode switch turns all of it off.
4. Hit-test order: menu bar, then window chrome, then topmost window, then
   desktop (drag comes in Phase 4).
5. Two tiny clients in rc or C: a desktop that paints a pattern and logs
   clicks, and a sample app with two windows and a menu.
6. Decide keyboard shortcut precedence and the default Command-key
   mapping (design open question 5).

Exit: under `h9 --gfx`, the sample app's menu follows focus between its
two windows and acme is unaffected; headless tests cover menu selection by
click and by file write, focus events, and desktop clicks. The full
Phase 0 suite still passes. Meets acceptance criterion 3.

## Phase 2: 9P Toolbox

Goal: a minimal libux whose controls are real 9P objects, and `uxd`
aggregating them.

Work:

1. `docs/protocols/ux.md`: the control tree, `ctl` command syntax, event
   message format (sequence numbers, `lost`/`resync`), versioning, and the
   permission model. Settle design open question 2 here (recommended:
   app-owned truth, `uxd` for discovery and policy).
2. libux core: app lifecycle on libthread, a UI proc that serializes all
   state changes, a 9P server proc that never blocks the UI proc, and
   per-open event cursors with bounded queues.
3. Controls: `Window`, `Button`, `TextField`, `Menu` (registered with
   rio's menu bar), `Dialog`, drawn with libdraw in a plain rio window.
4. `uxd`: registration, `winid`-to-app mapping, the `/mnt/ux/apps` tree,
   and policy (read allowed, writes need a grant; password fields never
   expose values).
5. Sample app (`sketch` or similar) using all five controls.

Exit: from an rc window, `ls /mnt/ux/apps`, read a text field's `value`,
write `activate` to a button's `ctl`, and see both the screen and a
second `events` reader update. Tests cover overflow and `lost`, a client
that stalls on `events` without stalling the UI, an unauthorized write
being refused, and `uxd` restarting. Meets acceptance criteria 4 and 8.

## Phase 3: Finder MVP

Goal: a usable desktop over local and remote volumes.

Work:

1. `uxmeta`: ndb parsing with layered precedence, one serialized writer
   with atomic replace and a generation number, safe handling of
   external edits, and generated plumbing rules for associations.
2. Volume registry and workspace context (design open question 3:
   explicit registry, no pretend global namespace).
3. Finder on libux: desktop icons for registered volumes, folder windows
   with icon view, select, open, rename, Get Info; window geometry and
   icon positions saved through `uxmeta`.
4. Opening documents through the plumber; launching libux apps.
5. Honest capability handling: a server that can't rename gets an error
   or an explicit copy-and-delete offer, not a silent failure.
6. Remote volume: a second h9 machine with `--serve` mounted as a volume,
   including what happens when it goes away.

Exit: the first half of the end-to-end demo works: a local and a remote
volume on the desktop, open a folder, double-click a text file into an
editor. Layout survives a restart and an external edit of the ndb file.
Meets acceptance criteria 2, 5, and 7.

## Phase 4: resources and full interaction

Goal: classic resources, drag and drop, clipboard, Trash, and the look.

Work:

1. resfs parser for the resource map and priority types, fuzzed on the
   host from day one, then the 9P server for `/mnt/res`.
2. Icons (`ICN#`, `icl4`, `icl8`, small variants) converted to 9front
   images for Finder and libux; then `MENU`, `STR#`, `DLOG`/`DITL`/`ALRT`
   feeding libux. Native resource source syntax waits (design open
   question 4).
3. Drag and drop through `/dev/ux/drag`: typed offers, target acceptance,
   payload fetched through a brokered endpoint, never a bare path across
   namespaces.
4. Text snarf interop, and the optional typed clipboard service.
5. Trash per file system, with its limits stated in the UI.
6. Visual design: original 9/UX icons, chrome, and fonts. No Apple
   system assets in the repo; users import their own `.rsrc` files.

Exit: imported `ICN#` and `icl8` icons render correctly, checked headless
against golden images and once on a real 9front display. Meets
acceptance criterion 6.

## v0.1 release

Run the [end-to-end demo](design.md#first-end-to-end-demo) as one
scripted headless test and once by hand under `h9 --gfx` and on a 9front
VM, confirm all eight acceptance criteria, then tag.

## Acceptance criteria by phase

| # | Criterion | Phase |
|---|---|---|
| 1 | No regressions to stock rio's file interfaces | 0, then every phase |
| 2 | Finder draws icons, opens directories, launches via plumbing | 3 |
| 3 | Menu bar follows a multi-window app's focus | 1 |
| 4 | Three controls with state, commands, and independent event streams | 2 |
| 5 | ndb persistence across restarts and external edits | 3 |
| 6 | `ICN#` and `icl8` render on real 9front displays | 4 |
| 7 | A remote 9P directory works as a Finder volume | 3 |
| 8 | Unprivileged clients can't mutate another app's controls | 2 |

## Running it in parallel

[workplan.md](workplan.md) splits these phases into workstreams with
owners, dependencies, and briefs that separate threads can run at once.
