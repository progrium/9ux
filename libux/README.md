# libux: the 9/UX Toolbox

A native C library on `libdraw` and `libthread` that gives 9/UX
applications their controls, windows, dialogs and menus.

Its central rule: **controls are application state, not pixels.** Every
libux application serves its windows and controls as a
[uitree](../docs/spike-uitree.md) node tree over 9P, so a shell script
and the on-screen UI see the same objects:

```text
/mnt/ux/apps/sample-1/win/main/save/{type,label,style,bounds,ctl,events,tree}
```

The tree is the source of truth. `uxwindow`, `uxbutton` and the rest of
the C API ([tapi.c](tapi.c)) make nodes in it (a window `main`, then
`KIND-N`, dialogs `dialog-N`; a control's node is named by its ID and
placed by `absolute left= top= w= h=` from its rectangle) and draw each
with a control of the drawing layer (the older functions, renamed `dx*`
by [dx.h](dx.h)). The setters change the node first; a 9P write changes
the node, and the control follows. What a person does to a control is a
uitree event, and that event reaches the control's handler, as one a
script causes does. The node types: button, input or textarea,
checkbox, text, group-box, list, scroll, icon-view, console,
file-chooser, plus div, svg, styled-text, select, radio-group, slider,
switch, tabs and canvas for the spike's controls.

