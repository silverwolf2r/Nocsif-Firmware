#!/usr/bin/env python3
"""
Regenerate the embedded LVGL bitmap fonts (UI-shell P1/P3.2). See README.md.

Serif = Fraunces (OFL); mono = JetBrains Mono Regular (OFL). Fraunces is variable —
its default instance is Black/9pt, so the display cuts MUST be instanced (axes pinned)
before conversion. Everything is 4bpp, --no-compress (no LV_USE_FONT_COMPRESSED dep),
glyph range ASCII 0x20-0x7F plus the spec glyphs `deg 0xB0 / middot 0xB7 / en-dash 0x2013`.

This is the single reproducible source for the fonts/*.c committed next to it: it fetches
the OFL sources if absent (into a gitignored work dir), instances the serif cuts, runs
`npx lv_font_conv`, and normalizes the generated `#ifdef LV_LVGL_H_INCLUDE_SIMPLE` block
to a plain `#include "lvgl.h"` (matches ui.c; avoids the managed-component path guard).

    python gen_fonts.py            # regenerate every cut in FONTS
    python gen_fonts.py serif_23   # regenerate just the named cut(s)

P3.2 note: the larger cuts (serif 23/26, mono 16/18) + a mono 12 caption cut are the
candidates for the on-device readability tuning loop; the final set is trimmed once the
sizes are locked on-panel (see README).
"""
import os
import re
import subprocess
import sys
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
WORK = os.path.join(HERE, ".fontwork")          # gitignored: TTF sources + instances

# OFL source faces (re-download if missing; see LICENSES/). Fraunces ships roman and
# italic as SEPARATE variable files — the roman VF has NO 'ital' axis (axes are just
# opsz/wght/SOFT/WONK), so the italic cut must come from Fraunces-Italic, not from
# setting ital=1 on the roman VF.
SOURCES = {
    "Fraunces": ("https://github.com/google/fonts/raw/main/ofl/fraunces/"
                 "Fraunces%5BSOFT%2CWONK%2Copsz%2Cwght%5D.ttf"),
    "Fraunces-Italic": ("https://github.com/google/fonts/raw/main/ofl/fraunces/"
                        "Fraunces-Italic%5BSOFT%2CWONK%2Copsz%2Cwght%5D.ttf"),
    "JetBrainsMono": ("https://github.com/JetBrains/JetBrainsMono/raw/master/"
                      "fonts/ttf/JetBrainsMono-Regular.ttf"),
}

# Serif static instances (variable default is Black/9pt — pin the axes). opsz is the
# optical-size axis: display cuts use opsz=72 (144 for the big boot wordmark). The italic
# cut instances the separate Fraunces-Italic VF (the whole file is italic; no ital axis).
INSTANCES = {
    "Fraunces-Disp72":  ("Fraunces",        {"opsz": 72,  "wght": 400, "SOFT": 0, "WONK": 0}),
    "Fraunces-Disp144": ("Fraunces",        {"opsz": 144, "wght": 400, "SOFT": 0, "WONK": 0}),
    "Fraunces-Ital40":  ("Fraunces-Italic", {"opsz": 40,  "wght": 400, "SOFT": 0, "WONK": 0}),
}

RANGES = ["0x20-0x7F", "0xB0", "0xB7", "0x2013"]   # ASCII + deg + middot + en-dash

