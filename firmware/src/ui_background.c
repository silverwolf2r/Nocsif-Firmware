/*
 * Static orrery/star background implementation (UI-shell P2). See ui_background.h.
 *
 * The orrery lives in one 410x502 lv_canvas in PSRAM (following the M1 framebuffer
 * precedent), drawn exactly once:
 *   - an opaque void background (lv_canvas_fill_bg)
 *   - two ring clusters bleeding off the top-right (356,56) and bottom-left (44,476)
 *     corners: concentric solid and dashed arcs (lv_draw_arc), color #3C3C44 at
 *     roughly 0.11-0.24 opacity
 *   - about 15 scattered gray star dots plus 2 violet body dots (lv_draw_rect, circular)
 *   - 4 engraved celestial stars (the recolored nocsif_img_star A8 asset, one violet),
 *     scaled down from its 64px master (lv_draw_image)
 * added as a child of lv_layer_bottom() and never invalidated again. Every screen has
 * a transparent background so this shows through underneath and survives screen swaps.
 * When a widget redraws, LVGL only re-blits the canvas region under the dirty
 * rectangle — the vector drawing above runs exactly once (guarded), never per frame.
 * Geometry and opacity values were taken from ui-mockup.html lines 141-165 (its
 * 410x502 viewBox maps 1:1 to device pixels).
 *
 * Colors are defined the normal way, via the ui_theme tokens / lv_color_hex — the
 * panel's BGR byte order is already handled in the M3 flush config (display.c/ui.c),
 * so don't hand-swap bytes here. Canvas draw buffers stay in internal RAM (M3); this
 * PSRAM canvas is only read by the CPU during compositing, never DMA'd as a flush
 * buffer, so it doesn't need a bounce buffer.
 */
#include "ui_background.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>           /* for snprintf, used to build the per-wallpaper NVS keys */

#include "ui_theme.h"        /* for the color/font tokens and the nocsif_img_star extern declaration */
#include "lvgl.h"
#include "esp_heap_caps.h"
#include "esp_random.h"      /* (UI-shell P6) used to randomize each shooting star's trajectory */
#include "esp_log.h"
#include "display.h"         /* (UI-shell P6) for nocsif_display_is_asleep, the screen-on gate */
#include "settings.h"        /* (UI-shell P6) for the reduce_motion setting (P5b) */

static const char *TAG = "ui_bg";

/* (section 4.1 theme) The render-once orrery canvas and its PSRAM buffer, kept
 * around so the wallpaper layer toggles can repaint it at runtime. Each layer is
 * independent, and turning them all off still leaves a clean opaque void (the canvas
 * itself is never hidden, since hiding the bottom layer used to leave stale pixels
 * on screen). The accent-tinted marks use their own separate star color, since they
 * are meant to be free to clash with the UI accent color. */
static lv_obj_t  *s_orrery_canvas;
static uint8_t   *s_orrery_buf;
static size_t     s_orrery_stride;
static int        s_wp_style;                  /* the currently active nocsif_wallpaper_t value */

/* Per-wallpaper settings, indexed by style: its own star color, layer toggles, and
 * comets flag. s_wp_star_col mirrors s_wp_star_rgb as a pre-converted lv_color_t for
 * the paint path. */
static uint32_t   s_wp_star_rgb[NOCSIF_WP_COUNT];
static lv_color_t s_wp_star_col[NOCSIF_WP_COUNT];
static bool       s_wp_lyr[NOCSIF_WP_COUNT][NOCSIF_WP_MAX_LAYERS];
static bool       s_wp_comet[NOCSIF_WP_COUNT];

/* Human-readable names for each style's layers (NULL past its layer count); the settings screen builds one toggle per name. */
static const char *const s_wp_lyr_name[NOCSIF_WP_COUNT][NOCSIF_WP_MAX_LAYERS] = {
    { "Rings", "Diamond stars", "Star dots" },        /* for NOCSIF_WP_ORRERY */
    { "Sunburst rays", "Coronae", "Star field" },     /* for NOCSIF_WP_SUN */
    { "Hexagram", "Tick ring", "Outer ring", "Inner ring", "Middle star" },/* NOCSIF_WP_GRIMOIRE */
};
static const int s_wp_lyr_count[NOCSIF_WP_COUNT] = { 3, 3, 5 };

#define BG_W          410
#define BG_H          502
#define STAR_MASTER   64     /* the nocsif_img_star master image size, in pixels */
#define CORNER_R      40     /* rounded-corner mask radius in pixels (spec section 4) */

/* the orrery ring color (#3C3C44) and the two scattered-mark colors */
#define RING_COLOR    lv_color_hex(0x3C3C44)
#define DOT_COLOR     lv_color_hex(0xB6B6BD)   /* color of the gray star dots */

