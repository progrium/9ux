# uxkit: the Toolbox as files

**Design draft:** v0.1 · 21 November 2026  
**Status:** Proposed; nothing here is built yet  
**Builds on:** [ux.md](protocols/ux.md), [devux.md](protocols/devux.md),
[README.md](protocols/README.md) (shared conventions),
[libux](../libux/README.md)

## 1. Summary

Today a program gets 9/UX windows and controls by linking libux, which
means it is written in C against `libdraw` and `libthread`. This
proposal lets any program build and run a 9/UX interface with nothing
but file I/O: `rc`, awk, a Go binary built with `GOOS=plan9`, or
anything else that can open, read and write files.

The idea is small because most of it already exists. Every libux app
already serves its control tree over 9P ([ux.md](protocols/ux.md)): a
script can read any control's state, act on it through `ctl`, and follow
its `events`. What a script can't do is **make** controls. The tree's
shape belongs to the app's C code.

**uxkit** is a libux program whose UI has no C code of its own. It runs
a client program, gives that program a private, writable view of its
own control tree, and builds whatever the client writes there. To
everything else in the system (uxd, rio's menu bar, Desktop, other
scripts) a uxkit app is an ordinary libux app, because it is one.

```rc
window uxkit -n greet -t Greeter ./greet
```

```go
// greet, built with GOOS=plan9: no 9/UX library, only package os
ctl, _ := os.OpenFile("/mnt/kit/ctl", os.O_WRONLY, 0)
fmt.Fprintln(ctl, "window main 'title=Greeter' rect=0,0,300,90")
w, _ := os.OpenFile("/mnt/kit/windows/main/ctl", os.O_WRONLY, 0)
fmt.Fprint(w, `new text-field name label=Name rect=10,10,200,32
new button hello label=Hello rect=210,10,280,34 default=on
`)
```

## 2. Goals and non-goals

### Goals

1. Any language that can do file I/O can build a 9/UX UI, with no
   library and no bindings.
2. A uxkit app is indistinguishable from a C libux app from the
   outside: same `/mnt/ux` tree, same events, same menu bar, same rights.
3. Reuse ux.md's grammar, control types, events and error phrases. The
   client-only parts are additions, written in the same shape.
4. Interaction that needs to be fast (typing, text selection, dragging a
   scroll bar, tracking a menu) never leaves the drawing process.
5. Every libux feature is reachable: menus, dialogs, resources, drag and
   drop, clipboard.

### Non-goals

- A display server or compositor. Drawing stays where it is today:
  libux draws into rio windows with `libdraw`.
- Replacing libux. C apps keep linking it; uxkit is one more libux app.
- A remote-GUI protocol. A uxkit client may run on another machine and
  reach its kit through an imported namespace, but making that fast is
  not a goal of this design.
