# res: the resfs tree

`resfs` serves classic Macintosh resource files, decoded, at `/mnt/res`.
Clients are libux (icons, menus, strings and dialogs from an app's
resources) and Finder (icons). The server is read-only.

This document follows the shared conventions in [README.md](README.md),
with one addition for resource text (escapes, below).

```text
/mnt/res/
    version                  res 1.0
    <file>/
        index                one line per resource
        icon/<id>/
            image            32x32 icon, best depth available
            small            16x16 icon, best depth available
        <type>/<id>/
            raw              the resource's bytes, unchanged
            name             the resource's name (only if it has one)
            image            decoded image (icon types and CURS)
            cursor           /dev/cursor form (CURS)
            text             text form (STR , STR#, MENU, DLOG, DITL, ALRT)
```

## Starting it

```text
resfs [-D] [-m mtpt] [-s srvname] [name=]file ...
```

- `-m` sets the mount point (default `/mnt/res`, which must exist;
  `-m ''` mounts nothing). `-s` posts the service in `/srv`. `-D` traces 9P.
- Each file appears as a directory named after it, without directory
  or extension: `sketch/resources.rsrc` is `resources`. `name=file`
  gives the name instead. A name used twice, or `version`, is an
  error.
- A file is a bare resource fork (`.rsrc`), or an AppleSingle or
  AppleDouble file (version 1 or 2), whose resource fork entry (ID 2)
  is served; the other entries are ignored. The directory name drops
  AppleDouble's `._` prefix too: `._Sketch` is `Sketch`.
- Every file is parsed and decoded when resfs starts; a file that
  doesn't parse stops resfs with an error naming it. Files are limited
  to 64 MB and 65536 resources.

## Versions

`version` holds `res 1.0` and no feature lines yet. Clients check it as
README.md says.

## Names

- `<type>` is the four-byte type code. Bytes outside printable ASCII
  (`0x21` to `0x7E`), and `/` and `%`, are written `%XX` with uppercase
  hex. So `STR ` is `STR%20`, `ICN#` is `ICN#`, and type `ab/c` is
  `ab%2Fc`. Type codes are case-sensitive.
- `<id>` is the signed 16-bit resource ID in decimal: `128`, `-16455`.
- A type directory lists every resource of that type. A file holding
  the same type and ID twice keeps the first in map order.
- Files are created only where the data decodes: a `MENU` whose bytes
  are truncated has `raw` but no `text`. A client that wants a decoded
  form checks that the file exists.

## index

One line per resource, sorted by type bytes and then ID:

```text
CURS 128 size=68 attr=purgeable name=Cross
ICN# 128 size=256 name=Sample
STR%20 128 size=34
zzzz -16455 size=4 'name=Opaque é'
```

Positional: type (escaped as above), ID. Attributes: `size` (bytes in
`raw`), `attr` (flags from the resource attribute byte: `sysref`,
`sysheap`, `purgeable`, `locked`, `protected`, `preload`, `changed`;
left out when none are set), `name` (left out when the resource has
none).

## Text conventions

Resource strings are MacRoman. resfs converts them to UTF-8 using
Apple's MacRoman table, with `0xDB` as the euro sign. In the decoded text
forms (not in `name` or `index`), the system font's control glyphs are
mapped too: `0x11` ⌘ U+2318, `0x12` ✓ U+2713, `0x13` ◆ U+25C6, `0x14`
the Apple logo, U+F8FF. A NUL byte becomes U+FFFD.

Resource strings can contain line breaks (usually CR), and the text
forms are line-oriented, so this protocol tightens README.md's rule
that values with newlines get their own file: in the text forms, string
values are **escaped**:

| In the string | Written as |
|---|---|
| backslash | `\\` |
| CR or LF | `\n` |
| tab | `\t` |
| any other control character | `\xHH` |

Escaping happens first; then an attribute token is quoted with
`quote(2)` rules as a whole (`'title=Open File'`). A reader tokenizes the
line, splits each attribute at the first `=`, and unescapes the value.

`name` holds the name in UTF-8, unescaped, followed by a newline.

## Images: `image` and `icon/`

Images are 9front `image(6)` files, uncompressed: a 60-byte header of
five 11-character right-justified fields each followed by a space
(`a8r8g8b8`, `0`, `0`, width, height), then the pixels, 4 bytes each,
stored B, G, R, A, rows top to bottom. Alpha is premultiplied, and since
masks are 1-bit every pixel is either opaque or all zero. Read it with
`readimage(2)`; a client that needs a different depth converts it.