/* ---- direct-buffer rasterizers for the rings and dots ---- *
 * The faint rings and dots are rasterized straight into the RGB888 canvas buffer
 * instead of going through lv_draw_arc. Reason (a load-bearing perf finding):
 * lv_draw_sw_arc builds a full anti-aliased radius mask per arc and scans its entire
 * 2r x 2r bounding box; for the two dashed rings, that's around 200 arc segments at
 * r=106/118, which took tens of seconds on the LVGL task and tripped the watchdog.
 * Plotting a roughly 1px ring outline directly is O(circumference), and testing for
 * a dash is a cheap arc-length check.
 *
 * lv_canvas_set_px on an RGB888 (no-alpha) buffer overwrites the pixel outright
 * (ignoring opacity) and invalidates the whole canvas on every call, so it's avoided
 * here too: instead the mark color is pre-blended over the void background by hand,
 * writing B, G, R bytes directly (LVGL's RGB888 buffer stores pixels as B,G,R in
 * memory — see the M3 notes). Since the rings and dots sit on the void background and
 * rarely overlap each other, and the 4 stars are drawn afterward via lv_draw_image so
 * they alpha-blend on top normally, pre-blending against void is exact everywhere it
 * actually matters. */

/* Blends one channel of `mark` over `base` at opacity `opa`, clamped to [0,255].
 * The clamp guards against a future mark color with a channel darker than the void
 * background, which would otherwise underflow and wrap when cast to uint8; the
 * current palette never triggers this. */
static uint8_t blend_ch(int base, int mark, int opa)
{
    int v = base + (mark - base) * opa / 255;
    if (v < 0) v = 0;
    else if (v > 255) v = 255;
    return (uint8_t)v;
}

/* Pre-blends a mark color over the NOCSIF_VOID background (0x070708) at opacity `opa`. */
static void blend_over_void(lv_color_t mark, lv_opa_t opa, uint8_t *r, uint8_t *g, uint8_t *b)
{
    *r = blend_ch(7, mark.red,   opa);
    *g = blend_ch(7, mark.green, opa);
    *b = blend_ch(8, mark.blue,  opa);
}

static inline void put_px(uint8_t *buf, size_t stride, int x, int y,
                          uint8_t r, uint8_t g, uint8_t b)
{
    if (x < 0 || x >= BG_W || y < 0 || y >= BG_H) return;
    uint8_t *p = buf + (size_t)y * stride + (size_t)x * 3;
    p[0] = b;  p[1] = g;  p[2] = r;    /* the RGB888 buffer stores pixels as B,G,R (see the M3 notes) */
}

/* Plots a roughly 1px ring outline, optionally dashed (dash_on/dash_gap in pixels
 * along the circumference; dash_gap == 0 means solid). Oversampled about 1.3x so the
 * outline stays visually continuous. */
static void plot_ring(uint8_t *buf, size_t stride, int cx, int cy, int radius,
                      lv_color_t color, lv_opa_t opa, float dash_on, float dash_gap)
{
    uint8_t r, g, b;
    blend_over_void(color, opa, &r, &g, &b);
    float circ = 2.0f * 3.14159265f * (float)radius;
    int steps = (int)(circ / 0.75f);
    if (steps < 8) steps = 8;
    float period = dash_on + dash_gap;
    for (int i = 0; i < steps; i++) {
        float t = (float)i / (float)steps;               /* fraction 0..1 around the circle */
        if (dash_gap > 0.0f && fmodf(t * circ, period) >= dash_on) {
            continue;                                    /* inside a dash gap, skip this pixel */
        }
        float ang = t * 2.0f * 3.14159265f;
        int x = cx + (int)lroundf((float)radius * cosf(ang));
        int y = cy + (int)lroundf((float)radius * sinf(ang));
        put_px(buf, stride, x, y, r, g, b);
    }
}

/* A small filled square dot; at 1-4px it's indistinguishable from a disc. */
static void plot_dot(uint8_t *buf, size_t stride, int cx, int cy, int diam,
                     lv_color_t color, lv_opa_t opa)
{
    if (diam < 1) diam = 1;
    uint8_t r, g, b;
    blend_over_void(color, opa, &r, &g, &b);
    int x0 = cx - diam / 2, y0 = cy - diam / 2;
    for (int dy = 0; dy < diam; dy++) {
        for (int dx = 0; dx < diam; dx++) {
            put_px(buf, stride, x0 + dx, y0 + dy, r, g, b);
        }
    }
}

/* Draws an engraved star: the A8 master image recolored (recolor sets the visible
 * pixel color for an A8 source), scaled about its top-left corner so the on-screen
 * top-left stays fixed at (x,y) regardless of the target `size`. */
static void draw_star(lv_layer_t *layer, int x, int y, int size,
                      lv_color_t color, lv_opa_t opa)
{
    lv_draw_image_dsc_t d;
    lv_draw_image_dsc_init(&d);
    d.src         = &nocsif_img_star;
    d.recolor     = color;
    d.recolor_opa = LV_OPA_COVER;
    d.opa         = opa;
    int scale     = (size * LV_SCALE_NONE) / STAR_MASTER;   /* LVGL's fixed-point scale unit: 256 means 1.0x */
    d.scale_x     = scale;
    d.scale_y     = scale;
    d.pivot.x     = 0;
    d.pivot.y     = 0;
    d.antialias   = 1;
    lv_area_t a = { x, y, x + STAR_MASTER - 1, y + STAR_MASTER - 1 };
    lv_draw_image(layer, &d, &a);
}

/* ---- scattered marks, positions taken from the mockup ---- */

typedef struct { int16_t x, y, diam; lv_opa_t opa; } dot_t;

