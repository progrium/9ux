# Protocol fakes

Small 9P servers that stand in for 9/UX components until the real ones
exist, so each side of a [protocol](../../docs/protocols/) can be built
and tested on its own. Each fake follows its protocol closely enough to
test clients against, and adds a `fake` control file outside the
protocol that plays the parts a test can't otherwise reach: rio's window
manager, the person clicking and typing.

| Fake | Stands in for | Protocol |
|---|---|---|
| `fakedevux` | rio's `/dev/ux` | [devux.md](../../docs/protocols/devux.md) |
| `fakeapp` | a libux app's control tree | [ux.md](../../docs/protocols/ux.md) |

Both are plain C on lib9p, built with 9front's `mk`:

```sh
h9 --rc 'cd test/fakes && mk'        # builds $O.fakedevux and $O.fakeapp
h9 --rc 'cd test/fakes && mk test'   # builds them and runs test/*.rc
```

## fakedevux

```rc
fakedevux [-D] [-q qmax] [-s srvname] [-m mtpt]
mount -a /srv/fakedevux /dev 7        # /dev/ux and /dev/fake; 7 is "our" window
```

It posts `/srv/fakedevux` (or `-s`). As with rio, the attach name is the
fid's own window, the default for `window` and `claim`. `-q` sets the
event queue bound (default 256), to test `lost`.

Everything in `/dev/ux` behaves as devux.md says, except:

- Nothing is drawn. Menus, focus and the desktop exist only as state,
  snapshots and events.
- Inside a `begin`/`commit` batch, commands are checked when `commit`
  runs them, not when written.
- Windows exist only when `fake/ctl` makes them.
- Drags follow `fake/ctl`'s `press`, `move` and `release` instead of a
  real pointer, and no window is sent a mouse event when one starts.

`/dev/fake/ctl` plays rio and the person at the screen:

| Command | Does |
|---|---|
| `new winid [rect=r] [label=text]` | Create a window (`new` event) |
| `delete winid` | Delete it (ends its app membership and desktop claim) |
| `focus winid` | Give it keyboard focus (`0`: none) |
| `rect winid rect=r`, `hide winid`, `unhide winid`, `top winid`, `label winid text` | Window changes, with their events |
| `screen rect=r` | Resize the screen |
| `click actionid` | Choose the foreground app's item from the bar (`via=click`) |
| `key shortcut` | Type Command plus `shortcut` (as in `key=`); follows devux.md's [Keyboard shortcuts](../../docs/protocols/devux.md#keyboard-shortcuts) |
| `press winid [pt=x,y]` | Press a mouse button with the pointer over window `winid` at window-local `pt` |
| `move winid [pt=x,y]` | Move the pointer over `winid` (`0`: over no window). During a drag this is what posts `enter`, `over` and `leave` |
| `release` | Let the button go: a drag in flight drops or cancels |

`/dev/fake/log` records every line written to any ctl file, in order,
and what each `key` did: `key q action quit`, `key z swallowed` (only a
disabled item matched), or `key x passed` (the window would get it).

## fakeapp

```rc
fakeapp [-D] [-r] [-q qmax] [-w winid] [-n name] [-s srvname] [-m mtpt]
```

Serves one app, `fake` (title Fake), with window `winid` (default 1)
holding:

| Control | Type | Notes |
|---|---|---|
| `ok` | button "OK" | the default button |
| `name` | text-field "Name" | |
| `password` | text-field "Password" | `secret`: its value is never served |
| `agree` | checkbox "I agree" | |

and a File menu with `new` (key n), `close` (key w), a separator, and
`quit` (key q).

Without `-r` it posts `/srv/ux.$user.fake` (or `-s`), the debugging mode
ux.md calls `uxpublic=1`, and mounts it on `-m` if given. With `-r` it
registers with uxd through `/mnt/ux/register` as ux.md describes, and
registers again whenever uxd restarts; that path waits for uxd to exist
to be tested.

`fakectl` at the app's root (outside the protocol) plays the person and
changes the tree. Events it causes have `via=ui` (or `menu`, `key`).

| Command | Does |
|---|---|
| `click ctlid` | Press a button or toggle a checkbox |
| `type ctlid text` | Type into a text field at its selection |
| `focus ctlid` | Move keyboard focus |
| `menu actionid` / `key actionid` | Choose a File item from the bar or by shortcut |
| `add ctlid type [label=text] [parent=ctlid]` | Add a control (`new` event) |
| `del ctlid` | Remove one (`delete`, then `gone` on its stream) |
| `state ctlid flag on/off` | Set a state flag, such as `enabled` or `sensitive` |
| `label ctlid text` | Change a label |
| `close` | Close the window |
| `quit` | Exit |

## Tests

`test/*.rc` exercise each fake against its protocol document, and are
also examples of driving the files from rc. `test/lib.rc` has the
helpers (`want`, `same`, `fails`, `drain`).

Two rc habits these files need:

- **Quote attributes.** rc treats `=` in an argument as an error, so
  write `echo menu file 'title=File'`, and for a value with spaces quote
  twice: `echo new 9 '''label=Read Me'''` sends `new 9 'label=Read
  Me'`.
- **Read events in one big read.** Event files never split a message,
  so `read` (which reads a byte at a time) fails with `short read`. Use
  `dd -bs 8192 -count 1` for what's queued, or `cat` in the background.
