/*
 * NocSif — static orrery/star background + rounded-corner masks (UI-shell P2). See ui_background.h.
 *
 * The orrery is a single 410x502 lv_canvas in PSRAM (M1 framebuffer precedent), drawn ONCE:
 *   - void ground (lv_canvas_fill_bg)
 *   - two orrery ring clusters bleeding off top-right (356,56) and bottom-left (44,476),
 *     concentric solid + dashed arcs (lv_draw_arc), colour #3C3C44 at ~0.11-0.24 opa
 *   - ~15 scattered gray star dots + 2 violet body dots (lv_draw_rect, circle radius)
 *   - 4 engraved celestial stars (nocsif_img_star A8, recoloured — one violet), scaled from
 *     the 64px master (lv_draw_image)
 * added as a child of lv_layer_bottom(); NEVER invalidated again. Every screen keeps a
 * transparent background so this shows through and persists across screen swaps. On a widget
 * redraw LVGL only re-blits the canvas region under the dirty rect — the vector draw above
 * runs exactly once (guarded), never per frame. Geometry/opacity lifted from ui-mockup.html
 * lines 141-165 (410x502 viewBox = device px 1:1).
 *
 * Colours are defined NORMALLY with the ui_theme tokens / lv_color_hex — the panel's BGR
 * element order is handled in the M3 flush config (display.c/ui.c); do NOT hand-swap bytes.
 * Canvas draw buffers stay in internal RAM (M3); this PSRAM canvas is only a COMPOSITING
 * source (CPU-read during blend), never DMA'd as a flush buffer, so it needs no bounce buffer.
 */
#include "ui_background.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>           /* snprintf for the per-wallpaper NVS keys */

#include "ui_theme.h"        /* tokens + nocsif_img_star extern */
#include "lvgl.h"
#include "esp_heap_caps.h"
#include "esp_random.h"      /* UI-shell P6: vary each shooting-star trajectory */
#include "esp_log.h"
#include "display.h"         /* UI-shell P6: nocsif_display_is_asleep (screen-on gate) */
#include "settings.h"        /* UI-shell P6: reduce_motion flag (P5b) */

static const char *TAG = "ui_bg";

/* §4.1 Theme — the render-once orrery canvas + its PSRAM buffer, kept so the wallpaper layer
 * toggles can repaint it at runtime. Each layer is independent; all off = a clean opaque void
 * (the canvas is never hidden — hiding the bottom layer left stale pixels). The accent-tinted
 * marks use a SEPARATE star colour so they can deliberately clash with the UI accent. */
static lv_obj_t  *s_orrery_canvas;
static uint8_t   *s_orrery_buf;
static size_t     s_orrery_stride;
static int        s_wp_style;                  /* the active nocsif_wallpaper_t */

/* Per-wallpaper settings (indexed by style): its own star colour, layer toggles, and comets flag.
 * s_wp_star_col mirrors s_wp_star_rgb as a cached lv_color_t for the paint path. */
static uint32_t   s_wp_star_rgb[NOCSIF_WP_COUNT];
static lv_color_t s_wp_star_col[NOCSIF_WP_COUNT];
static bool       s_wp_lyr[NOCSIF_WP_COUNT][NOCSIF_WP_MAX_LAYERS];
static bool       s_wp_comet[NOCSIF_WP_COUNT];

/* Human names for each style's layers (NULL past its count); the settings screen builds a toggle per name. */
static const char *const s_wp_lyr_name[NOCSIF_WP_COUNT][NOCSIF_WP_MAX_LAYERS] = {
    { "Rings", "Diamond stars", "Star dots" },        /* NOCSIF_WP_ORRERY   */
    { "Sunburst rays", "Coronae", "Star field" },     /* NOCSIF_WP_SUN      */
    { "Hexagram", "Tick ring", "Orbital circles" },   /* NOCSIF_WP_GRIMOIRE */
};
static const int s_wp_lyr_count[NOCSIF_WP_COUNT] = { 3, 3, 3 };

#define BG_W          410
#define BG_H          502
#define STAR_MASTER   64     /* nocsif_img_star master size (px) */
#define CORNER_R      40     /* rounded-corner mask radius (px, spec §4) */

/* Orrery ring colour (#3C3C44) and the two scattered-mark colours. */
#define RING_COLOR    lv_color_hex(0x3C3C44)
#define DOT_COLOR     lv_color_hex(0xB6B6BD)   /* gray star dots */

