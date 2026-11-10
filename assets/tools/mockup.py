#!/usr/bin/env python3
"""Render the 9/UX reference mockups.

This is a small software renderer of the 9/UX chrome and controls, driven
by palette.ndb, metrics.ndb, fonts.ndb and glyphs.txt. rio, libux and
Finder should draw the same pixels; when the two disagree, decide which
is right and fix the other (and the style guide).

usage: mockup.py [-f fontdir] outdir

fontdir is 9front's /lib/font/bit (default $UXFONTS, or $H9ROOT/lib/font/bit).
"""

import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
from uxassets import (Canvas, find_fontdir, full_palette, load_fonts,
                      load_glyphs, load_icons, load_metrics, load_patterns,
                      load_cursors)
import iconsheet


# Themes. "nine" is the style guide; the others are explorations of
# where the look could go. Every colour is still a palette name.
BASE = dict(
    desktop="ninegrey",
    sel="navy",              # object selection: menus, lists, icon labels
    title="rules",           # 'rules' (lines either side) or 'solid'
    tbg="sea", thl="mist", trule="teal", rulestep=3, tfg="ink",
    tbg_off="grey1", tfg_off="grey4",
    boxes="mac",             # 'mac' (close left, zoom right) or 'win'
    frame=0,                 # width of a sizing border, 0 for none
    button="round", bradius=4,
    face="grey1",            # dialog and settings window body
    dialogface="paper", dialogband="teal",
    field="cream", focus="teal",
    scroll="flat", track="grey2",
    corners=False,           # round the screen's corners
)
THEMES = {
    "nine": dict(),
    "nine-butter": dict(tbg="butter", thl="cream", trule="olive"),
    "nine-grey": dict(tbg="grey1", thl="paper", trule="grey3", tbg_off="paper"),
    "seven": dict(desktop="fifty", sel="ink", tbg="paper", thl=None,
                  trule="ink", rulestep=2, tbg_off="paper", tfg_off="grey3",
                  bradius=7, face="paper", dialogband="ink", field="paper",
                  focus=None, track="dither", corners=True),
    "win": dict(desktop="lagoon", title="solid", tbg="navy", thl=None,
                tfg="paper", tbg_off="paper", tfg_off="ink", boxes="win",
                frame=3, button="bevel", face="grey2", dialogface="grey2",
                dialogband="navy", field="paper", focus=None,
                scroll="bevel", track="grey1"),
}