| Type | Size | Depth | Mask |
|---|---|---|---|
| `ICN#` | 32x32 | 1 | second half of the `ICN#` |
| `icl4` | 32x32 | 4, classic 16-colour palette | `ICN#` with the same ID |
| `icl8` | 32x32 | 8, classic 256-colour system palette | `ICN#` with the same ID |
| `ics#` | 16x16 | 1 | second half of the `ics#` |
| `ics4` | 16x16 | 4 | `ics#` with the same ID |
| `ics8` | 16x16 | 8 | `ics#` with the same ID |
| `CURS` | 16x16 | 1 | the cursor's own mask |

- 1-bit pixels are black when set and white when clear.
- Where the mask is clear the pixel is transparent. A colour icon with
  no matching mask resource, or an `ICN#`/`ics#` with no mask half, is
  opaque.
- For `CURS`, a pixel set in the data but clear in the mask inverts the
  screen on a Mac; resfs draws it black.
- The palettes are the standard system colour tables (`clut` 4 and 8):
  the 16 colours from white to black, and the 6x6x6 cube with red,
  green, blue and grey ramps and black at 255.

`icon/<id>/image` is the deepest of `icl8`, `icl4`, `ICN#` with that ID
that decodes; `icon/<id>/small` is the deepest of `ics8`, `ics4`, `ics#`.
A file is absent when no member of its size exists. This is what Finder
and libux should normally read.

## cursor

`CURS/<id>/cursor` is the form `/dev/cursor` takes (see `mouse(3)`), so
`cp /mnt/res/app/CURS/128/cursor /dev/cursor` sets it: 72 bytes, the
offset as two 4-byte little-endian integers (x, then y), then `clr`
(32 bytes) and `set` (32 bytes), 16 rows of 2 bytes each. `set` is the
cursor data, `clr` is mask and not data, and the offset is minus the hot
spot.

## text

### STR and STR#

`STR%20/<id>/text` is the string, escaped, then a newline.
`STR#/<id>/text` is one escaped string per line, in order; line `n` is
string `n` (counting from 1). An empty string is an empty line.

```text
First
Two\nlines

tab\there \\ back
```

### MENU

The first line describes the menu; each following line is an item, in
order.

```text
menu id=128 title=File proc=0 enabled=on
item title=New key=N enabled=on
item 'title=Open…' key=O enabled=on
item title=- enabled=off
item title=Recent submenu=130 enabled=on
item 'title=Bold item' mark=✓ icon=3 style=bold,underline enabled=on
```

| Attribute | On | Meaning |
|---|---|---|
| `id` | menu | the menu ID stored in the resource |
| `title` | both | text; a title of `-` is a separator |
| `proc` | menu | the `MDEF` ID (0 is the standard menu) |
| `enabled` | both | on the menu, the whole menu; on an item, that item (items after the 31st are always on) |
| `key` | item | the Command-key equivalent, one character |
| `submenu` | item | the ID of a hierarchical submenu (replaces `key` and `mark`) |
| `script` | item | a script code for the item's text (replaces `icon`) |
| `mark` | item | the mark character, such as ✓ |
| `icon` | item | the item's icon number (the icon resource is 256 + this) |
| `style` | item | list of `bold`, `italic`, `underline`, `outline`, `shadow`, `condense`, `extend` |

Item attributes that are zero or empty are left out, except `enabled`.

### DLOG

One line:

```text
dlog rect=30,40,330,140 proc=1 visible=on goaway=off refcon=305419896 ditl=128 'title=Find File' position=12298
```

`rect` is the window rectangle (Plan 9 order: left, top, right,
bottom), `proc` the window definition ID, `refcon` the reference value
as an unsigned integer, `ditl` the item list's ID, and `position` the
optional automatic position word, present only when the resource has
one.

### ALRT

```text
alrt rect=80,60,400,160 ditl=129 stages=21845 position=12298
```

`stages` is the 16-bit stage word, as stored; `position` as in DLOG.

### DITL

One line per item, numbered from 1:

```text
item n=1 type=button rect=220,70,290,90 enabled=on text=OK
item n=3 type=text rect=10,10,100,26 enabled=off 'text=Find what:'
item n=6 type=icon rect=10,70,42,102 enabled=off id=128
item n=8 type=help rect=2,1,4,3 enabled=on data=00010080
```

