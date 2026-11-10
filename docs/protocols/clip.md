# clip: the typed clipboard, `/mnt/clip`

**Protocol:** `clip 1.0` · **Server:** `clipd` (`clipd/`) ·
**Fake:** none needed: `clipd -n FILE` runs against a plain file
standing in for `/dev/snarf` · **Conventions:** [README.md](README.md)

rio's `snarf` holds one piece of text, and every Plan 9 program already
uses it. `clipd` is an optional service on top of it that holds several
**representations** of one copied thing at once (text, a file list, an
image), each a **type**, so a program that pastes can pick the richest
one it understands. It never replaces snarf:

- whatever is copied with a `text` type is also written to snarf, so
  legacy programs paste it;
- whatever a legacy program copies into snarf becomes the clipboard (as
  `text` alone) the next time anyone looks.

A program that only needs text can keep using `/dev/snarf` and never
know `clipd` exists. libux uses `/mnt/clip` when it is mounted and
falls back to snarf when it isn't.

## Where it is

`clipd` posts `/srv/clip.$user` and mounts it on `/mnt/clip` in the
namespace it was started in (`-m` and `-s` change those). It reads and
writes the snarf of that namespace (`/dev/snarf`, or `-n FILE`), so it
should be started inside the rio whose clipboard it follows.

```text
/mnt/clip/
    version     r    protocol version
    ctl         rw   clear, begin, commit, abort
    types       r    snapshot: the generation and each type with its size
    events      r    stream: change
    data/       one file per type the clipboard holds
        TYPE    rw   the bytes of that representation
```

## Types

Type names are IDs (the form of a control ID in
[README.md](README.md#identifiers)) and are the same as drag types
([devux.md](devux.md#types)): `text` (UTF-8), `file` (a file list, one
`file name=...` line per file, as in a drag), `image` (`image(6)`), and
`x.`-prefixed names agreed between programs. A clipboard never holds a
path for another program to open: what is copied is the bytes.

## version

```text
clip 1.0
```

## Copying

Writing `data/TYPE` (create it, or open it with `OTRUNC`) and closing
the fid makes the clipboard exactly that one type, with a new
generation. To copy several representations of one thing, bracket the
writes with `begin` and `commit` on one open of `ctl`:

```rc
{
	echo begin >[1=3]
	echo -n 'Read Me' >/mnt/clip/data/text
	echo 'file ''name=Read Me'' size=12' >/mnt/clip/data/file
	echo commit >[1=3]
} >[3]/mnt/clip/ctl
```

| ctl command | Meaning |
|---|---|
| `begin` | Start a batch on this fid. Data files closed until `commit` are collected instead of replacing the clipboard. `busy` if another fid has a batch open |
| `commit` | Make the collected types the clipboard, replacing everything, with one new generation |
| `abort` | Drop the batch. Closing the fid does the same |
| `clear` | Empty the clipboard (a new generation with no types). snarf is left alone |

A type holds at most 16 MiB; a larger write fails with `too large`.
Writes to one `data` fid are appended in order, offsets ignored.

## Pasting

`types` is a snapshot, read whole at open:

```text
gen 7
type text size=7
type file size=28
```

Types are listed in the order they were written. Reading `data/TYPE`
returns that type's bytes from the generation current when it was
opened. `data/` holds a file only for each type the clipboard holds, so
walking to any other fails as for a missing file (and opening one left
over from an older generation fails with `not found`). `ls
/mnt/clip/data` lists the types too.

## Snarf

When the clipboard gets a `text` type, `clipd` writes it to snarf. When
`types`, `data/` or a `data` file is opened, and every second while
anyone has `events` open, `clipd` reads snarf; if it is not empty and
differs from what `clipd` last wrote or saw there, a legacy program has
copied, and the clipboard becomes that text as `text` alone, with a new
generation.

## events

A stream as in [README.md](README.md#stream-files-events).

| Event | Attributes | When |
|---|---|---|
| `change` | `gen`, `types`, `via` | The clipboard changed. `types` is the list now held (empty when cleared); `via` is `clip` (written here) or `snarf` (a legacy copy) |

## Errors

| Case | Error |
|---|---|
| Opening a type the clipboard no longer holds | `not found` (a missing one: no such file) |
| A type name that isn't an ID | `bad value` |
| `begin` while another fid has a batch | `busy` |
| `commit` or `abort` without `begin` | `bad syntax: no batch` |
| More than 16 MiB in one type | `too large` |

## Rights

`clipd` has no policy of its own; it is a per-user service, like
snarf. Exposing it to another program through uxd is what ux.md's
reserved `clipboard` right is for.

## Changes

| Version | Change |
|---|---|
| 1.0 | First version |
