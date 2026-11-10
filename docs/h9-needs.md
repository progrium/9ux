# What 9/UX needs from h9

Gaps found while building 9/UX on h9 (pinned in `scripts/pins.sh`). Each
entry says who found it, what happens, the workaround in this repo, and
what h9 would need. The h9 stream (H in [workplan.md](workplan.md))
takes them upstream; entries it has dealt with move to "Resolved" at
the end.

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

## Resolved

Fixed in h9 by [progrium/hyper9#153](https://github.com/progrium/hyper9/pull/153),
or found not to be a gap.

- **Occasional faults on fresh heap pages under load** (S2, S9). The
  guess was right. When one process of an `RFMEM` group grew the shared
  data segment, the native backend only queued the new mapping for the
  other processes' stubs. It carried the mapping out when each next
  entered h9. A libthread proc that stayed in user mode and touched
  memory another proc's malloc had just got faulted (`sys: trap: fault
  write` in `blocksetsize`). h9 now reruns a fault when mappings changed
  while the guest ran. Its fixture `tests/fixtures/rfmem/rfgrow.c`
  failed on every run before and passes 20/20 after. It also grows past
  400 MB: a process's memory is limited only by the machine's
  `--memory` (1 GB by default), and the "about 64 MB" limit didn't
  reproduce. The "write error: Bad address" from rc's `echo` in the same
  run wasn't reproduced or explained (it's the host's EFAULT string); if
  it comes back with the new pin, record the run here.
- **DMEXCL isn't enforced** (S5). h9's overlays (the root, `/tmp`,
  ephemeral paths) now refuse a second open of a `DMEXCL` file with
  "file is locked", as ramfs does, until the first is closed. Host
  directories (`#U`, such as the cwd or `--writable` paths) can't keep
  the bit, so a `DMEXCL` create there is an ordinary file (documented in
  h9's MANUAL.md). uxmeta's `/srv` lock stays: it works on either.
- **--gfx on X11 crashes when /dev/mouse is bound before /dev/draw**
  (S3). h9's X11 cursor calls now wait for the screen to open, without
  touching the vendored backend. `h9 --gfx rio` under Xvfb crashed at
  once before and runs after. The local `setcursor` patch isn't needed
  any more.
- **Writing `stop` to /proc/N/ctl never returns** (S4). Not a gap.
  `stop`, `start`, `startstop` and `waitstop` are implemented, and `stop`
  returns once the target stops (checked against a spinning process).
  Like 9front, a process stops only when it next leaves the kernel, so
  against `sleep 100` the write waits out the sleep, on 9front too.
  uxd's srvfs-based stall test is still the right way to test an app
  that stops answering.