/* ---- direct-buffer rasterisers for the rings + dots ------------------------- *
 * We rasterise the faint rings/dots straight into the RGB888 canvas buffer instead of
 * lv_draw_arc. WHY (load-bearing perf finding): lv_draw_sw_arc builds a full anti-aliased
 * radius mask (lv_draw_sw_mask_radius_init/circ_calc_aa4) per arc and scans the whole
 * 2r x 2r bounding box — for the two dashed rings that is ~200 arc segments on r=106/118,
 * which takes tens of seconds on the LVGL task and trips the task watchdog. Plotting the
 * ~1px ring outline is O(circumference) and dashes are a trivial arc-length test.
 *
 * lv_canvas_set_px on an RGB888 (no-alpha) buffer OVERWRITES (it ignores opa) and invalidates
 * the whole canvas per call, so we skip it: we pre-blend the mark colour over the void ground
 * ourselves and write B,G,R straight to the buffer (LVGL's RGB888 buffer is B,G,R in memory,
 * M3). Rings/dots sit on the void ground (they rarely overlap each other), and the 4 stars are
 * drawn AFTER via lv_draw_image so they alpha-blend on top — so pre-blending over void is exact
 * where it matters. */

/* One channel of `mark` blended over `base` at `opa`, clamped to [0,255]. The clamp guards a
 * future mark colour with a channel darker than the void base (would otherwise underflow and
 * wrap on the uint8 cast); the current palette never does. */
static uint8_t blend_ch(int base, int mark, int opa)
{
    int v = base + (mark - base) * opa / 255;
    if (v < 0) v = 0;
    else if (v > 255) v = 255;
    return (uint8_t)v;
}

/* Pre-blend a mark colour over the NOCSIF_VOID ground (0x070708) at `opa`. */
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
    p[0] = b;  p[1] = g;  p[2] = r;    /* RGB888 buffer is B,G,R in memory (M3) */
}

/* 1px ring outline, optionally dashed (dash_on/dash_gap in px along the circumference;
 * dash_gap == 0 => solid). Oversampled ~1.3x so the outline stays contiguous. */
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
        float t = (float)i / (float)steps;               /* 0..1 around the circle */
        if (dash_gap > 0.0f && fmodf(t * circ, period) >= dash_on) {
            continue;                                    /* in a gap */
        }
        float ang = t * 2.0f * 3.14159265f;
        int x = cx + (int)lroundf((float)radius * cosf(ang));
        int y = cy + (int)lroundf((float)radius * sinf(ang));
        put_px(buf, stride, x, y, r, g, b);
    }
}

/* Small filled dot (diam px square — indistinguishable from a disc at 1-4 px). */
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

/* Engraved star: the A8 master recoloured (recolor = visible-pixel colour for A8),
 * scaled about its top-left so the on-screen top-left stays at (x,y) at `size` px. */
static void draw_star(lv_layer_t *layer, int x, int y, int size,
                      lv_color_t color, lv_opa_t opa)
{
    lv_draw_image_dsc_t d;
    lv_draw_image_dsc_init(&d);
    d.src         = &nocsif_img_star;
    d.recolor     = color;
    d.recolor_opa = LV_OPA_COVER;
    d.opa         = opa;
    int scale     = (size * LV_SCALE_NONE) / STAR_MASTER;   /* 256 = 1.0 */
    d.scale_x     = scale;
    d.scale_y     = scale;
    d.pivot.x     = 0;
    d.pivot.y     = 0;
    d.antialias   = 1;
    lv_area_t a = { x, y, x + STAR_MASTER - 1, y + STAR_MASTER - 1 };
    lv_draw_image(layer, &d, &a);
}

/* ---- scattered marks (from the mockup) ------------------------------------- */

typedef struct { int16_t x, y, diam; lv_opa_t opa; } dot_t;

/* 15 gray star dots (mockup lines 153-159): diam = round(2*r), opa = round(op*255). */
static const dot_t s_dots[] = {
    {  40,  70, 2, 102 }, { 120,  30, 2,  82 }, {  70, 150, 2,  71 },
    {  24, 300, 2,  71 }, { 150, 470, 2,  82 }, { 210, 420, 2,  61 },
    {  60, 430, 2,  71 }, { 185, 120, 1,  61 }, {  95, 255, 2,  61 },
    {  18, 205, 2,  61 }, { 270, 360, 2,  61 }, { 132, 360, 1,  56 },
    { 300, 470, 2,  61 }, {  45, 360, 1,  56 }, { 232, 205, 1,  51 },
};