/* the 15 gray star dots (mockup lines 153-159): diam = round(2*r), opa = round(op*255) */
static const dot_t s_dots[] = {
    {  40,  70, 2, 102 }, { 120,  30, 2,  82 }, {  70, 150, 2,  71 },
    {  24, 300, 2,  71 }, { 150, 470, 2,  82 }, { 210, 420, 2,  61 },
    {  60, 430, 2,  71 }, { 185, 120, 1,  61 }, {  95, 255, 2,  61 },
    {  18, 205, 2,  61 }, { 270, 360, 2,  61 }, { 132, 360, 1,  56 },
    { 300, 470, 2,  61 }, {  45, 360, 1,  56 }, { 232, 205, 1,  51 },
};

/* the 2 violet body dots riding a ring (mockup line 151) */
static const dot_t s_violet_bodies[] = {
    { 250,  56, 4, 153 },   /* radius 2, opacity .6 */
    { 118, 476, 3, 115 },   /* radius 1.6, opacity .45 */
};

typedef struct { int16_t x, y, size; lv_opa_t opa; uint8_t violet; } star_t;

/* the 4 engraved celestial stars (mockup lines 161-164); one is violet (spec section 6) */
static const star_t s_stars[] = {
    { 288, 150, 30, 102, 0 },   /* gray, opacity .4 */
    {  58, 236, 26, 153, 1 },   /* violet, opacity .6 */
    { 196,  30, 20,  87, 0 },   /* gray, opacity .34 */
    { 330, 330, 17,  77, 0 },   /* gray, opacity .3 */
};

/* ---- corner masks, drawn on lv_layer_top ---- *
 * One ARGB8888 canvas per corner: opaque void fills the sub-glass nub outside the
 * rounded-rect arc, and stays transparent inside it so the content/orrery show
 * through, with a 1px feather at the boundary.
 *
 * These masks share lv_layer_top() with any global overlay (P8 v2.3's Control
 * Center, flashlight). Layer z-order follows child-add order, so an overlay added
 * after the masks would draw over the rounded corners and expose square nubs. The
 * mask object pointers are kept around so overlay code can re-raise them to the
 * front after adding itself. */
static lv_obj_t *s_corner_mask[4];
static int       s_corner_mask_n;

static esp_err_t build_corner_mask(int screen_x, int screen_y, float arc_cx, float arc_cy)
{
    size_t stride = lv_draw_buf_width_to_stride(CORNER_R, LV_COLOR_FORMAT_ARGB8888);
    size_t sz = stride * CORNER_R;
    uint8_t *buf = heap_caps_aligned_alloc(64, sz, MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGE(TAG, "corner mask buffer alloc failed (%u B)", (unsigned)sz);
        return ESP_ERR_NO_MEM;
    }
    lv_obj_t *cv = lv_canvas_create(lv_layer_top());
    if (!cv) {
        heap_caps_free(buf);
        return ESP_ERR_NO_MEM;
    }
    lv_canvas_set_buffer(cv, buf, CORNER_R, CORNER_R, LV_COLOR_FORMAT_ARGB8888);
    lv_obj_set_pos(cv, screen_x, screen_y);
    lv_obj_remove_flag(cv, LV_OBJ_FLAG_CLICKABLE);
    if (s_corner_mask_n < 4) s_corner_mask[s_corner_mask_n++] = cv;   /* keep the pointer so it can be re-raised later */
    lv_canvas_fill_bg(cv, NOCSIF_VOID, LV_OPA_TRANSP);   /* clear the canvas to fully transparent first */

    for (int y = 0; y < CORNER_R; y++) {
        for (int x = 0; x < CORNER_R; x++) {
            float dx = (float)x - arc_cx;
            float dy = (float)y - arc_cy;
            float dist = sqrtf(dx * dx + dy * dy);
            float cov = dist - ((float)CORNER_R - 0.5f);   /* a 1px feather at the arc boundary */
            if (cov <= 0.0f) continue;                      /* inside the glass area: leave it transparent */
            if (cov > 1.0f) cov = 1.0f;
            lv_canvas_set_px(cv, x, y, NOCSIF_VOID, (lv_opa_t)(cov * 255.0f));
        }
    }
    return ESP_OK;
}

/* Moves the four corner masks back to the front of lv_layer_top(). Call this after
 * adding a global top-layer overlay (Control Center, flashlight), so the physically
 * rounded glass edge stays masked over it. No-op if the masks don't exist yet. */
void nocsif_ui_background_raise_corner_masks(void)
{
    for (int i = 0; i < s_corner_mask_n; i++) {
        if (s_corner_mask[i]) lv_obj_move_foreground(s_corner_mask[i]);
    }
}

/* -------------------------------------------------------------- */

/* Draws a thin 1px line into the canvas buffer, one pixel at a time (plot_dot
 * already pre-blends each pixel over the void background). This only ever runs once,
 * so the per-pixel cost doesn't matter. */
static void plot_line(uint8_t *buf, size_t stride, int x0, int y0, int x1, int y1, lv_color_t col, lv_opa_t opa)
{
    int adx = x1 > x0 ? x1 - x0 : x0 - x1;
    int ady = y1 > y0 ? y1 - y0 : y0 - y1;
    int steps = adx > ady ? adx : ady;
    if (steps <= 0) { plot_dot(buf, stride, x0, y0, 1, col, opa); return; }
    float xi = (float)(x1 - x0) / steps, yi = (float)(y1 - y0) / steps;
    float x = (float)x0, y = (float)y0;
    for (int i = 0; i <= steps; i++) {
        plot_dot(buf, stride, (int)(x + 0.5f), (int)(y + 0.5f), 1, col, opa);
        x += xi; y += yi;
    }
}