# symbol -> (px, instance-or-face, [range override])
FONTS = {
    # serif (Fraunces)
    "nocsif_serif_30":  (30, "Fraunces-Disp144", ["0x41-0x5A", "0x20"]),  # boot wordmark: A-Z + space
    "nocsif_serif_21":  (21, "Fraunces-Disp72"),
    "nocsif_serif_23":  (23, "Fraunces-Disp72"),   # P3.2 title candidate
    "nocsif_serif_26":  (26, "Fraunces-Disp72"),   # P3.2 title candidate (large)
    # §4.13 x-large / XXL type-scale steps (operator ask): full-charset title cuts above 26. (serif_30 is
    # the caps-only boot wordmark and cannot carry a title — hence a separate full "30f".)
    "nocsif_serif_28":  (28, "Fraunces-Disp72"),
    "nocsif_serif_30f": (30, "Fraunces-Disp72"),
    "nocsif_serif_13i": (13, "Fraunces-Ital40"),
    "nocsif_serif_16i": (16, "Fraunces-Ital40"),   # P4.6 watchface date (larger italic cut)
    "nocsif_serif_20i": (20, "Fraunces-Ital40"),   # P8 v2.2 peek date (bigger, sits below the time)
    # mono (JetBrains Mono)
    "nocsif_mono_11":   (11, "JetBrainsMono"),
    "nocsif_mono_12":   (12, "JetBrainsMono"),      # P3.2 caption candidate
    "nocsif_mono_13":   (13, "JetBrainsMono"),
    "nocsif_mono_14":   (14, "JetBrainsMono"),
    "nocsif_mono_15":   (15, "JetBrainsMono"),
    "nocsif_mono_16":   (16, "JetBrainsMono"),      # P3.2 row-name candidate
    "nocsif_mono_18":   (18, "JetBrainsMono"),      # P3.2 row-name candidate (large)
    "nocsif_mono_20":   (20, "JetBrainsMono"),      # §4.13 row name — x-large
    "nocsif_mono_22":   (22, "JetBrainsMono"),      # §4.13 row name — XXL
    # P4.6 watchface / P5 boot hero: a big NUMERIC-ONLY cut (0-9 and ':' — the "%H:%M"
    # clock glyphs). Restricted range keeps the 64px face tiny (~12 glyphs). Mono, so the
    # hero time matches the header clock. Do NOT use for text — it has no letters.
    "nocsif_num_64":    (64, "JetBrainsMono", ["0x30-0x3A"]),   # 0123456789 and ':'
    # P8 v2.2 peek hero time — a SMALLER numeric cut so the time no longer dominates the
    # watchface (date sits just below, battery drops to the bottom, middle stays clear for
    # the carousel). Same 0-9 + ':' range as num_64.
    "nocsif_num_48":    (48, "JetBrainsMono", ["0x30-0x3A"]),
}


def ensure_sources():
    os.makedirs(WORK, exist_ok=True)
    for name, url in SOURCES.items():
        dst = os.path.join(WORK, name + ".ttf")
        if not os.path.exists(dst):
            print(f"fetch {name} <- {url}")
            urllib.request.urlretrieve(url, dst)


def ensure_instance(inst_name):
    """Instance a static serif cut from the variable Fraunces (idempotent)."""
    out = os.path.join(WORK, inst_name + ".ttf")
    if os.path.exists(out):
        return out
    src_face, axes = INSTANCES[inst_name][0], INSTANCES[inst_name][1]
    from fontTools.ttLib import TTFont
    from fontTools.varLib.instancer import instantiateVariableFont
    f = TTFont(os.path.join(WORK, src_face + ".ttf"))
    # Italic comes from the separate Fraunces-Italic VF (no 'ital' axis anywhere); keep
    # only axes the source actually has so a pin for a missing axis is a no-op, not a crash.
    have = {ax.axisTag for ax in f["fvar"].axes}
    a = {k: v for k, v in axes.items() if k in have}
    instantiateVariableFont(f, a, inplace=True)
    f.save(out)
    print(f"instanced {inst_name} from {src_face} {a}")
    return out


def face_path(face):
    if face in INSTANCES:
        return ensure_instance(face)
    return os.path.join(WORK, face + ".ttf")


def normalize_include(path):
    """Replace the generated #ifdef LV_LVGL_H_INCLUDE_SIMPLE block with a plain include."""
    with open(path, "r", encoding="utf-8") as fh:
        txt = fh.read()
    txt = re.sub(
        r'#ifdef LV_LVGL_H_INCLUDE_SIMPLE\s*\n#include "lvgl.h"\s*\n'
        r'#else\s*\n#include "lvgl/lvgl.h"\s*\n#endif',
        '#include "lvgl.h"', txt, count=1)
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(txt)


def build(sym):
    px, face = FONTS[sym][0], FONTS[sym][1]
    ranges = FONTS[sym][2] if len(FONTS[sym]) > 2 else RANGES
    src = face_path(face)
    out = os.path.join(HERE, sym + ".c")
    cmd = ["npx", "-y", "lv_font_conv@1.5.3", "--font", src, "--size", str(px),
           "--bpp", "4", "--format", "lvgl", "--no-compress"]
    for r in ranges:
        cmd += ["-r", r]
    cmd += ["--lv-font-name", sym, "-o", out]
    print("conv", sym, f"({px}px, {os.path.basename(src)})")
    subprocess.run(cmd, check=True, shell=(os.name == "nt"))
    normalize_include(out)


def main():
    ensure_sources()
    want = sys.argv[1:] or list(FONTS)
    want = [w if w.startswith("nocsif_") else "nocsif_" + w for w in want]
    for sym in want:
        if sym not in FONTS:
            print(f"skip unknown {sym}")
            continue
        build(sym)
    print("done:", ", ".join(want))


if __name__ == "__main__":
    main()
