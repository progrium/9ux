# Stock rio's files

What 9front 11952's rio serves, as the 9/UX fork must keep serving it.
9/UX only adds to rio (`/dev/ux/...`, [devux.md](devux.md)); everything
here stays as it is, and existing programs (`window`, acme, sam, a nested
rio, anything that opens `/dev/cons` or `/dev/mouse`) depend on it.

Sources: rio(4) and rio(1) in the 11952 tree, and the code in
[`rio/`](../../rio/) (`fsys.c`, `xfid.c`, `wctl.c`, `rio.c`, `wind.c`).
Where this document says more than the manual, the code is the
authority, and a regression test in [`test/rio/`](../../test/rio/) pins
it. Each section names its tests.

## Service and attaches

rio posts its file server as `/srv/rio.USER.PID` and sets `$wsys` to that
path in every process it starts. A mount of `$wsys` needs an attach
specifier, one of:

| Spec | Gives |
|---|---|
| `new [wctl options] [cmd]` | a new window, as the wctl `new` message below; the mount shows that window's files |
| `N` (a window id) | window N's files |
| `none` | a windowless context: `wctl` accepts `new`, and per-window files fail with "command needs to be run within a window" |

The `window` command uses the first two ways: plain `window` writes a
`new` message to a wctl file; `window -m` mounts `$wsys` with a `new`
spec at `/mnt/wsys`, binds it before `/dev`, and execs the command there.
Windows rio makes itself (button 3 New, `-i` startup) get the same
mounts at `/mnt/wsys` and `/dev`.

The `-i cmd` option runs `cmd` with `rc -c` in rio's namespace once rio is
up. 9/UX's test harness uses it to run its agent (`test/harness/agent.rc`).