/* 2 violet bodies riding a ring (mockup line 151). */
static const dot_t s_violet_bodies[] = {
    { 250,  56, 4, 153 },   /* r2   opa .6  */
    { 118, 476, 3, 115 },   /* r1.6 opa .45 */
};

typedef struct { int16_t x, y, size; lv_opa_t opa; uint8_t violet; } star_t;

/* 4 engraved celestial stars (mockup lines 161-164) — one violet (spec §6). */
static const star_t s_stars[] = {
    { 288, 150, 30, 102, 0 },   /* gray   opa .4  */
    {  58, 236, 26, 153, 1 },   /* VIOLET opa .6  */
    { 196,  30, 20,  87, 0 },   /* gray   opa .34 */
    { 330, 330, 17,  77, 0 },   /* gray   opa .3  */
};

/* ---- corner masks (lv_layer_top) ------------------------------------------ *
 * One ARGB8888 canvas per corner: opaque void in the sub-glass nub (outside the
 * rounded-rect arc), transparent inside so content/orrery show. 1px feather.
 *
 * The masks share lv_layer_top() with any global overlay (P8 v2.3 Control Center,
 * flashlight). Layer z-order is child-add order, so an overlay created AFTER the
 * masks would draw over the rounded corners (square nubs). We keep pointers so the
 * overlay code can re-raise the masks to the front after adding itself. */
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
    if (s_corner_mask_n < 4) s_corner_mask[s_corner_mask_n++] = cv;   /* keep for re-raise */
    lv_canvas_fill_bg(cv, NOCSIF_VOID, LV_OPA_TRANSP);   /* clear to transparent */

    for (int y = 0; y < CORNER_R; y++) {
        for (int x = 0; x < CORNER_R; x++) {
            float dx = (float)x - arc_cx;
            float dy = (float)y - arc_cy;
            float dist = sqrtf(dx * dx + dy * dy);
            float cov = dist - ((float)CORNER_R - 0.5f);   /* 1px feather at the arc */
            if (cov <= 0.0f) continue;                      /* inside glass: transparent */
            if (cov > 1.0f) cov = 1.0f;
            lv_canvas_set_px(cv, x, y, NOCSIF_VOID, (lv_opa_t)(cov * 255.0f));
        }
    }
    return ESP_OK;
}

/* Re-raise the four corner masks to the front of lv_layer_top(). Call after adding a
 * global top-layer overlay (Control Center / flashlight) so the physically-rounded glass
 * stays masked over it. No-op before the masks are built. */
void nocsif_ui_background_raise_corner_masks(void)
{
    for (int i = 0; i < s_corner_mask_n; i++) {
        if (s_corner_mask[i]) lv_obj_move_foreground(s_corner_mask[i]);
    }
}

/* --------------------------------------------------------------------------- */

/* Draw a thin 1px line into the canvas buffer a pixel at a time (plot_dot pre-blends each pixel over
 * the void ground). One-time render only, so the per-pixel cost is irrelevant. */
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

/* ---- wallpaper styles (each paints ONTO the already-void-filled canvas) --------------------- *
 * Each reads its OWN star colour + layer toggles (per-wallpaper), so the Star colour and the
 * layer switches on a wallpaper's settings screen drive only that wallpaper. */
