# libux: the 9/UX Toolbox

A native C library on `libdraw` and `libthread` that gives 9/UX
applications their controls, windows, dialogs and menus.

Its central rule: **controls are application state, not pixels.** Every
libux application serves its typed control tree over 9P, as
[ux.md](../docs/protocols/ux.md) describes, so a shell script and the
on-screen UI see the same objects:

```text
/mnt/ux/apps/sample-1/windows/17/controls/save/{type,label,state,rect,parent,ctl,events}
```

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
`uxiconview`, `uxscrollview`, `uxfilechooser`), menus (`uxmenu`,
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
| UI (the one calling `uxmain`) | input, drawing, running ctl commands, handlers | mouse, keyboard, resize, menu bar, its kick channel |
| 9P (`uxsrv`, one per connection) | walks, opens, snapshots, reads of state files; queues ctl writes for the UI | the connection |
| responder (`uxresponder`) | answers blocked `events` reads and finished ctl writes | its kick channel |
| menu bar reader | `/dev/ux/menubar/events` actions for this app | rio |
| registration | `/mnt/ux/register` with uxd, re-registering if uxd restarts | uxd |

One lock (`uxlk`, [impl.h](impl.h)) guards the model, and nobody holds
it while waiting. The UI thread never writes to a 9P connection: replies
go through the responder, so a client that stops reading can't stall the
UI. Each open of an `events` file is its own subscription with a queue
of 256 messages; overflow drops the queue and leaves `lost n=N`.

Windows: the first `uxwindow` is the rio window the app was started in.
Further windows (`uxdialog`) are new rio windows, made by mounting
`$wsys` with `new` in a private namespace. With no display (`initdraw`
fails, or `uxheadless=1`), libux runs headless: the tree is served the
same way, windows are numbered 1, 2, ..., and nothing is drawn.

Publishing: if uxd is running (`/mnt/ux/register`, or
`/srv/uxd.$user`), libux registers with it as ux.md says. With
`uxpublic=1` it instead posts `/srv/ux.$user.$name` for tests and
debugging. With neither, the app runs without being visible in
`/mnt/ux`.

Menus are kept under `menus/` and registered with rio's bar through
`/dev/ux/menubar/ctl` when `/dev/ux` exists, under the uxd app ID (or
`name-pid` without uxd).

## Building and testing

```sh
scripts/h9.sh --rc 'cd libux && mk'          # libux.a$O
scripts/h9.sh --rc 'cd apps/sample && mk'    # the sample app
scripts/h9.sh --rc 'cd libux && mk test'     # rc tests, headless
```

The tests ([test/](test/)) run the sample app headless and check its
tree from rc: state files, ctl commands and errors, the `value` file
write, two readers seeing the same events, a stalled reader getting
`lost` while the UI keeps answering, flush of a blocked read, a dialog
window coming and going, quit, and menu registration, clicks and
shortcuts against `test/fakes/fakedevux`, and the menus, strings and
an alert coming from the sample's resources.
[test/libux/res_test.zig](../test/libux/res_test.zig) (the GUI harness,
`scripts/test.sh gui`) checks what the resources draw against golden
images: `icl8` icons from the app's and the system's resources, a
selected icon, and an alert with a DITL icon.

## Status

Done: the app model, all eleven control types of ux.md (`Window`,
`Dialog`, `Menu`, `Button`, `TextField`, `Checkbox`, `Label`, `Group`,
`List`, `ScrollView`, `IconView`, `FileChooser`), uxd registration,
menu bar registration, the sample app, and menus, dialogs, alerts,
strings and icons from resources (S10).

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
- Icon views have no drag yet (S9), and text fields no snarf.
- The uxd registration path is written to ux.md but untested until uxd
  (S4) or a fake uxd exists.
