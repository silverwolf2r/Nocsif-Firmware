/*
 * UI theme: color/font tokens and shared styles (UI-shell P1).
 *
 * This is the single place the design language from
 * docs/design/ui-design-spec-2026-08-08.md lives in code: the section-2 grayscale +
 * violet color tokens, the section-3 serif-title/mono-data font pairing, and a set of
 * reusable lv_style_t objects built from those tokens, so a future palette swap
 * (violet -> gold, section 12) only has to change one place.
 *
 * COLOR ORDER (load-bearing, from M3): the CO5300 panel is wired for 24-bit RGB888 with
 * rgb_ele_order = BGR; that byte-order handling already lives in the flush config in
 * display.c/ui.c. Colors here should always be defined normally via lv_color_hex(0xRRGGBB)
 * — never hand-swap the bytes in this file.
 *
 * FONT SELECTION (load-bearing, M3/spec section 3): every title or caption must set the
 * serif font explicitly, either via lv_obj_set_style_text_font or one of the styles below.
 * Otherwise LVGL falls back to its built-in Montserrat 14, which silently breaks the
 * serif-over-mono look that defines this UI's identity.
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- section 2 color tokens (sRGB hex values, converted at runtime via lv_color_hex) ---- */
#define NOCSIF_VOID      lv_color_hex(0x070708)   /* screen background */
#define NOCSIF_PIT       lv_color_hex(0x0D0D0F)   /* a raised or selected surface */
#define NOCSIF_PIT_ON    lv_color_hex(0x121215)   /* wash shown on a pressed/active row */
#define NOCSIF_EDGE      lv_color_hex(0x1C1C20)   /* hairlines and the dashed header rule */
#define NOCSIF_EDGE2     lv_color_hex(0x28282D)   /* hover hairline */
#define NOCSIF_ASH       lv_color_hex(0x42424A)   /* dimmed text, idle glyphs, chevrons */
#define NOCSIF_STEEL     lv_color_hex(0x78787F)   /* secondary text and icons at rest */
/* BONE/WHITE were muted toward light gray per user feedback on 2026-08-09 — near-white
 * read as too bright on the AMOLED, so both were toned down twice on request. The
 * original spec-2 values were bone 0xBFBFC4 / white 0xE4E4E8; this is the one place to
 * retune them. Kept brighter than STEEL (0x78787F) so primary and secondary text stay
 * visually distinct. */
#define NOCSIF_BONE      lv_color_hex(0x8C8C92)   /* primary text: names, the clock */
#define NOCSIF_WHITE     lv_color_hex(0xA6A6AC)   /* titles and other emphasized text */
#define NOCSIF_VIOLET    nocsif_accent()          /* accent color — resolved at runtime, see the section 4.1 theme block below */
#define NOCSIF_VIOLET_DK nocsif_accent_dk()        /* accent darkened about 30%, used as a dark fill behind an accent-colored border;
 * tracks whatever the current accent is (section 4.1) */
#define NOCSIF_GOLD      lv_color_hex(0xC9AD82)   /* a secondary accent, kept visually distinct from the primary one */

/* ---- section 4.1 theme: runtime accent + type scale (System > Theme / Wallpaper / Font) ---- *
 * NOCSIF_VIOLET above is no longer a compile-time color — it calls nocsif_accent() at runtime,
 * so changing the palette recolors every place that uses it the next time a screen is rebuilt
 * (the shell always rebuilds a screen on navigation, so the new color shows up as you move
 * around). This works because lv_color_hex() is itself a function call, so NOCSIF_VIOLET was
 * never actually a compile-time constant to begin with. NOCSIF_GOLD stays fixed regardless;
 * NOCSIF_VIOLET_DK now also tracks the accent (darkened ~30% via nocsif_accent_dk), so a pill's
 * fill color follows whatever its border's accent color currently is. The saved accent and
 * type scale are both loaded from NVS in nocsif_theme_init. */
#define NOCSIF_ACCENT_N     5     /* the five palette entries: violet, gold, steel, teal, amber */
#define NOCSIF_TYPESCALE_N  5     /* the five type-scale steps: compact, default, large, x-large, xxl (section 4.13) */

