# devux: rio's `/dev/ux` files

**Protocol:** `devux 1.0` · **Server:** the 9/UX rio fork (`rio/`) ·
**Fake:** [`test/fakes/fakedevux`](../../test/fakes/) ·
**Conventions:** [README.md](README.md)

`/dev/ux` is what rio adds for 9/UX: window and focus events, the
desktop layer, the global menu bar, and (later) drag and drop. Everything
here is additive. Stock rio files keep their meaning (see
[rio.md](rio.md)), and a rio program that never opens `/dev/ux` sees no
difference.

## Decisions

These settle design open questions 1 and 5, and the hit-test order left
open in design §3.

1. **One rio binary, a runtime mode.** (Open question 1.) 9/UX is a mode
   of the rio fork, not a separate build. 9/UX code lives in new files
   (`rio/ux*.c`) with small hooks in upstream files. rio starts in
   stock mode unless given `-u`; the mode can be switched at run time
   through [`/dev/ux/ctl`](#devuxctl). The `/dev/ux` files exist and work
   in both modes, so clients don't have to care which mode is on; only
   presentation and input interception change.
2. **Command is the Super key.** (Open question 5.) The default Command
   key is `Kmod4` (the Windows/Super key on PC keyboards, Command on a
   Mac keyboard). It is configurable, never Alt by default (9front
   uses Alt for composing) and never Ctl by default (legacy programs own
   it). rio only ever intercepts keys typed with Command held, and only
   when they match a menu item of the focused window's app. See
   [Keyboard shortcuts](#keyboard-shortcuts).
3. **Hit-test order:** menu bar, then an active drag, then window chrome,
   then the topmost window under the pointer, then the desktop window,
   then rio's own background. See [Pointer routing](#pointer-routing).
4. **The desktop is a window.** A desktop client is an ordinary rio
   window that claims the desktop role; rio pins it behind every other
   window and covers the screen with it. It draws with libdraw and reads
   `/dev/mouse` and `/dev/kbd` like any window, so no new drawing or
   input path is needed. See [Desktop](#desktop).
5. **Menus belong to apps, keyed by window.** An app registers its
   menus once and claims its windows by `winid`. The bar shows the menus
   of the app owning the focused window, with optional per-window
   overrides.
6. **No policy in rio.** `/dev/ux` has the same trust as the rest of
   rio's files: whoever can open `/dev/wctl` or `/dev/kbd` can already
   drive the window system. rio never exports `/dev/ux` on its own, and
   it must not be exported to untrusted clients. Per-control policy
   lives in uxd ([ux.md](ux.md#rights)).

## Where it is

rio serves `/dev/ux` alongside its other files, so it appears wherever
rio's tree is mounted: `/dev/ux` in a window's namespace and
`/mnt/wsys/ux` from the window-system mount. When rio's tree was
attached for a particular window (the normal case for programs started
in a window), that window is the fid's **own window**; commands that
take an optional `winid` default to it. A fid with no own window must
name the window.

```text
/dev/ux/
    version          r    protocol version and features
    ctl              rw   mode and settings
    events           r    stream: windows, focus, mode
    windows          r    snapshot of all windows
    desktop/
        ctl          rw   claim and release the desktop
        state        r    snapshot: desktop window and rectangle
        events       r    stream: claim, release, resize
    menubar/
        ctl          rw   app, window and menu registration; invoke
        menus        r    snapshot of every registration
        events       r    stream: actions and bar focus
    drag/            (reserved for the drag feature; see below)
```

Permissions are like rio's other files: owned by the user running rio,
mode 0600 for files (0400 where read-only), 0500 for directories.

## version

```text
devux 1.0
feature focus
feature desktop
feature menubar
```

`drag` is added as a feature when the drag files exist.

## ctl (`/dev/ux/ctl`)

Reading returns the current settings, one per line:

```text
mode on
cmdkey mod4
```

Commands:

| Command | Meaning |
|---|---|
| `mode on` / `mode off` | Turn 9/UX presentation and interception on or off |
| `cmdkey key` | Set the Command key: `mod4` (default), `alt`, `ctl`, or `none` (no shortcuts) |

**Mode off** is stock rio: no menu bar strip, no shortcut
interception, and a claimed desktop window is hidden (as with rio's
Hide) until the mode comes back on. Every `/dev/ux` file keeps working
in either mode: registrations are accepted and kept, events still flow,
and `invoke` still delivers actions. Switching the mode queues a `mode`
event.

## events (`/dev/ux/events`)

A stream (see [README.md](README.md#stream-files-events)) of window-system
changes. Messages:

| Event | Attributes | When |
|---|---|---|
| `resync` | | First message of every subscription |
| `lost` | `n` | Queue overflowed; reread `windows` |
| `new` | `win`, `rect`, `label` | A window was created |
| `delete` | `win` | A window was deleted |
| `focus` | `win`, `app` | Keyboard focus moved (rio's current window). `win=0` when no window has focus; `app` absent when the window has no app |
| `rect` | `win`, `rect` | A window moved or was resized |
| `hide` / `unhide` | `win` | A window was hidden or shown again |
| `top` | `win` | A window was raised to the top |
| `label` | `win`, `label` | A window's `/dev/label` changed |
| `app` | `win`, `app` | A window joined an app (`app` absent: it left one) |
| `desktop` | `win` | The desktop was claimed (`win=0`: released) |
| `screen` | `rect` | The screen was resized |
| `mode` | `ux` | The mode changed (`ux=on` or `ux=off`) |

`rect` is the window's full rectangle in screen coordinates, as in
`/dev/wctl`.

Example:

```text
1 resync
2 new win=7 rect=100,100,500,400 label=rc
3 focus win=7
4 app win=7 app=sketch-1
5 focus win=7 app=sketch-1
6 rect win=7 rect=120,100,520,400
```

## windows (`/dev/ux/windows`)

A snapshot of every window, one line each, from the bottom of the stack
to the top:

```text
window win=3 rect=0,-4,1024,768 label=finder app=finder-1 state=desktop
window win=7 rect=120,100,520,400 label=rc state=
window win=9 rect=200,150,700,500 'label=Read Me' app=sketch-1 state=current
```

`state` flags: `current` (has keyboard focus), `hidden`, `desktop` (the
claimed desktop window). `app` is absent for windows without an app.

## Desktop

The desktop is the layer behind all windows. Finder (or any desktop
client) provides it by claiming the desktop role for one of its own
windows.

### desktop/ctl

| Command | Meaning |
|---|---|
| `claim [winid]` | Make the window (default: the fid's own window) the desktop window. Fails with `in use` if another fid holds the claim |
| `release` | Give up the claim; the window becomes an ordinary window again |

The claim belongs to the fid, as described in
[README.md](README.md#lifetimes-and-ownership): closing it, or the window
being deleted, releases it. Reading `desktop/ctl` returns its help.

While a window is the desktop window, in mode on, rio:

- keeps it below every other window; it is never raised, by clicks,
  `wctl top`, or `current`;
- sizes it so that its drawable area (the window inset by
  `Borderwidth`, as libdraw's `getwindow` computes it) is exactly the
  **desktop rectangle**: the screen minus the menu bar strip. The border
  falls off screen or under the bar, so no border is drawn. It is
  resized again when the screen changes;
- leaves it out of rio's window lists and Hide/Unhide menus, and refuses
  user and `wctl` moves, resizes, hides and deletes for it (wctl
  `delete` from the client itself still works);
- gives it mouse input wherever no other window or the bar is hit, and
  keyboard focus when it is clicked, through its own `/dev/mouse` and
  `/dev/kbd`, exactly as for any window.

### desktop/state

```text
win=3 rect=0,20,1024,768
```

`rect` is the desktop rectangle. `win=0` when nobody holds the claim.

### desktop/events

| Event | Attributes | When |
|---|---|---|
| `claim` | `win` | The desktop was claimed |
| `release` | `win` | The claim ended |
| `resize` | `rect` | The desktop rectangle changed (screen resize, mode change) |

## Menu bar

In mode on, rio draws a strip across the top of the screen and owns
everything in it. From left to right it shows:

1. **The rio menu**, always present and owned by rio: rio's own commands
   (New, Resize, Move, Delete, Hide), the list of windows, and Unhide.
   This is how the classic button-3 operations stay reachable when a
   desktop client owns the background.
2. **The foreground app's title** (its `title`, or the focused window's
   label for a window without an app).
3. **The foreground app's menus**, in order.

The **foreground app** is the app owning the window that has keyboard
focus. When that window has no app (a legacy window), the bar shows only
the rio menu and the window's label: legacy windows get no other menus
and lose nothing.

Menus are rendered and tracked by rio; an app never draws them. A
chosen item becomes an `action` event; the app never sees the clicks.

### menubar/ctl

One open of `menubar/ctl` is one **registration session**. An app opens
it once, declares itself, claims its windows, defines menus, and keeps
the fid open. Closing it removes everything it registered.

Commands that only make sense after `app` fail with `bad syntax: no app`
before it.

| Command | Meaning |
|---|---|
| `app appid [title=text]` | Declare the app for this session. Must be first. `appid` is the app's uxd app ID when uxd is running, and otherwise `name-pid`. Fails with `in use` if a live session already uses the ID |
| `title text` | Change the app title shown in the bar |
| `window [winid]` | Add a window to this app (default: the fid's own window). A window belongs to at most one app: `in use` if another session has it. The window leaving (`unwindow`, deletion, or the session ending) is reported as an `app` event |
| `unwindow winid` | Remove a window from this app |
| `menu menuid title=text [win=winid] [after=menuid] [top=off]` | Define or replace a menu. Menus appear in the order defined, or right after `after`. `win=` makes it a **window override**: shown only while that window has focus, replacing the app-wide menu with the same ID (or added, if there's none). `top=off` defines a menu that only appears as a submenu |
| `item menuid actionid [label=text] [key=shortcut] [enabled=on/off] [check=on/off/mixed] [sub=menuid] [sep=on] [after=actionid]` | Add or replace an item in a menu. Items appear in the order added, or after `after`. `sep=on` makes a separator (its other attributes are ignored, but it still needs an ID). `sub=` opens another menu as a submenu. The default is `enabled=on`, `check=off`, no key |
| `set actionid attr...` | Change `label`, `key`, `enabled` or `check` on every item with that action ID, in any menu |
| `del menuid [actionid]` | Remove a whole menu, or one item from it |
| `clear` | Remove all of this app's menus (windows stay claimed) |
| `begin` | Start a batch: nothing on screen changes until `commit` |
| `commit` | End the batch and show the result |
| `abort` | Discard everything since `begin` |
| `invoke appid actionid [win=winid]` | Fire an action as if chosen from the bar. Works on any fid, with or without `app`. `not found` if the app has no such action, `disabled` if every item for it is disabled |

A batch may span several writes. If a command in a batch fails, the
write returns the error and the batch stays open; the client should
`abort`. A batch left open when the fid closes is discarded with the
rest of the session.

An action ID may appear in several menus (a toolbar-like duplicate, or
the same command in a window override); all those items fire the same
action.

#### Shortcut syntax

`key=` names the key typed with Command held. It is optional modifier
prefixes, each ending in `+`, then one key:

- modifiers: `shift+`, `alt+`, `ctl+` (in addition to Command);
- key: one printable character (letters are written in lowercase and
  match either case), or one of `up down left right home end pgup pgdown
  ins del bs tab esc enter space f1` … `f12`.

Examples: `key=s`, `key=shift+z`, `key=,`, `key=alt+f4`.

### menubar/menus

A snapshot of every registration, in the `menubar/ctl` grammar, so it
can be read by tests and replayed. Sessions are separated by their `app`
line; the first line says what the bar shows now:

```text
focus app=sketch-1 win=9
app sketch-1 title=Sketch
window 9
window 12
menu file title=File
item file new label=New key=n
item file open 'label=Open…' key=o
item file s1 sep=on
item file close label=Close key=w
menu edit title=Edit
item edit undo label=Undo key=z enabled=off
menu view title=View win=12
item view grid 'label=Show Grid' check=on
```

`focus` has `win=0` and no `app` when no window has focus. Every
attribute with a non-default value is written; defaults are left out.

### menubar/events

| Event | Attributes | When |
|---|---|---|
| `action` | `app`, `win`, `id`, `via` | An item was chosen. `win` is the focused window at the time (or the `win=` given to `invoke`; `0` if none). `via` is `click`, `key` or `ctl` |
| `focus` | `app`, `win` | The foreground app changed (`app` absent: a legacy window or none) |

Every reader sees every app's actions; an app picks out its own by
`app`. Example:

```text
1 resync
2 focus app=sketch-1 win=9
3 action app=sketch-1 win=9 id=open via=click
4 action app=sketch-1 win=9 id=close via=key
5 action app=sketch-1 win=0 id=new via=ctl
```

## Keyboard shortcuts

rio already sees every key before a window does. In mode on, when a key
goes down with the Command key held:

1. If the focused window belongs to an app, rio looks for an item whose
   shortcut matches, first among that window's overrides, then among the
   app's menus (top-level menus in bar order, then submenus).
2. If it finds an enabled item, rio swallows the key (down and up) and
   queues an `action` with `via=key`. If the only match is disabled, rio
   swallows the key and does nothing (the bar may flash).
3. Otherwise the key goes to the window exactly as stock rio delivers it,
   Command modifier included.

rio never intercepts a key typed without Command, never intercepts
anything for a legacy window, and has no global Command shortcuts of its
own in 1.0. In mode off, or with `cmdkey none`, nothing is intercepted.

## Pointer routing

In mode on, a mouse event goes to the first of these that contains the
pointer:

1. **Menu bar strip**: rio tracks the menus; nothing is sent to windows.
2. **An active drag**: once the drag feature exists, the drag broker
   gets the pointer until the drop or cancel.
3. **Window chrome**: the border, or the 9/UX title bar on windows that
   opted into classic chrome; handled by rio (move, resize, close).
4. **The topmost window** under the pointer, through its `/dev/mouse`
   (stock rio rules for focus and click-to-type still apply).
5. **The desktop window**, if one is claimed.
6. **rio's background**: the stock button-3 menu.

In mode off, only stock rio's rules apply.

## drag (reserved)

`/dev/ux/drag/` is reserved for drag and drop (workstream S9). It will
have `ctl` (start a drag with typed offers, accept or refuse at a
target, cancel) and `events` (`enter`, `over`, `leave`, `drop`,
`cancel`), following these rules from the design: payloads are typed
offers, the target accepts a type, and bytes or files move through a
brokered endpoint, never as a bare path across namespaces. Until
`feature drag` appears in `version`, the directory may be absent and
clients must not rely on it. Its grammar is added to this document as a
minor version.

## Errors

All errors use the phrases in [README.md](README.md#errors). Specific
cases:

| Case | Error |
|---|---|
| `window` or `claim` on a window that doesn't exist | `not found` |
| A fid with no own window omits `winid` | `bad syntax: no window` |
| `window` on a window another app has | `in use` |
| `claim` when the desktop is claimed | `in use` |
| `item` for an undefined `menuid`, or `sub=` naming an undefined menu at `commit` (or at once, outside a batch) | `not found` |
| `invoke` of a disabled action | `disabled` |
| A bad shortcut, flag or rectangle | `bad value` |

## Examples in rc

Watch focus while clicking around:

```rc
cat /dev/ux/events
```

A tiny app with one window and a File menu. The registration lives as
long as fd 3 stays open, which is until the block ends:

```rc
{
	echo app hello-1 title=Hello >[1=3]
	echo window >[1=3]
	echo menu file title=File >[1=3]
	echo item file quit label=Quit key=q >[1=3]
	grep 'app=hello-1' </dev/ux/menubar/events
} >[3]/dev/ux/menubar/ctl
```

Choose its Quit item without the mouse:

```rc
echo invoke hello-1 quit >/dev/ux/menubar/ctl
```

## Changes

| Version | Change |
|---|---|
| 1.0 | First version |
