/*
 * UI theme shared styles, implementation (UI-shell P1). See ui_theme.h.
 *
 * Builds the reusable lv_style_t set from the section-2 color tokens and section-3
 * fonts. Kept minimal in P1 — just what the sampler and P3's header/rows need — and
 * grows as later phases add more components. Callers off the LVGL task must hold the
 * LVGL port lock, since style init only touches file-scope structs directly; applying
 * a style to a widget is the caller's job and that's where the lock actually matters. */
#include "ui_theme.h"
#include "settings.h"        /* for accent + type-scale persistence in NVS (section 4.1 theme) */
#include <stdbool.h>
#include <stdint.h>

lv_style_t nocsif_style_screen;
lv_style_t nocsif_style_title;
lv_style_t nocsif_style_caption;
lv_style_t nocsif_style_row;
lv_style_t nocsif_style_row_press;
lv_style_t nocsif_style_row_icon;
lv_style_t nocsif_style_chevron;
lv_style_t nocsif_style_row_name;
lv_style_t nocsif_style_row_tag;
lv_style_t nocsif_style_clock;
lv_style_t nocsif_style_battery;

/* ===== section 4.1 theme: runtime accent palette ===== *
 * lv_color_hex() is a function call, so this can't be a const lv_color_t[] at file scope;
 * instead the raw sRGB values are stored as ints and converted on demand. These are muted,
 * mid-luminance hues chosen for the near-black AMOLED, per the user's preference for nothing
 * too bright or saturated. Entry 0 (violet) is the default. */
static const uint32_t s_accent_rgb[NOCSIF_ACCENT_N] = {
    0x655578,  /* violet, the original default accent */
    0xC9AD82,  /* gold, matching NOCSIF_GOLD */
    0x8A8A93,  /* steel, a neutral near-monochrome option */
    0x4F8A80,  /* teal, a cool-toned option */
    0xB8824A,  /* amber, a warm-toned option */
};
static const char *const s_accent_names[NOCSIF_ACCENT_N] = { "violet", "gold", "steel", "teal", "amber" };

static uint32_t   s_accent_cur = 0x655578u;  /* the active accent as 0xRRGGBB; defaults to violet */
static lv_color_t s_accent_cache;            /* the same accent, pre-converted to lv_color_t for the fast-path getter */
static bool       s_accent_ready;            /* whether the cache above has been seeded yet */

static int theme_clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

lv_color_t nocsif_accent(void)
{
    /* Called roughly 150 times per screen build, so it just returns the cache. Safe to
     * call before theme_init runs (the first screen build always happens afterward),
     * falling back to the default violet if the cache isn't ready yet. */
    return s_accent_ready ? s_accent_cache : lv_color_hex(s_accent_rgb[0]);
}
uint32_t nocsif_accent_rgb(void) { return s_accent_cur; }
lv_color_t nocsif_accent_color(int idx) { return lv_color_hex(s_accent_rgb[theme_clampi(idx, 0, NOCSIF_ACCENT_N - 1)]); }
const char *nocsif_accent_name(int idx) { return s_accent_names[theme_clampi(idx, 0, NOCSIF_ACCENT_N - 1)]; }

/* The dark fill painted behind an accent-colored border — used by primary-action pills,
 * the alarm toggle's off state, the secondary tools button, and the edit-mode remove
 * badge. lv_color_darken(c, lvl) mixes in lvl/255 of black, keeping (255-lvl)/255 of the
 * original color; 179 keeps about 30% of the accent's luminance, which puts the default
 * violet (0x655578) at roughly 0x1E1923 — close to the old fixed NOCSIF_VIOLET_DK value
 * of 0x1E1A26, within a few units per channel. Like nocsif_accent(), this is evaluated at
 * runtime, so every NOCSIF_VIOLET_DK use recolors the next time its screen is rebuilt —
 * a pill's fill now tracks its border's accent color instead of staying fixed violet.
 * White and bone text keep good contrast against the darkened fill for every preset and
 * for a custom accent too. */
lv_color_t nocsif_accent_dk(void) { return lv_color_darken(nocsif_accent(), 179); }