class UX:
    def __init__(self, cv, fontdir, theme="nine"):
        self.cv = cv
        self.t = dict(BASE, **THEMES[theme])
        self.pal = full_palette()
        self.m = load_metrics()
        self.f = load_fonts(fontdir)
        self.g = load_glyphs()
        self.icons = {i.name: i for i in load_icons()}
        self.pats = {p.name: p for p in load_patterns()}

    def c(self, name):
        return self.pal[name].rgb

    # ------------------------------------------------------------ basics

    def glyph(self, name, x, y, color="ink", white="paper"):
        g = self.g[name]
        for gy in range(g.h):
            for gx in range(g.w):
                ch = g.at(gx, gy)
                if ch == "K":
                    self.cv.set(x + gx, y + gy, self.c(color))
                elif ch == "W":
                    self.cv.set(x + gx, y + gy, self.c(white))

    def text(self, role, x, y, s, color="ink"):
        return self.f[role].draw(self.cv, x, y, s, self.c(color))

    def tw(self, role, s):
        return self.f[role].width(s)

    def fh(self, role):
        return self.f[role].height

    def vcenter(self, role, y, h):
        return y + (h - self.fh(role)) // 2

    def icon(self, name, x, y, size=32, selected=False, mono=False, open_=False):
        g = self.icons[name].sizes[size]
        tint = (120, 120, 150) if selected else None
        self.cv.blit_grid(x, y, g, self.pal, mono=mono, tint=tint)
        if open_:
            # an icon whose window is open: hatch it with the dither
            for gy in range(g.h):
                for gx in range(g.w):
                    if g.at(gx, gy) != "." and (gx + gy) % 2 == 0:
                        self.cv.set(x + gx, y + gy, self.c("grey3"))

    def desktop(self, x, y, w, h, pattern=None):
        pattern = pattern or self.t["desktop"]
        self.cv.tile(x, y, w, h, self.pats[pattern].grid, self.pal)

    def bevel(self, x, y, w, h, pressed=False, fill="grey2", outline=True):
        """A raised (or pressed) box with light from the top left."""
        cv = self.cv
        cv.rect(x, y, w, h, self.c(fill))
        o = 1 if outline else 0
        if outline:
            cv.frame(x, y, w, h, self.c("ink"))
        hi, lo = ("grey4", "paper") if pressed else ("paper", "grey4")
        cv.hline(x + o, y + o, w - 2 * o, self.c(hi))
        cv.vline(x + o, y + o, h - 2 * o, self.c(hi))
        cv.hline(x + o, y + h - o - 1, w - 2 * o, self.c(lo))
        cv.vline(x + w - o - 1, y + o, h - 2 * o, self.c(lo))
        if not pressed:
            cv.hline(x + o + 1, y + h - o - 2, w - 2 * o - 2, self.c(lo))
            cv.vline(x + w - o - 2, y + o + 1, h - 2 * o - 2, self.c(lo))

    def corners(self, W, H, r=6):
        cv = self.cv
        for (cx, cy, sx, sy) in ((0, 0, 1, 1), (W - 1, 0, -1, 1),
                                 (0, H - 1, 1, -1), (W - 1, H - 1, -1, -1)):
            for dy in range(r):
                for dx in range(r):
                    if (r - dx - 0.5) ** 2 + (r - dy - 0.5) ** 2 > r * r:
                        cv.set(cx + sx * dx, cy + sy * dy, self.c("ink"))

    # ------------------------------------------------------------ menu bar

    def menubar(self, w, titles, open_index=None, clock="Tue 09:41",
                app=None):
        """titles: list of names; the first is the application menu and
        is drawn in the system font like the rest."""
        m = self.m
        H = m["menubar.height"]
        cv = self.cv
        cv.rect(0, 0, w, H - 1, self.c("paper"))
        cv.hline(0, H - 1, w, self.c("ink"))
        # the 9/UX mark: the system menu
        mx = m["menubar.markx"]
        self.icon("mark", mx, (H - 1 - 16) // 2, 16)
        x = m["menubar.firsttitle"]
        pos = []
        pad = m["menubar.titlepad"]
        for i, t in enumerate(titles):
            tw = self.tw("system", t)
            r = (x, 0, tw + 2 * pad, H - 1)
            pos.append(r)
            if i == open_index:
                cv.rect(*r, self.c(self.t["sel"]))
                self.text("system", x + pad, self.vcenter("system", 0, H - 1), t, "paper")
            else:
                self.text("system", x + pad, self.vcenter("system", 0, H - 1), t)
            x += tw + 2 * pad
        if clock:
            cw = self.tw("system", clock)
            self.text("system", w - m["menubar.clockpad"] - cw,
                      self.vcenter("system", 0, H - 1), clock)
        if self.t["corners"]:
            self.corners(w, self.cv.h)
        return pos

    def menu(self, x, y, items, hilite=None):
        """items: (label, key, flags) with flags a set of 'sep',
        'disabled', 'check', 'sub'. key is the shortcut letter or ''."""
        m = self.m
        cv = self.cv
        ih, sh = m["menu.itemheight"], m["menu.sepheight"]
        lw = max(self.tw("system", it[0]) for it in items if "sep" not in it[2])
        kw = max([self.keywidth(it[1]) for it in items if it[1]] + [0])
        w = m["menu.padleft"] + lw + m["menu.keygap"] + kw + m["menu.padright"]
        h = sum(sh if "sep" in it[2] else ih for it in items) + 2
        s = m["menu.shadow"]
        cv.rect(x + s, y + s, w, h, self.c("ink"))
        cv.rect(x, y, w, h, self.c("paper"))
        cv.frame(x, y, w, h, self.c("ink"))
        yy = y + 1
        for i, (label, key, flags) in enumerate(items):
            if "sep" in flags:
                for xx in range(x + 1, x + w - 1):
                    if xx % 2 == 0:
                        cv.set(xx, yy + sh // 2, self.c("grey3"))
                yy += sh
                continue
            on = (i == hilite and "disabled" not in flags)
            fg = "paper" if on else ("grey3" if "disabled" in flags else "ink")
            if on:
                cv.rect(x + 1, yy, w - 2, ih, self.c(self.t["sel"]))
            ty = self.vcenter("system", yy, ih)
            if "check" in flags:
                self.glyph("check", x + 6, yy + (ih - self.g["check"].h) // 2, fg)
            self.text("system", x + m["menu.padleft"], ty, label, fg)
            if key:
                self.drawkey(x + w - m["menu.padright"] - self.keywidth(key),
                             yy, ih, key, fg)
            if "sub" in flags:
                self.glyph("submenu", x + w - m["menu.padright"],
                           yy + (ih - self.g["submenu"].h) // 2, fg)
            yy += ih
        return (x, y, w, h)

    def _keyparts(self, key):
        """devux.md shortcut syntax -> glyph names and the key's text."""
        parts = key.split("+")
        mods = [p for p in ("ctl", "alt", "shift") if p in parts[:-1]]
        k = parts[-1]
        return mods + ["mod"], (k.upper() if len(k) == 1 else k)

    def keywidth(self, key):
        gl, k = self._keyparts(key)
        return sum(self.g[n].w + 2 for n in gl) + self.tw("system", k)

    def drawkey(self, x, y, h, key, fg):
        gl, k = self._keyparts(key)
        for n in gl:
            g = self.g[n]
            self.glyph(n, x, y + (h - g.h) // 2, fg)
            x += g.w + 2
        self.text("system", x, self.vcenter("system", y, h), k, fg)

    # ------------------------------------------------------------ windows

    def window(self, x, y, w, h, title, active=True, kind="document",
               vscroll=None, hscroll=None, grow=True, body="paper"):
        """Draw a 9/UX window frame. Returns the content rectangle.

        kind: 'document' (title bar, close and zoom) or 'dialog' (no
        title bar; a double frame). body 'face' means the theme's face."""
        t = self.t
        m = self.m
        cv = self.cv
        ink = self.c("ink")
        if body == "face":
            body = t["face"]
        if kind == "dialog":
            body = t["dialogface"]
        sh = m["window.shadow"]
        cv.rect(x + sh, y + sh, w, h, ink)
        cv.rect(x, y, w, h, self.c(body))
        cv.frame(x, y, w, h, ink)
        if kind == "dialog":
            cv.frame(x + 2, y + 2, w - 4, h - 4, self.c(t["dialogband"]), 2)
            cv.frame(x + 4, y + 4, w - 8, h - 8, ink)
            cv.rect(x + 5, y + 5, w - 10, h - 10, self.c(body))
            return (x + 5, y + 5, w - 10, h - 10)
        f = t["frame"]
        if f:
            # a sizing border: grey band between two ink lines
            cv.frame(x + 1, y + 1, w - 2, h - 2, self.c("grey2"), f)
            x, y, w, h = x + f, y + f, w - 2 * f, h - 2 * f
            cv.frame(x, y, w, h, ink)
        th = m["window.titleheight"]
        tb = (x + 1, y + 1, w - 2, th - 2)
        cv.rect(*tb, self.c(t["tbg"] if active else t["tbg_off"]))
        if active and t["thl"]:
            cv.hline(tb[0], tb[1], tb[2], self.c(t["thl"]))
        cv.hline(x, y + th - 1, w, ink)
        tw = self.tw("system", title)
        tx = x + (w - tw) // 2
        ty = self.vcenter("system", y + 1, th - 2)
        bs, bi = m["window.boxsize"], m["window.boxinset"]
        if active and t["title"] == "rules":
            p = m["window.titlepad"]
            l0 = x + 1 + bi + bs + 6
            r1 = x + w - 1 - bi - bs - 6
            for yy in range(y + 5, y + th - 5, t["rulestep"]):
                cv.hline(l0, yy, tx - p - l0, self.c(t["trule"]))
                cv.hline(tx + tw + p, yy, r1 - (tx + tw + p), self.c(t["trule"]))
        if active and t["boxes"] == "mac":
            by = y + (th - bs) // 2
            cv.rect(x + bi, by, bs, bs, self.c(t["tbg"]))
            cv.frame(x + bi, by, bs, bs, ink)
            zx = x + w - bi - bs
            cv.rect(zx, by, bs, bs, self.c(t["tbg"]))
            cv.frame(zx, by, bs, bs, ink)
            cv.frame(zx, by, 6, 6, ink)
        if t["boxes"] == "win":
            # control box at the left, shrink and grow at the right,
            # always shown (they are how the window is driven)
            b = th - 1
            cv.rect(x + 1, y + 1, b - 1, b - 1, self.c("grey2"))
            cv.vline(x + b, y + 1, b - 1, ink)
            cv.rect(x + 5, y + b // 2 - 1, b - 9, 3, self.c("paper"))
            cv.frame(x + 4, y + b // 2 - 2, b - 7, 5, ink)
            for k, up in ((0, True), (1, False)):
                bx = x + w - 1 - (k + 1) * b
                cv.vline(bx - 1, y + 1, b - 1, ink)
                self.bevel(bx, y + 1, b, b - 1, outline=False)
                cx_, cy_ = bx + b // 2, y + b // 2
                for i in range(4):
                    yy = cy_ - 2 + i if up else cy_ + 1 - i
                    cv.hline(cx_ - i, yy, 2 * i + 1, ink)
        self.text("system", tx, ty, title, t["tfg"] if active else t["tfg_off"])
        cx, cy, cw, ch = x + 1, y + th, w - 2, h - th - 1
        sw = m["scroll.width"]
        if vscroll is not None:
            self.scrollbar(x + w - sw, cy - 1, ch + 2 - (sw - 1 if hscroll is not None or grow else 0),
                           True, vscroll, active)
            cw -= sw - 1
        if hscroll is not None:
            self.scrollbar(x, y + h - sw, w - (sw - 1 if vscroll is not None or grow else 0),
                           False, hscroll, active)
            ch -= sw - 1
        if grow and (vscroll is not None or hscroll is not None):
            gx, gy = x + w - sw, y + h - sw
            cv.rect(gx, gy, sw, sw, self.c("grey1"))
            cv.frame(gx, gy, sw, sw, ink)
            if active and not f:
                self.glyph("grow", gx + 2, gy + 2, "grey5")
        return (cx, cy, cw, ch)

    def scrollbar(self, x, y, length, vertical, pos, active=True):
        """pos: thumb position 0..1, or None for nothing to scroll."""
        t = self.t
        cv = self.cv
        sw = self.m["scroll.width"]
        ink = self.c("ink")
        w, h = (sw, length) if vertical else (length, sw)
        live = (active or t["scroll"] == "bevel") and pos is not None
        if live and t["track"] == "dither":
            for yy in range(y, y + h):
                for xx in range(x, x + w):
                    cv.set(xx, yy, self.c("grey3" if (xx + yy) % 2 else "paper"))
        else:
            cv.rect(x, y, w, h, self.c(t["track"]) if live else self.c("paper"))
        cv.frame(x, y, w, h, ink)
        if not active and t["scroll"] != "bevel":
            return
        a1, a2 = ("arrow-up", "arrow-down") if vertical else ("arrow-left", "arrow-right")
        for i, a in enumerate((a1, a2)):
            if vertical:
                bx, by = x, (y if i == 0 else y + h - sw)
            else:
                bx, by = (x if i == 0 else x + w - sw), y
            g = self.g[a]
            if t["scroll"] == "bevel":
                self.bevel(bx, by, sw, sw)
                self.glyph(a, bx + (sw - g.w) // 2, by + (sw - g.h) // 2,
                           "ink" if pos is not None else "grey4",
                           "ink" if pos is not None else "grey4")
                continue
            cv.rect(bx + 1, by + 1, sw - 2, sw - 2, self.c("grey1"))
            cv.frame(bx, by, sw, sw, ink)
            self.glyph(a, bx + (sw - g.w) // 2, by + (sw - g.h) // 2,
                       "ink" if pos is not None else "grey3",
                       "paper" if pos is not None else "grey1")
        if pos is None:
            return
        track = (h if vertical else w) - 2 * sw + 2
        tl = max(self.m["scroll.thumbmin"], track // 4)
        if t["scroll"] == "bevel":
            tl = sw
        tpos = int((track - tl) * pos)
        if vertical:
            tx, ty, tw_, th_ = x, y + sw - 1 + tpos, sw, tl
        else:
            tx, ty, tw_, th_ = x + sw - 1 + tpos, y, tl, sw
        if t["scroll"] == "bevel":
            self.bevel(tx, ty, tw_, th_)
            return
        cv.rect(tx, ty, tw_, th_, self.c("paper"))
        cv.frame(tx, ty, tw_, th_, ink)
        g = self.g["thumbgrip"]
        if vertical:
            self.glyph("thumbgrip", tx + (tw_ - g.w) // 2, ty + (th_ - g.h) // 2, "grey4")
        else:
            for k in range(3):
                cv.vline(tx + tw_ // 2 - 2 + 2 * k, ty + 4, sw - 8, self.c("grey4"))

    def riowindow(self, x, y, w, h, lines, active=False):
        """A legacy rio window, drawn the way stock rio draws it."""
        cv = self.cv
        cv.rect(x, y, w, h, self.c("teal") if active else self.c("sea"))
        cv.rect(x + 4, y + 4, w - 8, h - 8, self.c("paper"))
        # rio's scroll bar is on the left
        cv.rect(x + 4, y + 4, 12, h - 8, self.c("sea"))
        cv.rect(x + 4, y + 4 + 2, 11, (h - 8) // 2, self.c("paper"))
        yy = y + 6
        for ln in lines:
            self.text("mono", x + 20, yy, ln)
            yy += self.fh("mono")
        return yy

    # ------------------------------------------------------------ controls

    def button(self, x, y, label, state="normal", w=None):
        m = self.m
        cv = self.cv
        h = m["button.height"]
        w = w or max(m["button.minwidth"], self.tw("system", label) + 2 * m["button.pad"])
        if self.t["button"] == "bevel":
            return self._bevelbutton(x, y, label, state, w, h)
        r = self.t["bradius"]
        fill, line, fg = "paper", "ink", "ink"
        if state == "pressed":
            fill, fg = self.t["sel"], "paper"
        if state == "disabled":
            line, fg = "grey3", "grey3"
        if state == "default":
            d = m["button.defaultring"]
            cv.roundrect(x - d, y - d, w + 2 * d, h + 2 * d, r + d, None, self.c("ink"))
            cv.roundrect(x - d + 1, y - d + 1, w + 2 * d - 2, h + 2 * d - 2, r + d - 1, None, self.c("ink"))
        cv.roundrect(x, y, w, h, r, self.c(fill), self.c(line))
        self.text("system", x + (w - self.tw("system", label)) // 2,
                  self.vcenter("system", y, h), label, fg)
        return w

    def _bevelbutton(self, x, y, label, state, w, h):
        cv = self.cv
        if state == "default":
            cv.roundrect(x - 1, y - 1, w + 2, h + 2, 2, None, self.c("ink"))
        cv.roundrect(x, y, w, h, 2, self.c("grey2"), self.c("ink"))
        self.bevel(x + 1, y + 1, w - 2, h - 2, pressed=(state == "pressed"),
                   outline=False)
        if state == "default":
            cv.roundrect(x, y, w, h, 2, None, self.c("ink"))
        d = 1 if state == "pressed" else 0
        fg = "grey4" if state == "disabled" else "ink"
        self.text("system", x + (w - self.tw("system", label)) // 2 + d,
                  self.vcenter("system", y, h) + d, label, fg)
        return w

    def checkbox(self, x, y, label, on=False, disabled=False, pressed=False):
        m = self.m
        cv = self.cv
        s = m["check.size"]
        line = "grey3" if disabled else "ink"
        cv.rect(x, y, s, s, self.c("grey2") if pressed else self.c("paper"))
        cv.frame(x, y, s, s, self.c(line))
        if pressed:
            cv.frame(x + 1, y + 1, s - 2, s - 2, self.c(line))
        if on:
            g = self.g["check"]
            self.glyph("check", x + (s - g.w) // 2 + 1, y + (s - g.h) // 2, line)
        fh = self.fh("system")
        self.text("system", x + s + m["check.gap"], y + (s - fh) // 2, label, line)

    def radio(self, x, y, label, on=False, disabled=False):
        m = self.m
        cv = self.cv
        s = m["radio.size"]
        line = "grey3" if disabled else "ink"
        cv.roundrect(x, y, s, s, s // 2, self.c("paper"), self.c(line))
        if on:
            g = self.g["radio"]
            self.glyph("radio", x + (s - g.w) // 2, y + (s - g.h) // 2, line)
        fh = self.fh("system")
        self.text("system", x + s + m["check.gap"], y + (s - fh) // 2, label, line)

    def field(self, x, y, w, text, focused=False, sel=None, caret=None,
              secret=False):
        m = self.m
        cv = self.cv
        h = m["field.height"]
        if focused and self.t["focus"]:
            fr = m["field.focusring"]
            cv.rect(x - fr, y - fr, w + 2 * fr, h + 2 * fr, self.c(self.t["focus"]))
        cv.rect(x, y, w, h, self.c(self.t["field"]))
        cv.frame(x, y, w, h, self.c("ink"))
        if secret:
            text = "•" * len(text)
        tx = x + m["field.pad"]
        ty = self.vcenter("body", y, h)
        if sel:
            a, b = sel
            sx = tx + self.tw("body", text[:a])
            cv.rect(sx, y + 3, self.tw("body", text[a:b]), h - 6, self.c("butter"))
        self.text("body", tx, ty, text)
        if caret is not None:
            cx = tx + self.tw("body", text[:caret])
            cv.vline(cx, y + 3, h - 6, self.c("ink"))

    def label(self, x, y, s, role="body", color="ink"):
        return self.text(role, x, y, s, color)

    def listview(self, x, y, w, rows, selected=None, header=None, icons=None,
                 active=True):
        m = self.m
        cv = self.cv
        rh = m["list.rowheight"]
        yy = y
        if header:
            cv.rect(x, yy, w, rh + 2, self.c("grey1"))
            cx = x + m["list.pad"]
            for col, cw in header:
                self.text("small", cx, self.vcenter("small", yy, rh + 2), col, "grey5")
                cx += cw
            cv.hline(x, yy + rh + 1, w, self.c("grey3"))
            yy += rh + 2
        for i, row in enumerate(rows):
            on = i == selected
            if on:
                cv.rect(x, yy, w, rh, self.c(self.t["sel"] if active else "grey2"))
            fg = "paper" if on and active else "ink"
            cx = x + m["list.pad"]
            cols = row if isinstance(row, (list, tuple)) else [row]
            widths = [c[1] for c in header] if header else [w]
            for j, col in enumerate(cols):
                ox = 0
                if j == 0 and icons:
                    self.icon(icons[i], cx, yy, 16)
                    ox = 20
                self.text("body", cx + ox, self.vcenter("body", yy, rh), col,
                          fg if j == 0 else ("paper" if on and active else "grey5"))
                cx += widths[j] if j < len(widths) else 0
            yy += rh
        return yy

    def iconitem(self, cx, y, name, label, selected=False, plate=False,
                 open_=False, active=True):
        """An icon with its label centred under it; cx is the centre."""
        m = self.m
        cv = self.cv
        self.icon(name, cx - 16, y, 32, selected=selected, open_=open_)
        lw = self.tw("body", label)
        p = m["iconview.labelpad"]
        ly = y + 32 + m["iconview.labelgap"]
        lx = cx - lw // 2
        if selected:
            cv.rect(lx - p, ly, lw + 2 * p, self.fh("body"),
                    self.c(self.t["sel"] if active else "grey2"))
            self.text("body", lx, ly, label, "paper" if active else "ink")
        else:
            if plate:
                cv.rect(lx - p, ly, lw + 2 * p, self.fh("body"), self.c("paper"))
            self.text("body", lx, ly, label)


# ---------------------------------------------------------------- scenes

def scene_desktop(ux, W=800, H=600):
    cv = ux.cv
    m = ux.m
    ux.desktop(0, 0, W, H)
    # desktop icons in the right column
    col = W - m["desktop.margin"] - 38
    y = m["menubar.height"] + 10
    for name, label, sel in (("disk", "plan9", False), ("disk-remote", "sources", False),
                             ("disk-ram", "ram", False)):
        ux.iconitem(col, y, name, label, selected=sel, plate=True,
                    open_=(name == "disk"))
        y += m["desktop.cellh"]
    ux.iconitem(col, H - 60, "trash-full", "Trash", plate=True)
    ux.iconitem(col - 76, H - 60, "folder-home", "glenda", plate=True)

    # a legacy rio window, untouched
    ux.riowindow(24, 380, 400, 190, [
        "% ls /mnt/ux/apps",
        "finder-1  sketch-42",
        "% cat /mnt/ux/apps/sketch-42/name",
        "Sketch",
        "% echo activate > .../controls/save/ctl",
        "% ",
    ])

    # an inactive Finder window in list view
    x, y, w, h = 300, 60, 380, 200
    cx, cy, cw, ch = ux.window(x, y, w, h, "sources", active=False,
                               vscroll=0.0, hscroll=None)
    ux.listview(cx, cy, cw, [("plan9", "folder", "--"), ("contrib", "folder", "--"),
                             ("extra", "folder", "--"), ("README", "text", "2 K"),
                             ("9front.iso", "file", "487 M"), ("motd", "text", "1 K")],
                selected=3, header=[("Name", 170), ("Kind", 100), ("Size", 60)],
                icons=["folder", "folder", "folder", "doc-text", "doc", "doc-text"],
                active=False)

    # the active Finder window in icon view
    x, y, w, h = 40, 110, 430, 250
    cx, cy, cw, ch = ux.window(x, y, w, h, "plan9", active=True,
                               vscroll=0.0, hscroll=0.0)
    cv.hline(cx, cy + 20, cw, ux.c("ink"))
    cv.rect(cx, cy, cw, 20, ux.c("paper"))
    ux.text("small", cx + 8, ux.vcenter("small", cy, 20), "9 items", "grey5")
    s = "4.1 G free"
    ux.text("small", cx + cw - 8 - ux.tw("small", s), ux.vcenter("small", cy, 20), s, "grey5")
    cv.hline(cx, cy + 20, cw, ux.c("ink"))
    cv.hline(cx, cy + 22, cw, ux.c("ink"))
    items = [("folder", "bin"), ("folder", "lib"), ("folder-apps", "apps"),
             ("folder-home", "usr"), ("doc-text", "README"), ("doc-image", "spirit.png"),
             ("doc-script", "startux"), ("app", "Sketch"), ("app-editor", "Notes")]
    cw_, chh = m["iconview.cellw"], m["iconview.cellh"]
    for i, (ic, lab) in enumerate(items):
        r, c = divmod(i, 5)
        ux.iconitem(cx + 44 + c * cw_, cy + 32 + r * chh, ic, lab,
                    selected=(lab == "README"))

    ux.menubar(W, ["Finder", "File", "Edit", "View", "Special"])
    return cv


def scene_app(ux, W=800, H=600):
    cv = ux.cv
    m = ux.m
    ux.desktop(0, 0, W, H)
    col = W - m["desktop.margin"] - 38
    y = m["menubar.height"] + 10
    for name, label in (("disk", "plan9"), ("disk-remote", "sources")):
        ux.iconitem(col, y, name, label, plate=True)
        y += m["desktop.cellh"]
    ux.iconitem(col, H - 60, "trash", "Trash", plate=True)

    # a background document window
    cx, cy, cw, ch = ux.window(330, 60, 360, 300, "untitled.sk", active=False,
                               vscroll=0.0, hscroll=0.0)
    # the app's main window with controls
    x, y, w, h = 110, 230, 390, 300
    cx, cy, cw, ch = ux.window(x, y, w, h, "Sketch Settings", active=True,
                               grow=False, body="face")
    px, py = cx + m["dialog.margin"], cy + m["dialog.margin"]
    ux.label(px, py + 3, "Name:", "system")
    ux.field(px + 64, py, 260, "Sketch of the harbor", focused=True,
             sel=(14, 20), caret=None)
    py += 34
    ux.label(px, py + 3, "Owner:", "system")
    ux.field(px + 64, py, 160, "glenda")
    py += 34
    ux.label(px, py + 3, "Secret:", "system")
    ux.field(px + 64, py, 160, "hunter22", secret=True)
    py += 36
    ux.checkbox(px, py, "Show grid", on=True)
    ux.checkbox(px + 150, py, "Snap to grid", on=False)
    py += 22
    ux.checkbox(px, py, "Autosave", on=True, disabled=True)
    py += 30
    ux.radio(px, py, "Pencil", on=True)
    ux.radio(px + 100, py, "Brush")
    ux.radio(px + 200, py, "Eraser", disabled=True)
    py += 36
    by = cy + ch - m["dialog.margin"] - m["button.height"]
    bw = ux.button(cx + cw - m["dialog.margin"] - 76, by, "Save", "default", w=76)
    ux.button(cx + cw - m["dialog.margin"] - 76 - m["control.gap"] - 8 - 76, by, "Cancel", w=76)
    ux.button(px, by, "Revert", "disabled", w=76)

    pos = ux.menubar(W, ["Sketch", "File", "Edit", "Draw", "Window"], open_index=1)
    ux.menu(pos[1][0], m["menubar.height"] - 1, [
        ("New", "N", set()), ("Open...", "O", set()), ("", "", {"sep"}),
        ("Close", "W", set()), ("Save", "S", set()), ("Save As...", "shift+s", set()),
        ("Revert", "", {"disabled"}), ("", "", {"sep"}),
        ("Export", "", {"sub"}), ("Show Rulers", "R", {"check"}),
        ("", "", {"sep"}), ("Quit", "Q", set()),
    ], hilite=4)
    return cv


def scene_alert(ux, W=440, H=170):
    cv = ux.cv
    m = ux.m
    ux.desktop(0, 0, W, H)
    x, y, w, h = 20, 20, 400, 130
    cx, cy, cw, ch = ux.window(x, y, w, h, "", kind="dialog")
    ux.icon("alert-caution", cx + m["dialog.margin"], cy + m["dialog.margin"])
    tx = cx + m["dialog.textx"]
    ux.text("system", tx, cy + m["dialog.margin"], "Save changes to “untitled.sk”")
    ux.text("system", tx, cy + m["dialog.margin"] + 15, "before closing?")
    ux.text("body", tx, cy + m["dialog.margin"] + 36,
            "Your changes are lost if you don’t save them.", "grey5")
    by = cy + ch - m["dialog.margin"] - m["button.height"]
    ux.button(cx + cw - m["dialog.margin"] - 76, by, "Save", "default", w=76)
    ux.button(cx + cw - m["dialog.margin"] - 76 - 12 - 76, by, "Cancel", w=76)
    ux.button(tx, by, "Don’t Save", w=96)
    return cv


def scene_controls(ux, W=430, H=268):
    cv = ux.cv
    m = ux.m
    cv.rect(0, 0, W, H, ux.c("grey1"))
    x, y = 14, 14
    ux.text("small", x, y, "Buttons: normal, default, pressed, disabled", "grey5")
    y += 18
    xx = x + 4
    for st, lab in (("normal", "Cancel"), ("default", "OK"), ("pressed", "Open"), ("disabled", "Revert")):
        xx += ux.button(xx, y, lab, st, w=76) + 16
    y += 36
    ux.text("small", x, y, "Check boxes and radio buttons", "grey5")
    y += 18
    ux.checkbox(x, y, "Off")
    ux.checkbox(x + 70, y, "On", on=True)
    ux.checkbox(x + 130, y, "Pressed", pressed=True)
    ux.checkbox(x + 220, y, "Disabled", on=True, disabled=True)
    y += 22
    ux.radio(x, y, "Off")
    ux.radio(x + 70, y, "On", on=True)
    ux.radio(x + 220, y, "Disabled", disabled=True)
    y += 28
    ux.text("small", x, y, "Text fields: plain, focused with selection, secret", "grey5")
    y += 18
    ux.field(x, y, 120, "glenda")
    ux.field(x + 136, y, 140, "9front rules", focused=True, sel=(7, 12))
    ux.field(x + 292, y, 110, "password", secret=True)
    y += 34
    ux.text("small", x, y, "Scroll bars: active, nothing to scroll, inactive", "grey5")
    y += 18
    ux.scrollbar(x, y, 160, False, 0.3, True)
    ux.scrollbar(x + 170, y, 110, False, None, True)
    ux.scrollbar(x + 290, y, 110, False, 0.3, False)
    y += 24
    ux.text("small", x, y, "Menu bar title (open) and glyphs", "grey5")
    y += 18
    for i, gname in enumerate(("check", "mod", "submenu", "popup", "grow", "close", "zoom")):
        ux.glyph(gname, x + i * 22, y)
    return cv


def scene_patterns(ux):
    pats = list(ux.pats.values())
    W = 16 + len(pats) * 88
    cv = ux.cv = Canvas(W, 112, ux.c("grey1") + (255,))
    for i, p in enumerate(pats):
        x = 16 + i * 88
        cv.tile(x, 12, 72, 72, p.grid, ux.pal)
        cv.frame(x - 1, 11, 74, 74, ux.c("ink"))
        ux.text("small", x, 90, p.title, "grey5")
    return cv


def scene_cursors(ux):
    curs = load_cursors()
    W = 16 + len(curs) * 64
    cv = ux.cv = Canvas(W, 70, ux.c("grey1") + (255,))
    for i, c in enumerate(curs):
        x = 16 + i * 64
        cv.rect(x, 8, 40, 40, ux.c("grey3"))
        cv.blit_grid(x + 12, 20, c.grid, ux.pal)
        cv.set(x + 12 + c.hot[0], 20 + c.hot[1], ux.c("red"))
        ux.text("small", x, 52, c.name, "grey5")
    return cv


def scene_titlebars(fontdir):
    """Each theme's title bars, active over inactive, side by side."""
    names = list(THEMES)
    W, H = 16 + 236 * len(names), 150
    cv = Canvas(W, H)
    for i, n in enumerate(names):
        ux = UX(cv, fontdir, n)
        x = 8 + i * 236
        ux.desktop(x - 8, 0, 236, H)
        ux.window(x + 6, 34, 210, 50, "Untitled", active=False, grow=False)
        ux.window(x, 12, 210, 70, "plan9", active=True, vscroll=0.0, hscroll=None)
        ux.window(x, 92, 210, 44, "Untitled", active=False, grow=False)
        ux.text("system", x, H - 12, "", "ink")
        lw = ux.tw("small", n)
        cv.rect(x, 140 - 2, lw + 4, 12, ux.c("paper"))
        ux.text("small", x + 2, 138, n, "ink")
    return cv


def main(argv):
    fontdir = None
    if len(argv) > 2 and argv[1] == "-f":
        fontdir = argv[2]
        argv = argv[:1] + argv[3:]
    fontdir = find_fontdir(fontdir)
    if not fontdir or len(argv) != 2:
        sys.exit("usage: mockup.py [-f /lib/font/bit] outdir (or set UXFONTS or H9ROOT)")
    out = argv[1]
    os.makedirs(out, exist_ok=True)
    for name, fn, size, zoom in (("desktop", scene_desktop, (800, 600), 2),
                                 ("app", scene_app, (800, 600), 2),
                                 ("alert", scene_alert, (440, 170), 2),
                                 ("controls", scene_controls, (430, 268), 3)):
        ux = UX(Canvas(*size), fontdir)
        fn(ux, *size)
        ux.cv.write_png(os.path.join(out, name + ".png"))
        ux.cv.scaled(zoom).write_png(os.path.join(out, name + "@%dx.png" % zoom))
    for name, fn in (("patterns", scene_patterns), ("cursors", scene_cursors)):
        ux = UX(Canvas(1, 1), fontdir)
        fn(ux).scaled(2).write_png(os.path.join(out, name + "@2x.png"))
    # explorations: the same scenes in each alternative theme
    tdir = os.path.join(out, "themes")
    os.makedirs(tdir, exist_ok=True)
    for theme in THEMES:
        if theme == "nine":
            continue
        for name, fn, size in (("desktop", scene_desktop, (800, 600)),
                               ("app", scene_app, (800, 600)),
                               ("alert", scene_alert, (440, 170))):
            ux = UX(Canvas(*size), fontdir, theme)
            fn(ux, *size)
            ux.cv.scaled(2).write_png(os.path.join(tdir, "%s-%s@2x.png" % (name, theme)))
    scene_titlebars(fontdir).scaled(2).write_png(os.path.join(tdir, "titlebars@2x.png"))
    ux = UX(Canvas(1, 1), fontdir)
    iconsheet.sheet(load_icons(), ux.pal, ux.f["body"], 2).write_png(
        os.path.join(out, "icons@2x.png"))


if __name__ == "__main__":
    main(sys.argv)
