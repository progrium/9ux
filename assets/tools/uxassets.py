"""Readers and writers for 9/UX asset sources.

Host-side build tool (Python 3, standard library only). It reads the text
sources in assets/ (palette, metrics, icons, cursors, patterns), reads
9front bitmap fonts for mockups, and writes 9front image(6) files, PNG
previews, and C source.

Nothing here runs on 9front; the outputs do.
"""

import os
import re
import struct
import zlib

ASSETS = os.path.normpath(os.path.join(os.path.dirname(__file__), ".."))


# ---------------------------------------------------------------- ndb

def read_ndb(path):
    """Parse an ndb(6)-style file into a list of dicts (one per entry).

    An entry starts on a line that begins with a non-blank; indented
    lines continue it. '#' starts a comment. Values may be "quoted".
    """
    entries = []
    cur = None
    tok = re.compile(r'([A-Za-z0-9_.\-]+)=("[^"]*"|\S*)')
    with open(path) as f:
        for line in f:
            line = line.split("#", 1)[0].rstrip()
            if not line.strip():
                continue
            if not line[0].isspace():
                cur = {}
                entries.append(cur)
            for k, v in tok.findall(line):
                if v.startswith('"'):
                    v = v[1:-1]
                cur.setdefault(k, v)
    return entries


# ---------------------------------------------------------------- palette

class Color:
    def __init__(self, name, char, rgb, bit):
        self.name = name
        self.char = char
        self.rgb = rgb          # (r, g, b)
        self.bit = bit          # "0" black, "1" white, "50" checkerboard

    def mono(self, x, y):
        """1-bit rendering at (x, y): 0 black, 1 white."""
        if self.bit == "50":
            return (x + y) & 1
        return int(self.bit)


def hexrgb(s):
    v = int(s, 16)
    return ((v >> 16) & 255, (v >> 8) & 255, v & 255)


def load_palette(path=None):
    path = path or os.path.join(ASSETS, "palette.ndb")
    byname, bychar = {}, {}
    for e in read_ndb(path):
        if "color" not in e:
            continue
        c = Color(e["color"], e["char"], hexrgb(e["rgb"]), e.get("bit", "0"))
        byname[c.name] = c
        bychar[c.char] = c
    return byname, bychar


def load_metrics(path=None):
    path = path or os.path.join(ASSETS, "metrics.ndb")
    m = {}
    for e in read_ndb(path):
        if "metric" in e:
            m[e["metric"]] = int(e["value"])
    return m


# ---------------------------------------------------------------- grids

class Grid:
    """A small pixel grid of palette characters; '.' is transparent."""

    def __init__(self, rows):
        self.rows = rows
        self.h = len(rows)
        self.w = len(rows[0]) if rows else 0
        for r in rows:
            if len(r) != self.w:
                raise ValueError("ragged grid row: %r" % r)

    def at(self, x, y):
        return self.rows[y][x]


def _read_blocks(path):
    """Read a source file of 'key value' lines and grid blocks.

    Returns (props, blocks): props maps key to value for header lines;
    blocks is a list of (header_words, rows) where a block starts with a
    line whose first word is one of the block keywords and continues
    with grid rows until a blank line.
    """
    props = {}
    blocks = []
    cur = None
    with open(path) as f:
        for raw in f:
            line = raw.rstrip("\n")
            if line.startswith("#"):
                continue
            if not line.strip():
                cur = None
                continue
            words = line.split()
            if cur is not None:
                cur[1].append(line.strip())
                continue
            if words[0] in ("size", "pattern", "cursor", "glyph"):
                cur = (words, [])
                blocks.append(cur)
            else:
                props[words[0]] = " ".join(words[1:])
    return props, blocks


class Icon:
    def __init__(self, path):
        self.path = path
        props, blocks = _read_blocks(path)
        self.name = props.get("name", os.path.splitext(os.path.basename(path))[0])
        self.title = props.get("title", self.name)
        self.sizes = {}         # size -> Grid
        for words, rows in blocks:
            if words[0] != "size":
                continue
            g = Grid(rows)
            n = int(words[1])
            if g.w != n or g.h != n:
                raise ValueError("%s: size %d grid is %dx%d" % (path, n, g.w, g.h))
            self.sizes[n] = g


