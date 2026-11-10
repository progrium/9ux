# uxd: UI registry and event broker

Discovers running 9/UX applications and presents them as one tree at
`/mnt/ux/apps/...`. Applications stay authoritative for their own control
state; `uxd` maps rio window IDs (`/dev/winid`) to logical applications,
aggregates their trees, routes events, and enforces the control policy
(introspection is separate from mutation, default deny for writes).

It is a broker, not a monolithic desktop daemon: rio's extensions and each
application's own 9P server keep working if `uxd` restarts.

Design: [§5](../docs/design.md#5-toolbox-libux-native-9p-visible-ui),
[§8](../docs/design.md#8-services-and-interfaces),
[§9](../docs/design.md#9-security-and-reliability).
Plan: [Phase 2](../docs/plan.md#phase-2-9p-toolbox).

## Running

```rc
uxd [-D] [-s srvname] [-m mtpt] [-g grantfile]
uxd -m /mnt/ux          # what a session's startup runs
```

It posts `/srv/uxd.$user` (the public service: `read`, or a grant's
rights when the attach name is a grant token) and `/srv/uxd.$user.admin`
(the session service: every right), and with `-m` mounts the session
service. Grants are kept in `$home/lib/ux/grants` (or `-g`). Both `/srv`
entries go away when uxd exits; apps notice and register again when it
comes back. `-D` traces 9P.

The protocol is [ux.md](../docs/protocols/ux.md); uxd implements its
`/mnt/ux` tree, registration, rights and grants.

## How it works

- **Pass-through, not copies.** uxd is a 9P client of each app's server
  (the `/srv/ux.*` pipe the app hands over at registration, which uxd
  removes from `/srv` at once). Every walk, open, read, write and stat
  under `apps/<id>/` becomes the same request to the app, so contents,
  `seq` numbers and errors are the app's own, and each open of an
  `events` file is its own subscription in the app. Flushes are passed
  on too. Qids are rewritten so they stay unique across apps.
- **Nothing blocks the broker.** lib9p's server procs only queue
  requests; each request runs as a thread in one proc that owns all of
  uxd's state (so no locks), and each app connection has its own reader
  and writer procs. A client that stops reading, or an app that stops
  answering, holds up only the requests waiting on it.
- **Policy at the boundary.** Reads are open to every attach, except the
  `value`, `items` and `selection` of controls whose `state` says
  `sensitive`, and `value=`, `sel=`, `item=` and `items=` in their
  events, which are replaced with `redacted=on`. `ctl` writes are checked line by line against the
  control's own `ctl` help (cached per file, re-read once for an
  unlisted verb; a `-` class counts as unlisted); `value` writes need `edit`; other writes, creates and
  removes are refused. The session service skips these checks and lets
  the app decide.
- **Lifetimes.** A registration lives as long as the app's `register`
  fid and its 9P connection; when either ends, its windows are unmapped
  and `unregister` is posted.

## Layout

| File | |
|---|---|
| `uxd.c` | main, the lib9p glue, the request threads |
| `fs.c` | the `/mnt/ux` tree, walking into apps, policy |
| `conn.c` | the 9P client multiplexer for app connections |
| `app.c` | registration and the window map |
| `grant.c` | rights and grants |
| `sub.c` | uxd's own `events` file |

## Tests

```sh
h9 --rc 'cd uxd && mk test'
```

rc tests in `test/` against [`fakeapp`](../test/fakes/): the tree and
pass-through (`tree.rc`), rights and redaction (`policy.rc`), grants
(`grants.rc`), registration by hand (`register.rc`), stalled readers and
apps and flushes (`stall.rc`), and restarts of uxd and of apps
(`restart.rc`).