Design: [§5 Toolbox](../docs/design.md#5-toolbox-libux-native-9p-visible-ui).
Plan: [Phase 2](../docs/plan.md#phase-2-9p-toolbox).

## Using it

```c
#include <u.h>
#include <libc.h>
#include <draw.h>
#include <thread.h>
#include <ux.h>

static void
saved(Uxev *e, void*)
{
	if(e->type == UXactivate)
		print("saved\n");
}

static void
quit(Uxev*, void*)
{
	uxquit();
}

void
threadmain(int, char**)
{
	Uxwin *w;
	Uxmenu *m;

	uxinit("hello", "Hello");
	m = uxmenu("file", "File");
	uxitem(m, "quit", "Quit", "q");
	uxaction("quit", quit, nil);
	w = uxwindow("window", "Hello", ZR);	/* the rio window we run in */
	uxtextfield(w, "name", "Name", Rect(10, 10, 200, 32), 0);
	uxhandler(uxbutton(w, "save", "Save", Rect(210, 10, 280, 34)), saved, nil);
	uxmain();
	threadexitsall(nil);
}
```

Link with `libux.a$O` and `-I` this directory (see
[apps/sample/mkfile](../apps/sample/mkfile)). [ux.h](ux.h) is the whole
API: windows (`uxwindow`, `uxdialog`), controls (`uxbutton`,
`uxtextfield`, `uxcheckbox`, `uxlabel`, `uxgroup`, `uxlist`,
`uxiconview`, `uxscrollview`, `uxfilechooser`, `uxconsole`), menus (`uxmenu`,
`uxitem`, `uxaction`), and getters and setters. Every function may be
called from any thread; handlers run on the UI thread with no lock held.

Control, menu and action IDs are what scripts see, so pick stable
lowercase names (`save`, not `button3`).

## Resources

An app can take its menus, dialogs, alerts, strings and icons from a
classic resource file, written as text and compiled with resfs's
`rescomp` ([res.md](../docs/protocols/res.md), "Resource source"):

```c
uxinit("sample", "Sample");
if(uxres(nil) < 0)			/* sample.rsrc next to the binary */
	sysfatal("resources: %r");
uxresmenu(128);				/* MENU 128: File */
s = uxresstr(128, 2);			/* string 2 of STR# 128 */
dlg = uxresdialog(128, dlgev, nil);	/* DLOG 128 and its DITL */
uxresparam(0, "3");			/* ^0 in dialog text */
uxresalert(129, dlgev, nil);		/* ALRT 129 */
```

- **Where.** `uxres(file)` takes a resource file (`.rsrc`, AppleSingle
  or AppleDouble) or a directory resfs already serves. With nil it
  tries `$uxres`, then `/mnt/res/NAME`, then `NAME.rsrc` and
  `resources.rsrc` beside the binary (`argv0`). The system's resources
  (`$uxsysres`, default `/lib/ux/system.rsrc`, made from
  `assets/gen/system.res`) are searched after the app's, so a DITL can
  use the system alert icons 0, 1 and 2.
- **How.** libux starts `resfs` (`$resfs`, default `/bin/resfs`) in a
  proc with its own namespace and mounts it there, so the mount is
  nobody else's and goes away with the app. Files are opened by that
  proc and read by whoever asked.
- **IDs.** Scripts see menus, items and controls by ID, which come from
  titles: lowercase letters and digits, other runs as `-` (`Save As…`
  is `save-as`). A title that gives no ID, or one already used, falls
  back to `menuN`, `MENUID-N` (the item's number, which separators get
  too) or `itemN` (the DITL item number). Command keys are lowercased.
- **Menus.** Titles, keys, separators, disabled items and marks (shown
  as checked) are used; a disabled menu disables its items. Submenus,
  item icons, styles and `MDEF`s aren't yet.
- **Dialogs.** Buttons, check boxes, radio buttons (as check boxes),
  static text (as labels, with `^0`-`^3` replaced) and edit text (as text
  fields, labelled by the static text before them) become controls;
  `icon` items are drawn from the icon resources; other item types are
  skipped. Item 1 is the default button (for an alert, item 2 when its
  first stage asks for it). `fn` gets the window's events and every
  control's; `uxresitem(ctl)` gives the item number an event came
  from, and `uxresctl(win, n)` the control for item `n`. A DITL item's
  disabled bit is ignored. An alert's title is the app's.
- **Icons.** An icon-view item's icon names an icon family (the name
  on its `ICN#`) or gives its resource ID; libux draws `icon/ID/image`
  (the deepest of `icl8`, `icl4`, `ICN#`), darkened through its mask
  when selected. A name with no resource is drawn as a gray square.

## How it works

| Proc | Does | Waits on |
|---|---|---|
| UI (`uxui`, a 64K-stack thread `uxmain` starts) | input, drawing, handlers | mouse, keyboard, resize, menu bar, its kick channel |
| 9P ([tfs.c](tfs.c), one per connection) | the uitree tree: reads, writes, verbs, events | the connection |
| responder ([tev.c](tev.c)) | answers blocked `events` reads | its kick channel |
| menu bar reader | `/dev/ux/menubar/events` actions for this app | rio |
| registration | `/mnt/ux/register` with uxd, re-registering if uxd restarts | uxd |

Two locks: the tree's model lock ([tree.h](tree.h)) and the drawing
layer's `uxlk` ([impl.h](impl.h)), always taken in that order, and
nobody holds either while waiting. The UI loop runs on its own 64K
stack, since drawing (layout, SVG, styled text) and handlers go deeper
than `threadmain`'s default 8K.

Windows: the first `uxwindow` is the rio window the app was started in.
Further windows (`uxdialog`) are new rio windows, made by mounting
`$wsys` with `new` in a private namespace. With no display (`initdraw`
fails, or `uxheadless=1`), libux runs headless: the tree is served the
same way and nothing is drawn.

Publishing: if uxd is running (`/mnt/ux/register`, or
`/srv/uxd.$user`), libux registers its tree with it (`version` says
`uitree 1.0`; uxd takes the rights from each `ctl`'s help, as
[ux.md](../docs/protocols/ux.md) says for uitree apps). With
`uxpublic=1` it instead posts `/srv/ux.$user.$name` for tests and
debugging. With neither, the app runs without being visible in
`/mnt/ux`.

Menus are in the tree's `menu/` (`menu/bar`, then a file per menu of
`item ID label=... key=cmd-K` lines) and registered with rio's bar
through `/dev/ux/menubar/ctl` when `/dev/ux` exists, under the uxd app
ID (or `name-pid` without uxd). A choice is `action ID - via=menu`, and
reaches the `uxaction` handler. Tests choose items with `menu item=ID
[win=NAME]` on `debug/inject`; `debug/windows` lists each window's
place on the screen (`NAME rio-id x0 y0 x1 y1`) for tests that click.

## Drag and drop