class Cursor:
    def __init__(self, path):
        props, blocks = _read_blocks(path)
        self.name = props.get("name", os.path.splitext(os.path.basename(path))[0])
        hx, hy = props["hotspot"].split()
        self.hot = (int(hx), int(hy))
        self.grid = Grid(blocks[0][1])
        if self.grid.w != 16 or self.grid.h != 16:
            raise ValueError("%s: cursors are 16x16" % path)


class Pattern:
    def __init__(self, name, title, rows):
        self.name = name
        self.title = title
        self.grid = Grid(rows)


def load_patterns(path=None):
    path = path or os.path.join(ASSETS, "patterns", "patterns.txt")
    pats = []
    _, blocks = _read_blocks(path)
    for words, rows in blocks:
        if words[0] == "pattern":
            title = " ".join(words[2:]).strip('"') or words[1]
            pats.append(Pattern(words[1], title, rows))
    return pats


def load_icons(dirpath=None):
    dirpath = dirpath or os.path.join(ASSETS, "icons")
    return [Icon(os.path.join(dirpath, f))
            for f in sorted(os.listdir(dirpath)) if f.endswith(".icon")]


def load_cursors(dirpath=None):
    dirpath = dirpath or os.path.join(ASSETS, "cursors")
    return [Cursor(os.path.join(dirpath, f))
            for f in sorted(os.listdir(dirpath)) if f.endswith(".cursor")]


# ---------------------------------------------------------------- canvas

class Canvas:
    """An RGBA framebuffer with the few primitives the mockups need."""

    def __init__(self, w, h, fill=(255, 255, 255, 255)):
        self.w, self.h = w, h
        self.px = [list(fill) for _ in range(w * h)]
        self.clip = (0, 0, w, h)

    def set(self, x, y, c):
        x0, y0, x1, y1 = self.clip
        if x0 <= x < x1 and y0 <= y < y1:
            if len(c) == 3 or c[3] == 255:
                self.px[y * self.w + x] = [c[0], c[1], c[2], 255]
            elif c[3] > 0:
                p = self.px[y * self.w + x]
                a = c[3] / 255.0
                for i in range(3):
                    p[i] = int(round(p[i] * (1 - a) + c[i] * a))

    def get(self, x, y):
        return tuple(self.px[y * self.w + x][:3])

    def rect(self, x, y, w, h, c):
        for yy in range(y, y + h):
            for xx in range(x, x + w):
                self.set(xx, yy, c)

    def frame(self, x, y, w, h, c, t=1):
        self.rect(x, y, w, t, c)
        self.rect(x, y + h - t, w, t, c)
        self.rect(x, y, t, h, c)
        self.rect(x + w - t, y, t, h, c)

    def hline(self, x, y, w, c):
        self.rect(x, y, w, 1, c)

    def vline(self, x, y, h, c):
        self.rect(x, y, 1, h, c)

    def tile(self, x, y, w, h, grid, pal):
        for yy in range(y, y + h):
            for xx in range(x, x + w):
                ch = grid.at(xx % grid.w, yy % grid.h)
                self.set(xx, yy, pal[ch].rgb)

    def blit_grid(self, x, y, grid, pal, mono=False, tint=None):
        for gy in range(grid.h):
            for gx in range(grid.w):
                ch = grid.at(gx, gy)
                if ch == ".":
                    continue
                col = pal[ch]
                if mono:
                    rgb = (255, 255, 255) if col.mono(gx, gy) else (0, 0, 0)
                else:
                    rgb = col.rgb
                if tint:
                    rgb = tuple(rgb[i] * tint[i] // 255 for i in range(3))
                self.set(x + gx, y + gy, rgb)

    def roundrect(self, x, y, w, h, r, fill, border=None):
        """Filled rectangle with corners cut on a circle of radius r."""
        for yy in range(h):
            for xx in range(w):
                inside, edge = _round_test(xx, yy, w, h, r)
                if not inside:
                    continue
                if border is not None and edge:
                    self.set(x + xx, y + yy, border)
                elif fill is not None:
                    self.set(x + xx, y + yy, fill)

    def scaled(self, k):
        out = Canvas(self.w * k, self.h * k)
        for y in range(self.h):
            for x in range(self.w):
                p = self.px[y * self.w + x]
                for yy in range(k):
                    base = (y * k + yy) * out.w + x * k
                    for xx in range(k):
                        out.px[base + xx] = list(p)
        return out

    def paste(self, x, y, other):
        for yy in range(other.h):
            for xx in range(other.w):
                self.set(x + xx, y + yy, other.px[yy * other.w + xx])

    def write_png(self, path):
        write_png(path, self.w, self.h,
                  [tuple(p) for p in self.px])


def _round_test(x, y, w, h, r):
    """Is (x, y) inside a w*h rounded rect, and on its 1px edge?"""
    def inside(px, py):
        if px < 0 or py < 0 or px >= w or py >= h:
            return False
        cx = min(max(px, r), w - 1 - r)
        cy = min(max(py, r), h - 1 - r)
        dx, dy = px - cx, py - cy
        return dx * dx + dy * dy <= r * r + r * 0.6
    if not inside(x, y):
        return False, False
    edge = not (inside(x - 1, y) and inside(x + 1, y) and
                inside(x, y - 1) and inside(x, y + 1))
    return True, edge


# ---------------------------------------------------------------- PNG

def write_png(path, w, h, pixels):
    """pixels: list of (r, g, b, a) tuples, row-major."""
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        for x in range(w):
            raw.extend(pixels[y * w + x][:4])

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xffffffff)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