static void wp_orrery(void)
{
    lv_color_t star = s_wp_star_col[NOCSIF_WP_ORRERY];
    if (s_wp_lyr[NOCSIF_WP_ORRERY][0]) {   /* Rings */
        /* Orrery ring cluster A — top-right (356,56). */
        plot_ring(s_orrery_buf, s_orrery_stride, 356, 56, 152, RING_COLOR, 56, 0.0f, 0.0f);   /* solid  */
        plot_ring(s_orrery_buf, s_orrery_stride, 356, 56, 106, RING_COLOR, 61, 1.5f, 5.0f);   /* dashed */
        plot_ring(s_orrery_buf, s_orrery_stride, 356, 56,  62, RING_COLOR, 41, 0.0f, 0.0f);   /* solid  */
        /* Orrery ring cluster B — bottom-left (44,476). */
        plot_ring(s_orrery_buf, s_orrery_stride,  44, 476, 118, RING_COLOR, 41, 2.0f, 6.0f);  /* dashed */
        plot_ring(s_orrery_buf, s_orrery_stride,  44, 476,  72, RING_COLOR, 28, 0.0f, 0.0f);   /* solid  */
    }
    if (s_wp_lyr[NOCSIF_WP_ORRERY][2]) {   /* Star dots (gray dots + accent orbital bodies) */
        for (size_t i = 0; i < sizeof(s_dots) / sizeof(s_dots[0]); i++)
            plot_dot(s_orrery_buf, s_orrery_stride, s_dots[i].x, s_dots[i].y, s_dots[i].diam, DOT_COLOR, s_dots[i].opa);
        for (size_t i = 0; i < sizeof(s_violet_bodies) / sizeof(s_violet_bodies[0]); i++)
            plot_dot(s_orrery_buf, s_orrery_stride, s_violet_bodies[i].x, s_violet_bodies[i].y,
                     s_violet_bodies[i].diam, star, s_violet_bodies[i].opa);
    }
    if (s_wp_lyr[NOCSIF_WP_ORRERY][1]) {   /* Diamond stars (one accent) */
        lv_layer_t layer;
        lv_canvas_init_layer(s_orrery_canvas, &layer);
        for (size_t i = 0; i < sizeof(s_stars) / sizeof(s_stars[0]); i++) {
            lv_color_t c = s_stars[i].violet ? star : DOT_COLOR;
            draw_star(&layer, s_stars[i].x, s_stars[i].y, s_stars[i].size, c, s_stars[i].opa);
        }
        lv_canvas_finish_layer(s_orrery_canvas, &layer);
    }
}

/* Engraved Sun (Radiant disc) — a sunburst of rays + concentric coronae + a scattered star field. The
 * per-wallpaper "star" colour tints the SUN (rays + coronae); the star field stays neutral gray. */
static void wp_sun(void)
{
    lv_color_t sun = s_wp_star_col[NOCSIF_WP_SUN];
    const int cx = 205, cy = 205;
    if (s_wp_lyr[NOCSIF_WP_SUN][0]) {   /* Sunburst rays — 24 spokes, alternating length */
        for (int i = 0; i < 24; i++) {
            float a = (float)i * 15.0f * 3.14159265f / 180.0f;
            float l = (i & 1) ? 58.0f : 70.0f;
            int x0 = cx + (int)lroundf(48.0f * cosf(a)), y0 = cy + (int)lroundf(48.0f * sinf(a));
            int x1 = cx + (int)lroundf(l    * cosf(a)), y1 = cy + (int)lroundf(l    * sinf(a));
            plot_line(s_orrery_buf, s_orrery_stride, x0, y0, x1, y1, sun, 120);
        }
    }
    if (s_wp_lyr[NOCSIF_WP_SUN][1]) {   /* Coronae — the disc + an inner ring */
        plot_ring(s_orrery_buf, s_orrery_stride, cx, cy, 46, sun, 150, 0.0f, 0.0f);
        plot_ring(s_orrery_buf, s_orrery_stride, cx, cy, 34, sun,  92, 0.0f, 0.0f);
    }
    if (s_wp_lyr[NOCSIF_WP_SUN][2]) {   /* Star field (neutral gray dots) */
        for (size_t i = 0; i < sizeof(s_dots) / sizeof(s_dots[0]); i++)
            plot_dot(s_orrery_buf, s_orrery_stride, s_dots[i].x, s_dots[i].y, s_dots[i].diam, DOT_COLOR, s_dots[i].opa);
    }
}

/* Grimoire — a mystical seal: concentric orbital circles + a hexagram (two overlapping triangles) + a
 * 36-tick outer ring, all in the neutral engraved gray (the SIGIL STAYS COLORLESS). The per-wallpaper
 * "star" colour tints ONLY the accent centre star; a few gray dots scatter the field. */
