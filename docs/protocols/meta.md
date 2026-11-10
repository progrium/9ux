# uxmeta: metadata files and the `/mnt/meta` protocol

**Status:** draft 1, implemented by [`uxmeta/`](../../uxmeta/).
**Owner:** S5 (uxmeta). Changes to this document go in their own commit
with the matching change to `uxmeta/` and its tests.

`uxmeta` keeps 9/UX's persistent metadata: application declarations,
file associations, icons, the volume registry, and Finder layout. The
data lives in human-editable [ndb(6)](https://9p.io/magic/man2html/6/ndb)
files. `uxmeta` is the only program that writes them on 9/UX's behalf: it
reads several files as ordered layers, answers queries about the merged
view over 9P, serializes every update, replaces files atomically, and
publishes a generation number and a change stream. People may still edit
the files by hand at any time; `uxmeta` notices and merges.

Design background: [design §6](../design.md#6-metadata-ndb--uxmeta).

## 1. Records

A metadata file is an ndb file. Each record (ndb "entry") starts at the
left margin; continuation lines are indented; `#` starts a comment.
Values containing white space or `#` are written in double quotes.
ndb has no escape for `"`, so a value can't contain both a double quote
and white space; `uxmeta` refuses to write such a value.

The **first attribute of a record names its kind, and its value is the
record's key**. Two records with the same kind and key, in any layers,
describe the same object. Attribute names are case-sensitive, at most
31 bytes, and may not contain `=`, `"`, `#` or white space. An attribute
may repeat inside a record (a list); its values keep their order.

```text
app=sketch exec=/apps/Sketch/sketch icon=sketch
	resources=/apps/Sketch/resources.rsrc

ext=.txt type=text
type=text open=edit icon=text desc="Plain text"

volume=home path=/usr/glenda icon=disk desktop=yes x=20 y=40

folder=/usr/glenda/doc view=icons sort=name
	x=100 y=90 width=550 height=400
item=/usr/glenda/doc/notes.txt x=32 y=48
```

### Kinds

| Kind | Key | Common attributes | Meaning |
|---|---|---|---|
| `app` | short application name | `exec` (command), `port` (plumb port it listens on), `icon`, `resources` (`.rsrc` path), `name` (display name), `type` (document types it can open, repeated) | An application 9/UX can launch |
| `type` | type name | `open` (app or port), `icon`, `desc` | A document type |
| `ext` | file name suffix with its dot, e.g. `.txt` | `type`, `open`, `icon`, `plumb` | Maps a file name suffix to a type and how to open it |
| `icon` | icon name | `image` (image(6) file), or `rsrc` with `res` (`TYPE ID`) | Where an icon's pixels come from |
| `volume` | volume name | `path` (mount point in the session namespace), `addr` (dial string of a 9P server to mount there) or `srv` (a `/srv` file to mount there), `spec` (attach name), `icon`, `desktop` (`yes`/`no`), `x`, `y` | A registered volume (design §4: volumes are declared, never inferred). With neither `addr` nor `srv`, `path` is already in the namespace. |
| `folder` | absolute directory path | `view` (`icons`/`list`), `sort` (`name`/`kind`/`date`/`size`), `x`, `y`, `width`, `height`, `scrollx`, `scrolly`, `icon` | A folder window's layout |
| `item` | absolute path of a file or directory | `x`, `y` (position in its parent's icon view), `icon` (custom icon), `label` | One entry's layout and per-item choices |

Paths in `folder` and `item` keys are paths **in the workspace's
namespace** (design §4). Desktop positions of volumes are the `x` and `y`
of their `volume` record. Other kinds are allowed and are kept, merged
and served like these, so new components can add kinds without changing
`uxmeta`.

Unknown attributes are always kept: `uxmeta` never drops an attribute it
doesn't understand, and rewriting a record keeps its other attributes and
their order.

### Lookup rules

- `ext` keys match the end of a file name, longest key first, ignoring
  ASCII case (`.tar.gz` beats `.gz`; `.TXT` matches `.txt`).
- An `ext` record's own attributes win over its `type` record's;
  `open` naming an `app` record brings in that app's `exec` and `port`.
- An `item` record's `icon` wins over the association's.

## 2. Layers and precedence

`uxmeta` reads an ordered list of files. A later layer overrides an
earlier one. From lowest to highest:

| Layer | Default files | Kinds it may define | Written by uxmeta |
|---|---|---|---|
| `system` | `/lib/ux/meta/*.ndb` (sorted by name) | all | never |
| `apps` | `/apps/*/app.ndb` (sorted by path) | `app`, `type`, `ext`, `icon` | never |
| `user` | `$home/lib/ux/meta.ndb` | all | yes |
| `workspace` | `$home/lib/ux/workspace/$workspace.ndb` | all | yes |

Each file is its own layer, named `system/NAME`, `apps/APP`, `user`, or
`workspace`. Command-line options change the directories (see
`uxmeta(4)` in [`uxmeta/README.md`](../../uxmeta/README.md)).

**Merging.** For each kind and key, the merged record starts from the
lowest layer that has it. Each higher layer's record then replaces, for
each attribute name it mentions, all values of that attribute; attributes
it doesn't mention show through from below. An attribute set to the empty
value (`x=""`) hides that attribute from lower layers. A record with
`removed=yes` hides the whole record from lower layers (for example to
take a system-defined volume off a user's desktop); `removed` isn't shown
in the merged view.

**Trust.** Layers only define the kinds listed above; records of other
kinds in a layer are ignored (and reported in `layers`). An `app.ndb`
can therefore declare its application and the types it opens, but can't
add a volume, move a window, or change an `item`. Only the configured
directories are read: an ndb file found on a mounted volume or inside a
document is never loaded as a layer just because it exists. Hints derived
from file contents (such as a resource file's `BNDL`/`FREF`) are not
layers; importing them is an explicit write, at the `user` layer, by
whoever imports them.

**Where writes go.** An update names its layer, or defaults to
`workspace` for `folder` and `item` and to `user` for everything else.
`system` and `apps` layers are read-only through `uxmeta`.

## 3. Files on disk

- **One writer.** At start `uxmeta` creates `$home/lib/ux/.uxmeta.lock`
  as an exclusive-use file (`DMEXCL`) and keeps it open. A second
  `uxmeta` for the same user directory fails to start.
- **Atomic replace.** To write layer file `F`, `uxmeta` writes the whole
  new contents to `F.new` ending with the line `#uxmeta end`, removes
  `F.old`, renames `F` to `F.old`, then renames `F.new` to `F` (rename is
  `wstat` within one directory; Plan 9 file servers can't rename over an
  existing file, hence the three steps). At start, if `F` is missing,
  `uxmeta` recovers from `F.new` if it ends with `#uxmeta end`, otherwise
  from `F.old`. Files `uxmeta` writes therefore end with that comment
  line; it is ignored when reading.
- **Formatting kept.** Comments, blank lines, and records `uxmeta` didn't
  change are written back byte for byte. A changed record is rewritten in
  place in a canonical form: kind and key first, then the other
  attributes in their existing order, new ones at the end, wrapped at
  about 72 columns with tab-indented continuation lines.
- **External edits.** Before any write, and at least once a second
  otherwise, `uxmeta` checks each layer file's qid version, length and
  modification time, and re-reads files that changed, appeared, or
  disappeared (including new `app.ndb` files). A write is applied to the
  freshly read file, so a hand edit made a moment earlier is kept, never
  overwritten. If a hand-edited file doesn't parse, its last good
  contents stay in effect and the error appears in `layers`.
- **Generated plumbing rules** go to `$home/lib/ux/plumbing` (see §5),
  also replaced atomically.

## 4. The 9P interface

`uxmeta` posts `/srv/uxmeta.$user` and mounts itself on `/mnt/meta`
(both configurable). Files:

```text
/mnt/meta/
	version     # read: "uxmeta 1" and a feature list
	gen         # read: current generation number
	ctl         # write: update commands; read: result of the last write on this fid
	query       # write: one query; read: its result
	db          # read: the whole merged view, as ndb text
	changes     # read: per-open change stream
	layers      # read: layers, files, state, errors
	plumbing    # read: the generated plumbing rules
```

Conventions follow the rest of 9/UX: UTF-8 text, one command or event per
line, fields split with `tokenize(2)` rules and written with `%q`
quoting (`quote(2)`), errors as 9P error strings.

### Generation

`gen` is a decimal number that increases by at least one every time the
merged view changes, whether from a write through `ctl` or an edit made
directly to a file. It never goes backwards within one `uxmeta`; it is
also saved (`$home/lib/ux/meta.gen`) and continues across restarts. A
client that sees a lower `gen` than before must resync.

### Queries: `query`

Open `query`, write one query, and read the result on the same fid.
Each write replaces the fid's result, and reads after a write start at
the beginning of the result whatever their offset, so `rc` can use one
`<>` file descriptor for both. The result starts with a
comment line `# gen N` (the generation it reflects) followed by matching
merged records in ndb form, one record per line, ordered by kind then
key.

```text
KIND [KEY] [ATTR=VALUE ...]   records of KIND, optionally with key KEY and
                              matching every ATTR=VALUE (VALUE * matches any)
* [ATTR=VALUE ...]            records of any kind
under KIND PATH               folder/item records whose key is PATH or below it
resolve PATH                  how to show and open the file PATH (below)
explain KIND KEY              each layer's own record for KIND KEY, lowest
                              first, each preceded by "# layer NAME"
```

`resolve` answers Finder's main question in one read. Its result is one
record:

```text
file=/usr/glenda/doc/notes.txt ext=.txt type=text open=edit icon=text port=edit
```

`ext`, `type`, `open`, `icon`, `port`, `exec`, and `desc` appear when
known, combining the lookup rules in §1. A file with no association gets
just `file=PATH`.

### Updates: `ctl`

Write one or more commands, one per line. **All commands in one write are
one transaction**: they all apply, the files are written, and `gen`
increases once, or none apply and the write fails with the first error.
After a successful write, reading the same fid (from the start, as with
`query`) returns `gen N`.

```text
set KIND KEY ATTR=VALUE ...    set attributes on KIND KEY (creating the
                               record in the target layer if needed)
unset KIND KEY ATTR ...        remove attributes from the target layer's
                               record, so lower layers show through
remove KIND KEY                hide KIND KEY: delete it from the target
                               layer and, if a lower layer still has it,
                               write removed=yes
delete KIND KEY                delete the target layer's record only
rename OLDPATH NEWPATH         move folder and item records keyed by
                               OLDPATH or anything below it (what Finder
                               does after it renames or moves a file)
reload                         re-read every layer now
```

Modifiers that may come first on any line:

- `layer=NAME` writes to that layer (`user` or `workspace`) instead of
  the default.
- `ifgen=N` fails the whole write with `generation mismatch` unless `gen`
  is still `N`. Clients use it for read-modify-write.

`rename` applies to the target layer; with no `layer=` it applies to
both `user` and `workspace`.

### Changes: `changes`

Each open of `changes` gets its own cursor starting at the current
generation, and the first line it reads is `GEN sync`. After that, reads
block until something changes and return whole lines:

```text
GEN set KIND KEY       the merged record KIND KEY was created or changed
GEN del KIND KEY       the merged record KIND KEY is gone
GEN lost               this reader fell behind; read again from a snapshot
```

`GEN` is the generation after the change; one transaction may produce
several lines with the same `GEN`. A reader takes a snapshot (`db` or a
query, which carry `# gen N`), then skips lines with `GEN` at or below
it. Each open holds at most 256 lines; when a slow reader would lose
lines, they are replaced by one `lost` line and the queue restarts from
there, and other readers and writers are never held up. A flush of a
blocked read returns `interrupted`.

### Layers: `layers`

One line per layer, lowest first:

```text
NAME PATH STATE [ERROR]
```

`STATE` is `ok`, `missing`, `rw` (writable, present), or `error`, with
the parse error or the ignored kinds after it.

## 5. Plumbing

Associations become plumbing rules so the plumber stays the dispatch path
(design §4). For each merged `ext` record that resolves to an `open`
(directly, or through its `type`) and doesn't have `plumb=no`, `uxmeta`
writes a rule to `$home/lib/ux/plumbing`:

```text
# ext .txt: type text, open edit
type is text
data matches '([^ ]+)\.[tT][xX][tT]'
arg isfile $0
plumb to edit
```

When `open` names an `app` with an `exec`, the rule also gets
`plumb client EXEC` if the app has a `port`, or `plumb start EXEC $file`
if it doesn't (and then `plumb to` is left out). Without an `app`
record, `open` is taken as a port name. Rules are ordered longest suffix
first so `.tar.gz` wins over `.gz`.

The file is regenerated whenever the merged associations change. The
user's own `$home/lib/plumbing` includes it, above `include basic` so the
associations win:

```text
include /usr/glenda/lib/ux/plumbing
include basic
```

`uxmeta -P` also reloads the plumber after regenerating, by writing the
user's `$home/lib/plumbing` to `/mnt/plumb/rules`. Without `-P` nothing
outside `$home/lib/ux` is touched.

## 6. Permissions

- The service is posted for one user with mode 0600, and attaches from
  any other user are refused.
- The attach name picks what a connection may do: empty or `rw` for
  everything; `layout` for queries plus updates that touch only `folder`
  and `item` records or the attributes `x`, `y`, `width`, `height`,
  `view`, `sort`, `scrollx`, `scrolly`, `desktop` on any record (enough
  for Finder's layout); `ro` for reads only. Writes outside a
  connection's rights fail with `permission denied`.
- `system` and `apps` layers can't be written through `uxmeta` at all.

## 7. Versioning

`version` reads `uxmeta 1` followed by feature words, currently
`query resolve explain changes rename ifgen plumbing`. New commands,
query forms, kinds and attributes are additive and announced as feature
words; anything incompatible bumps the number.