lv_color_t  nocsif_accent(void);                 /* the current accent color, cached — safe to call even before theme_init runs */
lv_color_t  nocsif_accent_dk(void);              /* the accent darkened ~30%, used as a dark fill behind an accent-colored border (NOCSIF_VIOLET_DK) */
uint32_t    nocsif_accent_rgb(void);             /* the current accent as a packed 0xRRGGBB value */
void        nocsif_accent_set_rgb(uint32_t rgb); /* pick a custom accent color, persist it, and let it apply as screens rebuild */
lv_color_t  nocsif_accent_color(int idx);        /* one preset palette entry, for a quick-pick swatch or the color picker */
const char *nocsif_accent_name(int idx);         /* preset name, one of "violet" through "amber" */

/* Re-points the shared title and row-name styles — and, as of section 4.13, the caption
 * and tag styles too — at larger or smaller embedded font cuts, and reflows every live
 * screen at once. The chosen scale is persisted and reloaded by theme_init. */
const char *nocsif_typescale_name(int idx);   /* one of "compact", "default", "large", etc. */
int         nocsif_typescale_idx(void);
void        nocsif_typescale_set(int idx);    /* applies the change immediately and persists it */

/* (section 4.13) Applies the type scale to a hand-built label's font: the mono 11/12/13
 * sizes are mapped to the matching font-only token (tag-small/caption/tag), which leaves
 * whatever color the label already has untouched, since those tokens carry no color of
 * their own. Any other font is applied directly. Helper functions that accept a font
 * pointer route through here so their own callers don't need to change. */
void nocsif_label_font_scaled(lv_obj_t *label, const lv_font_t *font);

/* ---- UTF-8 literals for the spec's non-ASCII glyphs (present in the embedded fonts) ---- *
 * Always concatenate these as adjacent string literals, since a \x escape would otherwise
 * swallow the following hex/ASCII character — e.g. "802.11 " NOCSIF_DOT " 2.4 GHz" and
 * NOCSIF_NDASH "41". */
#define NOCSIF_DOT   "\xC2\xB7"       /* U+00B7 middle dot */
#define NOCSIF_DEG   "\xC2\xB0"       /* U+00B0 degree sign */
#define NOCSIF_NDASH "\xE2\x80\x93"   /* U+2013 en dash */

/* ---- section 3 embedded fonts (generated 4bpp bitmaps, defined in fonts/*.c) ---- *
 * P3.2 added the larger cuts (serif 23/26, mono 12/16/18) to reach the bigger, more
 * legible sizes used in the done-state mockup (title 23, row name ~16, caption 12), with
 * some headroom left for on-device readability tuning. Whichever cut ends up looking best
 * on the panel is chosen by pointing the shared styles below at it — the size decision
 * never needs to touch any widget code. */
extern const lv_font_t nocsif_serif_30;      /* boot wordmark, capital letters only (Fraunces 144) */
extern const lv_font_t nocsif_serif_21;      /* screen titles, small cut */
extern const lv_font_t nocsif_serif_23;      /* screen titles, default size (mockup uses 23) */
extern const lv_font_t nocsif_serif_26;      /* screen titles, large cut */
extern const lv_font_t nocsif_serif_28;      /* screen titles, x-large (section 4.13) */
extern const lv_font_t nocsif_serif_30f;     /* screen titles, xxl — full charset, unlike the caps-only serif_30 (section 4.13) */
extern const lv_font_t nocsif_serif_13i;     /* italic section captions */
extern const lv_font_t nocsif_serif_16i;     /* watchface date, a larger italic cut (P4.6) */
extern const lv_font_t nocsif_serif_20i;     /* the peek screen's date (P8 v2.2), bigger italic, shown below the time */
extern const lv_font_t nocsif_mono_11;       /* status tags, small cut */
extern const lv_font_t nocsif_mono_12;       /* the section caption (the mockup's "// ..." line) */
extern const lv_font_t nocsif_mono_13;       /* battery and status tags, default size */
extern const lv_font_t nocsif_mono_14;       /* module names, small cut */
extern const lv_font_t nocsif_mono_15;       /* the clock */
extern const lv_font_t nocsif_mono_16;       /* module names, default size (mockup uses 15.5) */
extern const lv_font_t nocsif_mono_18;       /* module names, large cut */
extern const lv_font_t nocsif_mono_20;       /* module names, x-large (section 4.13) */
extern const lv_font_t nocsif_mono_22;       /* module names, xxl (section 4.13) */
extern const lv_font_t nocsif_num_64;        /* the big watchface/boot-screen time — digits and ':' only (P4.6) */
extern const lv_font_t nocsif_num_48;        /* the peek screen's hero time (P8 v2.2), a smaller cut, digits and ':' only */

