# Developing 9/UX with h9

9/UX is 9front code: C built with 9front's compilers and `mk`, running on
9front's kernel interfaces, libdraw, and 9P. The development environment
needs to build that code quickly, run it with a real display, and test it
automatically, ideally from a normal host workflow (host editor, git, CI).

[h9](https://github.com/progrium/hyper9) fits this well, and it's the
primary environment. A real 9front VM stays in the loop for a few
checks h9 can't stand in for.

## Why h9

h9 runs 9front's own binaries as host commands: each `h9` invocation is a
small Plan 9 machine whose system calls are answered by drawterm's kernel
inside the h9 process, on the host's hypervisor (Hypervisor.framework,
KVM, WHP), on an emulator, or directly on the CPU in Linux containers.
Nothing boots, and the current directory is shared at `/mnt/term/cwd`.
What that gives 9/UX:

| Need | What h9 provides |
|---|---|
| Build with the real toolchain | 9front's compilers and `mk` from a pinned 9front 11952 root (`zig build mkroot`): `h9 --rc 'cd rio && mk'` |
| Run rio with a display | `h9 --gfx` opens a host window and serves `/dev/draw`, `/dev/mouse`, `/dev/kbd`, `/dev/cursor`, with `/dev/snarf` mapped to the host clipboard. Stock rio and acme already run this way. |
| Automated GUI tests | The `headless` gfx backend is a screen in memory with hooks to inject keys and mouse events, read the clipboard, and read pixels (`h9.gfx.headless` in `src/gfx.zig`). h9's own tests already start rio, type into a window, and run acme inside it. |
| Drive the 9P interfaces from tests | Machines are embeddable from Zig (`docs/embedding.md`), and `h9 --serve` exports a machine's namespace over 9P, so `/dev/ux` and `/mnt/ux` can be read and written from the test harness or from host tools. |
| Remote volumes for Finder | A second h9 with `--serve` is a remote 9P file server; `--root 'tcp!host!564'` and `mount` let the session reach it. Good for acceptance criterion 7. |
| Isolation | Copy-on-write root by default, `--write=ephemeral`, and `--net=disable`, so `mk install` and test runs never damage the root tree. |
| CI | On x86_64 Linux without KVM, `--backend native` runs amd64 binaries at native speed, and an amd64 root can be unpacked from the release ISO. |
| Later: a browser demo | h9 has a browser host, and graphics there is an accepted proposal (`browser-gfx`), which would make a live 9/UX demo page possible. |

Both projects pin 9front 11952, and `rio/` is imported from that
release's source, so the rio we fork is the rio h9 runs.

## Setup

9/UX pins a known-good h9 commit in [`scripts/pins.sh`](../scripts/pins.sh),
since h9 is under active development. `scripts/h9.sh` runs h9 at that
commit: on first use it clones h9 into `.local/hyper9`, builds it, and
builds its 9front root with `/sys/src`. It needs what h9's
[DEVELOPING.md](https://github.com/progrium/hyper9/blob/main/DEVELOPING.md)
lists for your host (Zig, and QEMU for the root).

```sh
scripts/h9.sh --check            # first run builds h9; then shows the backends
```

To use your own h9 checkout instead (for example while changing h9 for
9/UX), set `H9=/path/to/hyper9/zig-out/bin/h9`. The pin is bumped in its
own commit once CI passes on the new commit.

On Linux, graphics needs a glibc build with X11: set
`H9_BUILD_FLAGS=-Dgfx=x11` before the first run. Headless tests don't
need it.

Below, `h9` means `scripts/h9.sh` or your own `$H9`.

## Everyday loop

From the 9ux checkout:

```sh
h9 --rc 'mk'                          # build every component
h9 --rc 'cd rio && mk'                # just the rio fork
h9 --gfx rio/7.out                    # run it (arm64; 6.out on amd64)
h9 --gfx rio/7.out -i 'window acme'   # with a legacy app to check compatibility
h9 --gfx=noclip rio/7.out             # keep snarf off the host clipboard
```

Object files and `?.out` are ignored by git. To try `mk install` into the
root without changing it, use `h9 --write=ephemeral --writable "$H9ROOT"`.

## Tests

Three layers, cheapest first:

1. **Unit tests** in each component, plain C run inside h9
   (`h9 --rc 'cd resfs && mk test'`). Pure parsers such as resfs's are
   also built for the host so they can be fuzzed with libFuzzer or AFL.
2. **Interface tests**: rc scripts that start a component in a machine
   and check its files (`cat /dev/ux/version`, write `activate` to a
   control's `ctl`, read its `events`). These need no display for
   anything below rio.
3. **GUI tests**: a Zig harness under `test/harness`, modeled on h9's
   `tests/backends.zig`, that embeds h9 with the headless backend, runs
   the rio fork, injects input, compares screen regions against golden
   images, and drives the 9P files at the same time. The Phase 0
   regression suite for stock rio behavior is `test/rio`.

`scripts/test.sh` runs all of them; [testing.md](testing.md) says how to
write each kind.

## What still needs a real 9front

h9 is one machine with one screen in one host process. Use a 9front VM
(QEMU with the 11952 image, reached with drawterm) for:

- CPU-server clients: `rcpu` sessions and drawterm connecting to a CPU
  server running the rio fork (Phase 0 regression list).
- Real display formats and depths on hardware or VESA (acceptance
  criterion 6).
- Release checks: install into a real system and boot into the fork.

## What Phase 0 checked

These were assumptions about h9 when this plan was written. Results, at
`H9_REV` b27cacd (details and gaps in [h9-needs.md](h9-needs.md)):

- **Headless with the native backend on Linux CI: works.** GitHub's
  x86\_64 runners have no KVM; `--backend native` runs the amd64 root
  unpacked from the release ISO, and the harness's headless screen runs
  there. `rio/` builds in well under a second and the whole regression
  suite takes about 15 seconds. `scripts/h9.sh` picks this setup on any
  Linux x86\_64 host without a usable `/dev/kvm`.
- **Embedding h9 from outside its tree: works, as a path dependency**
  (`test/harness/build.zig.zon` points at `.local/hyper9`), not as a
  fetched package.
- **Nested rio inside the fork: works** headless (`test/rio/apps_test.zig`),
  as do acme and sam. Not yet tried under `h9 --gfx` with a real window.
- **Exporting rio's `/dev/ux` with `--serve`:** the export is the
  namespace h9 built, not rio's, so `/dev/ux` isn't in it directly; the
  machine's `/srv` is, so a client can mount rio's posted
  `/srv/rio.USER.PID`.
- **A headless screen from the CLI**, so GUI tests could be rc alone:
  not in h9; the Zig harness ([testing.md](testing.md)) does it instead.