- Per-pixel custom rendering at frame rate. See [Canvas](#8-canvas-custom-drawing).

## 3. Where it lives

Three places could own this. The proposal is the third.

| Option | For | Against |
|---|---|---|
| **uxd** draws and hosts client UIs | One server for everything | Breaks ux.md decision 1 ("the app owns the truth; uxd brokers"). uxd keeps no state and owns no display today. One crash would take every file-built app with it. Rio's menu bar and window ownership are per app, keyed by the windows a process holds, so uxd would have to pretend to be many apps |
| **A language port of libux** (a Go toolkit on `/dev/draw`) | Native speed, no extra process | One port per language, each re-drawing the 9/UX look and re-implementing the ux tree. This is exactly the duplication the file interface avoids |
| **uxkit**: one libux process per client app | Reuses all of libux. Fails alone. Fits every existing protocol unchanged | One extra process per app, and every client action crosses 9P (cheap on a local pipe; see [§9](#9-latency)) |

## 4. How a client gets a kit

`uxkit` is a command that wraps the client, in the way `win` wraps a
command in acme:

```text
uxkit [-n name] [-t title] [-m mtpt] [-k] cmd [arg...]
uxkit [-n name] [-t title] -s srvname
```

1. uxkit starts in the rio window it was run in (that becomes the app's
   first window, as for any libux app), calls `uxinit(name, title)`, and
   registers with uxd and rio's menu bar as libux always does.
2. It makes a pipe, serves the **author view** of its tree on one end,
   forks the client with `rfork(RFNAMEG|RFFDG)`, mounts the other end
   at `mtpt` (default `/mnt/kit`) in the client's namespace only, sets
   `$uxkit` to `mtpt`, and execs `cmd`.
3. When the client exits, uxkit closes its windows and exits too (`-k`
   keeps them open, for a script that builds a UI and leaves it). When
   uxkit quits, the client's next read or write in `/mnt/kit` fails and
   its blocked `events` reads return end of file.

The author view is never posted in `/srv` and is never seen by uxd. It
exists only in the client's namespace, the same way a C app's own memory
is its own. `-s srvname` posts it as `/srv/srvname` instead of running a
command, mode 0600, for interactive use and tests; like libux's
`uxpublic=1`, it is never the default.

A Go program needs nothing to find its kit: `os.Getenv("uxkit")`, or
`/mnt/kit`. h9 runs `GOOS=plan9` binaries unchanged, so a Go client
runs under h9 and on 9front alike.

## 5. Two views of one tree

uxkit is libux, so its tree is libux's tree. What's new is that libux's
9P server ([srv.c](../libux/srv.c), one `Srv` per connection) learns to
mark a connection as the **author**. The same files are served on both
connections; the author connection adds verbs and files.

| | Public view (through uxd) | Author view (`/mnt/kit`) |
|---|---|---|
| Who | uxd, and through it every `/mnt/ux` client | The client program only |
| Protocol | `ux 1.0`, unchanged | `ux 1.1` with feature `author` |
| Shape | Fixed; create/remove refused (as ux.md says) | Changed with `new`, `window`, `delete` verbs |
| `ctl` help | ux.md's verbs, each with its right | ux.md's verbs plus the author verbs, class `author` |
| Secret fields | `value` always empty | `value` holds the text: the client *is* the app |
| Rights | uxd's policy | All of them; uxd isn't in the path |

Author verbs written on the public connection fail with
`unknown command` and are not listed in its `ctl` help, so uxd's
verb-to-right check needs no change, and a `/mnt/ux` client can't
reshape an app it doesn't own, even with the admin service.

Everything the author does goes through the same model and the same
event queues as a person using the window. A button the client creates
is a `new` event in `/mnt/ux/apps/greet-1/events`; a value the client
sets is a `value` event with `via=ctl` there and in the client's own
stream.

## 6. The author view

The tree is ux.md's app tree, with one difference in naming and a few
additions. Additions are marked **(A)**.

```text
/mnt/kit/
    version            ux 1.1, feature author, and the app tree's features
    name  id  title
    ctl                app verbs, plus (A) window, menu, item, res, quit
    events             every event in the app, plus (A) request events
    menus/<menuid>/...
    windows/
        <name>/        (A) the client's name for the window
            winid      (A) rio's winid, or empty headless
            kind title rect state ctl events
            controls/<ctlid>/
                type label state rect parent ctl events ...
                (A) image          canvas only
```

**Window names.** In the public view windows are named by rio's
`winid`, which the client can't know before the window exists. In the
author view they are named by an ID the client picks (control ID rules),
and `winid` gives rio's number. Event attributes in the author view
use the same names (`win=main`, `ctl=windows/main/controls/name`), so a
client never has to translate. uxd's view and its `windows` map stay
keyed by `winid`, as now.

### App `ctl` (author verbs)

| Verb | Meaning |
|---|---|
| `window id [kind=window\|dialog] [title=text] [rect=r]` | Make a window. The first `window` with `kind=window` takes the rio window uxkit started in (as `uxwindow` does); later ones open new rio windows. `kind=dialog` is modal (`uxdialog`). `rect` is the size wanted, in screen coordinates; omitted, the window keeps its size |
| `menu menuid title=text` | Make or replace a menu (`uxmenu`) |
| `item menuid actionid [label=text] [key=shortcut] [enabled=on/off] [check=on/off]` | Add or replace an item (`uxitem`), with devux's [item grammar](protocols/devux.md#menubarctl) |
| `sep menuid id` | A separator (`uxsep`) |
| `set actionid attr...` | Change `label`, `enabled` or `check` on an action's items (`uxitemenable`, `uxitemcheck`) |
| `title text` | The app's title in the menu bar (`uxsettitle`) |
| `res [path]` | Load resources (`uxres`; no path: the default search) |
| `resmenu id` | A menu from `MENU id` |
| `resdialog winid id` / `resalert winid id` | A dialog or alert from `DLOG`/`ALRT id` and its DITL, named `winid` in the author view |
| `resparam n text` | `^n` in dialog text |
| `quit` | Quit now, without asking (the public `quit` asks) |

Menus written here are the same menus that appear under `menus/` and in
rio's bar. The grammar is devux's `menubar/ctl` without the leading
`app`: uxkit owns the registration session.

### Window `ctl` (author verbs)

| Verb | Meaning |
|---|---|
| `new type id [attr...]` | Make a control of a ux.md type: `button`, `text-field`, `checkbox`, `label`, `group`, `list`, `icon-view`, `scroll-view`, `file-chooser`, and (A) `canvas`. `in use` if the ID is taken |
| `delete id` | Remove a control (`uxdelete`) |
| `config id attr...` | Change a control's attributes (below) |
| `title text` | `uxwinsettitle` |
| `destroy` | Close the window now, without asking (the public `close` asks) |
| `begin` / `commit` / `abort` | A batch, as in devux's `menubar/ctl`: nothing is drawn until `commit`, and `abort` drops everything since `begin` |

Attributes for `new` and `config`:

| Attribute | Types | Sets |
|---|---|---|
| `rect=r` | all | window-local rectangle (`uxsetrect`) |
| `label=text` | all with labels | `uxsetlabel` |
| `parent=id` | all | containing group or scroll view (`uxsetparent`); empty: top level |
| `enabled=on/off`, `visible=on/off` | all | `uxenable`, `uxshow` |
| `value=text` | text-field, checkbox, label, file-chooser | `uxsetvalue`; longer or multi-line text goes through the `value` file, as in ux.md |
| `flags=list` | text-field, label | `multiline`, `secret`, `sensitive`, `readonly`, `small`, `right` (at `new` only) |
| `default=on` | button | the window's default button (`uxdefault`) |
| `dir=path` | file-chooser | the starting directory (at `new` only) |
| `bars=list header=n` | scroll-view | `vbar`, `hbar`, `growbox`; `uxscrollbars` |
| `extent=r`, `origin=x,y` | scroll-view | `uxsetextent`, `uxsetorigin` |
| `columns=list` | list | titles and widths, `Name:200,Size:80` (`uxsetcolumns`) |
| `drop=types ops=list` | all | a drop target (`uxdroptypes`) |
| `mouse=off/buttons/motion` | canvas | which pointer events to send ([§8](#8-canvas-custom-drawing)) |

Lists and icon views hold many items, so their items are written as
files, not attributes. In the author view `items` is writable the way
`value` is in ux.md: open with `OTRUNC`, write any number of times,
close, and uxkit applies the whole set at once. A list takes ux.md's
read format back (one line per item, tabs between columns; an item's
16 px icon as a leading `icon=name` token, `uxsetitemicons`). An icon
view takes its `item itemid label=... icon=... pos=x,y` lines
(`uxiconclear` then `uxiconadd` each).

Every ux.md verb still works in the author view, so the client sets a
field's text the way a script would: `set 'hello'` on its `ctl`.

### A complete client in rc

```rc
#!/bin/rc
k=$uxkit
echo window main 'title=Greeter' 'rect=0,0,300,90' >$k/ctl
echo 'menu file title=File
item file quit label=Quit key=q' >$k/ctl
w=$k/windows/main
echo 'begin
new text-field name label=Name rect=10,10,200,32
new button hello label=Hello rect=210,10,280,34 default=on
new label out rect=10,44,280,62
commit' >$w/ctl
cat $k/events | while(l=`{read}){
	switch($l(2)){
	case activate
		name=`{cat $w/controls/name/value}
		echo config out '''value=Hello, '^$"name^'''' >$w/ctl
	case action
		if(~ $l 'id=quit') echo quit >$k/ctl
	case close quit
		echo quit >$k/ctl
	}
}
```

`cat` reads `events` with an 8 KiB buffer, as streams need (see
[Reading events](#reading-events)); `read` then takes one line at a
time from the pipe. When the app quits, `cat` gets end of file and the
loop ends. The `close` and `quit` cases answer the request events of
the next section.

## 7. Events and requests

Events are ux.md's events, on the same per-open streams, with `resync`
first, bounded queues, `lost` and `gone`. A client follows its app the
way any script does: open `events`, read the `resync`, read the state
it needs, then act on events. Events it caused itself carry `via=ctl`,
so it can skip its own echoes.

C apps get some things as handler calls that have no ux.md event,
because the app is asked and decides: a close box, Quit, a dialog's
default and cancel, a drop. uxkit turns these into **request events**:

| Event | Attributes | Asked when | If nobody answers |
|---|---|---|---|
| `close` | `win`, `via` | The close box, or the public `close` verb | The window closes |
| `quit` | `via` | Quit from the rio menu or the public `quit` verb | The app quits |
| `default` | `win`, `via` | Enter in a dialog, or the public `default` verb | Nothing |
| `cancel` | `win`, `via` | Esc in a dialog, or the public `cancel` verb | The dialog closes |
| `drop` | `ctl`, `drag`, `type`, `op`, `pt`, `item` | A drop on a drop target | The drop is refused |
| `dragend` | `ctl`, `drag`, `op` | A drag the client started ended | Nothing |

A client answers by doing the thing (`destroy`, app `quit`) or by not
doing it. **If nobody answers** is the important column: uxkit applies
the default when no author-view `events` file is open, so a client that
never reads events (a one-shot rc script with `-k`) still has a window
that closes and an app that quits. These events would also be useful to
`/mnt/ux` readers, so section [§12](#12-changes-to-existing-protocols)
proposes them for ux 1.1 in both views.

A drop is answered on the drag's own files. The client reads the
payload from rio's `/dev/ux/drag/data` with the drag ID in the event
([devux.md](protocols/devux.md#dragdata)), then writes `drop drag=N
op=copy` or `refuse drag=N` to the control's `ctl` (author verbs on
every drop target), which calls `uxdropresult`. Starting a drag from a
client is `drag ctl=id types=... ops=...` on the window's `ctl`, with
the payload written to a `drag/<n>/` directory first; this is the one
part of the design that still needs working out against `drag.c`.

### Reading events

ux.md's stream rule says a read must ask for at least 8192 bytes, or it
fails with `short read`. That's the one trap for clients in other
languages. Go's `bufio.NewReader` reads in 4096-byte chunks, so a Go
client wraps the file with `bufio.NewReaderSize(f, 8192)` (or larger)
and uses `ReadString('\n')`. In rc, `cat` on the file is fine and `read` on it is not.

## 8. Canvas: custom drawing

The fixed control set covers forms, dialogs, lists and browsers. A
paint program, a chart, a game board or a terminal needs to draw. A
client can always open its own rio window and use `/dev/draw` directly,
but then that window has no 9/UX controls. The `canvas` control fills
the gap inside a 9/UX window.

**v1: an image you write.** A canvas has an `image` file. The client
writes a whole image(6) image (compressed or not) to it, `OTRUNC`
then close, as with `value`. uxkit loads it with `readimage` and draws
it in the canvas's rectangle, clipped and not scaled. A canvas with no
image draws the window background. The size limit is 16 MiB (`too
large`). Writing is whole-frame, so this suits pictures that change on
an action (a chart, a preview, a board after a move), not animation.

**Pointer input.** `mouse=buttons` sends `mouse ctl=... pt=x,y
buttons=n` on presses and releases. `mouse=motion` adds motion while a
button is down, **coalesced**: if a motion event for the canvas is
still queued for a subscriber, the new one replaces it instead of
being added behind it. A slow client sees fewer, newer points, never
a growing backlog or `lost`. Keys typed while a canvas has focus come as
`key ctl=... key=...` events in devux's key syntax.

**Later, if v1 is not enough** (in order of cost):

1. A **retained item list**, as in Tk's canvas: `line`, `rect`,
   `ellipse`, `text`, `image` items with IDs, written to the canvas's
   `ctl` and changed by ID. Drawing stays in uxkit, so dragging an item
   costs one `config` per motion event, not one image.
2. A **draw-message passthrough**: the canvas exposes a `data` file
   that takes `draw(3)` messages against a screen image clipped to the
   canvas. This is `/dev/draw` scoped to a rectangle, fast but low level,
   and only for clients that already speak the draw protocol.

## 9. Latency

Every client action is a 9P write on a local pipe and every event a 9P
read. On 9front that is a few tens of microseconds per round trip; under
h9's emulation it is slower but still well under a frame. That is fast
enough for anything that happens once per user action.

The design keeps everything that happens many times per second inside
uxkit, where libux already handles it:

| Interaction | Where it runs | What the client sees |
|---|---|---|
| Typing, editing, selecting text | libux | `value` and `select` events |
| Dragging a scroll bar, scrolling | libux | `origin` events |
| Tracking a menu | rio | one `action` event |
| Moving icons, rubber-band selection | libux | `items`, `select` events |
| Drag and drop between windows | rio and libux | `drop` and `dragend` |
| Drawing on a canvas | the client | `mouse` events, coalesced |

Only the canvas puts a client in the per-frame loop, which is why its
motion events are coalesced and why its first version is whole images.

## 10. Rights and safety

- The author view is in the client's namespace only. Nothing else can
  reach it unless the client exports it. A process that can mount it
  can do anything the app can.
- uxd's rights apply unchanged to the public view. A grant to `edit` a
  uxkit app's fields is the same as one for a C app's.
- Secret fields work as in ux.md for everyone but the client. The
  client reads the text from `value` in its own view, because a
  password dialog is useless otherwise.
- uxkit applies author writes on the UI thread, in order. A write
  returns once it has taken effect (libux already answers ctl writes
  after the UI thread runs them), so a read after a write sees the
  change. A client that stops reading events stalls only its own
  subscription, as for any reader.
- Input from the client is untrusted in the same way 9P input always is:
  sizes, rectangles and counts are checked, images go through
  `readimage`'s own checks, and limits are the same as ux.md's.

## 11. What libux needs

Most of uxkit is libux as it is. The changes:

1. **Author connections in [srv.c](../libux/srv.c).** A flag on a `Srv`
   that enables the author verbs and files, the secret `value`, and
   window names instead of winids. A small exported call for uxkit,
   such as `int uxserveauthor(int fd)`.
2. **Window names.** Windows keep their winid for rio and uxd, and gain
   an author-chosen name.
3. **Request events.** The places that call a C handler for `UXclose`,
   `UXquit`, `UXdefault`, `UXcancel`, `UXdrop` and `UXdragend` also
   queue a ux event, and uxkit's handler applies the default when no
   author subscription is open.
4. **Item and image writes.** `items` writable for list and icon view,
   and the new `canvas` type with `image`, `mouse` and `key` events.
5. **Coalesced events.** A queue operation that replaces a queued event
   of the same kind and object instead of appending, for canvas motion.

uxkit itself is then a short program: parse flags, `uxinit`, make the
pipe, `uxserveauthor`, fork and exec the client, `uxmain`.

## 12. Changes to existing protocols

All of these are minor-version additions under ux.md's rules: new
events (which readers already ignore when unknown) and new features.

| Where | Change |
|---|---|
| ux.md | `ux 1.1`. Request events `close`, `quit`, `default`, `cancel` in every view, so `/mnt/ux` scripts can watch for them too. Type `canvas` (public view: `type`, `label`, `state`, `rect`, `parent`, `ctl` with no verbs, `events` with `mouse` and `key` events; `image` readable with `read`) |
| ux.md | Feature `author`, served only on author connections, and a new section that points to a `kit.md` protocol document for the author view |
| docs/protocols/kit.md | New: the author view as specified in §6 to §8 of this draft, with a fake (`test/fakes/fakekit`) per the [README](protocols/README.md#testing) rule |
| devux.md | None |
| uxd | None. It never sees the author view, and new public events pass through |

## 13. Plan

| Step | Delivers | Check |
|---|---|---|
| 1 | `kit.md` from this draft; `fakekit` serving the author view headless | rc tests against the fake, as for the other protocols |
| 2 | libux author connections, window names, `new`/`config`/`delete`, menus; `uxkit` | The rc Greeter above runs headless and its controls appear in `/mnt/ux` through uxd |
| 3 | Request events and their defaults; `items` writes | A client that never reads events gets a window that closes; one that does can refuse |
| 4 | A Go example client under `apps/` | Runs under h9 with `GOOS=plan9`, unchanged on 9front |
| 5 | `canvas` with `image`, coalesced `mouse` and `key` | GUI harness golden image; a stalled reader never gets `lost` from motion |
| 6 | Drag from the client; retained canvas items, if wanted | Later |

## 14. Open questions

- **Layout.** libux places controls by rectangle, and so does this draft.
  That is the classic Toolbox way and it matches DITL resources, but it
  is tedious from a script. A small layout attribute on `group`
  (`layout=column gap=8 pad=12`, children sized by a `size=` hint or
  their natural size) would help a lot. Does it belong in libux, for C
  apps too, or only in uxkit?
- **One process per app, or one uxkit for many.** The draft runs one
  uxkit per client, which keeps one app per libux process. A shared
  host would save memory but needs libux to hold several apps, and rio's
  menu bar would have to see them as separate apps.
- **Attaching to a running program.** The wrapper form starts the
  client. A program that is already running (a daemon wanting a status
  window) could instead `mount /srv/uxkit.$user.name`. Is `-s` enough,
  or does that need a factory?
- **Text editing beyond a field.** A multi-line text field is enough
  for notes, not for an editor. Larger text could reuse acme-style
  `body`, `addr` and `data` files on a `text-view` type. Worth doing
  only if a real client needs it.
- **Name.** `uxkit` and `/mnt/kit` are placeholders.

## 15. Prior art

- **Inferno's Tk** (`tk(2)`, `wmlib`): a program sends Tk commands as
  strings and receives named events on channels. uxkit uses files for
  both, and keeps one widget set shared with C apps.
- **acme**: windows are directories with `ctl`, `addr`, `data`, `body`
  and `event` files, and `win` wraps a command in a window. uxkit's
  wrapper and author view follow that model for controls.
- **libcontrol** (`control(2)`): controls driven by text messages on
  channels inside one process. ux.md already took its message shape.
- **ux.md** itself: the read, act and follow half of this design is
  already built and tested; uxkit adds the make half.