Tests: `windows_test.zig` ("window through wctl new", "window -m
attaches /mnt/wsys"), `boot_test.zig`.

## Per-window files

Each window's directory (the mount at `/dev` inside it, and
`/dev/wsys/ID/` from anywhere in rio's namespace) has:

| File | Read | Write |
|---|---|---|
| `cons` | typed lines (cooked), or keys (raw) | text drawn in the window |
| `consctl` | | `rawon`, `rawoff`, `holdon`, `holdoff`; closing reverts both |
| `cursor` | the window's cursor image | sets it (mouse(3) format) |
| `kbd` | raw key events, kbdfs(8) format: `k` and the keys held after a press, `K` after a release, `c` and a typed rune, each ending in NUL; while open, keys don't reach `cons` | |
| `label` | the window's label (initially the lead process's pid and command; `window -x` sets it to the command's name) | sets it |
| `mouse` | 49-byte mouse(3) messages in **screen** coordinates; `r` instead of `m` after a resize; while open, rio's own menus, scrolling and editing are off in the window | `m x y` moves the pointer, only while the window is current and under it |
| `screen` | the whole display, image(6) | |
| `snarf` | rio's snarf buffer | sets it |
| `text` | the window's whole text | appends; truncating clears |
| `wctl` | rectangle and state (below); a second read on the same fid blocks until either changes | wctl messages (below) |
| `wdir` | the window's working directory, as rio knows it | sets it (interactive rc keeps it current; rio never follows chdir(2) itself) |
| `window` | the window's image, image(6) | |
| `winid` | the window's id: decimal digits, padded with blanks to 11 characters plus a blank, unique and never reused | |
| `winname` | the name of the window's image in `/dev/draw`, `window.ID.N` | |
| `kbdtap` | global keyboard input, for programs such as ktrans; while open, keys reach windows through writes to it | injects keys |
| `wsys/` | one directory per window, named by its id | |

Tests: `windows_test.zig` (winid, winname, label, the set of files,
`label`, `text`, `wdir`), `devices_test.zig` (`cons`, `kbd`, `mouse`,
`snarf`).

### The window's devices

- **cons.** What's typed while the window is current goes to its
  `cons`, a line at a time unless raw. Typed text echoes in the window.
- **kbd.** A reader gets every key event while the window is current,
  and the same keys no longer go to `cons`.
- **mouse.** Coordinates are the screen's, not the window's. A reader
  gets movement and button changes only while the pointer is inside the
  window (or, with a button held, until it's released).
- **snarf.** One buffer for all windows: what one window writes (the
  buffer changes when the writing fid is closed), another reads. Button
  2 `snarf` and `cut` copy the selection into it and also write it
  through to the underlying `/dev/snarf` (under h9's `--gfx`, the host
  clipboard). Each read of `snarf`, and each `paste` and `send`, first
  reloads the buffer from the underlying `/dev/snarf` if that is not
  empty, so a plain write to rio's `snarf` is only seen until the
  underlying one has text. A nested rio uses its parent's buffer this
  way. (The harness starts each session with an empty host clipboard.)

## wctl

### Reading

Four decimal integers, each padded to 12 characters (image(6) style):
min x, min y, max x, max y of the window on the screen; then `current`
or `notcurrent`, and `hidden` or `visible`, also padded to 12. The first
read on an open fid returns at once; each later read on that fid blocks
until the window's size, place or state changes. (Read it with one
`read`/`dd -count 1`, not `cat`, if you don't want to wait.)

### Messages

A message is one write: a command and options, blank-separated
(`-dx 100`, not `-dx100`). Unknown commands fail with "unrecognized wctl
command"; unknown options, "unrecognized wctl parameter"; bad numbers,
"missing or bad wctl parameter"; text after the options of a command
that takes none, "extraneous text in wctl message".

| Command | Options | Effect, and errors |
|---|---|---|
| `new` | `-r minx miny maxx maxy`, `-minx`/`-miny`/`-maxx`/`-maxy [±]n`, `-dx n`, `-dy n`, `-pid pid`, `-cd dir`, `-hide`, `-scroll`, `-noscroll`, then an optional command and arguments | makes a window running the command with `rc -c` (default `rc -i`). Without geometry, rio picks one. "window allocation failed in wctl request" if it can't |
| `resize` | `-r`, the edge options, `-dx`, `-dy` | changes the window's rectangle |
| `move` | `-r` (min only), `-minx`, `-miny` | moves it, keeping its size, clamped onto the screen |
| `scroll`, `noscroll` | | turns automatic scrolling on or off |
| `set` | `-pid pid` | sets the note group's pid |
| `top`, `bottom` | | raises or lowers it; doesn't change which window is current |
| `current` | | makes it current and raises it; "window is hidden" if hidden |
| `hide`, `unhide` | | "window already hidden", "window not hidden" |
| `delete` | | closes it and hangs up its processes |

For `resize` and `move` (and the rectangle of `new`), a rectangle too
small or off the screen fails with "bad rectangle in wctl request" and
leaves the window as it was. **Only the current window can be resized
or moved through wctl**: on any other visible window both fail with
"window not current". (A hidden window can be resized; it stays hidden.)
A message that names the window's present rectangle succeeds and does
nothing.

Edge options take an absolute value, or a relative one with a sign
(`-maxx +100`). `-dx`/`-dy` set the size from the min corner.

Tests: `wctl_test.zig` (resize with `-r`, `-dx`/`-dy`, `-maxx`; move;
hide and unhide; current, top and bottom; delete; an unknown command and
a too-small rectangle; "window not current").

## Mouse and menus

These are behavior rather than files, but programs and people depend on
them, and the menu bar work (devux.md) must leave them alone for legacy
windows.

- **Button 1** in a window makes it current (and, in a text window,
  selects; a double click selects a word).
- **Button 2** in a text window: the menu `cut`, `paste`, `snarf`,
  `plumb`, `look`, `send`, `scroll`.
- **Button 3** anywhere: the menu `New`, `Resize`, `Move`, `Delete`,
  `Hide`, `Exit`, then the labels of hidden windows (choosing one
  unhides it). The menu opens with the last item chosen under the
  pointer. Items are the font's height plus 2 pixels apart (18 with the
  default font).
  - `New`: sweep a rectangle with button 3; a window running `rc -i`
    opens there. The swept rectangle includes the pixel under the
    release point.
  - `Resize`: click button 3 on a window, then sweep its new rectangle.
  - `Move`: press button 3 on a window and drag it; it moves by the
    drag's distance.
  - `Delete`, `Hide`: click button 3 on a window.
  - `Exit`: click button 3 again to confirm.
  Any other button cancels.

Tests: `windows_test.zig` (New), `wctl_test.zig` (Resize, Move, Hide and
unhide from the menu, Delete), `devices_test.zig` (button 2 snarf).

## Drawing

Stock rio's look, which the fork keeps for legacy windows: a gray
background (`0x777777`); 4-pixel window borders, dark cyan
(`0x55AAAA`) on the current window and pale cyan (`0x9EEEEE`) on
others; white text windows with a scroll bar on the left; the
pale green button 3 menu. `golden_test.zig` compares two windows with
text and the open button 3 menu with golden images pixel for pixel, on
h9's 1024x768 headless screen (x8r8g8b8) with the default font.

## Programs that must keep working

A nested rio in a window (with its own windows and `$wsys`), acme, and
sam: `apps_test.zig`. CPU-server sessions (`rcpu`, drawterm) are checked
by hand on a 9front VM ([vm.md](../vm.md)).