static void accent_apply_rgb(uint32_t rgb)
{
    s_accent_cur   = rgb & 0xFFFFFFu;
    s_accent_cache = lv_color_hex(s_accent_cur);
    s_accent_ready = true;
}
void nocsif_accent_set_rgb(uint32_t rgb)
{
    accent_apply_rgb(rgb);
    nocsif_settings_set_i32("ui.accentc", (int32_t)s_accent_cur);
    /* Inline accent references are resolved when a screen is built, so they pick up the new
     * color the next time that screen is rebuilt. No global rebuild happens here — screens
     * are rebuilt naturally as the shell navigates around. */
}

/* ===== section 4.1 theme: type scale ===== *
 * Re-points the two most visible type roles — screen title and menu-row name — to larger
 * or smaller embedded font cuts. The serif font only goes up to size 26 (the 30 cut is
 * caps-only boot art), so Default and Large share the same 26-size title and differ only
 * in the row-name cut; Default matches the shipped look exactly. */
/* five scale steps (section 4.13 added x-large and xxl): compact, default, large, x-large, xxl */
static const lv_font_t *const s_ts_title[NOCSIF_TYPESCALE_N] = { &nocsif_serif_23, &nocsif_serif_26, &nocsif_serif_26, &nocsif_serif_28, &nocsif_serif_30f };
static const lv_font_t *const s_ts_name[NOCSIF_TYPESCALE_N]  = { &nocsif_mono_14,  &nocsif_mono_16,  &nocsif_mono_18,  &nocsif_mono_20,  &nocsif_mono_22   };
/* (section 4.13) the caption and tag tiers scale too; Default matches the shipped 12/13/11
 * sizes exactly. 11 is the smallest embedded cut, so Compact keeps captions and tags at
 * 11 and only the larger steps actually grow them. */
static const lv_font_t *const s_ts_cap[NOCSIF_TYPESCALE_N]   = { &nocsif_mono_11,  &nocsif_mono_12,  &nocsif_mono_13,  &nocsif_mono_14,  &nocsif_mono_15   };
static const lv_font_t *const s_ts_tag[NOCSIF_TYPESCALE_N]   = { &nocsif_mono_11,  &nocsif_mono_13,  &nocsif_mono_15,  &nocsif_mono_16,  &nocsif_mono_18   };
static const lv_font_t *const s_ts_tagsm[NOCSIF_TYPESCALE_N] = { &nocsif_mono_11,  &nocsif_mono_11,  &nocsif_mono_13,  &nocsif_mono_14,  &nocsif_mono_15   };
static const char *const s_ts_names[NOCSIF_TYPESCALE_N]      = { "compact", "default", "large", "x-large", "xxl" };
static int s_ts_i = 1;   /* start at the default step */

lv_style_t nocsif_style_font_caption;      /* (section 4.13) font-only tokens — see ui_theme.h for details */
lv_style_t nocsif_style_font_tag;
lv_style_t nocsif_style_font_tag_small;

static void typescale_apply(int idx, bool live)
{
    s_ts_i = theme_clampi(idx, 0, NOCSIF_TYPESCALE_N - 1);
    lv_style_set_text_font(&nocsif_style_title,          s_ts_title[s_ts_i]);
    lv_style_set_text_font(&nocsif_style_row_name,       s_ts_name[s_ts_i]);
    lv_style_set_text_font(&nocsif_style_caption,        s_ts_cap[s_ts_i]);     /* used for scaffold section captions */
    lv_style_set_text_font(&nocsif_style_row_tag,        s_ts_tag[s_ts_i]);     /* used for menu-row status tags */
    lv_style_set_text_font(&nocsif_style_font_caption,   s_ts_cap[s_ts_i]);     /* used for hand-built labels via nocsif_label_font_scaled */
    lv_style_set_text_font(&nocsif_style_font_tag,       s_ts_tag[s_ts_i]);
    lv_style_set_text_font(&nocsif_style_font_tag_small, s_ts_tagsm[s_ts_i]);
    if (live) {
        /* Reflow every widget currently using these shared styles. LVGL task only. */
        lv_obj_report_style_change(&nocsif_style_title);
        lv_obj_report_style_change(&nocsif_style_row_name);
        lv_obj_report_style_change(&nocsif_style_caption);
        lv_obj_report_style_change(&nocsif_style_row_tag);
        lv_obj_report_style_change(&nocsif_style_font_caption);
        lv_obj_report_style_change(&nocsif_style_font_tag);
        lv_obj_report_style_change(&nocsif_style_font_tag_small);
    }
}