static void wp_grimoire(void)
{
    lv_color_t star = s_wp_star_col[NOCSIF_WP_GRIMOIRE];
    const int cx = 205, cy = 240;
    if (s_wp_lyr[NOCSIF_WP_GRIMOIRE][2]) {   /* Orbital circles */
        plot_ring(s_orrery_buf, s_orrery_stride, cx, cy, 150, RING_COLOR, 82, 0.0f, 0.0f);
        plot_ring(s_orrery_buf, s_orrery_stride, cx, cy, 126, RING_COLOR, 60, 1.0f, 7.0f);
        plot_ring(s_orrery_buf, s_orrery_stride, cx, cy,  70, RING_COLOR, 72, 0.0f, 0.0f);
    }
    if (s_wp_lyr[NOCSIF_WP_GRIMOIRE][0]) {   /* Hexagram — two overlapping triangles */
        plot_line(s_orrery_buf, s_orrery_stride, cx,     cy - 70, cx + 61, cy + 35, RING_COLOR, 85);  /* up  */
        plot_line(s_orrery_buf, s_orrery_stride, cx + 61, cy + 35, cx - 61, cy + 35, RING_COLOR, 85);
        plot_line(s_orrery_buf, s_orrery_stride, cx - 61, cy + 35, cx,      cy - 70, RING_COLOR, 85);
        plot_line(s_orrery_buf, s_orrery_stride, cx,     cy + 70, cx + 61, cy - 35, RING_COLOR, 85);  /* down */
        plot_line(s_orrery_buf, s_orrery_stride, cx + 61, cy - 35, cx - 61, cy - 35, RING_COLOR, 85);
        plot_line(s_orrery_buf, s_orrery_stride, cx - 61, cy - 35, cx,      cy + 70, RING_COLOR, 85);
    }
    if (s_wp_lyr[NOCSIF_WP_GRIMOIRE][1]) {   /* Tick ring — 36 ticks just inside r150 */
        for (int i = 0; i < 36; i++) {
            float a = (float)i * 10.0f * 3.14159265f / 180.0f;
            int x0 = cx + (int)lroundf(150.0f * cosf(a)), y0 = cy + (int)lroundf(150.0f * sinf(a));
            int x1 = cx + (int)lroundf(143.0f * cosf(a)), y1 = cy + (int)lroundf(143.0f * sinf(a));
            plot_line(s_orrery_buf, s_orrery_stride, x0, y0, x1, y1, RING_COLOR, 80);
        }
    }
    /* scattered gray dots (always) */
    static const dot_t gdots[] = {
        { 60, 90, 2, 72 }, { 350, 120, 2, 72 }, { 330, 430, 2, 80 },
        { 70, 420, 2, 64 }, { 205, 40, 2, 60 }, { 300, 470, 2, 60 },
    };
    for (size_t i = 0; i < sizeof(gdots) / sizeof(gdots[0]); i++)
        plot_dot(s_orrery_buf, s_orrery_stride, gdots[i].x, gdots[i].y, gdots[i].diam, DOT_COLOR, gdots[i].opa);
    /* centre accent star — the ONE coloured mark (26 px, centred at cx,cy) */
    {
        lv_layer_t layer;
        lv_canvas_init_layer(s_orrery_canvas, &layer);
        draw_star(&layer, cx - 13, cy - 13, 26, star, 150);
        lv_canvas_finish_layer(s_orrery_canvas, &layer);
    }
}

/* Paint the wallpaper: an always-opaque void ground + the selected style's marks. Runs once at init
 * and on any style / layer / star-colour change (a one-off vector pass on the LVGL task, never per frame). */
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

    /* PSRAM canvas buffer (RGB888). lv_canvas_set_buffer computes the stride itself
     * via lv_draw_buf_width_to_stride, so match that for the allocation size. */
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
    lv_obj_remove_flag(canvas, LV_OBJ_FLAG_CLICKABLE);   /* never intercept touch */

    /* §4.1 Theme — stash the canvas/buffer so the wallpaper layer toggles can repaint it later,
     * then paint the saved layers + star colour. The canvas stays visible always (an opaque void
     * fill is the base) — all layers off is a clean plain-void ground. */
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

    /* Four rounded-corner masks (spec §4). Arc centre = the inner corner of each CORNER_R
     * square; the outer nub (dist > R) is painted opaque void. Attempt all four (so three
     * still get masked if one alloc fails under PSRAM pressure) and log honestly. A mask
     * failure is only a COSMETIC degradation (that corner's nub left unmasked) — it does NOT
     * fail the call: the orrery ground is fully drawn, and returning an error here would make
     * the caller keep an opaque screen and hide the whole orrery. So we return ESP_OK as long
     * as the canvas (the ground) drew; the return code means "ground usable", not "all perfect". */
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

    drawn = true;   /* the one-time orrery vector pass ran; never repeat it */
    size_t heap_after = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "orrery drawn ONCE into lv_layer_bottom: %dx%d RGB888 canvas in PSRAM "
                  "(%u B, stride %u) + %d/4 corner masks; PSRAM free %u -> %u",
             BG_W, BG_H, (unsigned)buf_sz, (unsigned)stride,
             masks_ok, (unsigned)heap_before, (unsigned)heap_after);
    return ESP_OK;   /* ground is up; corner-mask failures are cosmetic + logged above */
}

