/*
 * NocSif — UI theme shared styles (UI-shell P1). See ui_theme.h.
 *
 * Initialises the reusable lv_style_t set keyed to the §2 tokens + §3 fonts.
 * Kept deliberately small in P1 (the styles the sampler + P3's header/rows need);
 * grows as later phases add components. Must run under the LVGL port lock if the
 * caller is off the LVGL task, though style init itself only touches file-scope
 * structs — the styles are applied to widgets by the caller, which holds the lock.
 */
#include "ui_theme.h"
#include "settings.h"        /* §4.1 Theme: accent + type-scale persistence (NVS) */
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

/* ===== §4.1 Theme — runtime accent palette ================================= *
 * lv_color_hex() is a function, so the palette can't be a const lv_color_t[] at file scope;
 * the sRGB values live as ints and convert on demand. Muted, mid-luminance hues tuned for the
 * near-black AMOLED (user pref: nothing too bright/saturated). Index 0 (violet) is the default. */
static const uint32_t s_accent_rgb[NOCSIF_ACCENT_N] = {
    0x655578,  /* violet — the original accent           */
    0xC9AD82,  /* gold   — matches NOCSIF_GOLD           */
    0x8A8A93,  /* steel  — neutral / near-monochrome     */
    0x4F8A80,  /* teal   — cool                          */
    0xB8824A,  /* amber  — warm                          */
};
static const char *const s_accent_names[NOCSIF_ACCENT_N] = { "violet", "gold", "steel", "teal", "amber" };

static uint32_t   s_accent_cur = 0x655578u;  /* current accent, 0xRRGGBB (default violet)   */
static lv_color_t s_accent_cache;            /* cached lv_color_t for the fast getter        */
static bool       s_accent_ready;            /* cache seeded yet?                            */

static int theme_clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

lv_color_t nocsif_accent(void)
{
    /* Called ~150x per screen build; returns the cache. Safe before theme_init seeds it
     * (the first build runs after theme_init) — falls back to the default violet. */
    return s_accent_ready ? s_accent_cache : lv_color_hex(s_accent_rgb[0]);
}
uint32_t nocsif_accent_rgb(void) { return s_accent_cur; }
lv_color_t nocsif_accent_color(int idx) { return lv_color_hex(s_accent_rgb[theme_clampi(idx, 0, NOCSIF_ACCENT_N - 1)]); }
const char *nocsif_accent_name(int idx) { return s_accent_names[theme_clampi(idx, 0, NOCSIF_ACCENT_N - 1)]; }

/* §4.1 Theme — the dark "fill" tint painted UNDER an accent-coloured border (primary-action pills, the
 * alarm-toggle off-state, secondary tools_btn, the edit-mode remove badge). lv_color_darken(c,lvl) mixes
 * lvl/255 black, i.e. keeps (255-lvl)/255 of c; 179 keeps ~30% of the accent luminance, which lands the
 * default violet 0x655578 at ~0x1E1923 — the legacy fixed NOCSIF_VIOLET_DK (0x1E1A26), within a few codes
 * per channel. Runtime like nocsif_accent(), so every NOCSIF_VIOLET_DK use re-colours on the next screen
 * (re)build — a pill fill now follows its accent border instead of staying violet. White/bone text keeps
 * strong contrast on the darkened fill for every preset and for a custom accent. */
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
    /* Inline accent uses are baked at build time; they re-colour when each screen is next
     * (re)built. No global rebuild here — the shell rebuilds screens on navigation. */
}

/* ===== §4.1 Theme — type scale ============================================= *
 * Re-points the two dominant type roles (screen title, menu-row name) to bigger/smaller embedded
 * cuts. The serif tops out at 26 (the 30 cut is caps-only boot art), so Default and Large share
 * the 26 title and differ in the row cut — Default equals the shipped look exactly. */
/* Five steps (§4.13 added x-large + XXL):      compact          default          large            x-large          xxl */
static const lv_font_t *const s_ts_title[NOCSIF_TYPESCALE_N] = { &nocsif_serif_23, &nocsif_serif_26, &nocsif_serif_26, &nocsif_serif_28, &nocsif_serif_30f };
static const lv_font_t *const s_ts_name[NOCSIF_TYPESCALE_N]  = { &nocsif_mono_14,  &nocsif_mono_16,  &nocsif_mono_18,  &nocsif_mono_20,  &nocsif_mono_22   };
/* §4.13 — the caption / tag tiers follow too (Default = the shipped 12 / 13 / 11 exactly). The 11 cut is
 * the smallest we embed, so Compact keeps tags + captions at 11 and only the bigger steps grow them. */
