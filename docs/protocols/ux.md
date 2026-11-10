# ux: app control trees and `/mnt/ux`

**Protocol:** `ux 1.0` · **Servers:** every libux app (its own tree),
and uxd (`/mnt/ux`, aggregating them) · **Fake:**
[`test/fakes/fakeapp`](../../test/fakes/) · **Conventions:**
[README.md](README.md)

Every 9/UX application serves its controls (windows, buttons, text
fields, menus) as a 9P tree. The on-screen UI and a shell script see the
same objects: reading a file gives a control's current state, writing a
command to its `ctl` acts on it the way a click or a keystroke would, and
reading its `events` follows what happens to it. uxd finds the running
apps and presents them together under `/mnt/ux/apps`, deciding who may
change what.

## Decisions

1. **The app owns the truth; uxd brokers.** (Design open question 2.)
   Each app's own 9P server, run by libux, is the only place its control
   state lives. uxd keeps no copy: it passes reads, writes and event
   streams through to the app, adding discovery (the `apps` directory and
   the `winid` map) and policy (rights, redaction). If uxd restarts, no
   app state is lost; apps register again.
2. **Only uxd talks to an app's server.** An app hands its server to uxd
   through a one-time `/srv` entry that uxd removes at once (see
   [Registration](#registration-with-uxd)), so the only way in from
   outside is through uxd and its policy. The app's server trusts what
   arrives over that connection.
3. **Reads are open, changes need rights.** Any client of uxd may read
   the tree, except values marked sensitive. Changing anything needs a
   right granted to the client's attach. Secrets (passwords) are never
   served by the app at all, to anyone.
4. **Everything is files and lines.** State files are snapshots; every
   change goes through a `ctl` command (or, for long text, a `value`
   write); every object has an `events` stream. All of it follows the
   shared [conventions](README.md).

## The app tree

This is what libux serves for one app, and what appears under
`/mnt/ux/apps/<appid>/` through uxd. Paths in events are relative to
this root.

```text
version              r    ux 1.0 and features
name                 r    the app's name (sketch)
id                   r    its app ID from uxd (sketch-1); empty before registration
title                r    the title the menu bar shows (Sketch)
ctl                  rw   app commands
events               r    stream: every event in the app
menus/
    <menuid>/
        title        r
        items        r    the menu's items, in devux item grammar
        ctl          rw   invoke an action
        events       r    stream: actions from this menu
windows/
    <winid>/          rio's window ID (/dev/winid); see below
        kind         r    window, dialog, alert, chooser (a window holding a file-chooser control)
        title        r
        rect         r    the window in screen coordinates
        state        r    flags
        ctl          rw
        events       r    stream: this window and its controls
        controls/
            <ctlid>/
                type     r
                label    r
                state    r    flags
                rect     r    window-local
                parent   r    containing control's ID, or empty
                ctl      rw
                events   r    stream: this control
                ...      type-specific files (below)
```

Window directories are named by rio's `winid` whenever the app runs
under rio. An app with no rio (headless tests, no `/dev/winid`) numbers
its windows 1, 2, … in the order it opens them; those numbers mean
nothing to rio or uxd's `windows` map.

Every directory is listable, and windows and controls come and go while
the app runs (dialogs, dynamic lists of controls). Control IDs are flat
within a window: nesting is described by `parent`, not by directories,
so a control's path never changes when it's moved into a group.

### App files

`ctl` commands:

| Verb | Right | Meaning |
|---|---|---|
| `front` | activate | Bring the app's frontmost window to the top and give it focus |
| `hide` / `show` | activate | Hide or show all the app's windows |
| `quit` | activate | Ask the app to quit, as its Quit menu item would (it may put up a dialog first) |

### Window files

`state` flags: `current` (has keyboard focus), `hidden`, `modal` (a
modal dialog: other windows of the app won't take input).

`ctl` commands:

| Verb | Right | Meaning |
|---|---|---|
| `front` | activate | Raise and focus the window |
| `close` | activate | Close it as its close box would (the app may ask first) |
| `focus ctlid` | activate | Move keyboard focus to a control |
| `default` | activate | Press the dialog's default button (`kind` dialog or alert) |
| `cancel` | activate | Cancel the dialog |

### Common control files

| File | Content |
|---|---|
| `type` | The control type (below), one word |
| `label` | The visible label; empty for controls without one |
| `state` | Flags: `enabled`, `visible`, `focused`, `default` (the default button), `readonly`, `multiline`, `sensitive`, `secret` |
| `rect` | `minx miny maxx maxy`, window-local (space-separated, as on every state file line; attributes in events use the comma form) |
| `parent` | The containing control's ID, or empty for a top-level control |
| `ctl` | Commands for the type; reading it lists `verb right` per line |
| `events` | The control's stream |

Every command on a control that lacks `enabled` fails with `disabled`,
`focus` included. `set`, `insert` and `value` writes on a control with
`readonly` fail with `bad value: readonly`.

### Control types

`ctl` verbs are listed with the right they need. A type's verbs and
files may grow in minor versions.

**`button`**

| Verb | Right | Meaning |
|---|---|---|
| `activate` | activate | Press it |

Events: `activate`.

**`text-field`**

| File | Content |
|---|---|
| `value` | The text. One-line fields: the text and `\n`. Fields with `multiline` in `state`: the raw text, no added newline. A `secret` field always reads empty |
| `selection` | `q0 q1`, rune offsets |

| Verb | Right | Meaning |
|---|---|---|
| `set text` | edit | Replace the whole value |
| `insert text` | edit | Replace the selection with `text`, as typing would |
| `select q0 q1` | edit | Set the selection |
| `focus` | activate | Give the field keyboard focus |
| `submit` | activate | Act as if Enter was typed in a one-line field |

Long or multi-line text can't fit in one token, so a field's `value` file
is also writable, with the `edit` right: open it with `OTRUNC`, write the
new text in any number of writes, and close it. The app applies the
whole text as one `set` when the file is closed. Values are limited to
1 MiB (`too large`).

Events: `value`, `select`, `submit`.

**`checkbox`**

| File | Content |
|---|---|
| `value` | `on`, `off` or `mixed` |

| Verb | Right | Meaning |
|---|---|---|
| `set on` / `set off` | activate | Set it |
| `toggle` | activate | Click it |

Events: `value`.

**`label`** (static text): `value` holds the text. No verbs.

**`group`**: a container for other controls (`parent` points at it).
No verbs.

**`list`**

| File | Content |
|---|---|
| `items` | One line per item, the item's text. The first line is item 0. A list with columns (a Finder list window) separates them with tabs |
| `selection` | The selected item numbers, separated by spaces, then `\n` |

| Verb | Right | Meaning |
|---|---|---|
| `select n...` | activate | Set the selection (no numbers: clear it) |
| `open n` | activate | Double-click an item |

Events: `select` (with `sel=1,4`), `open` (`item=n`), `items` (the items
changed: read `items` again).

**`icon-view`**

| File | Content |
|---|---|
| `items` | One line per item: `item itemid label=text icon=name pos=x,y [state=selected]`. `itemid` follows control ID rules |
| `selection` | The selected item IDs, separated by spaces, then `\n` |

| Verb | Right | Meaning |
|---|---|---|
| `select itemid...` | activate | Set the selection |
| `open itemid...` | activate | Double-click the items |
| `move itemid x,y` | edit | Move an icon |
| `rename itemid text` | edit | Rename an item, as editing its label in place would |

Events: `select` (`sel=a,b`), `open` (`items=a,b`), `items`.

**`scroll-view`**

| File | Content |
|---|---|
| `origin` | `x y`: the content point at the view's top left |
| `extent` | `minx miny maxx maxy`: the whole content |

| Verb | Right | Meaning |
|---|---|---|
| `scroll x,y` | activate | Scroll so `x,y` is at the top left, kept within `extent` |

Events: `origin`, `extent`.

Controls whose `parent` is a scroll view are laid out in content
coordinates and shown moved by `origin`, clipped to what shows of the
view; their `rect` doesn't change as it scrolls. The view's scroll bars
(the vertical one, or both and a grow box as in a folder window) are
always drawn, live when the extent is bigger than what shows.

**`file-chooser`**

| File | Content |
|---|---|
| `dir` | The directory being shown, in the app's namespace |
| `value` | The chosen or typed name |

| Verb | Right | Meaning |
|---|---|---|
| `cd path` | activate | Show another directory |
| `choose path` | edit | Choose a file and close the chooser |
| `cancel` | activate | Close it without choosing |

Events: `dir`, `value`, `choose` (`path=`), `cancel`.

Paths in a file chooser are in the **app's** namespace. A client in a
different namespace must not assume they name the same files for it.

### Menus

An app's menus are served under `menus/`, mirroring what libux
registered with rio's menu bar ([devux.md](devux.md#menu-bar)), so a
script can see and use them without `/dev/ux`.

`items` uses the `item` lines of devux's `menubar/ctl` grammar, without
the leading menu ID:

```text
item new label=New key=n
item open 'label=Open…' key=o
item s1 sep=on
item close label=Close key=w enabled=off
```

| Verb | Right | Meaning |
|---|---|---|
| `invoke actionid [win=winid]` | activate | Choose the item, as from the bar |

Events: `action` (`id=`, `win=`, `via=`, and `menu=` when known), `items`.
A menu's own `events` carries every action whose ID is among its items.

## Events

Event streams follow the [shared rules](README.md#stream-files-events):
per-open subscriptions, `resync` first, bounded queues (256) with
`lost`, and `gone` when the object goes away. Sequence numbers come from
one counter per app, so the same event has the same `seq` in the app's
`events`, its window's, and its control's.

Every message names the object it's about, so streams can be merged:
`ctl=windows/<winid>/controls/<ctlid>` for controls, `win=<winid>` for
windows, `menu=<menuid>` for menus. A window's `events` carries its own
events and those of all its controls; the app's `events` carries
everything.

| Event | Attributes | Meaning |
|---|---|---|
| `resync` | | First message of a subscription |
| `lost` | `n` | Overflow; reread snapshots |
| `gone` | | The object is gone; end of file follows |
| `new` | `win`, `kind` / `ctl`, `type` / `menu` | A window, control or menu appeared |
| `delete` | `win` / `ctl` / `menu` | One went away |
| `activate` | `ctl`, `via` | A button was pressed |
| `value` | `ctl`, `value`, `redacted` | A value changed. `value` is included when it is one line of at most 1024 bytes and not redacted; otherwise read the `value` file |
| `select` | `ctl`, `q0`,`q1` / `sel` | The selection changed |
| `submit` | `ctl`, `via` | Enter in a one-line field |
| `open` | `ctl`, `item` / `items`, `via` | Items were opened |
| `items` | `ctl` / `menu` | The items changed; reread `items` |
| `state` | `ctl` / `win`, `state` | Flags changed |
| `label` | `ctl`, `label` | The label changed |
| `rect` | `ctl` / `win`, `rect` | It moved or was resized |
| `title` | `win` (absent for the app's title), `title` | A title changed |
| `focus` | `win`, `ctl` | Keyboard focus moved to a control |
| `origin`, `extent` | `ctl`, `pt` / `rect` | A scroll view changed |
| `dir`, `choose`, `cancel` | `ctl`, `path` | A file chooser changed or finished |
| `action` | `menu`, `id`, `win`, `via` | A menu action was chosen. `menu` is optional: an action ID can be in several menus, and the menu bar reports only the ID. libux names the first menu holding the action when it knows it |

`via` says where an action came from: `ui` (a person using the window),
`key` (a keyboard shortcut), `menu` (the menu bar), or `ctl` (a 9P
command). Example, as read from an app's `events`:

```text
1 resync
2 focus win=9 ctl=windows/9/controls/name
3 value ctl=windows/9/controls/name value=hello
4 activate ctl=windows/9/controls/save via=ui
5 activate ctl=windows/9/controls/save via=ctl
6 action menu=file id=close win=9 via=key
7 delete win=9
```

## Registration with uxd

An app publishes its tree by handing its 9P server to uxd. libux does
all of this; it is written down so other implementations (and the fake)
can do the same.

1. The app makes a pipe, serves its tree on one end, and posts the other
   in `/srv` as `ux.$user.$pid.$nonce`, mode 0600, where `$nonce` is 16
   random hex digits.
2. It opens `/mnt/ux/register` for reading and writing, and writes one
   command:

   ```text
   register name=sketch srv=/srv/ux.glenda.123.0f3a9c2e1b7d4c55 [title=Sketch] [id=sketch-1] [win=9]...
   ```

   `id=` asks for a particular app ID (used when re-registering after
   uxd restarts); uxd grants it if it is free and its name part matches
   `name`. Otherwise uxd assigns `name-N`, so an app that restarts
   without `id=` gets a new `N`. `srv=` must name an entry directly in
   `/srv` whose name starts with `ux.` (`/srv/ux.*`, no further `/`);
   anything else fails with `bad value`.
3. uxd opens the `/srv` entry, removes it, attaches, and reads the app's
   `version`. A different major version fails the write with
   `version mismatch`. On success the write returns, and a read of the
   same fid returns the app ID and a newline.
4. The app keeps the fid open. Further commands on it: `window winid`
   and `unwindow winid` as windows open and close, and `title text`.
   uxd keeps the `winid` map from these. Before a successful `register`
   on the fid, these fail with `bad value: not registered`.
5. A second read of the fid blocks until the registration ends, then
   returns end of file (or an error, if uxd went away). The registration
   ends when the app closes the fid, when the app's 9P connection hangs
   up, or when uxd exits.

**When uxd restarts**, the app's blocked read fails. libux then waits
for `/srv/uxd.$user` to come back (retrying with backoff from 250 ms up
to 1 s), mounts that public service on `/mnt/ux` again (registering is
open to every attach, so the app needs no more than that), and registers again with `id=` set to its old
ID. Nothing in the app is lost, and its app ID normally doesn't change.

**Without uxd**, an app runs normally and is simply not visible in
`/mnt/ux`. For tests and debugging, libux can instead post its server
permanently as `/srv/ux.$user.$name` (environment `uxpublic=1`), which
anything can mount directly. That bypasses all policy and is never the
default.

## `/mnt/ux`: uxd's tree

uxd posts two entries in `/srv`:

- `/srv/uxd.$user` — the **public** service. Attaching with an empty
  aname gives the `read` right only. Attaching with a grant token as the
  aname gives that grant's rights.
- `/srv/uxd.$user.admin` — the **session** service: every right,
  including `admin`. The session's startup mounts this one at `/mnt/ux`,
  so the user's own shells can drive their apps.

```text
/mnt/ux/
    version            r    ux 1.0, features
    rights             r    this attach's rights, e.g. "read activate"
    register           rw   app registration (above); open to every attach
    windows            r    winid to app map: "window win=9 app=sketch-1" per line
    events             r    stream: apps and windows registering and leaving
    apps/
        <appid>/       the app's tree, passed through (above)
    grants/            only on attaches with the admin right
        new            rw   mint a grant
        list           r    the grants
        ctl            rw   revoke
```

`/mnt/ux/events`:

| Event | Attributes | When |
|---|---|---|
| `register` | `app`, `name` | An app registered |
| `unregister` | `app` | An app's registration ended |
| `window` | `win`, `app` | A window was mapped to an app (`app` absent: unmapped) |

uxd passes the app's files through unchanged: the same contents, the
same `seq` numbers, the same errors. It adds only what the rights below
require. Each open of a file under `apps/` is one open of the app's
file, so every subscriber has its own queue in the app, and a stalled
reader of `/mnt/ux` stalls only itself.

## Rights

| Right | Allows |
|---|---|
| `read` | Reading every file, except the values of `sensitive` controls. Every attach has it |
| `sensitive` | Reading `value`, `items` and `selection` of controls marked `sensitive`, and their values in events |
| `activate` | `ctl` verbs marked `activate`: pressing, choosing, focusing, scrolling, invoking menu actions, `front`, `close`, `quit` |
| `edit` | `ctl` verbs marked `edit`, and writing `value` files: changing text, renaming, moving icons |
| `keys` | Verbs marked `keys` (typing into a control as raw keyboard input; none in 1.0) |
| `clipboard` | Reserved for the typed clipboard service |
| `admin` | The `grants/` directory |

uxd decides what a `ctl` write needs from the control's own `ctl` help
(`verb right` lines), which it reads once per control and caches; an
unlisted verb is refused. In an app tree every verb names a right; a
verb whose class is `-` is treated like an unlisted one. Refusals
return `permission denied`. A write is checked line by line, and
nothing is forwarded if any line is refused.

On an attach with the `admin` right (the session service), uxd skips
the verb check and forwards the write, so an unknown verb gets the
app's own `unknown command`. Create, remove and wstat are refused
everywhere under `/mnt/ux` with `permission denied`: the tree's shape
belongs to the apps.

**Sensitive and secret controls.** A control with `sensitive` in its
`state` has its `value`, `items` and `selection` readable only with the
`sensitive` right; without it, opening them fails with `permission
denied`, and its events reach the client with every attribute that
carries content removed (`value`, `sel`, `item`, `items`) and
`redacted=on` added. A control with `secret` (a password field) is
stronger: libux never serves its value or its keystrokes to anyone,
uxd included, so `value` always reads empty and its `value` events never
carry a value. Its `value` events are still sent, with `redacted=on`,
so watchers know it changed. Its `type`, `state`, `label` and `rect`
stay visible.

Display focus is not authority: a client's rights come only from how it
attached, never from which window has focus.

### Grants

On an admin attach, a grant is made by opening `grants/new`, writing

```text
grant rights=activate,edit [app=pattern] [note=text]
```

and reading back the token (32 hex digits). `app=` limits the grant to
apps whose ID or name matches a pattern with `*` wildcards (default:
all). A client then attaches to `/srv/uxd.$user` with the token as its
aname:

```rc
mount /srv/uxd.$user /n/ux 0f3a9c2e1b7d4c55e1a2b3c4d5e6f708
```

`rights=` may name any right except `admin`, which comes only from the
session service; a grant asking for it fails with `bad value`. A
client's `rights` file lists its rights only, not the grant's `app=`
scope.

`grants/list` lists `grant token=... rights=... app=... note=...` per
line. `grants/ctl` accepts `revoke token`; attaches using a revoked
token keep only `read`, starting with their next request. uxd keeps
grants in `$home/lib/ux/grants` (mode 0600), so they survive restarts.

Policy is a uxd matter: what crosses a namespace or machine boundary is
decided by which uxd service is mounted there. Export or mount the public
service (with a token if needed) for anything less trusted than the
user's own session; never export the admin service.

## Versions and features

The app tree's `version` reads `ux 1.0` and features; uxd's reads the
same protocol version with its own features (`grants`). A client checks
the version of whichever it opened. Feature names in 1.0:

| Feature | Where | Meaning |
|---|---|---|
| `menus` | app | The `menus/` directory is served |
| `value-write` | app | `value` files accept writes |
| `grants` | uxd | The `grants/` directory and token attaches |

## Examples in rc

```rc
ls /mnt/ux/apps
cat /mnt/ux/apps/sketch-1/windows/9/controls/name/value
cat /mnt/ux/apps/sketch-1/windows/9/controls/save/events &
echo activate >/mnt/ux/apps/sketch-1/windows/9/controls/save/ctl
echo 'set ''hello, world''' >/mnt/ux/apps/sketch-1/windows/9/controls/name/ctl
echo invoke close >/mnt/ux/apps/sketch-1/menus/file/ctl
```

From a less trusted namespace, with no token:

```rc
mount /srv/uxd.$user /n/ux
cat /n/ux/rights                    # read
echo activate >/n/ux/apps/sketch-1/windows/9/controls/save/ctl
# write error: permission denied
```

## Changes

| Version | Change |
|---|---|
| 1.0 | First version |