/* §4.1 Theme — wallpaper layer toggles + star colour. Repaint = re-run the vector pass into the
 * (always-visible) canvas + invalidate to recomposite the bottom layer. No-op before the canvas
 * exists. All run on the LVGL task (a settings-row tap or the colour picker). */
static void bg_repaint(void)
{
    if (!s_orrery_canvas) {
        return;
    }
    paint_wallpaper();
    lv_obj_invalidate(s_orrery_canvas);
}

/* Per-wallpaper NVS key: "wp<style>.<suffix>" (e.g. wp0.star, wp0.l0, wp0.com) — all <= 15 chars. */
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
    /* no repaint — comets are a live overlay, checked each streak beat */
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

/* ---- P6 occasional shooting stars (spec §7.2) ------------------------------ *
 * A rare (~every 1.5-4 min), brief (~1.6 s) comet across the star field: a thin bar with an opacity
 * gradient (clear tail -> bright head), rotated to the travel angle, on lv_layer_bottom — ABOVE the
 * render-once orrery canvas, BELOW the transparent screens, so it shows through the UI in the star
 * field — that translates diagonally with a fade in/out. Screen-ON only and OFF under the P5b
 * reduced-motion flag. Parked hidden between streaks (no render/invalidate); a streak is a brief
 * burst of full-frame flushes only while visible, like a nav slide. esp_random() varies the
 * trajectory each time so no two look identical. */
static lv_obj_t     *s_streak;
static lv_timer_t   *s_streak_timer;
static lv_grad_dsc_t s_streak_grad;   /* opacity gradient (clear tail -> bright head); style stores a ptr, so static */
static int32_t       s_streak_x0, s_streak_y0, s_streak_x1, s_streak_y1;

#define COMET_LEN        58        /* comet length tail->head, px */
#define COMET_W          4         /* comet thickness, px         */
#define STREAK_DUR_MS    1600      /* traverse time; larger = slower glide across the screen */
/* NOCSIF_STREAK_TEST=1 makes streaks frequent (~8-16 s) so the effect is verifiable on-device
 * without waiting minutes. Set to 0 for the spec cadence (~1.5-4 min) — the shipped value. */
#define NOCSIF_STREAK_TEST 0
#if NOCSIF_STREAK_TEST
#define STREAK_MIN_MS    8000
#define STREAK_MAX_MS    16000
#else
#define STREAK_MIN_MS    90000     /* 1.5 min */
#define STREAK_MAX_MS    240000    /* 4 min   */
#endif
#define STREAK_PEAK_OPA  220       /* subtle highlight, not a full-white flash */

static uint32_t streak_next_period(void)
{
    return STREAK_MIN_MS + (esp_random() % (STREAK_MAX_MS - STREAK_MIN_MS));
}

/* anim var = the line, value 0..1000: lerp the object position start->end and apply a fade-in / hold
 * / fade-out opacity envelope (so it appears mid-field and fades before the edge, not a hard pop). */
static void streak_anim_cb(void *var, int32_t p)
{
    lv_obj_t *s = (lv_obj_t *)var;
    int32_t x = s_streak_x0 + (s_streak_x1 - s_streak_x0) * p / 1000;
    int32_t y = s_streak_y0 + (s_streak_y1 - s_streak_y0) * p / 1000;
    lv_obj_set_pos(s, x, y);

    lv_opa_t opa;
    if (p < 250)      opa = (lv_opa_t)(STREAK_PEAK_OPA * p / 250);          /* fade in  */
    else if (p > 550) opa = (lv_opa_t)(STREAK_PEAK_OPA * (1000 - p) / 450); /* fade out */
    else              opa = STREAK_PEAK_OPA;
    lv_obj_set_style_opa(s, opa, 0);
}

