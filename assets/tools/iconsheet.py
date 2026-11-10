#!/usr/bin/env python3
"""Render a contact sheet of every icon: colour and 1-bit, 32 and 16 px,
on the desktop and on white, plus selected (darkened) state."""

import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
from uxassets import Canvas, load_icons, full_palette, Font, find_fontdir


def sheet(icons, pal, font=None, zoom=3):
    cellw, cellh = 168, 44
    cv = Canvas(cellw * 2 + 8, cellh * len(icons) + 8, (238, 238, 238, 255))
    for i, ic in enumerate(icons):
        y = 4 + i * cellh
        x = 4
        g32, g16 = ic.sizes.get(32), ic.sizes.get(16)
        # colour on white, colour on desktop grey, 1-bit, selected
        cv.rect(x, y, 40, 40, (255, 255, 255))
        if g32: cv.blit_grid(x + 4, y + 4, g32, pal)
        cv.rect(x + 40, y, 40, 40, pal["grey4"].rgb)
        if g32: cv.blit_grid(x + 44, y + 4, g32, pal)
        cv.rect(x + 80, y, 40, 40, (255, 255, 255))
        if g32: cv.blit_grid(x + 84, y + 4, g32, pal, mono=True)
        cv.rect(x + 120, y, 40, 40, (255, 255, 255))
        if g32: cv.blit_grid(x + 124, y + 4, g32, pal, tint=(120, 120, 140))
        x = 4 + cellw
        cv.rect(x, y, 24, 24, (255, 255, 255))
        if g16: cv.blit_grid(x + 4, y + 4, g16, pal)
        cv.rect(x + 24, y, 24, 24, pal["grey4"].rgb)
        if g16: cv.blit_grid(x + 28, y + 4, g16, pal)
        cv.rect(x + 48, y, 24, 24, (255, 255, 255))
        if g16: cv.blit_grid(x + 52, y + 4, g16, pal, mono=True)
        if font:
            font.draw(cv, x + 78, y + 2, ic.name, (0, 0, 0))
    return cv.scaled(zoom)


if __name__ == "__main__":
    pal = full_palette()
    fd = find_fontdir(sys.argv[2] if len(sys.argv) > 2 else None)
    font = Font(os.path.join(fd, "lucidasans/unicode.7.font")) if fd else None
    sheet(load_icons(), pal, font, 2).write_png(sys.argv[1])