/* ---- wallpaper styles, each painting onto the already-void-filled canvas ---- *
 * Each style reads only its own star color and layer toggles, so the Star color
 * control and layer switches on a given wallpaper's settings screen only affect that
 * one wallpaper. */
static void wp_orrery(void)
{
    lv_color_t star = s_wp_star_col[NOCSIF_WP_ORRERY];
    if (s_wp_lyr[NOCSIF_WP_ORRERY][0]) {   /* Rings layer */
        /* orrery ring cluster A, top-right at (356,56) */
        plot_ring(s_orrery_buf, s_orrery_stride, 356, 56, 152, RING_COLOR, 56, 0.0f, 0.0f);   /* solid ring */
        plot_ring(s_orrery_buf, s_orrery_stride, 356, 56, 106, RING_COLOR, 61, 1.5f, 5.0f);   /* dashed ring */
        plot_ring(s_orrery_buf, s_orrery_stride, 356, 56,  62, RING_COLOR, 41, 0.0f, 0.0f);   /* solid ring */
        /* orrery ring cluster B, bottom-left at (44,476) */
        plot_ring(s_orrery_buf, s_orrery_stride,  44, 476, 118, RING_COLOR, 41, 2.0f, 6.0f);  /* dashed ring */
        plot_ring(s_orrery_buf, s_orrery_stride,  44, 476,  72, RING_COLOR, 28, 0.0f, 0.0f);   /* solid ring */
    }
    if (s_wp_lyr[NOCSIF_WP_ORRERY][2]) {   /* Star dots layer: gray dots plus the accent-colored orbital bodies */
        for (size_t i = 0; i < sizeof(s_dots) / sizeof(s_dots[0]); i++)
            plot_dot(s_orrery_buf, s_orrery_stride, s_dots[i].x, s_dots[i].y, s_dots[i].diam, DOT_COLOR, s_dots[i].opa);
        for (size_t i = 0; i < sizeof(s_violet_bodies) / sizeof(s_violet_bodies[0]); i++)
            plot_dot(s_orrery_buf, s_orrery_stride, s_violet_bodies[i].x, s_violet_bodies[i].y,
                     s_violet_bodies[i].diam, star, s_violet_bodies[i].opa);
    }
    if (s_wp_lyr[NOCSIF_WP_ORRERY][1]) {   /* Diamond stars layer: one of the four is accent-colored */
        lv_layer_t layer;
        lv_canvas_init_layer(s_orrery_canvas, &layer);
        for (size_t i = 0; i < sizeof(s_stars) / sizeof(s_stars[0]); i++) {
            lv_color_t c = s_stars[i].violet ? star : DOT_COLOR;
            draw_star(&layer, s_stars[i].x, s_stars[i].y, s_stars[i].size, c, s_stars[i].opa);
        }
        lv_canvas_finish_layer(s_orrery_canvas, &layer);
    }
}

/* The engraved sun (radiant disc): a sunburst of rays, concentric coronae, and a
 * scattered star field. The per-wallpaper star color tints the sun itself (rays and
 * coronae); the star field stays neutral gray. */
static void wp_sun(void)
{
    lv_color_t sun = s_wp_star_col[NOCSIF_WP_SUN];
    const int cx = 205, cy = 205;
    if (s_wp_lyr[NOCSIF_WP_SUN][0]) {   /* Sunburst rays layer: 24 spokes of alternating length */
        for (int i = 0; i < 24; i++) {
            float a = (float)i * 15.0f * 3.14159265f / 180.0f;
            float l = (i & 1) ? 58.0f : 70.0f;
            int x0 = cx + (int)lroundf(48.0f * cosf(a)), y0 = cy + (int)lroundf(48.0f * sinf(a));
            int x1 = cx + (int)lroundf(l    * cosf(a)), y1 = cy + (int)lroundf(l    * sinf(a));
            plot_line(s_orrery_buf, s_orrery_stride, x0, y0, x1, y1, sun, 120);
        }
    }
    if (s_wp_lyr[NOCSIF_WP_SUN][1]) {   /* Coronae layer: the disc plus one inner ring */
        plot_ring(s_orrery_buf, s_orrery_stride, cx, cy, 46, sun, 150, 0.0f, 0.0f);
        plot_ring(s_orrery_buf, s_orrery_stride, cx, cy, 34, sun,  92, 0.0f, 0.0f);
    }
    if (s_wp_lyr[NOCSIF_WP_SUN][2]) {   /* Star field layer: neutral gray dots */
        for (size_t i = 0; i < sizeof(s_dots) / sizeof(s_dots[0]); i++)
            plot_dot(s_orrery_buf, s_orrery_stride, s_dots[i].x, s_dots[i].y, s_dots[i].diam, DOT_COLOR, s_dots[i].opa);
    }
}

/* Grimoire: a mystical seal made of concentric orbital circles, a hexagram (two
 * overlapping triangles), and a 36-tick outer ring, all drawn in the neutral engraved
 * gray — the sigil itself stays colorless. The per-wallpaper star color only tints
 * the single accent-colored center star; a handful of gray dots scatter the field. */
