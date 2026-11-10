# Testing 9/UX

One command builds everything and runs every test, locally and in CI:

```sh
scripts/test.sh                     # build, host, unit, gui
scripts/test.sh unit                # just one layer (or several)
scripts/test.sh gui -- -Dfilter=wctl          # GUI tests whose names match
scripts/test.sh gui -- -Dupdate-golden        # rewrite golden images
```

A fresh checkout needs a C compiler, `curl`, `git`, `python3` and an
internet connection the first time; `scripts/h9.sh --setup` (which
test.sh runs) fetches Zig (`scripts/zig.sh`), builds h9 at `H9_REV`, and
makes the 9front root. See [development.md](development.md) for h9.

## Layers

| Layer | What runs | Where | Owned by |
|---|---|---|---|
| `build` | `mk` at the top: every component's mkfile | inside h9 | each component |
| `host` | `make test` in each component with a host `Makefile` that has a `test:` target: portable parsers built with ASan and UBSan | host | each component |
| `unit` | `mk test` in each component, and in each `test/*/` with a mkfile (such as `test/fakes`). 9front's mkone/mkmany/mklib run a component's `test/mkfile` for `mk test`; put unit tests and rc interface tests there | inside h9 | each component |
| `gui` | the Zig harness: every `test/*/*_test.zig` | h9 embedded, headless screen | S0 (`test/rio`), and anyone adding a directory |

CI (`.github/workflows/ci.yml`) runs all four on Linux x86_64 on every
push to `dev` and `main`, and a `fuzz` job runs `make fuzz FUZZTIME=300`
in each component whose `Makefile` has a `fuzz:` target. Keep those
target names and a component is in CI with no workflow changes.

A component's tests must work on amd64 and arm64: use `$O` (or
`$objtype`), not `6.` or `7.`, for binary names.

## Writing GUI tests

GUI tests are Zig, but most of what they do is rc. A file
`test/<dir>/<name>_test.zig` is built and run automatically:

```zig
const std = @import("std");
const harness = @import("harness");

test "a window opens where asked" {
    var s = try harness.Session.start(.{ .name = "my-test" });
    defer s.stop();
    try s.openWindow(harness.rect(100, 100, 500, 400), "echo hi; sleep 1000");
    const ids = try s.waitWindows(1);
    defer std.testing.allocator.free(ids);
    try std.testing.expectEqual(harness.rect(100, 100, 500, 400), try s.windowRect(ids[0]));
}
```

`Session.start` boots a machine on h9's headless screen (1024x768,
x8r8g8b8, no cursor drawn) running the rio fork (`rio/6.out` or
`rio/7.out`; build it first, as test.sh does) with an agent as its `-i`
command. `.rio_args` adds rio flags; `.rio` picks another binary. Only
one session runs per test binary at a time; test files run one after
another.

What a session gives a test:

- **rc inside rio.** `s.rc(cmd)` runs `cmd` with rc in rio's namespace
  (so `window`, `/dev/wsys`, `/mnt/wsys` and `$wsys` work) and returns
  its output, failing the test on a non-empty status; `s.exec` returns
  the status too; `s.waitRc(cmd, ms)` retries until it succeeds. `$work`
  is a scratch directory, `.local/test/run/NAME` on the host, which
  `s.readWork`/`s.waitWork` read from the host.
- **Files.** `s.readFile`, `s.writeFile` (one write, for ctl files), and
  `s.readOnce` (one read: for files like `wctl` where a second read
  blocks).
- **Windows.** `s.openWindow(rect, cmd)` (`window -m`: the command gets
  the agent's environment, so `$work` works, and its own `/dev`),
  `s.windows()`, `s.waitWindows(n)`, `s.windowRect(id)`,
  `s.windowState(id)`.
- **Keyboard and mouse.** `s.typeText`, `s.key` (`harness.Key` has the
  special keys), `s.keyEvent`, `s.move`, `s.press`, `s.release`,
  `s.click`, `s.drag`, and `s.menuPick(at, button, item, opens_on)` for
  libdraw pop-up menus.
- **The screen.** `s.screenshot()` (an `Image` of `0xRRGGBB` pixels with
  `at`, `crop`, `count`), `s.waitScreen(ctx, pred, ms)`, `s.settle()`
  (until two screenshots match), `s.saveScreen(name)`.
- **Golden images.** `s.expectGolden("test/x/golden/y.png", .{ .rect =
  r })` waits up to 5 s for the screen (or `r`) to match the PNG exactly
  (`.tolerance` allows some pixels to differ). With `-Dupdate-golden` it
  writes the PNG instead; look at it before committing. On a mismatch,
  the screen and a diff (differing pixels magenta) are saved to
  `.local/test/artifacts/`, which CI uploads.

Waiting: never sleep for a fixed time when there's something to wait
for. Wait on a file (`waitWork`), a command (`waitRc`), a window
(`waitWindows`, and the helpers in `test/rio/common.zig`), or the screen
(`waitScreen`, `expectGolden`). Input is asynchronous: keys typed before
a program reads them can be lost, so wait for the window to be current
first.

Write results from inside windows to `$work/NAME.tmp` and `mv` them to
`$work/NAME`, so the host never reads half a file.

## The stock rio regression suite

`test/rio/` pins stock rio's behavior ([protocols/rio.md](protocols/rio.md))
so the fork can't change it by accident:

| File | Covers |
|---|---|
| `boot_test.zig` | rio starts; `window` opens a window |
| `windows_test.zig` | `window` (wctl new), `window -m` (`/mnt/wsys` attach), button 3 New, per-window files |
| `wctl_test.zig` | wctl resize, move, hide/unhide, current/top/bottom, delete, errors; button 3 Resize, Move, Hide, Delete |
| `devices_test.zig` | `/dev/snarf` (and button 2 snarf), `/dev/kbd`, `/dev/mouse`, `/dev/cons` |
| `apps_test.zig` | rio nested in a window, acme, sam |
| `golden_test.zig` | window borders and text, the button 3 menu, pixel for pixel |

Run it before every push that touches `rio/`. CPU-server sessions, real
displays and installs are checked by hand on a 9front VM:
[vm.md](vm.md).
