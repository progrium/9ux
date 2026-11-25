# Spike: uitree layout, svg and styled-text on libux

**Status:** spike, not for merging as is. It answers one question from
the uitree evaluation: can libux on libdraw do the three parts of
uitree's required `core` profile that it lacked (flexbox layout, `svg`
and `styled-text`)? It can, in about 1,700 lines of C.

Screenshots come from `test/spike/spike_test.zig`, which saves them to
`.local/test/artifacts/spike-*.png`:

```sh
scripts/test.sh build
scripts/test.sh gui -- -Dfilter=spike
```

## What's here

| File | What |
| --- | --- |
| `libux/flex.c` | uitree's style words, a flexbox layout, theme tokens, and the `div` element |
| `libux/svg.c` | An SVG subset, with a 4x4 supersampled rasterizer, and the `svg` element |
| `libux/stext.c` | The `styled-text` element: runs, mixed fonts, wrapping |
| `apps/spike/` | A demo app: a mail screen (`spike`) and an svg gallery (`spike -s svg`) |
| `test/spike/` | The GUI test that checks a few rectangles and saves the screenshots |

The C API is a stand-in for uitree's files: `uxwinstyle(w, words)` gives
a window a root style, `uxstyle(c, words)` styles a control, and
`uxdiv`, `uxsvg` and `uxstyledtext` make the new elements. A window with
a root style is laid out on every redraw. Every rectangle in
`apps/spike` is `ZR`.

## Flexbox

- **Every portable style word in uitree's core.md is accepted.** Most of
  them work: `flex flex-row flex-col v-flex h-flex flex-1 flex-none`,
  `items-*`, `justify-*` (including `between`), `gap= p= px= py= pt= pb=
  pl= pr= m= mx= my=`, `w= h= min-*= max-*=` in pixels, `%`, or `rem`,
  `size-full w-full h-full`, `absolute` with `top= left= right= bottom=`,
  `bg= text-color= border-1 border-color=`, `text-xs` to `text-3xl`,
  `font-bold font-semibold`, and `text-left/center/right`.
- **Accepted and not drawn:** `flex-wrap`, `overflow-*`, `rounded-*`,
  `truncate`, and the state prefixes (`hover:` and the rest).
- Unknown words fail with `unknown style word: WORD`, and a failed write
  changes nothing, as uitree says.
- The layout follows CSS flexbox closely enough for app screens: the
  basis, grow, and shrink are weighted by basis, and a leaf with
  `min-width: auto` won't shrink below its content. Cross-axis stretch
  is the default. Container sizes come from their children, and
  absolute children are placed against the padding box. Leaves measure
  themselves from libux's metrics. For example, a button's width is its
  label plus `UXbuttonpad`, and a `styled-text` node's height is its
  wrapped height at the width it's given.
- `div` and `group` are containers. Elsewhere, controls keep their
  existing `parent` links, so nesting is still flat directories in the
  ux.md tree. The `rect` files show the layout's results.
- Theme tokens map onto the 9/UX palette (`primary` is deep teal,
  `accent` is butter, `muted` is grey1, and so on). The palette's own
  names (`teal`, `navy`, `cream`, ...) also work, as host extras.

## svg

- **Supported:** `<svg>` with `viewBox`, `width` and `height`; `<g>`
  inheritance; `<path>` with every command, including arcs and implicit
  repeats; `<rect>` with `rx`/`ry`; `<circle>`, `<ellipse>`, `<polygon>`,
  `<polyline>` and `<line>`. Also `fill`, `stroke`, `stroke-width`,
  `fill-rule`, `#rrggbb` colours, theme tokens, and `currentColor`,
  which is the node's `text-color`, or ink by default.
- **Drawing:** curves are flattened. Fills use nonzero or even-odd
  coverage at 4x4 samples per pixel into a GREY8 mask, so edges are
  antialiased. Strokes are a quad per segment plus a disc at every
  vertex, which gives round joins and caps. Each shape is drawn through
  its mask in its colour. The result is cached as an ARGB32 image per
  control and is redrawn only when the size or `currentColor` changes.
