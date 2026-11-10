# Finder

The 9/UX desktop: volume and alias icons on rio's desktop layer, spatial
folder windows (icon and list views), select, open, rename, duplicate,
Get Info, Trash, and remembered geometry and layout. It is an ordinary
libux application with its own published UI tree.

Finder is a view of a Plan 9 namespace, not a global filesystem. Volumes
are only what the volume registry declares; operations follow what each
file server actually supports and report honestly when it doesn't; and
documents open through the plumber.

Design: [§4 Finder](../docs/design.md#4-finder).
Plan: [Phase 3](../docs/plan.md#phase-3-finder-mvp).

## Core library (`libfcore`)

Everything in Finder that isn't drawing lives in a library,
[`fcore.h`](fcore.h), so it can be tested headless and driven from the
shell through [`fcore`](#fcore-from-the-shell). The UI (S7b) builds on it.

| File | What it does |
|---|---|
| [`meta.c`](meta.c) | Client of uxmeta's `/mnt/meta` ([meta.md](../docs/protocols/meta.md)): queries, `ctl` writes, the generation number, and a parser for the ndb records it returns |
| [`vol.c`](vol.c) | The workspace and its volume registry, loaded from uxmeta's `volume` records (or added by hand); checking a volume, attaching one that declares `addr=` or `srv=`, and resolving `VOL:path` names |
| [`folder.c`](folder.c) | The directory model: a folder's entries, rescanning and reporting what was added, removed, changed or renamed, and sorting by name, kind, date or size |
| [`caps.c`](caps.c) | Classifying a server's error strings, and what each volume's server supports, learned by probing or from use |
| [`ops.c`](ops.c) | Rename, move, copy, duplicate, delete, new folder, permissions |
| [`info.c`](info.c) | Get Info: kind, size (optionally of a whole folder), owner, mode, writability, and the association uxmeta resolves |
| [`trash.c`](trash.c) | The Trash: one per volume, Put Back, emptying, and what each volume's Trash can't do |
| [`launch.c`](launch.c) | Opening documents through the plumber, Open With, and launching apps in the workspace |

### Volumes and the workspace

A workspace (`Fcws`) has a name, carried by every launch as
`$workspace` and as a `workspace=` plumb attribute, and a list of
volumes. Volumes are only those declared in uxmeta (`volume=NAME
path=MOUNTPOINT`, with `addr=` or `srv=` and `spec=` for one Finder
mounts itself); paths that aren't under a volume still work, but Finder
never invents a volume for them. A record whose `path` isn't absolute is
ignored.

`fcvolcheck` stats the mount point and opens it. While a volume is
online its identity (server type, device, root qid) is remembered, so if
its server goes away (`gone: ... i/o on hungup channel`) or its mount is
replaced by something else, the volume shows **offline** with the
reason, instead of quietly showing whatever is now at that path.

### Folders and change detection

`fcfolderopen` reads a directory; `fcrescan` reads it again and returns
the changes: `add`, `del`, `mod` (qid version, length, mtime, mode or
owner changed), and `rename`, when an entry disappears and another with
the same qid appears in the same scan. Qids are only hints within one
session (design §4); nothing relies on them across remounts. If the
folder can't be read, `fcrescan` returns -1, keeps the last entries,
and says why in `f->err`.

### Honest file operations

9P renames only within a directory (`wstat` of the name), and some
servers have no `wstat` at all. So:

- **Rename** uses `wstat`. On a server that can't, it fails with
  `not supported: VOL can't rename (SERVER ERROR); copy and delete
  instead`, and records `rename no` for the volume. With `Fcopydel` it
  copies and then deletes instead.
- **Move** to another folder always needs copy and delete, and fails
  with `not supported` unless the caller passes `Fcopydel`: the UI
  offers it, the library never does it silently.
- **Copy** is recursive, keeps modes and (where the server allows)
  modification times, and removes what it copied if it fails part way
  or is cancelled through the workspace's progress hook.
- **Delete** of a non-empty folder needs `Frecursive`, refuses to
  delete a volume's root, and refuses to descend into a mount point. A
  delete that stops part way says `partial: deleted N items, then ...`.
- **Duplicate** names copies `name copy.ext`, `name copy 2.ext`, ...;
  **new folder** makes `untitled folder`, `untitled folder 2`, ...
- After a rename or move, Finder tells uxmeta (`rename OLD NEW`) so the
  file's layout follows it.

`fcprobe` tests a volume's server on a scratch file (create, write,
chmod, mtime, rename, mkdir, remove), removing everything it made.
Operations also update what's known, so a capability can be `yes`, `no`
(with the server's error), or `unknown`. Permission errors never mark a
capability as missing: the server supports it, this user may not.

### Errors

Library errors start with one of these phrases, then `: ` and detail;
callers match on the phrase. They extend the shared list in
[protocols/README.md](../docs/protocols/README.md#errors).

| Phrase | Meaning |
|---|---|
| `not supported` | The server doesn't do this (no `wstat`, read-only, ...), or 9P can't (moving between folders) |
| `permission denied` | The server does it, but not for this user |
| `gone` | The server or its connection went away |
| `not found` | No such file, volume, application, or nothing opens this document |
| `exists` | The destination name is taken |
| `not empty` | Deleting a folder with things in it, without `Frecursive` |
| `partial` | Part of an operation happened; the detail says what's left |
| `cancelled` | The progress hook cancelled a copy or delete |
| `bad value` | A bad name, a folder into itself, a volume's root |
| `failed` | Any other error from the server, with its text |

### Trash

Every volume has its own Trash, `.trash` at its root, and nothing ever
goes to another volume's Trash. 9P can't move a file between folders,
so `fctrash` copies the item into a fresh slot of its volume's Trash
(`.trash/STAMP/NAME`, next to `.trash/STAMP/.origin`, which records
`path=` within the volume and `date=`) and then deletes the original;
if the delete fails, the copy is removed again and the original stays.
`fcputback` copies an item back where it came from (`exists` if
something has taken its place, `not found` if its folder is gone),
`fctrashlist` lists a volume's Trash oldest first, and `fcemptytrash`
deletes everything in it. A path not on a volume, or a volume that
can't make the folder, fails with `not supported`, and the UI offers
Delete instead. `fctrashlimit` gives the sentence the UI shows about a
volume's Trash: that items are copied then deleted (slow for big
folders), or why this volume can't have one. Folder views should hide
`.trash`.

### Opening and launching

`fcopen` returns `Ofolder` for a folder (Finder opens it itself).
Anything else is sent to the plumber with `src=finder`, `wdir` the
file's folder, `type=text`, the path as data and `workspace=NAME`. If
uxmeta resolves the file to a port, the message is addressed there;
otherwise the plumbing rules (uxmeta's generated rules and the user's
own) decide. `fcopenwith` addresses a particular app's port, or runs its
`exec` with the file. `fclaunch` runs an app record's `exec` (or an
absolute command) in a copy of Finder's namespace and environment with
`$workspace` set, in a new rio window when Finder has one (`$wsys`).

## fcore from the shell

```text
fcore [-M] [-m metaroot] [-w workspace] [-v name=path[@addr]]... [verb args...]
```

`-m` names where uxmeta is mounted (`/mnt/meta`; without it there, no
registry is used), `-M` uses none, `-v` adds a volume by hand (with `@addr`, one that `attach` dials). A path
argument may be `VOL:rel/path`. With no verb, fcore reads one command per
line from standard input and keeps what it learns (capabilities, volume
identity) between them, printing `> command` before each result and
`error: ...` for failures.

| Verb | Prints |
|---|---|
| `volumes` | `NAME PATH STATE desktop=yes\|no [icon=] [addr=] [srv=] ['err=...']` per volume |
| `check VOL` | the volume's line; fails if it's offline |
| `attach VOL`, `detach VOL` | mount or unmount a volume with `addr=` or `srv=` |
| `caps VOL`, `probe VOL [DIR]` | `KIND yes\|no\|unknown ['SERVER ERROR']` per capability |
| `ls PATH [name\|kind\|date\|size]` | `NAME d\|- LENGTH MODE MTIME` per entry |
| `watch PATH MS N` | `scan N`, then `add\|del\|mod NAME` or `rename OLD NEW` over N rescans MS apart |
| `info [-d] PATH` | `name`, `path`, `where`, `kind`, `volume`, `size`, `items`, `mode`, `owner`, `group`, `modified`, `writable`, and `type`/`open`/`icon` from uxmeta |
| `rename [-c] PATH NAME`, `move [-c] PATH DIR` | nothing; `-c` allows copy and delete |
| `copy PATH DIR [NAME]`, `dup PATH`, `mkdir DIR [NAME]` | the new path |
| `rm [-r] PATH`, `chmod MODE PATH` | nothing |
| `trash PATH`, `putback TRASHPATH` | where it went |
| `trashlist VOL` | `TRASHPATH ORIGIN DATE` per item, oldest first |
| `emptytrash VOL` | nothing |
| `trashlimit VOL` | the sentence the UI shows about the volume's Trash |
| `open PATH` | `folder PATH` or `plumbed PATH PORT` |
| `openwith PATH APP`, `launch APP [DIR [ARGS...]]` | `plumbed`, `launched` or `launched PID` |

## The Finder app

```sh
finder [-D] [-M] [-m metaroot] [-w workspace] [-i ms] [-v name=path]...
```

Run under rio, Finder claims the desktop (`/dev/ux/desktop/ctl claim`),
turns off its own window's chrome, and shows the workspace's volumes
down the right edge with the Trash in the bottom right corner. A double
click opens a volume or folder in its own window; opening it again
brings that window forward. `-D` leaves the desktop alone (the desktop
becomes an ordinary window), `-M` runs without uxmeta, `-m` names
uxmeta's mount (default `/mnt/meta`), `-w` the workspace, `-i` how often
open folders are rescanned, and `-v` adds a volume by hand. The look
comes from `uxstyle.h` and the system icons from
`$uxsysres` (default `/lib/ux/system.rsrc`).

The menus:

| Menu | Items |
|---|---|
| File | New Folder (n), Open (o), Close Window (w), Get Info (i), Duplicate (d), Rename (r), Move to Trash, Put Back, Eject (e) |
| Edit | Select All (a) |
| View | as Icons, as List, by Name, by Kind, by Date, by Size, Clean Up |
| Special | Empty Trash |

Items are selected by clicking or with a rubber band from empty space,
and moved by dragging. Dragging onto a folder, another window or the
Trash moves within a volume and copies across volumes; other apps can
drop files in through rio's drag layer. Rename edits the label in place;
Get Info is a window per item with an editable name, and it refreshes
when the file changes. When a rename or move has to copy and delete
(a server with no `wstat`), Finder asks first. A volume whose Trash
can't work offers Delete, after a warning, instead.

### Layout in uxmeta

Finder keeps all its layout in uxmeta's workspace layer and reads it
back on start:

- `folder=PATH x= y= width= height= view= sort= open=yes|no`: a
  window's rectangle, view and sort, and whether it was open when
  Finder last ran. Windows with `open=yes` are reopened at start.
- `item=PATH x= y=`: an icon's centre, in its window or on the desktop.
- `volume=NAME x= y=`: a volume icon on the desktop.

Finder follows uxmeta's `changes` stream, so an edit to the ndb file by
hand, or by another program, moves icons at once; a resync reloads
every position. Items without a position go in the first free cell.

### Limits

- Mouse events carry no modifiers, so there is no shift-click to extend
  a selection; use the rubber band or Select All.
- Icon views don't scroll; items past the window's edge are out of
  reach until the window is made bigger or cleaned up.
- An external edit to a window's geometry takes effect the next time it
  opens, not live.
- List view is rows of text in the icon view, not a table control.

## Building and testing

```sh
scripts/h9.sh --rc 'cd finder && mk'        # fcore.$O.a and $O.fcore
scripts/h9.sh --rc 'cd finder && mk test'   # unit tests, test/run, then test/ui.rc
scripts/test.sh gui -- -Dfilter=finder      # Finder under rio
finder/test/remote_test.sh                       # a remote volume (on the host)
```

[`test/unit.c`](test/unit.c) covers the record parser, error classes,
names, volume lookup, rescans (including renames), sorting, every
operation and its errors, Get Info, and cancelling a copy.
[`test/run`](test/run) drives `fcore` against
[`nowstatfs`](test/nowstatfs.c), a server with no `wstat` (rename and
chmod are honestly refused, copy and delete works when asked, and the
volume goes offline as `gone` when the server is killed), and against a
real uxmeta and plumber: volumes from the registry and from an edit to
the ndb file, associations in Get Info, layout following a rename,
opening through the plumber, and launching in the workspace. The
uxmeta part is skipped if `uxmeta/` isn't built.
[`test/remote_test.sh`](test/remote_test.sh) starts a second h9 with `--serve` as
a remote volume, does the operations over it, then stops it and checks
the volume reports `gone`.
[`test/ui.rc`](test/ui.rc) runs the Finder app headless and drives its
published UI tree: the desktop, opening, renaming in place and from Get
Info, moving, new folders, duplicates, list view, the Trash, an external
edit to the layout, a restart, and copy and delete over `nowstatfs`.
[`../test/finder`](../test/finder) runs it under rio: the desktop claim,
a double click, a drag saved to uxmeta and a rubber band.