static void wp_grimoire(void)
{
    lv_color_t star = s_wp_star_col[NOCSIF_WP_GRIMOIRE];
    const int cx = 205, cy = 240;
    if (s_wp_lyr[NOCSIF_WP_GRIMOIRE][2]) {   /* Outer ring — the r150 rim + its r126 dashed companion */
        plot_ring(s_orrery_buf, s_orrery_stride, cx, cy, 150, RING_COLOR, 82, 0.0f, 0.0f);
        plot_ring(s_orrery_buf, s_orrery_stride, cx, cy, 126, RING_COLOR, 60, 1.0f, 7.0f);
    }
    if (s_wp_lyr[NOCSIF_WP_GRIMOIRE][3]) {   /* Inner ring — the r70 circle around the sigil */
        plot_ring(s_orrery_buf, s_orrery_stride, cx, cy,  70, RING_COLOR, 72, 0.0f, 0.0f);
    }
    if (s_wp_lyr[NOCSIF_WP_GRIMOIRE][0]) {   /* Hexagram layer: two overlapping triangles */
        plot_line(s_orrery_buf, s_orrery_stride, cx,     cy - 70, cx + 61, cy + 35, RING_COLOR, 85);  /* the upward-pointing triangle */
        plot_line(s_orrery_buf, s_orrery_stride, cx + 61, cy + 35, cx - 61, cy + 35, RING_COLOR, 85);
        plot_line(s_orrery_buf, s_orrery_stride, cx - 61, cy + 35, cx,      cy - 70, RING_COLOR, 85);
        plot_line(s_orrery_buf, s_orrery_stride, cx,     cy + 70, cx + 61, cy - 35, RING_COLOR, 85);  /* the downward-pointing triangle */
        plot_line(s_orrery_buf, s_orrery_stride, cx + 61, cy - 35, cx - 61, cy - 35, RING_COLOR, 85);
        plot_line(s_orrery_buf, s_orrery_stride, cx - 61, cy - 35, cx,      cy + 70, RING_COLOR, 85);
    }
    if (s_wp_lyr[NOCSIF_WP_GRIMOIRE][1]) {   /* Tick ring layer: 36 ticks placed just inside radius 150 */
        for (int i = 0; i < 36; i++) {
            float a = (float)i * 10.0f * 3.14159265f / 180.0f;
            int x0 = cx + (int)lroundf(150.0f * cosf(a)), y0 = cy + (int)lroundf(150.0f * sinf(a));
            int x1 = cx + (int)lroundf(143.0f * cosf(a)), y1 = cy + (int)lroundf(143.0f * sinf(a));
            plot_line(s_orrery_buf, s_orrery_stride, x0, y0, x1, y1, RING_COLOR, 80);
        }
    }
    /* a handful of scattered gray dots, drawn regardless of layer toggles */
    static const dot_t gdots[] = {
        { 60, 90, 2, 72 }, { 350, 120, 2, 72 }, { 330, 430, 2, 80 },
        { 70, 420, 2, 64 }, { 205, 40, 2, 60 }, { 300, 470, 2, 60 },
    };
    for (size_t i = 0; i < sizeof(gdots) / sizeof(gdots[0]); i++)
        plot_dot(s_orrery_buf, s_orrery_stride, gdots[i].x, gdots[i].y, gdots[i].diam, DOT_COLOR, gdots[i].opa);
    /* centre accent star — the ONE coloured mark (26 px, centred at cx,cy); gated by "Middle star" */
    if (s_wp_lyr[NOCSIF_WP_GRIMOIRE][4]) {
        lv_layer_t layer;
        lv_canvas_init_layer(s_orrery_canvas, &layer);
        draw_star(&layer, cx - 13, cy - 13, 26, star, 150);
        lv_canvas_finish_layer(s_orrery_canvas, &layer);
    }
}

/* Paints the wallpaper: an always-opaque void background plus the active style's
 * marks. Runs once at init and again on any style, layer, or star-color change — a
 * one-off vector drawing pass on the LVGL task, never on a per-frame basis. */
static void paint_wallpaper(void)
{
    if (!s_orrery_canvas || !s_orrery_buf) {
        return;
    }
    lv_canvas_fill_bg(s_orrery_canvas, NOCSIF_VOID, LV_OPA_COVER);
    switch (s_wp_style) {
        default:
        case NOCSIF_WP_ORRERY:   wp_orrery();   break;
        case NOCSIF_WP_SUN:      wp_sun();      break;
        case NOCSIF_WP_GRIMOIRE: wp_grimoire(); break;
    }
}

