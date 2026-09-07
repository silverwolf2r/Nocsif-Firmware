/*
 * NocSif — UI theme tokens, embedded fonts, and shared styles (UI-shell P1)
 *
 * The single source of truth for the design language in
 * docs/design/ui-design-spec-2026-08-08.md: the §2 grayscale + violet color
 * tokens, the §3 serif-title-over-mono-data font set, and a few reusable
 * lv_style_t keyed to those tokens so a later theme swap (violet -> gold, §12)
 * is one place.
 *
 * COLOR-ORDER (M3, load-bearing): the CO5300 panel runs 24-bit RGB888 with
 * rgb_ele_order = BGR, and the BGR handling lives in the flush config in
 * display.c/ui.c. Define colors NORMALLY with lv_color_hex(0xRRGGBB) — do NOT
 * hand-swap bytes here.
 *
 * FONT SELECTION (M3/spec §3, load-bearing): titles/captions MUST select the
 * serif explicitly (lv_obj_set_style_text_font / a style below). LVGL otherwise
 * renders them in the default Montserrat 14 — the serif-over-mono pairing is the
 * identity and silently collapses if a title falls back.
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- §2 color tokens (sRGB hex; runtime lv_color_t via lv_color_hex) -------- */
#define NOCSIF_VOID      lv_color_hex(0x070708)   /* screen background            */
#define NOCSIF_PIT       lv_color_hex(0x0D0D0F)   /* raised/selected surface      */
#define NOCSIF_PIT_ON    lv_color_hex(0x121215)   /* pressed/active row wash      */
#define NOCSIF_EDGE      lv_color_hex(0x1C1C20)   /* hairlines, dashed header rule*/
#define NOCSIF_EDGE2     lv_color_hex(0x28282D)   /* hover hairline               */
#define NOCSIF_ASH       lv_color_hex(0x42424A)   /* dim text, idle glyphs, chevrons */
#define NOCSIF_STEEL     lv_color_hex(0x78787F)   /* secondary text, icons at rest*/
/* BONE/WHITE muted toward light gray (user pref 2026-08-09: near-white read too bright on the
 * AMOLED; muted twice on request). Spec §2 originals were bone 0xBFBFC4 / white 0xE4E4E8 — tune
 * here in one place. Kept above STEEL 0x78787F so primary/secondary text stays distinct. */
#define NOCSIF_BONE      lv_color_hex(0x8C8C92)   /* primary text (names, clock)  */
#define NOCSIF_WHITE     lv_color_hex(0xA6A6AC)   /* titles, emphasis             */
#define NOCSIF_VIOLET    nocsif_accent()          /* accent — RUNTIME palette (§4.1 Theme; see below)*/
#define NOCSIF_VIOLET_DK nocsif_accent_dk()        /* accent darkened ~30% — dark fill UNDER an accent border; tracks the accent (§4.1) */
#define NOCSIF_GOLD      lv_color_hex(0xC9AD82)   /* secondary accent, distinct from the primary   */

/* ---- §4.1 Theme — runtime accent + type scale (System > Theme / Wallpaper / Font) ---- *
 * The primary accent (NOCSIF_VIOLET above) is now a RUNTIME value: it resolves to nocsif_accent()
 * so a palette swap re-colours every inline accent use the next time a screen is (re)built — the
 * shell rebuilds each screen on navigation, so a change propagates as you move around. This is
 * safe because NOCSIF_VIOLET is only ever used as a runtime argument (lv_color_hex is itself a
 * function, so no use is a compile-time constant). NOCSIF_GOLD stays fixed; NOCSIF_VIOLET_DK now tracks
 * the accent (darkened ~30%, via nocsif_accent_dk) so a pill fill follows its accent-coloured border.
 * Seeded from NVS in nocsif_theme_init. */
#define NOCSIF_ACCENT_N     5     /* palette: violet · gold · steel · teal · amber */
#define NOCSIF_TYPESCALE_N  5     /* compact · default · large · x-large · xxl (§4.13) */

