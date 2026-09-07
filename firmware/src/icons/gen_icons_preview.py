#!/usr/bin/env python3
"""
Render the built icon TTF (.iconwork/nocsif_icons.ttf) to a PNG contact sheet so the
stroke-expanded glyphs can be eyeballed before they are trusted on-device (UI-shell P3.2).
PIL rasterizes the same outlines lv_font_conv does, at a few sizes, on a dark ground with
the labels — a stand-in for the on-panel look. Run gen_icons.py first.

    python gen_icons_preview.py   # writes icons_preview.png
"""
import os
from PIL import Image, ImageDraw, ImageFont
from gen_icons import ICONS, WORK, HERE

TTF = os.path.join(WORK, "nocsif_icons.ttf")
SIZES = [22, 40]          # row size + a large cut to inspect shape
COLS = 8
CELL = 92
PADTOP = 26


def main():
    n = len(ICONS)
    rows = (n + COLS - 1) // COLS
    W = COLS * CELL
    H = rows * (CELL + PADTOP) + 20
    img = Image.new("RGB", (W, H), (7, 7, 8))          # --void ground
    dr = ImageDraw.Draw(img)
    big = ImageFont.truetype(TTF, 44)
    small = ImageFont.truetype(TTF, 22)
    try:
        lab = ImageFont.truetype("consola.ttf", 12)
    except Exception:
        lab = ImageFont.load_default()
    steel = (120, 120, 127)
    violet = (150, 130, 175)
    for i, (name, _sw, _svg) in enumerate(ICONS):
        cp = 0xE000 + i
        ch = chr(cp)
        cx = (i % COLS) * CELL
        cy = (i // COLS) * (CELL + PADTOP) + 10
        # big glyph (steel) + small glyph (violet) side by side
        dr.text((cx + 8, cy + 6), ch, font=big, fill=steel)
        dr.text((cx + 60, cy + 30), ch, font=small, fill=violet)
        dr.text((cx + 6, cy + CELL - 2), "%02d %s" % (i, name), font=lab, fill=(120, 120, 127))
    out = os.path.join(HERE, "icons_preview.png")
    img.save(out)
    print("wrote", out, img.size)


if __name__ == "__main__":
    main()