esp_err_t nocsif_ui_background_init(void)
{
    static bool drawn;
    if (drawn) {
        ESP_LOGW(TAG, "background_init called again — orrery is render-once, ignoring");
        return ESP_OK;
    }

    /* Allocates the PSRAM canvas buffer (RGB888). lv_canvas_set_buffer computes its own
     * stride via lv_draw_buf_width_to_stride, so the allocation size must match that. */
    size_t stride = lv_draw_buf_width_to_stride(BG_W, LV_COLOR_FORMAT_RGB888);
    size_t buf_sz = stride * BG_H;
    size_t heap_before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    uint8_t *buf = heap_caps_aligned_alloc(64, buf_sz, MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGE(TAG, "orrery canvas alloc failed (%u B PSRAM) — falling back to no background",
                 (unsigned)buf_sz);
        return ESP_ERR_NO_MEM;
    }

    lv_obj_t *canvas = lv_canvas_create(lv_layer_bottom());
    if (!canvas) {
        heap_caps_free(buf);
        return ESP_ERR_NO_MEM;
    }
    lv_canvas_set_buffer(canvas, buf, BG_W, BG_H, LV_COLOR_FORMAT_RGB888);
    lv_obj_set_pos(canvas, 0, 0);
    lv_obj_remove_flag(canvas, LV_OBJ_FLAG_CLICKABLE);   /* never intercept touch events */

    /* (section 4.1 theme) Stash the canvas and buffer pointers so the wallpaper layer
     * toggles can repaint them later, then paint whatever layers and star color were
     * saved. The canvas itself always stays visible — an opaque void fill is the base
     * layer — so turning every layer off just leaves a clean plain-void background. */
    s_orrery_canvas = canvas;
    s_orrery_buf    = buf;
    s_orrery_stride = stride;
    for (int s = 0; s < NOCSIF_WP_COUNT; s++) {
        char k[16];
        snprintf(k, sizeof k, "wp%d.star", s);
        s_wp_star_rgb[s] = (uint32_t)nocsif_settings_get_i32(k, 0x655578);
        s_wp_star_col[s] = lv_color_hex(s_wp_star_rgb[s]);
        for (int i = 0; i < s_wp_lyr_count[s]; i++) {
            snprintf(k, sizeof k, "wp%d.l%d", s, i);
            s_wp_lyr[s][i] = nocsif_settings_get_i32(k, 1) != 0;
        }
        snprintf(k, sizeof k, "wp%d.com", s);
        s_wp_comet[s] = nocsif_settings_get_i32(k, 1) != 0;
    }
    s_wp_style = nocsif_settings_get_i32("ui.bg.style", 0);
    if (s_wp_style < 0 || s_wp_style >= NOCSIF_WP_COUNT) s_wp_style = 0;
    paint_wallpaper();

    /* Build the four rounded-corner masks (spec section 4). Each mask's arc center is
     * the inner corner of its CORNER_R square, and the outer nub (where distance > R)
     * is painted opaque void. All four are attempted independently, so three can still
     * get masked even if one allocation fails under PSRAM pressure, and any failure is
     * logged honestly. A mask failure is purely cosmetic — that corner's nub is left
     * unmasked — and does not fail this call, since the orrery background is already
     * fully drawn; returning an error here would make the caller keep the screen opaque
     * and hide the whole orrery for a merely cosmetic problem. So this returns ESP_OK
     * as long as the canvas (the background) drew successfully; the return value means
     * "background usable", not "everything about it is perfect". */
    static const struct { int sx, sy; float acx, acy; const char *name; } corners[] = {
        { 0,               0,               CORNER_R, CORNER_R, "top-left"     },
        { BG_W - CORNER_R, 0,               0,        CORNER_R, "top-right"    },
        { 0,               BG_H - CORNER_R, CORNER_R, 0,        "bottom-left"  },
        { BG_W - CORNER_R, BG_H - CORNER_R, 0,        0,        "bottom-right" },
    };
    int masks_ok = 0;
    for (int i = 0; i < 4; i++) {
        if (build_corner_mask(corners[i].sx, corners[i].sy, corners[i].acx, corners[i].acy) == ESP_OK) {
            masks_ok++;
        } else {
            ESP_LOGE(TAG, "corner mask '%s' failed — that corner left unmasked (cosmetic)", corners[i].name);
        }
    }

    drawn = true;   /* mark that the one-time vector pass has run, so it's never repeated */
    size_t heap_after = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "orrery drawn ONCE into lv_layer_bottom: %dx%d RGB888 canvas in PSRAM "
                  "(%u B, stride %u) + %d/4 corner masks; PSRAM free %u -> %u",
             BG_W, BG_H, (unsigned)buf_sz, (unsigned)stride,
             masks_ok, (unsigned)heap_before, (unsigned)heap_after);
    return ESP_OK;   /* the background is up; any corner-mask failures were cosmetic and already logged */
}

/* (section 4.1 theme) Wallpaper layer toggles and star color changes. Repainting
 * means re-running the vector drawing pass into the always-visible canvas and then
 * invalidating it so the bottom layer recomposites. No-op before the canvas exists.
 * All of this runs on the LVGL task, triggered by a settings-row tap or the color
 * picker. */
static void bg_repaint(void)
{
    if (!s_orrery_canvas) {
        return;
    }
    paint_wallpaper();
    lv_obj_invalidate(s_orrery_canvas);
}

/* Builds a per-wallpaper NVS key of the form "wp<style>.<suffix>" (e.g. wp0.star,
 * wp0.l0, wp0.com) — always 15 characters or fewer, NVS's key length limit. */
static void wp_key(char *buf, size_t n, int style, const char *suffix)
{
    snprintf(buf, n, "wp%d.%s", style, suffix);
}
static int wp_clamp_style(int s) { return (s < 0 || s >= NOCSIF_WP_COUNT) ? 0 : s; }

uint32_t nocsif_wp_star_color(int style) { return s_wp_star_rgb[wp_clamp_style(style)]; }

void nocsif_wp_set_star_color(int style, uint32_t rgb)
{
    style = wp_clamp_style(style);
    s_wp_star_rgb[style] = rgb & 0xFFFFFFu;
    s_wp_star_col[style] = lv_color_hex(s_wp_star_rgb[style]);
    char k[16]; wp_key(k, sizeof k, style, "star");
    nocsif_settings_set_i32(k, (int32_t)s_wp_star_rgb[style]);
    if (style == s_wp_style) bg_repaint();
}