lv_color_t  nocsif_accent(void);                 /* current accent colour (cached; safe before init) */
lv_color_t  nocsif_accent_dk(void);              /* accent darkened ~30% — dark fill under an accent border (NOCSIF_VIOLET_DK) */
uint32_t    nocsif_accent_rgb(void);             /* current accent as 0xRRGGBB                        */
void        nocsif_accent_set_rgb(uint32_t rgb); /* set a custom accent + persist (applies as screens rebuild) */
lv_color_t  nocsif_accent_color(int idx);        /* a preset palette entry (quick-pick / picker chip) */
const char *nocsif_accent_name(int idx);         /* preset name "violet".."amber"                     */

/* Type scale re-points the shared title + row-name styles (and, §4.13, the caption / tag tiers) to
 * bigger/smaller embedded cuts and reports the change, so it reflows every live screen at once.
 * Persisted; seeded in theme_init. */
const char *nocsif_typescale_name(int idx);   /* "compact"/"default"/"large" */
int         nocsif_typescale_idx(void);
void        nocsif_typescale_set(int idx);    /* apply live + persist        */

/* §4.13 — give a hand-built label a font THROUGH the type scale: mono 11 / 12 / 13 map to the font-only
 * tag-small / caption / tag tokens (the label keeps whatever colour it set or inherited — the tokens carry
 * no colour); any other font is set directly. Helpers that take a font pointer route through this so
 * their callers need no change. */
void nocsif_label_font_scaled(lv_obj_t *label, const lv_font_t *font);

/* ---- UTF-8 helper literals for the spec's non-ASCII glyphs (in the fonts) ---- *
 * Use as ADJACENT string literals so a \x escape never over-consumes the next
 * hex/ASCII char: e.g.  "802.11 " NOCSIF_DOT " 2.4 GHz"  and  NOCSIF_NDASH "41". */
#define NOCSIF_DOT   "\xC2\xB7"       /* U+00B7 middle dot ·  */
#define NOCSIF_DEG   "\xC2\xB0"       /* U+00B0 degree °      */
#define NOCSIF_NDASH "\xE2\x80\x93"   /* U+2013 en dash –     */

/* ---- §3 embedded fonts (defined in fonts/*.c, generated 4bpp) --------------- *
 * P3.2 added the larger cuts (serif 23/26, mono 12/16/18) for the bigger, more
 * legible type scale (done-state mockup: title 23, row name ~16, caption 12) plus
 * headroom for the on-device readability tuning loop. Final sizes are selected by
 * pointing the shared styles below at whichever cut reads best on-panel — the styles
 * are the one place to swap, so the size choice never touches widget code. */
extern const lv_font_t nocsif_serif_30;      /* boot wordmark, A-Z (Fraunces 144) */
extern const lv_font_t nocsif_serif_21;      /* screen titles — small cut         */
extern const lv_font_t nocsif_serif_23;      /* screen titles — default (mockup 23)*/
extern const lv_font_t nocsif_serif_26;      /* screen titles — large cut         */
extern const lv_font_t nocsif_serif_28;      /* §4.13 screen titles — x-large     */
extern const lv_font_t nocsif_serif_30f;     /* §4.13 screen titles — XXL (full charset; serif_30 is caps-only) */
extern const lv_font_t nocsif_serif_13i;     /* italic section captions           */
extern const lv_font_t nocsif_serif_16i;     /* watchface date — larger italic (P4.6)*/
extern const lv_font_t nocsif_serif_20i;     /* P8 v2.2 peek date — bigger italic, below the time */
extern const lv_font_t nocsif_mono_11;       /* status tags — small cut           */
extern const lv_font_t nocsif_mono_12;       /* section caption (mockup //)        */
extern const lv_font_t nocsif_mono_13;       /* battery / status tags — default    */
extern const lv_font_t nocsif_mono_14;       /* module names — small cut          */
extern const lv_font_t nocsif_mono_15;       /* clock                             */
extern const lv_font_t nocsif_mono_16;       /* module names — default (mockup 15.5)*/
extern const lv_font_t nocsif_mono_18;       /* module names — large cut          */
extern const lv_font_t nocsif_mono_20;       /* §4.13 module names — x-large      */
extern const lv_font_t nocsif_mono_22;       /* §4.13 module names — XXL          */
extern const lv_font_t nocsif_num_64;        /* watchface/boot HERO time — 0-9 ':' only (P4.6)*/
extern const lv_font_t nocsif_num_48;        /* P8 v2.2 peek hero time — smaller cut, 0-9 ':' only */

