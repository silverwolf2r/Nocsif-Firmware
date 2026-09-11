/*
 * Static orrery/star background and rounded-corner masks (UI-shell P2).
 *
 * Draws the design spec's antique orrery — concentric and dashed orbital rings,
 * scattered star dots, engraved celestial stars (spec section 6, mockup lines 141-165)
 * — exactly once onto the display's bottom layer (lv_layer_bottom()), and adds four
 * opaque rounded-corner masks on the top layer (lv_layer_top(), spec section 4) so the
 * full-bleed orrery art never shows through the physically rounded glass edges.
 *
 * Compositing approach: the orrery lives in one lv_canvas allocated in PSRAM, drawn a
 * single time at init. Every screen has a transparent background so the orrery shows
 * through underneath and survives screen swaps. When a widget redraws, LVGL only
 * re-blits the canvas region under the dirty rectangle — the actual vector drawing
 * (arcs, dots, stars) runs exactly once, never on every frame. See ui.c's flush-audit
 * instrumentation for how this is verified.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Draw the orrery onto lv_layer_bottom() and the corner masks onto lv_layer_top().
 * Must be called while holding the LVGL port lock, after the display has been
 * registered (lv_layer_bottom/top need the default display to resolve). Returns
 * ESP_OK once the orrery canvas — the opaque background — is drawn; that's the signal
 * callers use to start making their screens transparent. Only returns ESP_ERR_NO_MEM
 * if the canvas itself couldn't be allocated, in which case nothing was drawn and
 * screens must stay opaque (safe to retry). A corner-mask allocation failure is purely
 * cosmetic — that corner is left unmasked and a warning is logged, but it does not
 * fail the call, since that would hide an orrery that actually did draw successfully.
 * The one-time vector drawing pass is guarded so it can only ever run once the canvas
 * exists. */
esp_err_t nocsif_ui_background_init(void);

/* (UI-shell P6) Occasional shooting stars (spec section 7.2). Creates the streak
 * object on lv_layer_bottom() (above the orrery canvas, below the transparent screens)
 * and starts the timer that fires a rare (roughly every 1.5-4 minutes), brief (about
 * 1.6s) diagonal comet across the star field. Only runs while the screen is on, and is
 * disabled entirely under the reduced-motion setting (P5b). Call once, holding the
 * LVGL port lock, after nocsif_ui_background_init(). Safe to call more than once; if
 * the line object can't be allocated, streaks are just silently disabled. */
void nocsif_ui_background_shooting_stars_init(void);

/* (P8 v2.3) Moves the rounded-corner masks back to the front of lv_layer_top() after
 * a global overlay (Control Center, flashlight) has been added there, so the overlay's
 * square corners stay hidden behind the masked glass edge. Safe to call before the
 * masks exist yet (no-op). LVGL task, under the port lock. */
void nocsif_ui_background_raise_corner_masks(void);

/* (section 4.1 theme) The wallpaper style — the base artwork painted onto the always-
 * opaque background. The chosen style is persisted to NVS and loaded at init; changing
 * it repaints the existing render-once canvas at runtime rather than rebuilding it. */
typedef enum {
    NOCSIF_WP_ORRERY = 0,      /* the original: antique orbital rings plus engraved stars */
    NOCSIF_WP_SUN,             /* an engraved radiant sun: sunburst rays, coronae, and a star field */
    NOCSIF_WP_GRIMOIRE,        /* a mystical seal: orbital circles, a hexagram, and a tick ring */
    NOCSIF_WP_COUNT,
} nocsif_wallpaper_t;

int         nocsif_ui_background_style(void);          /* the currently active wallpaper style */
void        nocsif_ui_background_set_style(int style); /* changes the active style, persists it, and repaints */
const char *nocsif_ui_background_style_name(int style);

/* Per-wallpaper settings — EACH style keeps its own "star" (accent-tinted marks) colour, its own layer
 * toggles, and its own comets flag, all indexed by nocsif_wallpaper_t and persisted separately. A
 * style's layers are named by nocsif_wp_layer_name (Orrery: Rings/Diamond stars/Star dots; Grimoire:
 * Hexagram/Tick ring/Outer ring/Inner ring/Middle star). Setters repaint only if `style` is the active
 * one. LVGL-task / port-lock. */
#define NOCSIF_WP_MAX_LAYERS 5   /* was 3; Grimoire uses 5 (rings split + a toggleable centre star) */
uint32_t    nocsif_wp_star_color(int style);
void        nocsif_wp_set_star_color(int style, uint32_t rgb);
int         nocsif_wp_layer_count(int style);
const char *nocsif_wp_layer_name(int style, int idx);
bool        nocsif_wp_layer(int style, int idx);
void        nocsif_wp_set_layer(int style, int idx, bool on);
bool        nocsif_wp_comets(int style);
void        nocsif_wp_set_comets(int style, bool on);

#ifdef __cplusplus
}
#endif
