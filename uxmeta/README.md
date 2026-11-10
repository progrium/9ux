# uxmeta: metadata service

Owns 9/UX's persistent metadata: file associations, application
declarations, the volume registry, icon choices, and Desktop layout. The
data lives in human-editable ndb files; `uxmeta` is the single serialized
writer, replaces files atomically, publishes a generation number and a
change stream, and generates plumbing rules so the plumber stays the
dispatch path.

```text
ext=.txt type=text
type=text open=edit icon=text
volume=home path=/usr/glenda icon=disk desktop=yes
folder=/usr/glenda/doc view=icons sort=name x=100 y=90 width=550 height=400
```

Precedence: system defaults, then installed apps, then user preferences,
then workspace overrides. The protocol, schema and file handling are in
[docs/protocols/meta.md](../docs/protocols/meta.md).

## Usage

```text
uxmeta [-DP] [-s srvname] [-m mtpt | -n] [-d sysdir] [-a appsdir]
	[-u userdir] [-w workspace] [-i pollms]
```

| Option | Default | |
|---|---|---|
| `-s` | `uxmeta.$user` | name posted in `/srv` |
| `-m` | `/mnt/meta` | mount point; `-n` mounts nothing |
| `-d` | `/lib/ux/meta` | system layer: every `*.ndb` in it |
| `-a` | `/apps` | apps layer: every `*/app.ndb` in it |
| `-u` | `$home/lib/ux` | user layer `meta.ndb`, `workspace/`, `plumbing`, `meta.gen`, the lock |
| `-w` | `$workspace` or `default` | workspace layer `userdir/workspace/NAME.ndb` |
| `-i` | 1000 | how often (ms) to look for edits made to the files directly |
| `-P` | off | after regenerating rules, reload the plumber from `$home/lib/plumbing` |
| `-D` | off | 9P trace |

From rc:

```rc
uxmeta
echo resolve /usr/glenda/notes.txt >/mnt/meta/query; cat /mnt/meta/query
echo 'set folder /usr/glenda/doc view=list' >/mnt/meta/ctl
cat /mnt/meta/changes &
```

To have associations open through the plumber, put
`include /usr/$user/lib/ux/plumbing` above `include basic` in
`$home/lib/plumbing`.

## Source

| File | |
|---|---|
| `meta.h` | types and the library interface |
| `ndb.c` | ndb text kept chunk by chunk, so unchanged records and comments are written back byte for byte |
| `layer.c` | layer files: reading, rescanning, atomic replace and recovery, the lock |
| `view.c` | the merged view, change diffs, queries, `resolve` |
| `txn.c` | `ctl` transactions and rights |
| `plumb.c` | generated plumbing rules |
| `fs.c` | the 9P server and `main` |

## Tests

`mk test` (inside h9: `scripts/h9.sh --rc 'cd uxmeta && mk test'`) runs
`test/unit.c`, the library's unit tests, and `test/run`, rc tests against
a running `uxmeta`: queries, transactions, attach rights, the change
stream, edits made behind its back, concurrent writers (including
read-modify-write with `ifgen`), the single-writer lock, restart
persistence, and recovery from an interrupted replace.

Design: [§6 Metadata](../docs/design.md#6-metadata-ndb--uxmeta).
Plan: [Phase 3](../docs/plan.md#phase-3-desktop-mvp).