static void streak_done_cb(lv_anim_t *a)
{
    (void)a;
    if (s_streak) {
        lv_obj_add_flag(s_streak, LV_OBJ_FLAG_HIDDEN);   /* park: no render/invalidate between streaks */
    }
}

static void streak_launch(void)
{
    if (!s_streak) return;
    uint32_t rnd = esp_random();
    int dir = (rnd & 1) ? -1 : 1;                          /* +1 down-right, -1 down-left */
    int sx  = (dir > 0) ? -30 + (int)((rnd >> 1) % 90)     /* enter from a top corner region */
                        :  350 - (int)((rnd >> 1) % 90);
    int sy  = -30 + (int)((rnd >> 9) % 80);                /* start at/above the top edge */
    int dx  = dir * 300, dy = 380;                         /* diagonal travel across the panel */

    /* Orient the comet along the travel direction: rotate the gradient bar (clear tail -> bright
     * head) to the travel angle. +y is down, so atan2(dy,dx) is the clockwise screen angle; the
     * bar's opaque end (local +x) then points along travel, so the head leads and the tail trails.
     * A rotated rect has no negative-point clipping, so both directions render identically. */
    float ang_deg = atan2f((float)dy, (float)dx) * 57.2957795f;         /* rad -> deg */
    lv_obj_set_style_transform_rotation(s_streak, (int32_t)(ang_deg * 10.0f), 0);   /* 0.1 deg units */

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
    lv_anim_set_path_cb(&a, lv_anim_path_linear);          /* constant speed (a streak, not an ease) */
    lv_anim_set_ready_cb(&a, streak_done_cb);
    lv_anim_start(&a);
}

static void streak_timer_cb(lv_timer_t *t)
{
    lv_timer_set_period(t, streak_next_period());          /* fresh random interval each time */
    if (nocsif_display_is_asleep()) return;                /* screen-ON only (spec §7.2) */
    if (nocsif_settings_get_i32("reduce_motion", 0)) return;   /* OFF under reduced motion (P5b) */
    if (!s_wp_comet[s_wp_style]) return;  /* §4.1 Theme: per-wallpaper comets toggle */
    streak_launch();
}

void nocsif_ui_background_shooting_stars_init(void)
{
    if (s_streak) return;                                  /* once */
    s_streak = lv_obj_create(lv_layer_bottom());           /* above the orrery canvas, below the screens */
    if (!s_streak) {
        ESP_LOGW(TAG, "shooting-star object alloc failed — P6 streaks disabled (cosmetic)");
        return;
    }
    lv_obj_remove_style_all(s_streak);
    lv_obj_remove_flag(s_streak, LV_OBJ_FLAG_CLICKABLE);   /* never intercept touch */
    lv_obj_remove_flag(s_streak, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_streak, LV_OBJ_FLAG_HIDDEN);         /* parked until the first streak */

    /* The comet is a thin horizontal bar with an OPACITY gradient — transparent tail (left) to
     * opaque head (right) — rotated per streak to the travel angle (streak_launch). */
    lv_obj_set_size(s_streak, COMET_LEN, COMET_W);
    lv_obj_set_style_radius(s_streak, LV_RADIUS_CIRCLE, 0);     /* rounded (pill) head/tail caps */
    s_streak_grad.dir = LV_GRAD_DIR_HOR;
    s_streak_grad.stops_count = 2;
    s_streak_grad.stops[0].color = NOCSIF_WHITE; s_streak_grad.stops[0].opa = LV_OPA_TRANSP; s_streak_grad.stops[0].frac = 0;   /* tail */
    s_streak_grad.stops[1].color = NOCSIF_WHITE; s_streak_grad.stops[1].opa = LV_OPA_COVER;  s_streak_grad.stops[1].frac = 255; /* head */
    lv_obj_set_style_bg_grad(s_streak, &s_streak_grad, 0);
    lv_obj_set_style_bg_opa(s_streak, LV_OPA_COVER, 0);
    /* Rotate about the left-centre (the tail) so the object position tracks the tail along the path. */
    lv_obj_set_style_transform_pivot_x(s_streak, 0, 0);
    lv_obj_set_style_transform_pivot_y(s_streak, COMET_W / 2, 0);

    s_streak_timer = lv_timer_create(streak_timer_cb, streak_next_period(), NULL);
}