static const lv_font_t *const s_ts_cap[NOCSIF_TYPESCALE_N]   = { &nocsif_mono_11,  &nocsif_mono_12,  &nocsif_mono_13,  &nocsif_mono_14,  &nocsif_mono_15   };
static const lv_font_t *const s_ts_tag[NOCSIF_TYPESCALE_N]   = { &nocsif_mono_11,  &nocsif_mono_13,  &nocsif_mono_15,  &nocsif_mono_16,  &nocsif_mono_18   };
static const lv_font_t *const s_ts_tagsm[NOCSIF_TYPESCALE_N] = { &nocsif_mono_11,  &nocsif_mono_11,  &nocsif_mono_13,  &nocsif_mono_14,  &nocsif_mono_15   };
static const char *const s_ts_names[NOCSIF_TYPESCALE_N]      = { "compact", "default", "large", "x-large", "xxl" };
static int s_ts_i = 1;   /* default */

lv_style_t nocsif_style_font_caption;      /* §4.13 font-only tokens (see ui_theme.h) */
lv_style_t nocsif_style_font_tag;
lv_style_t nocsif_style_font_tag_small;

static void typescale_apply(int idx, bool live)
{
    s_ts_i = theme_clampi(idx, 0, NOCSIF_TYPESCALE_N - 1);
    lv_style_set_text_font(&nocsif_style_title,          s_ts_title[s_ts_i]);
    lv_style_set_text_font(&nocsif_style_row_name,       s_ts_name[s_ts_i]);
    lv_style_set_text_font(&nocsif_style_caption,        s_ts_cap[s_ts_i]);     /* scaffold captions      */
    lv_style_set_text_font(&nocsif_style_row_tag,        s_ts_tag[s_ts_i]);     /* menu-row status tags   */
    lv_style_set_text_font(&nocsif_style_font_caption,   s_ts_cap[s_ts_i]);     /* hand-built labels ...  */
    lv_style_set_text_font(&nocsif_style_font_tag,       s_ts_tag[s_ts_i]);
    lv_style_set_text_font(&nocsif_style_font_tag_small, s_ts_tagsm[s_ts_i]);
    if (live) {
        /* Reflow every widget currently using these shared styles (LVGL task only). */
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

    /* Flat near-black ground: no radius, no border, no padding. Screens are
     * opaque void in P1; P2 flips screen backgrounds transparent so the orrery
     * bottom layer shows through — consumers can override bg_opa then. */
    lv_style_init(&nocsif_style_screen);
    lv_style_set_bg_color(&nocsif_style_screen, NOCSIF_VOID);
    lv_style_set_bg_opa(&nocsif_style_screen, LV_OPA_COVER);
    lv_style_set_border_width(&nocsif_style_screen, 0);
    lv_style_set_radius(&nocsif_style_screen, 0);
    lv_style_set_pad_all(&nocsif_style_screen, 0);

    /* Type scale bumped to the done-state mockup / P3.2 targets (bigger + more legible
     * than P1's mockup-matched cuts, which read too small on-panel). Each role points at
     * a generated cut; to retune a size on-device, change ONLY the font handle here — no
     * widget code selects a size. Candidates available: serif 21/23/26, mono 11/13/14/16/18. */
    lv_style_init(&nocsif_style_title);
    lv_style_set_text_font(&nocsif_style_title, &nocsif_serif_26);   /* title — locked on-device (26) */
    lv_style_set_text_color(&nocsif_style_title, NOCSIF_WHITE);

    /* Section caption: the mockup's mono `// ...` line (was serif-italic in P1). Steel;
     * callers colour a leading `//` violet (spec §6 / mockup .sub b). */
    lv_style_init(&nocsif_style_caption);
    lv_style_set_text_font(&nocsif_style_caption, &nocsif_mono_12);
    lv_style_set_text_color(&nocsif_style_caption, NOCSIF_STEEL);

    /* Invariant menu-row layout + faint bottom hairline as ONE shared style: a row then
     * costs two add_style calls instead of ~15 per-row lv_obj_set_style_* calls. Each
     * set_style refreshes the object, so ~15 per row x ~490 rows was slow enough to trip
     * the task watchdog while building the whole menu; a shared style refreshes once. */
    lv_style_init(&nocsif_style_row);
    lv_style_set_layout(&nocsif_style_row, LV_LAYOUT_FLEX);
    lv_style_set_flex_flow(&nocsif_style_row, LV_FLEX_FLOW_ROW);
    lv_style_set_flex_main_place(&nocsif_style_row, LV_FLEX_ALIGN_START);
    lv_style_set_flex_cross_place(&nocsif_style_row, LV_FLEX_ALIGN_CENTER);
    lv_style_set_flex_track_place(&nocsif_style_row, LV_FLEX_ALIGN_CENTER);
    lv_style_set_width(&nocsif_style_row, lv_pct(100));
    lv_style_set_height(&nocsif_style_row, LV_SIZE_CONTENT);
    lv_style_set_min_height(&nocsif_style_row, 44);          /* min tap target (spec §4)  */
    lv_style_set_pad_top(&nocsif_style_row, 15);
    lv_style_set_pad_bottom(&nocsif_style_row, 15);
    lv_style_set_pad_left(&nocsif_style_row, 2);
    lv_style_set_pad_right(&nocsif_style_row, 2);
    lv_style_set_pad_column(&nocsif_style_row, 16);          /* icon->name gap (spec §4)  */
    lv_style_set_border_color(&nocsif_style_row, lv_color_hex(0x101013));  /* mockup .row divider */
    lv_style_set_border_opa(&nocsif_style_row, LV_OPA_COVER);
    lv_style_set_border_width(&nocsif_style_row, 1);
    lv_style_set_border_side(&nocsif_style_row, LV_BORDER_SIDE_BOTTOM);

    lv_style_init(&nocsif_style_row_press);                  /* applied at LV_STATE_PRESSED */
    lv_style_set_bg_color(&nocsif_style_row_press, NOCSIF_PIT_ON);
    lv_style_set_bg_opa(&nocsif_style_row_press, LV_OPA_COVER);
    lv_style_set_radius(&nocsif_style_row_press, 4);

    /* Row icon: the menu icon font, steel. The mockup's accent-tinted icon is a hover-only
     * rule; a touch panel has no hover, so the icon stays steel and the row's pit-on wash is
     * the press feedback. (An explicit press-accent would need a row event — child labels
     * don't inherit the row's LV_STATE_PRESSED. Left as a deliberate non-goal.) */
    lv_style_init(&nocsif_style_row_icon);
    lv_style_set_text_font(&nocsif_style_row_icon, &nocsif_icons);
    lv_style_set_text_color(&nocsif_style_row_icon, NOCSIF_STEEL);

    /* Row drill-in chevron: icon font, ash (same 22px cut — the mockup's 14px is a follow-up). */
    lv_style_init(&nocsif_style_chevron);
    lv_style_set_text_font(&nocsif_style_chevron, &nocsif_icons);
    lv_style_set_text_color(&nocsif_style_chevron, NOCSIF_ASH);

    lv_style_init(&nocsif_style_row_name);
    lv_style_set_text_font(&nocsif_style_row_name, &nocsif_mono_16);   /* name (mockup 15.5) */
    lv_style_set_text_color(&nocsif_style_row_name, NOCSIF_BONE);

    lv_style_init(&nocsif_style_row_tag);
    lv_style_set_text_font(&nocsif_style_row_tag, &nocsif_mono_13);
    lv_style_set_text_color(&nocsif_style_row_tag, NOCSIF_ASH);

    /* §4.13 — font-only scale tokens (no colour): the Default cuts here; typescale_apply re-points them. */
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

    /* §4.1 — seed the saved accent + type scale (NVS). Accent only fills the cache (inline uses
     * read it at build time). Type scale re-points the styles just initialised above; not "live"
     * (no widgets exist yet — the first screen build picks up the cut). */
    accent_apply_rgb((uint32_t)nocsif_settings_get_i32("ui.accentc", 0x655578));
    typescale_apply(nocsif_settings_get_i32("ui.typescale", 1), false);
}
