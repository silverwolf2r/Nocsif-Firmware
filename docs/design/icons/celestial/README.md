# Celestial icon set (candidate) — engraved, traced from plates

Hand-drawn woodcut-engraving celestial icons, **vectorized directly from the reference plates**
(`refs/`) rather than redrawn — so the linework matches the source exactly. Proposed to replace
some planet icons in the v2 UI (Home hub motifs + peek-dial planets). **Two choices per motif —
pick one.**

| file | motif | choice |
|------|-------|--------|
| `sun-flame.svg` | organic flame corona, hollow centre, hatched | **Sun A** |
| `sun-ray.svg` | geometric double-layer ray-burst, hollow centre | **Sun B** |
| `moon-full.svg` | engraved full sphere (concentric hatch + highlight) | **Moon A** |
| `moon-crescent.svg` | striated crescent | **Moon B** |
| `moon-stars.svg` | crescent + 3 sparkle-stars completing the circle | **Moon C** |
| `star-spark.svg` | 4-point spark + diagonal spark accents, hatched | **Star A** |
| `star-burst.svg` | 6-point burst, hatched | **Star B** |

Intended slots (once a choice is locked): sun → **Life**, moon → **System**, star → **Cyber**.

`moon-stars.svg` is the hand-built crescent-with-stars from the earlier exploration (not a plate
trace) — clean lightweight paths, already accent-friendly.

## How to reference these (for the build agent)

Use the **repo-relative path on `main`** — that's the unambiguous handle the agent opens and reads.
The friendly A/B/C names are only for humans.

| say | path to hand the agent |
|-----|------------------------|
| Sun A  | `docs/design/icons/celestial/sun-flame.svg` |
| Sun B  | `docs/design/icons/celestial/sun-ray.svg` |
| Moon A | `docs/design/icons/celestial/moon-full.svg` |
| Moon B | `docs/design/icons/celestial/moon-crescent.svg` |
| Moon C | `docs/design/icons/celestial/moon-stars.svg` |
| Star A | `docs/design/icons/celestial/star-spark.svg` |
| Star B | `docs/design/icons/celestial/star-burst.svg` |

Example instruction: *"Set the **Cyber** hub-circle icon to `docs/design/icons/celestial/moon-stars.svg`
(Moon C). Read that SVG, recolor to the theme accent, cut it as a glyph, and wire it into the
hub-motif picker."* The agent must be on latest `main` first (`git pull`).

- **Palette:** white line-art (`#ECEAF2`) on transparent, for the AMOLED black surface. Recolor to a
  single tint / the one accent as needed (e.g. the violet used by the shipped engraved star).
- **Canvas:** square viewBox, transparent, centered — drop-in at any size. Verified 32–170 px;
  fine hatching softens below ~40 px.
- **Preview:** open `gallery.html` (detail + dial/planet scale).
- **Provenance:** `refs/plate1-4pt.png`, `refs/plate2-6pt.png`, `refs/plate3-sunray.png` (the
  originals the user supplied). Regenerate with the design-chat scratchpad `trace.js` (jimp crop +
  potrace vectorize, white-on-black).
- **Status:** design candidates. These are traced outline paths (faithful but heavier than
  hand-built glyphs — `sun-flame.svg` ≈ 68 KB). To bake into firmware: pick one per motif, run
  through SVGO, recolor to `currentColor`/one tint, and cut as an SVG asset or font glyph, then wire
  into the hub-motif picker + the peek-dial launch-by-id registry. A locked choice can also be
  re-drawn as a clean vector glyph if the traced path weight matters.
