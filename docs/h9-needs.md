# What 9/UX needs from h9

Gaps found while building 9/UX on h9 (pinned in `scripts/pins.sh`). Each
entry says who found it, what happens, the workaround in this repo, and
what h9 would need. The h9 stream (H in [workplan.md](workplan.md))
takes them upstream.

## DMEXCL isn't enforced on the host-tree root

- **Found by:** S5 (uxmeta), at H9_REV b27cacd, native backend.
- **What happens:** a file created with `DMEXCL` shows the `l` mode bit,
  but a second `open` of it succeeds while the first is still open. On
  9front's own file servers the second open fails with "file in use".
- **Workaround:** uxmeta's single-writer lock also creates a `/srv`
  entry with `ORCLOSE`, which devsrv refuses to create twice and removes
  when the holder exits (`uxmeta/layer.c`, `metalock`).
- **h9 would need:** exclusive-use files in the host-tree file server
  (refuse a second open of a `DMEXCL` file while one is open in the
  machine), or a documented statement that it doesn't, so programs don't
  rely on it.

## isoroot trees lack the kernel's mount points

- **Found by:** S0 (harness and CI), at H9_REV b27cacd, native backend.
- **What happens:** a root unpacked from the release's amd64 ISO with
  `zig build isoroot` has no `/dev`, `/env`, `/mnt/term`, `/srv` and the
  other directories the 9front kernel supplies, so the first run fails
  with `bind #y /dev: file does not exist: '/dev'`. `zig build mkroot`
  adds them (its `mount_points` list); isoroot doesn't.
  The ISO also leaves out the empty directories an install has under
  `/mnt` and `/n` (`/mnt/plumb`, `/mnt/factotum`, ...; see
  `sys/lib/sysconfig/proto/portproto`), so the plumber and factotum
  can't mount themselves.
- **Workaround:** `scripts/h9.sh` makes both sets of directories (on
  every run, so older trees get them), and drops the installer payload
  in `dist/`.
- **h9 would need:** isoroot to add `mount_points` as mkroot does, and
  the empty directories from portproto (and perhaps skip `dist/`), so an
  ISO tree is a usable `--root` as is.

## h9 can't be a fetched Zig package

- **Found by:** S0, at H9_REV b27cacd.
- **What happens:** h9's `build.zig.zon` `paths` leave out
  `third_party/` (drawterm) and `scripts/` (the pins), so a URL or hash
  dependency on h9 doesn't build. h9's docs/embedding.md says so.
- **Workaround:** `test/harness/build.zig.zon` depends on h9 by path,
  `../../.local/hyper9`, the checkout `scripts/h9.sh` keeps at `H9_REV`.
  The harness therefore needs `scripts/h9.sh --setup` first.
- **h9 would need:** the package paths to include what the build reads,
  so `zig fetch --save git+https://github.com/progrium/hyper9#REV` works
  and the pin can live in `build.zig.zon`.

## No headless screen from the command line

- **Found by:** S0.
- **What happens:** the headless gfx backend and its hooks (inject keys
  and mouse, read pixels and the clipboard, close the window) exist only
  in `-Dgfx=headless` builds of the library, so a GUI test has to be a
  Zig program embedding h9. `h9 --gfx` on Linux needs an X11 build and a
  display.
- **Workaround:** the Zig harness in `test/harness`, with an rc agent
  inside rio so tests can still be mostly rc commands.
- **h9 would need:** a CLI switch for a headless screen, with input
  injection and screenshots through files or a control socket, so tests
  for 9/UX components could be plain rc (as development.md hoped). Not
  urgent: the harness covers it.

## --serve exports the plan's namespace, not the program's

- **Found by:** S0, checking development.md's assumption that rio's
  `/dev/ux` could be exported with `--serve` while rio runs.
- **What happens:** as the manual says, a `--serve` client sees the
  namespace h9 built, not binds the program made later, so rio's
  `/dev/wsys` and `/dev/ux` aren't there. The machine's `/srv` is,
  though (checked: a file posted in `/srv` by the program shows up in
  the client's mount of the export), and rio posts its file server as
  `/srv/rio.USER.PID`.
- **Workaround:** none needed yet. A host-side client reaches rio by
  mounting the exported tree and then the posted `/srv/rio.*` (with a
  window id or `none` as the attach spec). The harness doesn't use
  `--serve`; its agent runs inside rio.
- **h9 would need:** nothing, unless tests want a host-side 9P client;
  then a way to export a named process's namespace would be simpler.

## A GUI session once failed to start under load

- **Found by:** S0, running five GUI test binaries at once (each a
  machine running rio) on a 4-core container, native backend.
- **What happens:** one session's rio was running but its `-i` command
  hadn't run after 60 seconds. Seen once in about ten runs; not
  reproduced since.
- **Workaround:** the harness runs test binaries one after another, and
  saves the screen as `start-failed.png` if a start times out.
- **h9 would need:** to be looked at if it happens again; record the
  run here.

## --gfx on X11 crashes when /dev/mouse is bound before /dev/draw

- **Found by:** S3 (libux), at H9_REV b27cacd, Linux x86\_64, h9 built
  with `-Dgfx=x11` (which needs `libxt-dev` for `X11/IntrinsicP.h`), under
  Xvfb, native backend.
- **What happens:** `h9 --backend native --gfx rio` segfaults at start in
  drawterm's `gui-x11/x11.c` `setcursor`, called from `mouseattach` while
  the namespace is bound, because `xdisplay` is still 0: `/dev/mouse` is
  attached before `/dev/draw` has opened the display.
- **Workaround:** a local `if(xdisplay == 0) return;` at the top of
  `setcursor` in the h9 checkout; then rio and libux apps draw under Xvfb
  (screenshots with `import -window root`). Nothing in this repo depends
  on it: the GUI tests use the headless backend.
- **h9 would need:** `setcursor` (and anything else `mouseattach` calls)
  to wait for or skip a display that isn't open yet, in the X11 backend.


## Writing `stop` to /proc/N/ctl never returns

- **Found by:** S4 (uxd), at H9_REV b27cacd, native backend.
- **What happens:** `sleep 100 & echo stop >/proc/$apid/ctl` blocks the
  `echo` forever (until h9's timeout hangs everything up). On 9front the
  write returns once the target has stopped, and `start` resumes it.
- **Workaround:** uxd's test for an app that stops answering uses an app
  tree served by `srvfs` with a pipe in it, so one read waits inside the
  app, instead of stopping the app's process (`uxd/test/stall.rc`).
- **h9 would need:** `stop`, `start` and `waitstop` in `/proc/N/ctl`, or
  an error from the write saying they aren't supported.