# ---------------------------------------------------------------- image(6)

def image6_header(chan, w, h):
    return ("%11s %11d %11d %11d %11d " % (chan, 0, 0, w, h)).encode()


def write_image6_argb(path, w, h, pixels):
    """Uncompressed a8r8g8b8 image(6), premultiplied alpha.

    pixels: list of (r, g, b, a). Each pixel is stored as a little-endian
    32-bit word 0xAARRGGBB, so the bytes are b, g, r, a.
    """
    data = bytearray(image6_header("a8r8g8b8", w, h))
    for (r, g, b, a) in pixels:
        data.extend((b * a // 255, g * a // 255, r * a // 255, a))
    with open(path, "wb") as f:
        f.write(data)


def write_image6_rgb(path, w, h, pixels):
    """Uncompressed r8g8b8 image(6): bytes b, g, r per pixel."""
    data = bytearray(image6_header("r8g8b8", w, h))
    for p in pixels:
        data.extend((p[2], p[1], p[0]))
    with open(path, "wb") as f:
        f.write(data)


def _chan_depth(chan):
    return sum(int(n) for n in re.findall(r"[a-z](\d+)", chan))


def read_image6(data, pos=0):
    """Read an image(6) (plain or compressed) from data at pos.

    Returns (chan, (minx, miny, maxx, maxy), rowbytes list, newpos).
    Only the raw bytes are returned; callers interpret the channel.
    """
    compressed = data[pos:pos + 11] == b"compressed\n"
    if compressed:
        pos += 11
    hdr = data[pos:pos + 60].decode().split()
    pos += 60
    chan = hdr[0]
    minx, miny, maxx, maxy = (int(v) for v in hdr[1:5])
    depth = _chan_depth(chan)
    bpl = ((maxx - minx) * depth + 7) // 8
    h = maxy - miny
    if not compressed:
        rows = [data[pos + i * bpl:pos + (i + 1) * bpl] for i in range(h)]
        return chan, (minx, miny, maxx, maxy), rows, pos + h * bpl
    out = bytearray()
    y = miny
    while y < maxy:
        ymax = int(data[pos:pos + 12])
        n = int(data[pos + 12:pos + 24])
        pos += 24
        blk = data[pos:pos + n]
        pos += n
        buf = bytearray()
        i = 0
        while i < len(blk):
            c = blk[i]
            i += 1
            if c & 0x80:
                cnt = (c & 0x7f) + 1
                buf.extend(blk[i:i + cnt])
                i += cnt
            else:
                cnt = (c >> 2) + 3
                off = ((c & 3) << 8 | blk[i]) + 1
                i += 1
                for _ in range(cnt):
                    buf.append(buf[len(buf) - off])
        out.extend(buf)
        y = ymax
    rows = [bytes(out[i * bpl:(i + 1) * bpl]) for i in range(h)]
    return chan, (minx, miny, maxx, maxy), rows, pos


def grey_value(chan, row, x):
    """Coverage 0..1 of pixel x in a k1/k2/k4/k8 row."""
    d = _chan_depth(chan)
    bit = x * d
    b = row[bit // 8]
    shift = 8 - d - (bit % 8)
    v = (b >> shift) & ((1 << d) - 1)
    return v / ((1 << d) - 1)


# ---------------------------------------------------------------- fonts

class Subfont:
    def __init__(self, path):
        data = open(path, "rb").read()
        self.chan, r, self.rows, pos = read_image6(data)
        self.minx = r[0]
        hdr = data[pos:pos + 36].split()
        self.n, self.height, self.ascent = (int(v) for v in hdr)
        pos += 36
        self.info = []
        for i in range(self.n + 1):
            b = data[pos + 6 * i:pos + 6 * i + 6]
            x = b[0] | b[1] << 8
            left = b[4] - 256 if b[4] > 127 else b[4]
            self.info.append((x, b[2], b[3], left, b[5]))


class Font:
    """A 9front bitmap font (.font file and its subfonts)."""

    def __init__(self, path):
        self.dir = os.path.dirname(path)
        lines = open(path).read().split("\n")
        self.height, self.ascent = (int(v) for v in lines[0].split())
        self.ranges = []
        for line in lines[1:]:
            w = line.split()
            if len(w) < 3:
                continue
            lo, hi = int(w[0], 0), int(w[1], 0)
            off, fn = (int(w[2], 0), w[3]) if len(w) == 4 else (0, w[2])
            self.ranges.append((lo, hi, off, fn))
        self.subfonts = {}

    def _sub(self, fn):
        if fn not in self.subfonts:
            p = os.path.join(self.dir, fn)
            if not os.path.exists(p) and os.path.isdir(os.path.dirname(p)):
                # subfont names may carry a size suffix the file lacks
                alt = [f for f in os.listdir(os.path.dirname(p))
                       if f.startswith(os.path.basename(p))]
                p = os.path.join(os.path.dirname(p), alt[0]) if alt else None
            self.subfonts[fn] = Subfont(p) if p and os.path.exists(p) else None
        return self.subfonts[fn]

    def glyph(self, r):
        for lo, hi, off, fn in self.ranges:
            if lo <= r <= hi:
                sf = self._sub(fn)
                i = r - lo + off
                if sf and 0 <= i < sf.n:
                    return sf, i
        return None, None

    def width(self, s):
        w = 0
        for ch in s:
            sf, i = self.glyph(ord(ch))
            if sf:
                w += sf.info[i][4]
        return w

    def draw(self, cv, x, y, s, color):
        """Draw s with the top of the line at y. Returns the end x."""
        for ch in s:
            sf, i = self.glyph(ord(ch))
            if not sf:
                continue
            gx, top, bottom, left, width = sf.info[i]
            gx1 = sf.info[i + 1][0]
            # subfont ascent may differ from the font's
            dy = y + self.ascent - sf.ascent
            for yy in range(top, bottom):
                row = sf.rows[yy]
                for xx in range(gx, gx1):
                    a = grey_value(sf.chan, row, xx - sf.minx)
                    if a > 0:
                        cv.set(x + left + xx - gx, dy + yy,
                               (color[0], color[1], color[2], int(a * 255)))
            x += width
        return x


def find_fontdir(explicit=None):
    """Locate 9front's /lib/font/bit on the host."""
    cands = [explicit, os.environ.get("UXFONTS")]
    root = os.environ.get("H9ROOT")
    if root:
        cands.append(os.path.join(root, "lib", "font", "bit"))
    for c in cands:
        if c and os.path.isdir(c):
            return c
    return None


def load_glyphs(path=None):
    path = path or os.path.join(ASSETS, "glyphs.txt")
    _, blocks = _read_blocks(path)
    return {w[1]: Grid(rows) for w, rows in blocks if w[0] == "glyph"}


def load_fonts(fontdir, path=None):
    """Map role -> Font for the roles in fonts.ndb."""
    path = path or os.path.join(ASSETS, "fonts.ndb")
    fonts = {}
    for e in read_ndb(path):
        if "role" in e:
            fonts[e["role"]] = Font(os.path.join(fontdir, e["font"]))
    return fonts


def full_palette():
    """One dict addressable by colour name and by source letter."""
    byname, bychar = load_palette()
    pal = dict(bychar)
    pal.update(byname)
    return pal
