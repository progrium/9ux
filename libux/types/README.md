# libux/types: 9ux's extensions of uitree's type specs

These are uitree type specs in uitree's own format, meant to go back
upstream. [gencatalog.sh](../gencatalog.sh) lays this directory over
uitree's `core/types`: a type here replaces uitree's spec of the same
name, or adds a type uitree doesn't have. To regenerate the catalog
libux embeds:

```sh
libux/gencatalog.sh UITREE/core/types libux/types \
	core form nav feedback layout data overlay canvas desktop +combobox \
	>libux/tcatalog.c
```

| Spec | Change from upstream | Why |
|---|---|---|
| [input](input/spec), [textarea](textarea/spec) | adds `prop selection text file`: rune offsets `Q0 Q1`, the caret when they're equal | A script can't select part of a field without it, so it can only cut, copy or paste the whole value. libux keeps the prop current after its own edits (typing, paste) and sets the field's selection when a script writes it. |
| [list](list/spec) | adds `prop columns data file` with `file columns format=columns` (table's format, `id label width`) | A Finder list view shows columns under headers. With columns, an item is a line of tab-separated fields: its id, then each column's text. A script can change the headers and widths, and the rows. |
| [file-chooser](file-chooser/spec) | new type, profile `desktop` | A Mac Open dialog's file list. `dir` is the directory shown, `items` its entries (`N name`, a directory ending in `/`), `value` the name chosen last. Verbs `cd PATH`, `choose NAME` and `cancel`; events `change dir=` when it goes to another directory, `choose value=` and `cancel`. A script writing `dir` goes there too. |

Each change is marked with a `# 9ux:` comment in its spec, so a diff
against upstream shows exactly what to propose.
