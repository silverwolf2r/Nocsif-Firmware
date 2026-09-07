/*
 * NocSif — static orrery/star background + rounded-corner masks (UI-shell P2)
 *
 * Renders the design spec's antique orrery (concentric + dashed orbital rings,
 * scattered star dots, engraved celestial stars — spec §6, mockup lines 141-165)
 * ONCE into the per-display bottom layer (lv_layer_bottom()), and installs four
 * opaque `void` rounded-corner masks on the top layer (lv_layer_top(), spec §4)
 * so the full-bleed orrery cannot bleed under the physically rounded glass.
 *
 * Compositing model (the P2 proof): the orrery is a single lv_canvas in PSRAM,
 * drawn once at init; every screen keeps a TRANSPARENT background so the orrery
 * shows through and persists across screen swaps. On a widget redraw LVGL only
 * re-blits the canvas region under the dirty rect — the vector draw (arcs/dots/
 * stars) runs exactly once, never per frame. See ui.c's flush-audit instrument.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Build the orrery on lv_layer_bottom() and the corner masks on lv_layer_top().
 * MUST be called on/under the LVGL port lock (the caller holds it) AFTER the
 * display is registered (lv_layer_bottom/top resolve the default display).
 * Returns ESP_OK once the orrery CANVAS (the opaque ground) is drawn — that is the
 * signal the caller uses to make screens transparent. ESP_ERR_NO_MEM only if the canvas
 * itself cannot be allocated (nothing drawn — screens must stay opaque; safe to retry).
 * A corner-mask allocation failure is cosmetic (that corner's nub left unmasked), logged
 * but NOT returned — it must not hide the drawn orrery. The one-time orrery vector draw
 * runs at most once (guarded once the canvas exists). */
esp_err_t nocsif_ui_background_init(void);

/* UI-shell P6 — occasional shooting stars (spec §7.2). Create the streak object on lv_layer_bottom()
 * (above the orrery canvas, below the transparent screens) and start the trigger timer that launches
 * a rare (~every 1.5-4 min), brief (~1.6 s) diagonal comet across the star field. Screen-ON
 * only; OFF under the reduced-motion flag (P5b). Call once, on/under the LVGL port lock, AFTER
 * nocsif_ui_background_init(). Idempotent; a line-alloc failure just disables streaks (cosmetic). */
void nocsif_ui_background_shooting_stars_init(void);

/* P8 v2.3 — re-raise the rounded-corner masks to the front of lv_layer_top() after a global
 * overlay (Control Center / flashlight) is added there, so the overlay's square corners stay
 * hidden under the masked glass. Safe before the masks exist (no-op). LVGL-task / port-lock. */
void nocsif_ui_background_raise_corner_masks(void);

/* §4.1 Theme — wallpaper STYLE: the base composition painted on the (always-opaque) void ground.
 * The active style is persisted (NVS); seeded at init. Repaints the render-once canvas at runtime. */
typedef enum {
    NOCSIF_WP_ORRERY = 0,      /* antique orbital rings + engraved stars (the original)   */
    NOCSIF_WP_SUN,             /* engraved radiant sun: sunburst rays + coronae + stars   */
    NOCSIF_WP_GRIMOIRE,        /* mystical seal: orbital circles + hexagram + tick ring   */
    NOCSIF_WP_COUNT,
} nocsif_wallpaper_t;

int         nocsif_ui_background_style(void);          /* the active wallpaper */
void        nocsif_ui_background_set_style(int style); /* select + persist + repaint */
const char *nocsif_ui_background_style_name(int style);

/* Per-wallpaper settings — EACH style keeps its own "star" (accent-tinted marks) colour, its own layer
 * toggles, and its own comets flag, all indexed by nocsif_wallpaper_t and persisted separately. A
 * style's layers are named by nocsif_wp_layer_name (Orrery: Rings/Diamond stars/Star dots; Constellation:
 * Lines; Blueprint: Nodes). Setters repaint only if `style` is the active one. LVGL-task / port-lock. */
#define NOCSIF_WP_MAX_LAYERS 3
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
