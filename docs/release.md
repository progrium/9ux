# 9/UX v0.1: release checks

Where each [v0.1 acceptance criterion](design.md#acceptance-criteria-for-v01)
stands, what checks it, and what is still to be done by hand before the
`v0.1` tag. Workstream S11 in [workplan.md](workplan.md).

## Status

Checked on 2026-11-10, `dev` at b5e87da (and the commit adding this file), Linux x86\_64 (no KVM), h9 at
`H9_REV` b27cacd on the native backend. `scripts/test.sh` passed every
layer: build, host (resfs), unit (clipd, desktop, libux, resfs, rio, uxd,
uxmeta, test/fakes), system (desktop's remote volume) and gui (46 tests
in 13 binaries, including the two end-to-end tests below).

| # | Criterion | Result | Checked by |
|---|---|---|---|
| 1 | No regressions to stock rio's documented file interfaces | **Pass** headless; CPU-server sessions still to do on the VM | `test/rio/` (24 tests on the fork: windows, wctl, devices, nested rio, acme, sam, golden images), and devux's mode-off test; `e2e-demo` checks acme and an rc window beside 9/UX apps |
| 2 | Desktop draws desktop icons, opens directories, launches documents via plumbing | **Pass** | `test/desktop/`, `desktop/test/ui.rc`, and `e2e-demo`: a double click on a text file plumbs it to acme, which opens it |
| 3 | Menu bar follows a multi-window 9/UX app with correct focus | **Pass** | `test/devux/devux_test.zig` (bar follows focus between an app's two windows and back; actions by click, drag, Command key and ctl); `e2e-demo` clicks File > New in the bar for the libux sample |
| 4 | Three native controls with read state, write commands, independent event subscriptions over 9P | **Pass** | `libux/test` (text field, button, checkbox, list, icon view; two `events` readers; a stalled reader doesn't stall the UI); `e2e-demo` sets a text field, reads it back, presses a button through `ctl` and sees it on a second reader, all through uxd's `/mnt/ux` |
| 5 | ndb persistence survives restarts and handles external edits safely | **Pass** | `uxmeta/test` (precedence, concurrent writers, external edit, restart), `desktop/test/ui.rc` (layout across a restart and an external edit), `test/desktop` (a dragged icon's position saved) |
| 6 | Imported `ICN#` and `icl8` render correctly on actual 9front display formats | **Pass** at 32 bits on 9front's own kernel; 16- and 8-bit screens still to see on hardware | headless: `test/libux/res_test.zig` and resfs's golden images; on 9front's own kernel and VESA framebuffer: `test/e2e/vm-display.py`, below |
| 7 | A mounted remote 9P directory works as a Desktop volume with honest reporting | **Pass** | `desktop/test/remote_test.sh` (a second h9 with `--serve`: operations, refusals, and `gone` when it stops); `e2e-demo` opens such a volume from the desktop |
| 8 | Unprivileged clients can't mutate another app's controls without authorization | **Pass** | `uxd/test` (policy, grants, redaction, restart keeps grants) |

## The end-to-end demo

[`test/e2e/demo_test.zig`](../test/e2e/demo_test.zig) is design §10's
first end-to-end demo as one headless run, in rio's 9/UX mode:

1. The session's services are started as a profile would
   ([`services.rc`](../test/e2e/services.rc)): the plumber, uxd on
   `/mnt/ux`, and uxmeta on `/mnt/meta` with a local volume and a remote
   one.
2. Desktop claims the desktop and shows both volumes. The remote one is
   a second h9 on the host, `h9 --serve`, which the test starts.
3. A double click on the remote volume attaches it and opens its window
   with the server's files.
4. The local volume opens, then a folder in it, then a double click on a
   text file plumbs it; acme, already running in a window, opens it.
5. The libux sample launches and registers with uxd; the bar follows it,
   and a click on File, New in the bar reaches it.
6. From rc: `ls /mnt/ux/apps` lists Desktop and the sample; a text
   field's value is set and read back; `echo activate` to the Save
   button's `ctl` presses it, and a second reader of the button's
   `events` sees the press.
7. An ordinary rc window is still moved with `wctl` and typed into.

[`test/e2e/drag_test.zig`](../test/e2e/drag_test.zig) does drag and
drop the same way (workplan S9's "done when"): a file dragged between
two Desktop folder windows (moved, within one volume), onto another
volume's desktop icon (copied), and onto the libux sample's Notes field,
whose contents then come through rio's drag broker, never as a path.

Both run in `scripts/test.sh gui`, and alone with
`scripts/test.sh gui -- -Dfilter=e2e`. Screenshots of each step land in
`.local/test/artifacts/e2e-*.png`.

## On a real 9front display

[`test/e2e/vm-display.py`](../test/e2e/vm-display.py) boots the 11952
amd64 release in QEMU (the image `scripts/vm.sh fetch` gets), with
UEFI firmware (OVMF) handing the kernel its framebuffer, mounts the checkout from an `h9 --serve` on
the host, and runs the rio fork there with the libux sample
([`vm-sample.rc`](../test/e2e/vm-sample.rc)), whose window draws
`ICN#`/`icl8` icons from its own `.rsrc` and the system's through resfs.
It takes QEMU screendumps and compares the window with the headless
golden image.

Run on 2026-11-10 (QEMU 8, no KVM, about 15 minutes): the kernel's
screen was 1280x800, `x8r8g8b8`, `softscreen on` (9front's own devdraw
on the firmware's linear framebuffer), and the sample's window matched
the headless golden image pixel for pixel (0 of 210,000 pixels differ),
including the selected icon drawn darkened through its mask.
The screendump lands in `.local/test/artifacts/vm-sample-32.png`.

Why UEFI: the amd64 kernel has no realmode, so neither 9boot on BIOS
nor `aux/vga -m vesa` can set a VESA mode in QEMU (`/dev/realmode` is
missing), and QEMU's standard VGA has no native driver in the amd64
kernel. The EFI loader passes the firmware's mode as `*bootscreen`,
which is always 32 bits deep there, so 16- and 8-bit screens (where
devdraw converts the icons' `m8` and `k1` images) still need real
hardware or a 386 kernel with VESA. The script copies 9front's
`bootx64.efi` onto the image's 9fat, and turns off glenda's profile
starting stock rio, on a throwaway overlay.

## Found and fixed while integrating

- **Desktop never attached a remote volume on open** (`desktop/view.c`):
  `dcvolcheck`'s result was compared with `< 0`, but it returns
  `Vonline` or `Voffline`. A volume with `addr=` that Desktop hadn't
  mounted yet failed to open with "not found", and an offline volume's
  alert never showed.
- **libux didn't follow rio's focus** (`libux/win.c`, `menu.c`): rio
  takes the click that makes a window current, so libux kept the old
  window current; Desktop drew and reported the wrong window as active,
  and the next drag from the newly current window was lost. libux now
  acts on `/dev/ux/events` focus events.
- **Desktop's folder windows had no title bars** (`libux/menu.c`): libux
  asked rio for chrome only on windows of kind `window`. Every kind but
  `dialog` and `alert` gets it now.
- **uxd's tests all failed after one hung** (`uxd/test/setup.rc`): a
  leftover uxd kept `/srv/uxd.$user`; tests now clear it first.
- The sample app's Notes field takes dropped files and text, so there
  is a drop target outside Desktop.

## Open issues

- **rio heap fault, intermittent.** Seen by S2 and S9 under load
  (`sys: trap: fault write` in libc's `blocksetsize`); not reproduced in
  ten full GUI runs here, and pool debugging found no corruption.
  Logged in [h9-needs.md](h9-needs.md) as a likely h9 fault on fresh
  heap pages; a rerun passes.
- **A remote volume shows "(offline)" until it's first opened.** That's
  honest (Desktop hasn't mounted it), but "not mounted" would read
  better, or Desktop could attach such volumes in the background at
  start.
- **Desktop's own icons can't be dragged with a modifier**: mouse events
  carry no modifiers, so there's no shift-click or option-drag (Desktop
  README, Limits).

## Still to do by hand before the tag

- [ ] Jeff: the demo once under `h9 --gfx` with a real window
      (`H9_BUILD_FLAGS=-Dgfx=x11`; the steps are those of
      `demo_test.zig`, with `test/e2e/services.rc` run in the first
      window).
- [ ] Criterion 6 at 16 and 8 bits: the sample on a real 9front
      machine (or a 386 kernel with VESA) set to those depths.
- [ ] The VM checks in [vm.md](vm.md) that are still "not yet":
      drawterm into the VM running the fork, `rcpu`, and `mk install`
      and booting into the fork (criterion 1 for CPU-server sessions).
- [ ] Tag `v0.1` on `main` once `dev` is merged and CI is green.