int nocsif_wp_layer_count(int style) { return s_wp_lyr_count[wp_clamp_style(style)]; }

const char *nocsif_wp_layer_name(int style, int idx)
{
    if (idx < 0 || idx >= NOCSIF_WP_MAX_LAYERS) return NULL;
    return s_wp_lyr_name[wp_clamp_style(style)][idx];
}

bool nocsif_wp_layer(int style, int idx)
{
    if (idx < 0 || idx >= NOCSIF_WP_MAX_LAYERS) return false;
    return s_wp_lyr[wp_clamp_style(style)][idx];
}

void nocsif_wp_set_layer(int style, int idx, bool on)
{
    style = wp_clamp_style(style);
    if (idx < 0 || idx >= NOCSIF_WP_MAX_LAYERS) return;
    s_wp_lyr[style][idx] = on;
    char suf[4] = { 'l', (char)('0' + idx), 0 };
    char k[16]; wp_key(k, sizeof k, style, suf);
    nocsif_settings_set_i32(k, on ? 1 : 0);
    if (style == s_wp_style) bg_repaint();
}

bool nocsif_wp_comets(int style) { return s_wp_comet[wp_clamp_style(style)]; }

void nocsif_wp_set_comets(int style, bool on)
{
    style = wp_clamp_style(style);
    s_wp_comet[style] = on;
    char k[16]; wp_key(k, sizeof k, style, "com");
    nocsif_settings_set_i32(k, on ? 1 : 0);
    /* no repaint needed — comets are a live overlay checked on each streak timer beat */
}

int nocsif_ui_background_style(void) { return s_wp_style; }

const char *nocsif_ui_background_style_name(int style)
{
    switch (style) {
        case NOCSIF_WP_SUN:      return "engraved sun";
        case NOCSIF_WP_GRIMOIRE: return "grimoire";
        default:                 return "orrery";
    }
}

void nocsif_ui_background_set_style(int style)
{
    if (style < 0 || style >= NOCSIF_WP_COUNT) style = 0;
    s_wp_style = style;
    nocsif_settings_set_i32("ui.bg.style", style);
    bg_repaint();
}

/* ---- P6 occasional shooting stars (spec section 7.2) ---- *
 * A rare (roughly every 1.5-4 minutes), brief (about 1.6s) comet crosses the star
 * field: a thin bar with an opacity gradient from a clear tail to a bright head,
 * rotated to face its direction of travel, living on lv_layer_bottom — above the
 * render-once orrery canvas but below the transparent screens, so it's visible
 * through the UI in the star field — translating diagonally with a fade in and out.
 * Only runs while the screen is on, and is disabled entirely under the P5b
 * reduced-motion setting. It stays parked and hidden between streaks (no rendering
 * or invalidation happens then); a streak itself is just a brief burst of full-frame
 * flushes while visible, similar to a nav slide animation. esp_random() varies each
 * trajectory so no two streaks look identical. */
static lv_obj_t     *s_streak;
static lv_timer_t   *s_streak_timer;
static lv_grad_dsc_t s_streak_grad;   /* the opacity gradient from clear tail to bright head; the style struct stores a pointer to this, so it must be static */
static int32_t       s_streak_x0, s_streak_y0, s_streak_x1, s_streak_y1;

#define COMET_LEN        58        /* comet length from tail to head, in pixels */
#define COMET_W          4         /* comet thickness, in pixels */
#define STREAK_DUR_MS    1600      /* how long one traversal takes; a larger value glides more slowly across the screen */
/* Setting NOCSIF_STREAK_TEST to 1 makes streaks fire frequently (every 8-16s) so the
 * effect can be verified on-device without waiting minutes between occurrences. Leave
 * it at 0 for the spec's normal cadence (1.5-4 min) — that's the value actually shipped. */
#define NOCSIF_STREAK_TEST 0
#if NOCSIF_STREAK_TEST
#define STREAK_MIN_MS    8000
#define STREAK_MAX_MS    16000
#else
#define STREAK_MIN_MS    90000     /* 1.5 minutes, in milliseconds */
#define STREAK_MAX_MS    240000    /* 4 minutes, in milliseconds */
#endif
#define STREAK_PEAK_OPA  220       /* a subtle highlight rather than a full-white flash */

static uint32_t streak_next_period(void)
{
    return STREAK_MIN_MS + (esp_random() % (STREAK_MAX_MS - STREAK_MIN_MS));
}

/* The animation variable is the line object; `p` runs 0..1000. This linearly
 * interpolates the object's position from start to end and applies a fade-in/hold/
 * fade-out opacity envelope, so the comet appears mid-field and fades out before
 * reaching the edge instead of popping abruptly. */
static void streak_anim_cb(void *var, int32_t p)
{
    lv_obj_t *s = (lv_obj_t *)var;
    int32_t x = s_streak_x0 + (s_streak_x1 - s_streak_x0) * p / 1000;
    int32_t y = s_streak_y0 + (s_streak_y1 - s_streak_y0) * p / 1000;
    lv_obj_set_pos(s, x, y);

    lv_opa_t opa;
    if (p < 250)      opa = (lv_opa_t)(STREAK_PEAK_OPA * p / 250);          /* fading in */
    else if (p > 550) opa = (lv_opa_t)(STREAK_PEAK_OPA * (1000 - p) / 450); /* fading out */
    else              opa = STREAK_PEAK_OPA;
    lv_obj_set_style_opa(s, opa, 0);
}

