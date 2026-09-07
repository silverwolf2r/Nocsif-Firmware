# Embedded UI fonts (M-UI-shell P1)

LVGL v9 bitmap fonts for the on-watch UI (design spec §3). Generated with `lv_font_conv` 1.5.3 at
**4 bpp**, `--no-compress` (so no dependency on `LV_USE_FONT_COMPRESSED`). Serif titles over mono data is
the identity — titles/captions must select the serif explicitly or LVGL falls back to Montserrat.

## Why these faces (license-driven substitution)
The design spec named **Recia / Boska** (Fontshare). Their Fontshare **Free Font EULA** forbids extracting
the font, restricts derivative works, and limits embedding to read-only PDFs — so rasterizing them into an
LVGL bitmap committed to this repo (slated to go public later) is **not permitted**. Georgia / Times /
Consolas (Windows) are proprietary too. We therefore ship **SIL OFL 1.1** faces, which explicitly permit
bitmap conversion, embedding, modification, and redistribution bundled with software:

| Role | Face | License | Source |
|---|---|---|---|
| Serif (titles/wordmark/captions) | **Fraunces** — old-style, high-contrast, editorial (antique character; opsz axis stays legible small) | OFL 1.1 | `github.com/google/fonts` `ofl/fraunces` |
| Mono (all data) | **JetBrains Mono** Regular | OFL 1.1 | `github.com/JetBrains/JetBrainsMono` v2.304 |

**Fraunces was chosen on-panel over Playfair Display** (the P1 A/B, both OFL) and the Playfair cut was dropped;
its `nocsif_serif_21_alt.c` and OFL text are removed. The reproduce step below still shows how to regenerate an
alternate for a future re-evaluation.

License texts are in `LICENSES/`. Fraunces & Playfair are variable; static cuts were instantiated with
`fonttools varLib.instancer` before conversion (their variable default is Black/9 pt, so axes **must** be
pinned). Panel is ~314 ppi, so display optical sizes anti-alias cleanly at 4 bpp even at 13 px.

## Generated fonts
| C symbol | px | Face / static instance | Glyph set |
|---|---|---|---|
| `nocsif_serif_30` | 30 | Fraunces opsz=144 wght=400 | A–Z + space (boot wordmark only) |
| `nocsif_serif_21` | 21 | Fraunces opsz=72 wght=400 | ASCII 0x20–0x7F + `° · –` |
| `nocsif_serif_13i` | 13 | Fraunces **Italic** opsz=40 wght=400 | ASCII 0x20–0x7F + `° · –` |
| `nocsif_mono_11/13/14/15` | 11/13/14/15 | JetBrains Mono Regular | ASCII 0x20–0x7F + `° · –` |

`✦` (U+2726) is intentionally absent — the engraved star is an image asset (P2), not a text glyph.

## Reproduce
```bash
# 1. static serif cuts (variable default is Black/9pt — pin the axes)
python -c "from fontTools.ttLib import TTFont; from fontTools.varLib.instancer import instantiateVariableFont as I; \
  f=TTFont('Fraunces[SOFT,WONK,opsz,wght].ttf'); I(f,{'opsz':72,'wght':400,'SOFT':0,'WONK':0},inplace=True); f.save('Fraunces-Disp72.ttf')"
# 2. convert (repeat per size; --no-compress; symbols via codepoints 0xB0 ° 0xB7 · 0x2013 –)
npx lv_font_conv --font Fraunces-Disp72.ttf --size 21 --bpp 4 --format lvgl --no-compress \
  -r 0x20-0x7F -r 0xB0 -r 0xB7 -r 0x2013 --lv-font-name nocsif_serif_21 -o nocsif_serif_21.c
```
The generated `#ifdef LV_LVGL_H_INCLUDE_SIMPLE …` include block is normalized to a plain `#include "lvgl.h"`
(matches `ui.c`; avoids the `lvgl/lvgl.h` path guard under the managed component).
