# Menu icon font (LVGL, UI-shell P3.2)

`nocsif_icons` — one 4bpp LVGL bitmap font whose glyphs are the done-state mockup's line
icons (`docs/design/full-app-mockup.html`, the `<symbol id="i-*">` set). An icon **font**
(not per-image assets) is the design target: a glyph drops into a row as a label, recolors
with the ordinary **text colour** (steel at rest → accent on press), and scales with the
font size — one file, consistent metrics, matching the line-icon weight.

## Why a font from stroked SVGs
Fonts store **filled** outlines; the mockup icons are **strokes** (`fill="none"
stroke="currentColor" stroke-width=W`, round caps/joins). `gen_icons.py` stroke-**expands**
each icon into filled contours — every segment → a quad, every vertex/endpoint → a round
disc (round join/cap), each `fill="currentColor"` sub-element → a filled disc/rect — all
emitted with one winding so TrueType's nonzero fill unions them (rings keep their centre
hole because nothing covers it). fontTools compiles the glyphs to a TTF mapped to a
Private-Use range (U+E000+); `lv_font_conv` rasterizes it to the LVGL font exactly like the
text faces (`fonts/`). This mirrors `img/gen_star.py` but emits vector contours, not a raster.

## Files
| File | What |
|---|---|
| `nocsif_icons.c` | generated 4bpp LVGL font (built into the image; add to `CMakeLists.txt`) |
| `nocsif_icons.h` | `extern const lv_font_t nocsif_icons;` + `NOCSIF_ICON_*` UTF-8 codepoint macros |
| `gen_icons.py` | SVG line-icons → stroke-expand → TTF → `lv_font_conv` (the reproducible source) |
| `gen_icons_preview.py` | render the TTF to `icons_preview.png` for off-device eyeballing |

Icons are the project's own line-icon geometry (transcribed from the repo mockup) — no
third-party font is embedded, so no icon licence file is needed.

## Reproduce
```bash
python gen_icons.py            # writes nocsif_icons.c + nocsif_icons.h
python gen_icons_preview.py    # writes icons_preview.png (contact sheet)
```
Rendered at **22 px** (`FONT_PX`, the mockup row-icon size). To change the icon set or size,
edit `ICONS` / `FONT_PX` and regenerate; the header's `NOCSIF_ICON_*` macros stay in sync.
The generated `#include` block is normalized to a plain `lvgl.h` (matches `ui.c`).
