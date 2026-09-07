# UI image assets (LVGL A8)

Generated alpha-only (A8) LVGL v9.3 image descriptors for the on-watch UI. A8 = 1 byte/px
alpha; the firmware recolors each placement (gray / violet) via `lv_draw_image` recolor, so
one master serves every size and color.

| C symbol | master | Source | Used by |
|---|---|---|---|
| `nocsif_img_star` | 64x64 | mockup `#i-star` path (8 cubic Beziers, 24x24 viewBox, 0.9 stroke) | orrery background (P2), boot wordmark star (P5), System > About |

The engraved 4-point celestial star keeps the mockup's concave hand-drawn outline (spec
§7 — do not substitute a geometric sparkle). Downscaled in-firmware (64 -> 30/26/20/17 px
for the orrery) with antialiasing; recolored per placement.

## Reproduce
`gen_star.py` evaluates the SVG path's cubic Beziers directly (no SVG lib), strokes the
closed outline at 8x supersample with Pillow, box-downsamples to the A8 master, and emits
the LVGL descriptor. `star_preview.png` is an eyeball reference.

```bash
python gen_star.py   # writes nocsif_img_star.c + star_preview.png
```
The generated `#include` is normalized to a plain `lvgl.h` (matches ui.c / the fonts, avoids
the `lvgl/lvgl.h` managed-component path guard).