`type` is one of `user`, `help`, `button`, `checkbox`, `radio`,
`control`, `text` (static text), `edit`, `icon`, `picture`, or the
decimal type number for anything else. `rect` is relative to the
dialog. `enabled=off` is the item's disabled bit. Buttons, check boxes,
radio buttons, static and edit text have `text`. Controls, icons and
pictures have `id`, the resource ID they draw. Other items with data
have `data`, the bytes in lowercase hex.

## Resource source

`rescomp` compiles a text source into a resource file, so apps and
9/UX's own assets need no classic Mac tools; `rescomp -d` turns a
resource file back into source. The bodies are the text forms above,
so whatever resfs serves as `text` can be pasted back in.

```text
rescomp [-o out.rsrc] file.res     compile (to standard output without -o)
rescomp -d file.rsrc               decompile to standard output
```

A source file is lines. Blank lines and lines starting with `#` are
ignored between resources. Two commands:

```text
resource TYPE ID [name=...] [attr=flags] [hex=... | file=PATH | image=PATH [bw=PATH] [hotspot=X,Y]]
icon ID [name=...] [large=PATH] [large1=PATH] [small=PATH] [small1=PATH]
```

`TYPE` is escaped as in the tree (`STR%20`), `ID` is decimal, `name`
is escaped text like any other value, and `attr` takes the flags the
index uses. Paths are relative to the source file.

How the resource's bytes are given:

| Form | Bytes |
|---|---|
| `hex=0001ff` | the bytes, in hex |
| `file=PATH` | the file's contents |
| `image=PATH` | an icon type (`ICN#`, `icl4`, `icl8`, `ics#`, `ics4`, `ics8`) or `CURS`, from an uncompressed image(6) file |
| none of these | a text form: the following lines, up to a line that is just `end`, as served in `text` for `STR `, `STR#`, `MENU`, `DLOG`, `DITL` and `ALRT` |

```text
resource STR# 128 name=Messages
First
Two\nlines
end

resource MENU 128
menu id=128 title=File
item title=New key=N
item title=- enabled=off
end

resource CURS 128 image=cursors/cross.img hotspot=8,7
icon 128 name=Document large=icons/doc.32.img large1=icons/doc.32.1.img small=icons/doc.16.img
```

`icon` makes a whole family: `ICN#`, `icl4` and `icl8` from `large`,
and `ics#`, `ics4` and `ics8` from `small`, each with that ID, and
`name` on the `ICN#`. `large1` and `small1` give the 1-bit forms;
without them the 1-bit form is the colour image thresholded on
brightness.

In text forms, attributes may be left out where they have a natural
default: `enabled=on`, `visible=on`, `goaway=off`, `proc=0`,
`refcon=0`, `stages=0`, and no `position`. A DITL item's `n` is ignored
(items are numbered by order), and a MENU item past the 31st can't be
disabled, as on the Mac. A `MENU` sets the enable flags of items it
doesn't list, as Rez does. In a string, `\n` is written as CR, the
Mac line break. A string whose text is `end` is written `en\x64`;
`\xHH` takes any ASCII byte except NUL.

Images are converted like this:

- A pixel is in the mask when its alpha is at least half; the mask of
  `ICN#`/`ics#` and of `CURS` comes from the image's alpha. `icl4`,
  `icl8` and their small forms use the matching `ICN#`/`ics#` as their
  mask, as on the Mac, so a family's images should share one mask.
- In 1-bit forms a pixel is set (black) when it's in the mask and its
  brightness is under half. A `bw` (or `large1`/`small1`) image must
  not draw outside the colour image's mask.
- Colour pixels map to the nearest colour (by RGB distance) in the
  classic 16- or 256-colour palette; pixels outside the mask are
  written as palette index 0.
- `CURS`: black where the image is dark and opaque, white where it is
  light and opaque, clear elsewhere. The hot spot defaults to `0,0`.

What a round trip keeps: `rescomp -d` writes a text form only when
compiling it gives back exactly the same bytes; otherwise it writes
`hex=`. Icons and cursors always decompile to `hex=`. Compiling from
images loses whatever lies under a clear mask, and the Mac's
inverted cursor pixels.

## Safety

Resource files are untrusted. The parser checks every offset, length
and count against the bytes present before using it; refuses maps whose
type or reference lists run past the map, or that claim more than 65536
resources; skips (and reports on stderr) references whose data lies
outside the data area; and the decoders check the data length for every
type before reading. The parser and decoders are fuzzed on the host
(`resfs/fuzz`).

## Not yet

- `PICT`, `cicn`, `crsr`, and `BNDL`/`FREF` as import hints.