/* The menu icon font (icons/nocsif_icons.{c,h}) — a 4bpp bitmap face holding the mockup's
 * line icons. Each glyph is used as ordinary label text, so it recolors with the text
 * color. The NOCSIF_ICON_* macros give each glyph a UTF-8 name. */
#include "icons/nocsif_icons.h"

/* ---- shared image asset from sections 6/7 (generated A8, in img/*.c) ---- *
 * A four-point engraved celestial star, an alpha-only 64px master image. Recolored per
 * placement via lv_draw_image or lv_obj_set_style_image_recolor. Used by the orrery (P2),
 * the boot wordmark (P5), and System > About. */
extern const lv_image_dsc_t nocsif_img_star;

/* (P8 v2.6) engraved celestial planet icons — A8 alpha, tinted via image_recolor, used as
 * the Home screen's ring hero icons. Traced-plate art rasterized with resvg so the
 * even-odd engraving effect survives, which a plain font glyph can't reproduce. */
extern const lv_image_dsc_t nocsif_cel_sun;    /* Life */
extern const lv_image_dsc_t nocsif_cel_star;   /* System */
extern const lv_image_dsc_t nocsif_cel_moon;   /* Cyber */
/* (P8 v2.8) the full celestial engraving set (docs/design/icons/celestial), used by the
 * planet-icon picker. */
extern const lv_image_dsc_t nocsif_cel_sun_flame;     /* Sun A */
extern const lv_image_dsc_t nocsif_cel_sun_ray;       /* Sun B */
extern const lv_image_dsc_t nocsif_cel_moon_full;     /* Moon A */
extern const lv_image_dsc_t nocsif_cel_moon_crescent; /* Moon B */
extern const lv_image_dsc_t nocsif_cel_moon_stars;    /* Moon C */
extern const lv_image_dsc_t nocsif_cel_star_spark;    /* Star A */
extern const lv_image_dsc_t nocsif_cel_star_burst;    /* Star B */

/* ---- shared styles, initialized once by nocsif_theme_init ---- *
 * Built from the tokens and fonts above, matching the typography roles in spec section 3.
 * Apply them with lv_obj_add_style(obj, &nocsif_style_X, 0). Init is safe to call more
 * than once. */
extern lv_style_t nocsif_style_screen;   /* flat void background, no radius, border, or padding */
extern lv_style_t nocsif_style_title;    /* serif 23, white — section 3 screen title */
extern lv_style_t nocsif_style_caption;  /* mono 12, steel — the mockup's "//" caption */
extern lv_style_t nocsif_style_row;      /* the invariant menu-row layout plus its bottom hairline */
extern lv_style_t nocsif_style_row_press;/* the pressed-state wash (pit-on color), applied at LV_STATE_PRESSED */
extern lv_style_t nocsif_style_row_icon; /* icon font, steel — section 6 row icon */
extern lv_style_t nocsif_style_chevron;  /* icon font, ash — the row's drill-in chevron */
extern lv_style_t nocsif_style_row_name; /* mono 16, bone — section 3 module name */
extern lv_style_t nocsif_style_row_tag;  /* mono 13, ash — section 3 status tag */
/* (section 4.13) font-only scale tokens for hand-built labels, carrying no color. The
 * defaults exactly match the shipped look (mono 12/13/11); Compact and Large move these
 * with the rest of the type scale. Prefer nocsif_label_font_scaled() over using these
 * directly. */
extern lv_style_t nocsif_style_font_caption;    /* mono 11 / 12 / 13 across the scale steps */
extern lv_style_t nocsif_style_font_tag;        /* mono 11 / 13 / 15 across the scale steps */
extern lv_style_t nocsif_style_font_tag_small;  /* mono 11 / 11 / 13 across the scale steps */
extern lv_style_t nocsif_style_clock;    /* mono 15, bone — section 3 clock */
extern lv_style_t nocsif_style_battery;  /* mono 13, steel — section 3 battery */

/* Initialize the shared styles. The first call does the work; later calls are no-ops. */
void nocsif_theme_init(void);

#ifdef __cplusplus
}
#endif