static void streak_done_cb(lv_anim_t *a)
{
    (void)a;
    if (s_streak) {
        lv_obj_add_flag(s_streak, LV_OBJ_FLAG_HIDDEN);   /* park it: no rendering or invalidation happens between streaks */
    }
}

static void streak_launch(void)
{
    if (!s_streak) return;
    uint32_t rnd = esp_random();
    int dir = (rnd & 1) ? -1 : 1;                          /* +1 travels down-right, -1 travels down-left */
    int sx  = (dir > 0) ? -30 + (int)((rnd >> 1) % 90)     /* enter from somewhere near a top corner */
                        :  350 - (int)((rnd >> 1) % 90);
    int sy  = -30 + (int)((rnd >> 9) % 80);                /* start at or slightly above the top edge */
    int dx  = dir * 300, dy = 380;                         /* diagonal travel distance across the panel */

    /* Orients the comet along its travel direction by rotating the gradient bar (clear
     * tail to bright head) to match the travel angle. Since +y points down, atan2(dy,dx)
     * gives the clockwise screen angle; the bar's opaque end (its local +x) then points
     * along the travel direction, so the head leads and the tail trails behind. A
     * rotated rect has no negative-point clipping issue, so both travel directions
     * render identically. */
    float ang_deg = atan2f((float)dy, (float)dx) * 57.2957795f;         /* convert radians to degrees */
    lv_obj_set_style_transform_rotation(s_streak, (int32_t)(ang_deg * 10.0f), 0);   /* LVGL rotation is in units of 0.1 degree */

    s_streak_x0 = sx;       s_streak_y0 = sy;
    s_streak_x1 = sx + dx;  s_streak_y1 = sy + dy;
    lv_obj_set_pos(s_streak, sx, sy);
    lv_obj_set_style_opa(s_streak, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(s_streak, LV_OBJ_FLAG_HIDDEN);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_streak);
    lv_anim_set_exec_cb(&a, streak_anim_cb);
    lv_anim_set_values(&a, 0, 1000);
    lv_anim_set_duration(&a, STREAK_DUR_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_linear);          /* constant speed — this is a streak, not an eased animation */
    lv_anim_set_ready_cb(&a, streak_done_cb);
    lv_anim_start(&a);
}

static void streak_timer_cb(lv_timer_t *t)
{
    lv_timer_set_period(t, streak_next_period());          /* pick a fresh random interval for next time */
    if (nocsif_display_is_asleep()) return;                /* only run while the screen is on (spec section 7.2) */
    if (nocsif_settings_get_i32("reduce_motion", 0)) return;   /* stay off under the reduced-motion setting (P5b) */
    if (!s_wp_comet[s_wp_style]) return;  /* (section 4.1 theme) respect the per-wallpaper comets toggle */
    streak_launch();
}

void nocsif_ui_background_shooting_stars_init(void)
{
    if (s_streak) return;                                  /* only create the object once */
    s_streak = lv_obj_create(lv_layer_bottom());           /* above the orrery canvas, below the screens */
    if (!s_streak) {
        ESP_LOGW(TAG, "shooting-star object alloc failed — P6 streaks disabled (cosmetic)");
        return;
    }
    lv_obj_remove_style_all(s_streak);
    lv_obj_remove_flag(s_streak, LV_OBJ_FLAG_CLICKABLE);   /* never intercept touch events */
    lv_obj_remove_flag(s_streak, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_streak, LV_OBJ_FLAG_HIDDEN);         /* stays parked until the first streak fires */

    /* The comet is a thin horizontal bar with an opacity gradient — transparent tail on
     * the left, opaque head on the right — rotated per-streak to match its travel angle
     * (done in streak_launch). */
    lv_obj_set_size(s_streak, COMET_LEN, COMET_W);
    lv_obj_set_style_radius(s_streak, LV_RADIUS_CIRCLE, 0);     /* rounded, pill-shaped head and tail caps */
    s_streak_grad.dir = LV_GRAD_DIR_HOR;
    s_streak_grad.stops_count = 2;
    s_streak_grad.stops[0].color = NOCSIF_WHITE; s_streak_grad.stops[0].opa = LV_OPA_TRANSP; s_streak_grad.stops[0].frac = 0;   /* the transparent tail end */
    s_streak_grad.stops[1].color = NOCSIF_WHITE; s_streak_grad.stops[1].opa = LV_OPA_COVER;  s_streak_grad.stops[1].frac = 255; /* the opaque head end */
    lv_obj_set_style_bg_grad(s_streak, &s_streak_grad, 0);
    lv_obj_set_style_bg_opa(s_streak, LV_OPA_COVER, 0);
    /* rotate about the left-center point (the tail), so the object's position tracks the tail as it moves along the path */
    lv_obj_set_style_transform_pivot_x(s_streak, 0, 0);
    lv_obj_set_style_transform_pivot_y(s_streak, COMET_W / 2, 0);

    s_streak_timer = lv_timer_create(streak_timer_cb, streak_next_period(), NULL);
}