void nocsif_label_font_scaled(lv_obj_t *label, const lv_font_t *font)
{
    if (label == NULL) return;
    if (font == &nocsif_mono_12)      lv_obj_add_style(label, &nocsif_style_font_caption, 0);
    else if (font == &nocsif_mono_13) lv_obj_add_style(label, &nocsif_style_font_tag, 0);
    else if (font == &nocsif_mono_11) lv_obj_add_style(label, &nocsif_style_font_tag_small, 0);
    else                              lv_obj_set_style_text_font(label, font, 0);
}
const char *nocsif_typescale_name(int idx) { return s_ts_names[theme_clampi(idx, 0, NOCSIF_TYPESCALE_N - 1)]; }
int nocsif_typescale_idx(void) { return s_ts_i; }
void nocsif_typescale_set(int idx)
{
    typescale_apply(idx, true);
    nocsif_settings_set_i32("ui.typescale", s_ts_i);
}

void nocsif_theme_init(void)
{
    static bool inited;
    if (inited) {
        return;
    }
    inited = true;

    /* A flat near-black background: no radius, border, or padding. In P1 every screen is
     * opaque; P2 makes screen backgrounds transparent instead so the orrery layer behind
     * shows through, at which point a consumer can override bg_opa as needed. */
    lv_style_init(&nocsif_style_screen);
    lv_style_set_bg_color(&nocsif_style_screen, NOCSIF_VOID);
    lv_style_set_bg_opa(&nocsif_style_screen, LV_OPA_COVER);
    lv_style_set_border_width(&nocsif_style_screen, 0);
    lv_style_set_radius(&nocsif_style_screen, 0);
    lv_style_set_pad_all(&nocsif_style_screen, 0);

    /* Sizes were bumped up to match the done-state mockup / P3.2 targets, since P1's
     * mockup-matched sizes read too small on the actual panel. Each role points at one
     * generated font cut; to retune a size on-device, change only the font handle here —
     * no widget code ever picks a size directly. Other cuts available: serif 21/23/26,
     * mono 11/13/14/16/18. */
    lv_style_init(&nocsif_style_title);
    lv_style_set_text_font(&nocsif_style_title, &nocsif_serif_26);   /* the title size is fixed at 26 on-device */
    lv_style_set_text_color(&nocsif_style_title, NOCSIF_WHITE);

    /* The section caption: the mockup's mono "// ..." line (this was serif-italic in P1).
     * Steel by default; callers color a leading "//" violet themselves (spec section 6 /
     * mockup .sub b). */
    lv_style_init(&nocsif_style_caption);
    lv_style_set_text_font(&nocsif_style_caption, &nocsif_mono_12);
    lv_style_set_text_color(&nocsif_style_caption, NOCSIF_STEEL);

    /* Bundles the fixed menu-row layout plus its faint bottom hairline into one shared
     * style, so building a row costs two add_style calls instead of roughly 15 individual
     * lv_obj_set_style_* calls. Each set_style call refreshes the object, and ~15 calls per
     * row across ~490 rows was slow enough to trip the task watchdog while the whole menu
     * built; a shared style only needs to refresh once per row. */
    lv_style_init(&nocsif_style_row);
    lv_style_set_layout(&nocsif_style_row, LV_LAYOUT_FLEX);
    lv_style_set_flex_flow(&nocsif_style_row, LV_FLEX_FLOW_ROW);
    lv_style_set_flex_main_place(&nocsif_style_row, LV_FLEX_ALIGN_START);
    lv_style_set_flex_cross_place(&nocsif_style_row, LV_FLEX_ALIGN_CENTER);
    lv_style_set_flex_track_place(&nocsif_style_row, LV_FLEX_ALIGN_CENTER);
    lv_style_set_width(&nocsif_style_row, lv_pct(100));
    lv_style_set_height(&nocsif_style_row, LV_SIZE_CONTENT);
    lv_style_set_min_height(&nocsif_style_row, 44);          /* the minimum tap target size (spec section 4) */
    lv_style_set_pad_top(&nocsif_style_row, 15);
    lv_style_set_pad_bottom(&nocsif_style_row, 15);
    lv_style_set_pad_left(&nocsif_style_row, 2);
    lv_style_set_pad_right(&nocsif_style_row, 2);
    lv_style_set_pad_column(&nocsif_style_row, 16);          /* the gap between a row's icon and its name (spec section 4) */
    lv_style_set_border_color(&nocsif_style_row, lv_color_hex(0x101013));  /* matches the mockup's row divider color */
    lv_style_set_border_opa(&nocsif_style_row, LV_OPA_COVER);
    lv_style_set_border_width(&nocsif_style_row, 1);
    lv_style_set_border_side(&nocsif_style_row, LV_BORDER_SIDE_BOTTOM);

    lv_style_init(&nocsif_style_row_press);                  /* applied only in the LV_STATE_PRESSED state */
    lv_style_set_bg_color(&nocsif_style_row_press, NOCSIF_PIT_ON);
    lv_style_set_bg_opa(&nocsif_style_row_press, LV_OPA_COVER);
    lv_style_set_radius(&nocsif_style_row_press, 4);

    /* Row icon: uses the menu icon font, colored steel. The mockup's accent-tinted icon is a
     * hover-only rule, but a touch panel has no hover state, so the icon stays steel and
     * the row's pit-on wash is the only press feedback. (A real press-accent would need its
     * own row event, since child labels don't inherit the row's LV_STATE_PRESSED — left as
     * a deliberate non-goal.) */
    lv_style_init(&nocsif_style_row_icon);
    lv_style_set_text_font(&nocsif_style_row_icon, &nocsif_icons);
    lv_style_set_text_color(&nocsif_style_row_icon, NOCSIF_STEEL);

    /* Row drill-in chevron: icon font, colored ash, using the same 22px cut as everything
     * else (matching the mockup's 14px is a follow-up task). */
    lv_style_init(&nocsif_style_chevron);
    lv_style_set_text_font(&nocsif_style_chevron, &nocsif_icons);
    lv_style_set_text_color(&nocsif_style_chevron, NOCSIF_ASH);

    lv_style_init(&nocsif_style_row_name);
    lv_style_set_text_font(&nocsif_style_row_name, &nocsif_mono_16);   /* matches the mockup's 15.5px name size */
    lv_style_set_text_color(&nocsif_style_row_name, NOCSIF_BONE);

    lv_style_init(&nocsif_style_row_tag);
    lv_style_set_text_font(&nocsif_style_row_tag, &nocsif_mono_13);
    lv_style_set_text_color(&nocsif_style_row_tag, NOCSIF_ASH);

    /* (section 4.13) font-only scale tokens carrying no color; these are the Default sizes,
     * and typescale_apply re-points them when the scale changes. */
    lv_style_init(&nocsif_style_font_caption);
    lv_style_set_text_font(&nocsif_style_font_caption, &nocsif_mono_12);
    lv_style_init(&nocsif_style_font_tag);
    lv_style_set_text_font(&nocsif_style_font_tag, &nocsif_mono_13);
    lv_style_init(&nocsif_style_font_tag_small);
    lv_style_set_text_font(&nocsif_style_font_tag_small, &nocsif_mono_11);

    lv_style_init(&nocsif_style_clock);
    lv_style_set_text_font(&nocsif_style_clock, &nocsif_mono_15);
    lv_style_set_text_color(&nocsif_style_clock, NOCSIF_BONE);

    lv_style_init(&nocsif_style_battery);
    lv_style_set_text_font(&nocsif_style_battery, &nocsif_mono_13);
    lv_style_set_text_color(&nocsif_style_battery, NOCSIF_STEEL);

    /* (section 4.1) Load the saved accent and type scale from NVS. The accent call only
     * fills the cache, since inline accent uses read it lazily at build time. The type
     * scale call re-points the styles just initialized above; it isn't "live" here since
     * no widgets exist yet — the first screen build simply picks up whatever cut is set. */
    accent_apply_rgb((uint32_t)nocsif_settings_get_i32("ui.accentc", 0x655578));
    typescale_apply(nocsif_settings_get_i32("ui.typescale", 1), false);
}