/* Menu icon font (icons/nocsif_icons.{c,h}) — one 4bpp face of the mockup's line
 * icons; a glyph is used as label text and recolours via the text colour. The
 * NOCSIF_ICON_* UTF-8 macros name each glyph. */
#include "icons/nocsif_icons.h"

/* ---- §6/§7 shared image asset (img/*.c, generated A8) ----------------------- *
 * Engraved 4-point celestial star, alpha-only 64px master. Recolour per placement
 * (lv_draw_image / lv_obj_set_style_image_recolor). Used by the orrery (P2), the
 * boot wordmark (P5), and System > About. */
extern const lv_image_dsc_t nocsif_img_star;

/* P8 v2.6 — engraved celestial planet icons (A8 alpha, tinted via image_recolor; Home ring heroes).
 * Traced-plate art rasterized with resvg so the evenodd engraving survives (a font glyph can't). */
extern const lv_image_dsc_t nocsif_cel_sun;    /* Life   */
extern const lv_image_dsc_t nocsif_cel_star;   /* System */
extern const lv_image_dsc_t nocsif_cel_moon;   /* Cyber  */
/* P8 v2.8 — the full celestial engraving set (docs/design/icons/celestial), for the planet-icon picker. */
extern const lv_image_dsc_t nocsif_cel_sun_flame;     /* Sun A  */
extern const lv_image_dsc_t nocsif_cel_sun_ray;       /* Sun B  */
extern const lv_image_dsc_t nocsif_cel_moon_full;     /* Moon A */
extern const lv_image_dsc_t nocsif_cel_moon_crescent; /* Moon B */
extern const lv_image_dsc_t nocsif_cel_moon_stars;    /* Moon C */
extern const lv_image_dsc_t nocsif_cel_star_spark;    /* Star A */
extern const lv_image_dsc_t nocsif_cel_star_burst;    /* Star B */

/* ---- shared styles (initialised once by nocsif_theme_init) ------------------ *
 * Keyed to the tokens + fonts above (spec §3 typography roles). Consumers apply
 * them with lv_obj_add_style(obj, &nocsif_style_X, 0). Idempotent init. */
extern lv_style_t nocsif_style_screen;   /* flat void ground, no radius/border/pad */
extern lv_style_t nocsif_style_title;    /* serif 23, white   (§3 screen title)   */
extern lv_style_t nocsif_style_caption;  /* mono 12, steel    (mockup // caption)  */
extern lv_style_t nocsif_style_row;      /* invariant menu-row layout + bottom hairline */
extern lv_style_t nocsif_style_row_press;/* pressed wash (pit-on), STATE_PRESSED selector */
extern lv_style_t nocsif_style_row_icon; /* icon font, steel  (§6 row icon)       */
extern lv_style_t nocsif_style_chevron;  /* icon font, ash    (row drill-in chevron) */
extern lv_style_t nocsif_style_row_name; /* mono 16, bone     (§3 module name)    */
extern lv_style_t nocsif_style_row_tag;  /* mono 13, ash      (§3 status tag)     */
/* §4.13 — FONT-ONLY scale tokens for hand-built labels (no colour). Default = mono 12 / 13 / 11 exactly
 * (the shipped look); Compact / Large move them with the type scale. Prefer nocsif_label_font_scaled(). */
extern lv_style_t nocsif_style_font_caption;    /* mono 11 / 12 / 13 */
extern lv_style_t nocsif_style_font_tag;        /* mono 11 / 13 / 15 */
extern lv_style_t nocsif_style_font_tag_small;  /* mono 11 / 11 / 13 */
extern lv_style_t nocsif_style_clock;    /* mono 15, bone     (§3 clock)          */
extern lv_style_t nocsif_style_battery;  /* mono 13, steel    (§3 battery)        */

/* Initialise the shared styles. Safe to call more than once (first call wins). */
void nocsif_theme_init(void);

#ifdef __cplusplus
}
#endif
