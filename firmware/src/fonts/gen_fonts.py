#!/usr/bin/env python3
"""
Regenerates the committed fonts/*.c LVGL bitmap fonts from their OFL source faces.

Downloads the source TTFs into a gitignored work dir if missing, instances the
variable Fraunces serif into static display cuts, converts each cut with
`npx lv_font_conv` (4bpp, uncompressed, ASCII + deg/middot/en-dash), then rewrites
the generated include guard to a plain `#include "lvgl.h"`.

    python gen_fonts.py            # regenerate every cut in FONTS
    python gen_fonts.py serif_23   # regenerate just the named cut(s)
"""
import os
import re
import subprocess
import sys
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
WORK = os.path.join(HERE, ".fontwork")          # gitignored download/instance cache

# OFL source faces to fetch (see LICENSES/). Fraunces roman and italic ship as
# separate variable files, so the italic cut is instanced from Fraunces-Italic
# rather than from an 'ital' axis on the roman VF (which has none).
SOURCES = {
    "Fraunces": ("https://github.com/google/fonts/raw/main/ofl/fraunces/"
                 "Fraunces%5BSOFT%2CWONK%2Copsz%2Cwght%5D.ttf"),
    "Fraunces-Italic": ("https://github.com/google/fonts/raw/main/ofl/fraunces/"
                        "Fraunces-Italic%5BSOFT%2CWONK%2Copsz%2Cwght%5D.ttf"),
    "JetBrainsMono": ("https://github.com/JetBrains/JetBrainsMono/raw/master/"
                      "fonts/ttf/JetBrainsMono-Regular.ttf"),
}

# Static Fraunces instances derived from the variable font (default instance is
# Black/9pt, so weight/optical-size axes must be pinned). opsz 72/144 are the two
# display optical sizes; the italic instances come from the separate italic VF.
INSTANCES = {
    "Fraunces-Disp72":  ("Fraunces",        {"opsz": 72,  "wght": 400, "SOFT": 0, "WONK": 0}),
    "Fraunces-Disp144": ("Fraunces",        {"opsz": 144, "wght": 400, "SOFT": 0, "WONK": 0}),
    "Fraunces-Ital40":  ("Fraunces-Italic", {"opsz": 40,  "wght": 400, "SOFT": 0, "WONK": 0}),
}

RANGES = ["0x20-0x7F", "0xB0", "0xB7", "0x2013"]   # default glyph set for text cuts

# Font cuts to generate: symbol -> (pixel size, source face/instance, [glyph range override])
FONTS = {
    # serif (Fraunces) cuts
    "nocsif_serif_30":  (30, "Fraunces-Disp144", ["0x41-0x5A", "0x20"]),  # caps-only boot wordmark
    "nocsif_serif_21":  (21, "Fraunces-Disp72"),
    "nocsif_serif_23":  (23, "Fraunces-Disp72"),
    "nocsif_serif_26":  (26, "Fraunces-Disp72"),
    "nocsif_serif_28":  (28, "Fraunces-Disp72"),
    "nocsif_serif_30f": (30, "Fraunces-Disp72"),   # full charset, unlike the caps-only serif_30
    "nocsif_serif_13i": (13, "Fraunces-Ital40"),
    "nocsif_serif_16i": (16, "Fraunces-Ital40"),
    "nocsif_serif_20i": (20, "Fraunces-Ital40"),
    # mono (JetBrains Mono) cuts
    "nocsif_mono_11":   (11, "JetBrainsMono"),
    "nocsif_mono_12":   (12, "JetBrainsMono"),
    "nocsif_mono_13":   (13, "JetBrainsMono"),
    "nocsif_mono_14":   (14, "JetBrainsMono"),
    "nocsif_mono_15":   (15, "JetBrainsMono"),
    "nocsif_mono_16":   (16, "JetBrainsMono"),
    "nocsif_mono_18":   (18, "JetBrainsMono"),
    "nocsif_mono_20":   (20, "JetBrainsMono"),
    "nocsif_mono_22":   (22, "JetBrainsMono"),
    # numeric-only mono cuts (0-9 and ':') for large clock displays
    "nocsif_num_64":    (64, "JetBrainsMono", ["0x30-0x3A"]),
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
    # Drop axes the source font doesn't have, so pinning a missing axis is a no-op.
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