[drag.c](drag.c) speaks rio's `/dev/ux/drag` ([devux.md](../docs/protocols/devux.md#drag-and-drop)).
A source builds a `Uxdrag` (`uxdragnew`, then `uxdragdata` for bytes
put up front and `uxdragfile` for files) and calls `uxdragstart` while
the button is down; its control later gets `UXdragend` with the op the
target did, or nil if nobody took it. `uxdroptypes` makes a control a
drop target: libux accepts for it while the pointer is over it, and its
handler gets `UXdrop` and fetches with `uxdropdata`, `uxdropfiles` and
`uxdropsave`. Files are read by the source app in its own namespace and
travel through rio, so a target never gets a path; only when the drop
came from the same app (`uxdragfromself`) does `uxdragpaths` give the
source's paths, for moves within one program.

## Clipboard

[clip.c](clip.c): `uxclipget`, `uxclipput`, `uxclipputn` (several types
of one copy) and `uxcliptypes` use clipd's `/mnt/clip`
([clip.md](../docs/protocols/clip.md)) when it is mounted, and rio's
`/dev/snarf` (text only) when it isn't, so text always interoperates
with legacy programs. `uxedit("cut" | "copy" | "paste" | "selectall")`
works on the front window's focused text field; wire an Edit menu's
actions to it. Secret fields refuse copy and cut.

## Building and testing

```sh
scripts/h9.sh --rc 'cd libux && mk'          # libux.a$O
scripts/h9.sh --rc 'cd apps/sample && mk'    # the sample app
scripts/h9.sh --rc 'cd libux && mk test'     # rc tests, headless
```

The tests ([test/](test/)) run the sample app headless and check its
tree from rc: every control as a node with its props and bounds, sets
from 9P reaching the controls, clicks reaching handlers, lists, icon
views (select, open, move, rename), scroll origins, menus and actions,
a dialog window coming and going, quit, a stalled reader that keeps
every click and the last of the moves, flush of a blocked read, menu
registration, clicks and shortcuts against `test/fakes/fakedevux`, drag
and drop and the clipboard (copying a text field's selection), the
menus, strings and an alert coming from the sample's resources, and
(through [test/ctlapp.c](test/ctlapp.c)) the file chooser's props and
verbs and the spike controls' props, values and style words.
[test/libux/res_test.zig](../test/libux/res_test.zig) (the GUI harness,
`scripts/test.sh gui`) checks what the resources draw against golden
images: `icl8` icons from the app's and the system's resources, a
selected icon, and an alert with a DITL icon.

## Status

Done: the app model on the uitree tree, the control types (window,
dialog, menu, button, text field, checkbox, label, group, list, scroll
view, icon view, file chooser), and `console`, a shell window's text
([cons.c](cons.c), used by [Run Command](../apps/runcmd/runcmd.c)), uxd
registration, menu bar registration, the sample app, and menus,
dialogs, alerts, strings and icons from resources (S10).

Beyond uitree's own types, libux's catalog adds two things of its own
([types/](types/README.md)): a `selection` prop on `input` and
`textarea` (rune offsets `Q0 Q1`), and a `file-chooser` type with `dir`,
`value` and `items` and the verbs `cd`, `choose` and `cancel`. The
spike's controls carry their props (select and radio-group `items` and
`value`, slider `min`/`max`/`step`/`value`, switch `label`/`value`,
tabs with a `tab` child per tab, svg `src`, styled-text `text` and
`runs`, canvas `ops`) and their style words, both ways.

Not yet in the tree: list columns' layout.

Not yet:

- Controls are drawn in the 9/UX look from [assets/](../assets/README.md)
  (roles from `uxstyle.h`, glyphs from `uxglyphs.c`, fonts from
  `fonts.ndb`); icons with no resource are grey squares, and radio
  buttons aren't done yet.
- Drawing has been checked by hand under `h9 --gfx` with rio, not by the
  GUI harness yet; the input paths (clicks, typing, Tab, Enter, Esc)
  have no automated test until S0's harness can inject input.
- Window `close` from rio's own close (deleting the window) isn't
  reported for dialogs yet; the dialog just goes away.
