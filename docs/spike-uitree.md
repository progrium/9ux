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

## Next, if this goes ahead

1. Serve `style`, `src`, `text` and `runs` as files, and `new` over the
   ux.md tree. That makes this reachable from rc.
2. Run uitree's `08-elements.rc` conformance test against it.
3. Add `img`, `icon`, `scroll` and `spacer` to finish `core`.