- **Not handled:** transforms, gradients, `<text>`, `<use>`, CSS
  `style=`, dashes, miter and square joins, opacity. Icon sets in the
  Lucide or Feather style (stroked, round, 24-unit grid) and flat logos
  draw correctly.
- `img`-style scaling comes for free: one source draws at 16, 24, 44 and
  128 pixels in the gallery.

## styled-text

- uitree's `text` and `runs`: one `start len word...` line per range,
  with byte offsets, as in the GPUI host. The words are `text-color= bg=
  font-bold font-semibold italic underline line-through`, plus the size
  words.
- Fonts are 9front's lucidasans at sizes 6, 7, 8, 10 and 13, with bold
  and italic. Bold doesn't exist at every size, and it falls back to the
  regular face.
- Text wraps at blanks to the node's width, and a newline breaks the
  line. The fonts in one line share a baseline, and the line's height is
  its tallest font. There's no extra leading yet, and no selection or
  editing (`selectable` isn't done).

## Numbers

On h9's headless screen (native amd64), a whole-window redraw takes:

| Screen | First redraw | Later redraws |
| --- | --- | --- |
| Mail, 860x556, 50 controls with flex, svg and styled-text | 21 ms | 0.7 to 1.2 ms |
| svg gallery, 760x556, 44 svgs up to 128 px | 9 ms | 0.6 ms |

The first redraw includes parsing every SVG and rasterizing it once. Set
`uxspikedebug=1` to print these times.

## What the spike found

- **Flexbox is the right fit, and it's cheap.** Laying the window out
  on every redraw costs too little to measure, so there's no incremental
  layout to maintain. Resizing just works (see the `-narrow`
  screenshot).
- **The UI thread's stack is small.** libthread gives
  `threadmain` 8 KB unless the app sets `mainstacksize`, and recursive layout with arrays on the
  stack corrupted the heap. That's fixed here by allocating on the heap.
  A real host must treat stack depth as a limit, because uitree allows
  64 levels of nesting.
- **libux's flat `controls/` directory is the awkward part**, not the
  drawing. The spike keeps it and uses `parent`. A uitree host needs the
  nested node tree from the evaluation's step 4.
- **`rounded-*` should probably stay a no-op on 9/UX.** The classic look
  has square boxes, and uitree lets a host draw styles its own way.
- **Not covered:** `img`, `icon` (resfs icons by name), `scroll`,
  `spacer`, and making any of this reachable through files. The spike is
  C API only.

## uxkit: uitree's conformance suite

`uxkit/` is a uitree host for the file protocol (draft 0.2): a lib9p
server that serves `$ui` and runs a client in it, in the form uitree's
conformance suite drives (`uitree-headless CMD ARGS`). It claims `layer
core`, `layer components` and the profiles `core`, `form`, `nav`,
`feedback`, `layout`, `data`, `overlay` and `canvas`. That's every
profile the suite tests; only the transfer layer is missing.

```sh
h9 --rc 'cd uxkit && mk'                              # or scripts/test.sh build
UITREE=../scratch/uitree test/conformance/run.sh      # all 28; args go to uitree's run
```

**Result: 27 passed, 0 failed, 1 skipped.** The skip is 16 transfer
(drag and drop, `drag/N`, `clipdata/`), which is a layer of its own.

The descriptors aren't hand-written: `uxkit/gencatalog.sh` turns
uitree's `core/types/*/spec` files for the named profiles into
`catalog.c`, and uxkit parses them at start. Props, words, verbs, slots,
child rules, the overlay tier and data file formats all come from there.

| File | What |
| --- | --- |
| `uxkit/model.c` | The app state: windows, node trees, props, theme, keys, menus, snapshots, the core descriptors, style and value checks, tree printing |
| `uxkit/cmd.c` | The ctl grammar (tokens, here-values, all or nothing), the node, window and app verbs, requests, batches, documents, keys, menus, `/debug/inject` |
| `uxkit/ev.c` | Subscriptions at the three grains, coalescing state events, blocking reads, `gone`, error events and the loud stderr lines |
| `uxkit/fs.c` | The 9P file tree, and `main`: mount at `/mnt/ui`, run the client, exit with its status |
| `uxkit/canvas.c` | canvas's `ops` display list and its ctl verbs (set, add, del, raise, lower) |
| `uxkit/ui.c` | With `-d`, mirrors the model into libux controls and sends clicks, close and resize back as events |
| `uxkit/catalog.c` | Generated by `gencatalog.sh` from uitree's type specs |
| `test/conformance/run.sh` | Copies the suite to `.local/conformance` and runs it with uxkit in h9 |

Notes from getting there:

- A ctl write is all or nothing by snapshot: the whole app state is
  copied before the write and put back if a line fails, and the events
  the write made are held until it succeeds. Node ids survive a restore,
  so open fids stay good. Batches use the same snapshot. Copying is cheap
  at app-screen sizes; it would want a journal for very large trees.
- Overlays live in each window's `over/`, a node beside the root rather
  than one of its children, so it never shows up in the root's tree or
  order and a document rewrite can't delete it.
- Event attrs come out sorted by name, `win=` included, because every
  golden has them that way.
- Where the spec leaves a format open, uxkit follows the goldens and
  uitree's `GAPS.md`. `style/words` lists valued words as `NAME value`,
  because the 06 check reads the first field.

## uxkit -d: drawing the tree

With `-d`, uxkit also draws: each window becomes a libux window, and
each node a libux control. text becomes a label, styled-text and svg use
the spike's code, and everything else is a flex div with the node's
style words. A client builds a screen through files and sees it drawn.
Clicks on a node with `on=click` come back as `click` events, and the
title bar's close and a resize come back as requests and state events.
`test/uxkit/` builds a mail screen from an rc script, checks bounds,
clicks a card, and waits for the client's restyle to land.

Two things it took:

- The display belongs to the UI thread, so model changes queue a sync
  onto it (`uxcall`) instead of drawing from the 9P proc.
- lib9p's `respond()` must run on the server proc's descriptors. Events
  caused by the UI thread wake a responder proc that answers pending
  reads.

### Native controls

These types are drawn with libux's own controls, inside the flex layout:

| uitree type | libux control | What comes back |
| --- | --- | --- |
| `button` | button | `click` (none while `disabled`) |
| `input`, `textarea` | text field (one line or multiline; `secret` masks) | `change value=V via=user`, mirrored in `value`; `submit` on Enter |
| `checkbox`, `switch` | checkbox | `change value=on\|off via=user` |
| `list` | list, from `items` (groups and separators aren't rows) | `select value=ID via=user`; `open` |
| `table` | list with column headers, from `columns` and `rows` | `select row=ID via=user` |
| `alert-dialog` in `over/` | a modal dialog window: title, description, cancel and confirm buttons | `confirm` or `cancel` (Enter, Esc, the close box), then it closes |
| `dialog`, `sheet` in `over/` | a modal dialog window holding the overlay's children | `close` as a request; the app closes it with `ctl close` |
| `select` | a pop-up menu button (`libux/widget.c`) | `change value=ID via=user` |
| `radio-group` | radio buttons, in a column or a row (`dir`) | `change value=ID via=user` |
| `slider` | a slider with `min`, `max` and `step` | `change value=N via=user` while dragging, `commit` on release |
| `switch` | a sliding switch | `change value=on\|off via=user` |
| `tabs` | a tab bar over the chosen tab's panel | `change value=TAB via=user`; the panel follows `value` |
| `popover` in `over/` | a box placed by its anchor, over the window | `open` and `close` when its anchor is clicked |
| `context-menu` in `over/` | a pop-up menu at the pointer on a right-button press on its anchor | `action ID over/NAME via=user` |
| `number-input` | a field with − and + buttons, kept to `min`, `max` and `step` | `change value=N via=user` |
| `combobox` | a field and a pop-up of the `items` that match what's typed | `query value=TEXT`, then `change value=ID via=user` |
| `sidebar`, `sidebar-group`, `sidebar-item` | a column of groups and items; `active` is highlighted, `badge` on the right | `click` with the item's tag |
| `toolbar` | a row of its children in a bar | (its children's events) |
| `breadcrumb` | its `items` as links, the last one bold | `click item=ID` |
| `tooltip` in `over/` | a small box under its anchor while the pointer is over it | the overlay's `open` follows the pointer |
| `canvas` | its `ops` painted: rect, circle, ellipse, line, polyline, polygon, path, text | `mouse-down`, `mouse-up`, `drag`, `move` with `item=`, `x=`, `y=`, `button=`, and `click` when the press and release are on one item |

libux didn't have a pop-up menu button, radio buttons, a slider, a
switch or tabs, so `libux/widget.c` adds them in the classic look, plus
a pop-up menu over a window that takes the mouse until it's done (press,
drag and release, or two clicks). select and context menus both use it.

Three clients drive these in `test/uxkit/`. `form.rc` is an account
settings screen: its test types in a field, unchecks a box, picks a list
item, saves, then opens the alert dialog with Delete and confirms it
with Enter. `widgets.rc` is a preferences window: its test picks from
the select's menu, a radio button, the slider's end, the switch, opens
and closes the popover, chooses from the context menu and changes tab.
`tools.rc` is a projects screen: its test clicks a sidebar item, steps
the number input, follows a crumb, types in the combobox and picks from
its menu, presses on a canvas item and hovers a toolbar button for its
tooltip. Each test checks every event and the client's answer to it.

Still drawn as plain divs: hover-card, and the layout and feedback
types (they lay out, but without their own look). combobox has no
profile in uitree yet, so `gencatalog.sh` takes it by name (`+combobox`).

### The flaky start

Now and then uxkit hung or crashed in the heap at start (about one run
in ten under load). The cause was `client()`: the forked child built the
client's path with `smprint` before `exec`. A fork in a program with
several procs copies the heap as it is at that moment, and another proc
can be halfway through changing it, so the child's malloc found the lock
held or the free list broken. The path is now made before the fork, and
the child only execs. 240 runs in a row came up clean; before the fix,
failures came within 4 to 37 runs. With `uxkitdebug=1`, a failed heap
assertion leaves the proc asleep so acid can read its stack.

## uxd: uitree apps beside ux.md apps

This is step 3 of the migration: uxd takes both shapes of tree, and a
uitree app shows up under `/mnt/ux/apps` next to the ux.md ones.

- **Registration.** uxd accepts an app whose `version` starts
  `uitree 1.`, and policy.md's `register NAME srv=...` as well as
  ux.md's `name=` form. Windows map by rio's `winid` as before.
- **uxkit registers.** With `-d`, libux's registration (its retries,
  window lines and re-registering when uxd restarts) now posts uxkit's
  uitree server instead of libux's own tree (`uxpublish`). The app's
  name is the client's, or its script's for rc: `tools.rc` is
  `tools-1`; `-n` sets it. uxkit serves that second connection from
  the same model, so the model lock is now uxkit's own, around every
  request on either connection, instead of lib9p's per-connection lock.
- **Rights.** ctl help now names a right per verb, as policy.md asks:
  `set edit`, `new author`, a window's `close activate`, and so on, so
  uxd needs no type knowledge. uxd checks each `ctl` line against it,
  skips here-value bodies, and refuses `author` verbs to every attach,
  the session's too, since it holds the whole tree. Writes other than a
  `value` or a window's `bounds` are refused.
- **Secrets.** A node flagged `secret` (a word in the first line of its
  `tree`) reads empty through uxd, and its values are cut from event
  lines, which get `redacted=on` before `seq=`. A `sensitive` flag
  works the same way for readers without that right, ready for when
  uitree settles where that flag lives (policy.md open question 1).

`uxd/test/uitree.rc` runs uxkit (headless) beside fakeapp and checks
all of this: reads, refused writes, the session's clicks and sets,
a grant, a here-value, redaction, and unregistering. The tools gui
test now runs uxd too, and checks that the drawn window is in
`/mnt/ux/windows` under rio's id and that a click sent through uxd
reaches the client.

Not done yet: `set` needs `edit` whatever prop it names (policy.md
would let a descriptor mark some props `activate`), and a ctl write
split over several 9P writes is checked one write at a time.

## Next, if this goes ahead

1. Step 4: make the node tree libux's source of truth, with the C API
   (`uxbutton`, `uxwindow`, ...) making nodes, so Desktop and the other
   apps' public paths become `win/NAME/...`.
2. Give hover-card and the layout and feedback types their own look.
3. The transfer layer (drag and drop), the last skipped script.
