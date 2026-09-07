/*
 * NocSif — menu-first navigation shell: screen stack + reusable widgets (UI-shell P3).
 * See ui_nav.h.
 *
 * Mechanism only (no app/screen knowledge — ui.c builds the screens). The screen STACK
 * animates with a CUSTOM transition (slide_in): lv_screen_load() swaps instantly, then one
 * lv_anim drives lv_obj_set_x (a small ±30px directional slide, ease-out) + a plain
 * LV_STYLE_OPA fade, NAV_ANIM_MS. This replaces lv_screen_load_anim's full-screen MOVE:
 * translate_x is inert on a screen root, and a 410px MOVE is coarse at the panel's
 * full-frame flush rate whereas a ~30px set_x + fade reads smooth (spec §5). Screens are
 * transparent-root so the P2 orrery on lv_layer_bottom() shows through and is never
 * invalidated; corner masks stay on lv_layer_top().
 *
 * All entry points run on the LVGL task (build under the port lock in nocsif_ui_init;
 * nav_push/back from LVGL event callbacks with the lock held) — no extra locking here.
 *
 * LVGL 9.3 APIs verified against the pinned managed_components/lvgl__lvgl headers:
 * lv_screen_load / lv_screen_load_anim (+ LV_SCR_LOAD_ANIM_MOVE_LEFT/RIGHT), lv_obj_create(NULL)
 * detached screen, LV_STATE_DISABLED, lv_canvas_set_px/fill_bg, lv_draw_buf_width_to_stride.
 */
#include "ui_nav.h"

#include <stdint.h>
#include <string.h>

#include "display.h"        /* NOCSIF_DISP_W / NOCSIF_DISP_H */
#include "ui_theme.h"       /* tokens, fonts, shared styles */
#include "rtc.h"            /* nocsif_rtc_clock_str (header clock live data, P4.2) */
#include "power.h"          /* nocsif_power_batt_str (header battery live data, P4.3) */
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "ui_nav";

#define NAV_STACK_MAX   8               /* Home + drilldowns; boot heap is ~8.5 MB */
#define HEADER_INSET    20              /* dashed rule inset each side (spec §4)   */
#define LIST_INSET      26              /* list horizontal safe inset (spec §4)    */
/* Header content sits at the TOP row, inside the rounded-glass corner arcs — the
 * 24-26px safe inset isn't enough there (on-device: the title's top, clock/battery,
 * and back arrow were clipped by the physical curve). Push header content further in
 * and down so it clears a ~56px corner radius (mockup .screen border-radius). */
/* Corner clearance: the physical panel clips a ~56px rounded corner (mockup .screen border-radius,
 * and it clips a touch MORE than the software CORNER_R=40 mask). The header's corner elements — the
 * top-left back arrow and the top-RIGHT battery — sit exactly at these insets, so anything short of
 * the corner radius nips them (the battery was still clipped at 48/42). Give both a full corner-
 * radius of clearance so the corner cluster clears the arc. Applies to every screen's header.
 * See docs/LESSONS.md (rounded-corner clip). */
#define HEADER_HINSET   56              /* header horizontal inset (>= corner radius) */
#define HEADER_TOP_PAD  56              /* header top pad (clear the corner arc)      */
#define ROW_MIN_H       44              /* min tap target (spec §4)                */
#define ROW_VPAD        15              /* row vertical padding (mockup .row 15px) */
#define ROW_ICON_GAP    16              /* icon->name gap (spec §4)                */
#define ROW_ICON_W      24              /* fixed icon column width (name alignment)*/
/* Directional slide (spec §5). 0 = DISABLED (jank fix): a full-screen frame composites in
 * ~105 ms (~8-9 fps, orrery re-blit bound — measured 2026-08-12), and slide_in pre-sets the
 * root to its dim+offset START state before load, so the first rendered frame ALWAYS shows
 * that dim/offset for one ~105 ms frame before snapping — which reads as a laggy fade-in no
 * matter how short the duration. So we skip the animation and load each screen at rest
 * (instant, full opacity). Re-enable with a value > 0 once the background rework lifts the
 * full-frame rate enough for a slide to look smooth rather than step. Tunable on-device. */
#define NAV_ANIM_MS     0
/* Slide DISTANCE. The design mockup's transition is a SUBTLE directional translate
 * (translateX(±26px) + fade), NOT a full-screen slide. On this panel every animation
 * frame re-flushes the whole 410x502 frame over QSPI (~30-50 fps ceiling), so a
 * full-screen 410px slide steps ~coarsely and reads as janky; a small ~30px translate
 * steps ~5px/frame and reads smooth at the SAME frame rate. Matches the mockup. */
#define NAV_SLIDE_PX    30

/* A screen root carrying this flag is PINNED: nocsif_nav_back will not free it when
 * it is popped (set via nocsif_nav_pin — for screens that own lv_timers/live state). */
#define NAV_FLAG_PIN    LV_OBJ_FLAG_USER_1

/* Nav stack of screen roots. s_stack[0] is Home; s_sp indexes the top. */
static lv_obj_t *s_stack[NAV_STACK_MAX];
static int       s_sp = -1;

/* Slide-in start offset for the in-flight transition (one at a time, LVGL task). */
static int32_t   s_slide_from;

/* ---- live-data label registry (P4 header clock/battery + Time readout) ------ *
 * Labels that show periodically-updated data register here with a getter; the single
 * header-tick timer refreshes them update-on-change. Sized for the live set: each of up to
 * NAV_STACK_MAX stacked headers can carry a clock, a battery, and the P4.5.4 status badge, plus
 * the Time screen's readout — a generous cap that never grows unbounded (a label frees its slot
 * on delete). Bumped from *2 to *3 when the badge became a third per-header live label. */
#define LIVE_LABELS_MAX  (NAV_STACK_MAX * 3 + 4)
typedef struct { lv_obj_t *label; nocsif_live_getter_t getter; } live_label_t;
static live_label_t s_live[LIVE_LABELS_MAX];

/* Optional app-supplied header status-badge getter (P4.5.4). NULL until nocsif_nav_set_header_badge;
 * when set, build_header adds a badge label bound to it (hidden while it returns ""). */
static nocsif_live_getter_t s_badge_getter;

/* P8 v2.3/2.7 — header swipe-down -> Control Center. header_drag_cb fires the registered "begin" callback
 * ONCE when a downward, vertical-dominant drag that started on the (non-scrolling) header clears the
 * threshold; that callback (ui.c: cc_open) snaps the shade open. The move/end callbacks are unused now (the
 * old finger-follow was dropped — it felt glitchy). One touch, so a single module-static start point is enough. */
#define NAV_PULL_START  30            /* px of downward travel before the swipe-open commits */
static nocsif_drag_begin_cb_t s_drag_begin;
static nocsif_drag_move_cb_t  s_drag_move;
static nocsif_drag_end_cb_t   s_drag_end;
static int32_t                s_pull_x0, s_pull_y0;
static bool                   s_pull_active;

/* Clear a label's slot when it is deleted (its screen freed on nav-back), so the tick never
 * writes a dangling widget. Runs on the LVGL task before the label is torn down. */
static void live_label_deleted_cb(lv_event_t *e)
{
    lv_obj_t *lbl = lv_event_get_target(e);
    for (int i = 0; i < LIVE_LABELS_MAX; i++) {
        if (s_live[i].label == lbl) {
            s_live[i].label = NULL;
            s_live[i].getter = NULL;
            return;
        }
    }
}

void nocsif_nav_register_live_label(lv_obj_t *label, nocsif_live_getter_t getter)
{
    if (label == NULL || getter == NULL) {
        return;
    }
    for (int i = 0; i < LIVE_LABELS_MAX; i++) {
        if (s_live[i].label == NULL) {
            s_live[i].label = label;
            s_live[i].getter = getter;
            lv_obj_add_event_cb(label, live_label_deleted_cb, LV_EVENT_DELETE, NULL);
            return;
        }
    }
    ESP_LOGW(TAG, "live-label registry full (%d) — label not tracked", LIVE_LABELS_MAX);
}

void nocsif_nav_set_header_badge(nocsif_live_getter_t getter)
{
    s_badge_getter = getter;   /* takes effect on the next header built (call before nav_init) */
}

void nocsif_nav_set_top_drag_cbs(nocsif_drag_begin_cb_t begin,
                                 nocsif_drag_move_cb_t move,
                                 nocsif_drag_end_cb_t end)
{
    s_drag_begin = begin; s_drag_move = move; s_drag_end = end;   /* takes effect on the next header */
}

/* Header PRESSED/PRESSING/RELEASED router: a vertical-dominant downward drag that STARTS on the
 * header drives the Control-Center open, card following the finger. Horizontal swipe-back is left to
 * the screen's gesture handler — this only engages on dy > |dx|, so the two never collide. */
static void header_drag_cb(lv_event_t *e)
{
    if (s_drag_begin == NULL) return;
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_active();
    if (indev == NULL) return;
    lv_point_t pt; lv_indev_get_point(indev, &pt);

    if (code == LV_EVENT_PRESSED) {
        s_pull_x0 = pt.x; s_pull_y0 = pt.y; s_pull_active = false;
        return;
    }
    if (code == LV_EVENT_PRESSING) {
        int32_t dy = pt.y - s_pull_y0, dx = pt.x - s_pull_x0;
        if (s_pull_active || (dy > NAV_PULL_START && dy > (dx < 0 ? -dx : dx))) {
            if (!s_pull_active) { s_pull_active = true; s_drag_begin(); }
            if (s_drag_move) s_drag_move(dy);
        }
        return;
    }
    if (code == LV_EVENT_RELEASED) {
        if (s_pull_active && s_drag_end) s_drag_end(pt.y - s_pull_y0);
        s_pull_active = false;
    }
}

void nocsif_nav_header_tick(void)
{
    for (int i = 0; i < LIVE_LABELS_MAX; i++) {
        if (s_live[i].label == NULL) {
            continue;
        }
        const char *s = s_live[i].getter();
        if (s == NULL) {
            continue;
        }
        lv_obj_t *lbl = s_live[i].label;
        /* Empty getter => nothing to say: HIDE the label so it takes no header space (the badge
         * collapses when nothing is armed; clock/battery never return "", so they are unaffected).
         * Toggling hidden invalidates once, on the arm/disarm edge only — acceptable. */
        if (s[0] == '\0') {
            if (!lv_obj_has_flag(lbl, LV_OBJ_FLAG_HIDDEN)) {
                lv_obj_add_flag(lbl, LV_OBJ_FLAG_HIDDEN);
            }
            continue;
        }
        if (lv_obj_has_flag(lbl, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_remove_flag(lbl, LV_OBJ_FLAG_HIDDEN);
        }
        /* Update-on-change ONLY: lv_label_set_text always reallocs + invalidates, and under
         * full_refresh every invalidation repaints the whole 410x502 frame — so a same-value
         * write would burn a full flush per tick for nothing. Invalidations on an inactive
         * (loaded-but-not-shown) screen, e.g. pinned Home under a submenu, are dropped by LVGL
         * until it becomes active again, so idle labels cost nothing. */
        if (strcmp(lv_label_get_text(lbl), s) != 0) {
            lv_label_set_text(lbl, s);
        }
    }
}

/* ---- navigation ------------------------------------------------------------ */

/* One animation drives BOTH the position (set_x) and a subtle opacity fade, keyed on
 * progress p (0..256). CRITICAL: on a screen root (parent==NULL) lv_obj_set_style_
 * translate_x is INERT — only lv_obj_set_x moves a screen (lv_obj_pos.c:621, and LVGL's
 * own MOVE anim uses set_x). The fade is a plain LV_STYLE_OPA (NON-layered: calculate_
 * layer_type only layers for opa_LAYERED/transform/mask/blend), so it costs the same per
 * frame — no 617KB intermediate layer — while masking positional stepping at the panel's
 * ~25-35 fps full-frame flush rate. */
static void slide_exec_cb(void *var, int32_t p)
{
    lv_obj_t *root = (lv_obj_t *)var;
    lv_obj_set_x(root, s_slide_from - (s_slide_from * p) / 256);       /* from_x -> 0 */
    lv_obj_set_style_opa(root, (lv_opa_t)(150 + (105 * p) / 256), 0);  /* 150 -> 255 */
}

/* Pin the settled state (x=0, full opacity) and drop the local opa override. */
static void slide_completed_cb(lv_anim_t *a)
{
    lv_obj_t *root = (lv_obj_t *)lv_anim_get_user_data(a);
    lv_obj_set_x(root, 0);
    lv_obj_remove_local_style_prop(root, LV_STYLE_OPA, 0);
}

/* Swap to `root` instantly (old screen unloads), then slide it in a small directional
 * distance (mockup's translateX(±30px) + fade). A full-screen 410px slide is coarse at
 * the panel's frame rate; a ~30px set_x + fade reads smooth at the SAME rate. */
static void slide_in(lv_obj_t *root, int32_t from_x)
{
    lv_anim_delete(NULL, slide_exec_cb);            /* cancel any in-flight slide */
    if (NAV_ANIM_MS <= 0) {
        /* Animation disabled (jank fix): load at rest — no dim/offset start frame, so no
         * laggy fade-in at the current ~8-9 fps. Clear any leftover slide state first. */
        lv_obj_set_x(root, 0);
        lv_obj_remove_local_style_prop(root, LV_STYLE_OPA, 0);
        lv_screen_load(root);
        return;
    }
    s_slide_from = from_x;
    lv_obj_set_x(root, from_x);                     /* pre-offset + pre-dim before it shows */
    lv_obj_set_style_opa(root, 150, 0);
    lv_screen_load(root);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, root);
    lv_anim_set_user_data(&a, root);               /* completed_cb reads this */
    lv_anim_set_exec_cb(&a, slide_exec_cb);
    lv_anim_set_values(&a, 0, 256);
    lv_anim_set_duration(&a, NAV_ANIM_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);   /* decelerate into rest — no hard stop */
    lv_anim_set_completed_cb(&a, slide_completed_cb);
    lv_anim_start(&a);
}

void nocsif_nav_init(lv_obj_t *home_root)
{
    s_stack[0] = home_root;
    s_sp = 0;
    lv_screen_load(home_root);           /* active, no animation */
}

void nocsif_nav_push(lv_obj_t *root)
{
    if (root == NULL || s_sp < 0 || s_sp + 1 >= NAV_STACK_MAX) {
        return;
    }
    s_stack[++s_sp] = root;
    slide_in(root, NAV_SLIDE_PX);        /* forward: enter from the right */
}

void nocsif_nav_pin(lv_obj_t *root)
{
    if (root) {
        lv_obj_add_flag(root, NAV_FLAG_PIN);
    }
}

bool nocsif_nav_at_root(void)
{
    return s_sp <= 0;
}

lv_obj_t *nocsif_nav_top(void)
{
    return (s_sp >= 0) ? s_stack[s_sp] : NULL;
}

void nocsif_nav_pop_to_root(void)
{
    if (s_sp <= 0) {
        return;                          /* already at Home */
    }
    /* Load Home (active) + slide it in from the left, THEN free the screens above it —
     * mirrors nocsif_nav_back's "parent loaded before the child is deleted" ordering, so
     * no active screen is ever deleted. Pinned screens (live timers/state) are kept, like
     * nav_back; their registry cache pointer survives so a re-launch reuses them. */
    slide_in(s_stack[0], -NAV_SLIDE_PX);
    for (int i = s_sp; i >= 1; i--) {
        lv_obj_t *scr = s_stack[i];
        s_stack[i] = NULL;
        if (scr != NULL && !lv_obj_has_flag(scr, NAV_FLAG_PIN)) {
            lv_obj_delete(scr);
        }
    }
    s_sp = 0;
}

void nocsif_nav_back(void)
{
    if (s_sp <= 0) {
        return;                          /* Home has no back */
    }
    lv_obj_t *leaving = s_stack[s_sp];        /* the screen being popped */
    s_sp--;
    slide_in(s_stack[s_sp], -NAV_SLIDE_PX);   /* back: load + slide the parent from the left */

    /* Free the popped screen (unless pinned) so cached widgets don't pile up in LVGL's
     * FIXED object pool — which is separate from the system heap and small (48 KB).
     * Unbounded caching ran it dry after a few screens: the next render's glyph/draw-buf
     * allocation failed and the LVGL task span in lv_draw_label (a hang, not a crash).
     * The parent is already loaded above, so `leaving` is inactive and safe to delete;
     * slide_in() just cancelled any in-flight slide on it. Its LV_EVENT_DELETE clears the
     * app-registry cache (ui.c) so a re-launch rebuilds it, and frees its dashed-rule
     * buffer. Only the pinned Home screen (stack[0]) is kept. */
    if (leaving != NULL && !lv_obj_has_flag(leaving, NAV_FLAG_PIN)) {
        s_stack[s_sp + 1] = NULL;
        lv_obj_delete(leaving);
    }
}

/* §4.14 — the optional back interceptor (see ui_nav.h): one hook, bound to the screen that installed it,
 * dropped on that screen's delete. Only the USER back paths below consult it. */
static nocsif_back_hook_t s_back_hook;
static lv_obj_t          *s_back_hook_scr;

static void back_hook_deleted_cb(lv_event_t *e)
{
    if (lv_event_get_target(e) == s_back_hook_scr) { s_back_hook = NULL; s_back_hook_scr = NULL; }
}

void nocsif_nav_set_back_hook(lv_obj_t *scr, nocsif_back_hook_t hook)
{
    if (scr == NULL) return;
    s_back_hook     = hook;
    s_back_hook_scr = scr;
    lv_obj_add_event_cb(scr, back_hook_deleted_cb, LV_EVENT_DELETE, NULL);
}

/* A user-initiated back: the active screen's hook gets first refusal, then the normal pop. */
static void nav_back_user(void)
{
    if (s_back_hook != NULL && s_sp >= 0 && s_stack[s_sp] == s_back_hook_scr && s_back_hook()) {
        return;                                            /* consumed by the screen */
    }
    nocsif_nav_back();
}

/* ---- event callbacks ------------------------------------------------------- */

static void back_click_cb(lv_event_t *e)
{
    (void)e;
    nav_back_user();
}

/* Row tap → push the target screen captured as the event user_data. */
static void row_click_cb(lv_event_t *e)
{
    lv_obj_t *target = (lv_obj_t *)lv_event_get_user_data(e);
    nocsif_nav_push(target);
}

/* Swipe-right anywhere on a screen → go back one level (natural edge-back gesture).
 * Children set LV_OBJ_FLAG_GESTURE_BUBBLE so a swipe that starts on a row/header/button
 * bubbles up to the screen root, which carries this handler. A tap (no travel) still
 * fires LV_EVENT_CLICKED instead, so row navigation is unaffected. */
static void screen_gesture_cb(lv_event_t *e)
{
    (void)e;
    lv_indev_t *indev = lv_indev_active();
    if (indev != NULL && lv_indev_get_gesture_dir(indev) == LV_DIR_RIGHT) {
        nav_back_user();
    }
}

/* ---- dashed header rule ---------------------------------------------------- *
 * LVGL v9 borders are solid-only, so the one antique dashed rule (spec §4/§6) is a
 * 1px ARGB8888 canvas with a 2-on/4-off dash written per-pixel. One small persistent
 * PSRAM buffer per screen (~1.5 KB); screens are cached for the app's life so it is
 * never freed. A failure is cosmetic (no rule) — never fatal. */
/* Free the canvas's PSRAM buffer when the canvas is deleted. lv_canvas does NOT own the
 * buffer, so a screen freed on nav-back would otherwise leak it on each rebuild. */
static void dashed_rule_deleted_cb(lv_event_t *e)
{
    uint8_t *buf = (uint8_t *)lv_event_get_user_data(e);
    if (buf) {
        heap_caps_free(buf);
    }
}

static void build_dashed_rule(lv_obj_t *parent)
{
    const int32_t w = NOCSIF_DISP_W - 2 * HEADER_INSET;
    const int32_t h = 1;
    size_t stride = lv_draw_buf_width_to_stride((uint32_t)w, LV_COLOR_FORMAT_ARGB8888);
    uint8_t *buf = heap_caps_aligned_alloc(64, stride * h, MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGW(TAG, "dashed-rule buffer alloc failed (%u B) — omitting rule (cosmetic)",
                 (unsigned)(stride * h));
        return;
    }
    lv_obj_t *cv = lv_canvas_create(parent);
    if (!cv) {
        heap_caps_free(buf);
        return;
    }
    lv_canvas_set_buffer(cv, buf, w, h, LV_COLOR_FORMAT_ARGB8888);
    lv_obj_remove_flag(cv, LV_OBJ_FLAG_CLICKABLE);
    lv_canvas_fill_bg(cv, NOCSIF_EDGE, LV_OPA_TRANSP);   /* clear to transparent */
    for (int32_t x = 0; x < w; x++) {
        if ((x % 6) < 2) {                               /* 2 on / 4 off */
            lv_canvas_set_px(cv, x, 0, NOCSIF_EDGE, LV_OPA_COVER);
        }
    }
    lv_obj_set_style_margin_left(cv, HEADER_INSET, 0);
    lv_obj_set_style_margin_right(cv, HEADER_INSET, 0);

    /* Own the buffer's lifetime: free it when this canvas is deleted (screen freed on
     * nav-back). Safe — LVGL's canvas teardown does not read the buffer contents. */
    lv_obj_add_event_cb(cv, dashed_rule_deleted_cb, LV_EVENT_DELETE, buf);
}

/* ---- header ---------------------------------------------------------------- *
 * 26px top pad, ~64px tall (spec §4): [back? + serif title?] … [mono clock] [battery].
 * title==NULL => Home (no back, no title). Clock is bound to the RTC (P4.2) and battery to the
 * PMU gauge (P4.3): both are seeded at build time and kept current update-on-change by the
 * single header tick (they show the live value at once on a rebuilt header, never a placeholder). */
/* §4.8a Companion — the current screen's title, mirrored to the phone (nocsif_companion_state_json).
 * Seeded whenever a header is built; NULL title (Home) reads back as "Home". A copy (never the caller's
 * pointer, which may be a stack string). Read from the HTTP/timer task — a scalar snapshot, benign race. */
static char s_cur_title[40] = "Home";
const char *nocsif_nav_current_title(void) { return s_cur_title; }

static void build_header(lv_obj_t *screen_root, const char *title)
{
    snprintf(s_cur_title, sizeof s_cur_title, "%s", title ? title : "Home");
    lv_obj_t *h = lv_obj_create(screen_root);
    lv_obj_remove_style_all(h);
    lv_obj_set_width(h, NOCSIF_DISP_W);
    lv_obj_set_height(h, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_top(h, HEADER_TOP_PAD, 0);
    lv_obj_set_style_pad_bottom(h, 16, 0);
    lv_obj_set_style_pad_left(h, HEADER_HINSET, 0);
    lv_obj_set_style_pad_right(h, HEADER_HINSET, 0);
    lv_obj_set_style_min_height(h, 64, 0);
    lv_obj_set_flex_flow(h, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(h, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(h, 12, 0);
    lv_obj_set_scrollbar_mode(h, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(h, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(h, LV_OBJ_FLAG_GESTURE_BUBBLE);   /* swipe-to-back reaches the screen */

    /* P8 v2.3 — the header doubles as the Control-Center pull-down surface. Clickable so it gets
     * PRESSED/PRESSING/RELEASED; a vertical-dominant downward drag pulls the shade down following the
     * finger (header_drag_cb). It carries no styles, so this adds no visual, and swipe-back (a bubbled
     * GESTURE) is untouched. */
    if (s_drag_begin != NULL) {
        lv_obj_add_flag(h, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(h, header_drag_cb, LV_EVENT_PRESSED,  NULL);
        lv_obj_add_event_cb(h, header_drag_cb, LV_EVENT_PRESSING, NULL);
        lv_obj_add_event_cb(h, header_drag_cb, LV_EVENT_RELEASED, NULL);
    }

    if (title != NULL) {
        /* Back arrow (violet), the menu icon font's i-back glyph. The label is the tap target. */
        lv_obj_t *bk = lv_label_create(h);
        lv_label_set_text(bk, NOCSIF_ICON_BACK);
        lv_obj_set_style_text_font(bk, &nocsif_icons, 0);
        lv_obj_set_style_text_color(bk, NOCSIF_VIOLET, 0);
        lv_obj_add_flag(bk, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(bk, LV_OBJ_FLAG_GESTURE_BUBBLE);   /* swipe starting on the arrow still bubbles */
        lv_obj_set_ext_click_area(bk, 12);           /* comfortable hit target */
        lv_obj_add_event_cb(bk, back_click_cb, LV_EVENT_CLICKED, NULL);

        lv_obj_t *t = lv_label_create(h);
        lv_label_set_text(t, title);
        lv_obj_add_style(t, &nocsif_style_title, 0);
    }

    /* Spacer pushes the badge/clock/battery cluster to the right. */
    lv_obj_t *sp = lv_obj_create(h);
    lv_obj_remove_style_all(sp);
    lv_obj_set_height(sp, 1);
    lv_obj_set_flex_grow(sp, 1);
    lv_obj_remove_flag(sp, LV_OBJ_FLAG_CLICKABLE);

    /* Optional status badge (P4.5.4), right cluster, left of the clock: a small violet hairline pill
     * bound to the app getter. Seeded now so a header built while something is armed shows it at once;
     * the header tick keeps it current and HIDES it while the getter returns "" (no gap when idle).
     * ui.c binds this to the HID-armed indicator (names the HID keyboard when it is the live class). */
    if (s_badge_getter != NULL) {
        const char *b0 = s_badge_getter();
        lv_obj_t *bdg = lv_label_create(h);
        lv_label_set_text(bdg, (b0 != NULL) ? b0 : "");
        lv_obj_set_style_text_font(bdg, &nocsif_mono_11, 0);
        lv_obj_set_style_text_color(bdg, NOCSIF_VIOLET, 0);
        lv_obj_set_style_text_letter_space(bdg, 1, 0);
        lv_obj_set_style_border_color(bdg, NOCSIF_VIOLET, 0);
        lv_obj_set_style_border_width(bdg, 1, 0);
        lv_obj_set_style_border_opa(bdg, LV_OPA_40, 0);
        lv_obj_set_style_radius(bdg, 5, 0);
        lv_obj_set_style_pad_left(bdg, 6, 0);
        lv_obj_set_style_pad_right(bdg, 6, 0);
        lv_obj_set_style_pad_top(bdg, 2, 0);
        lv_obj_set_style_pad_bottom(bdg, 2, 0);
        if (b0 == NULL || b0[0] == '\0') {
            lv_obj_add_flag(bdg, LV_OBJ_FLAG_HIDDEN);
        }
        nocsif_nav_register_live_label(bdg, s_badge_getter);
    }

    /* Clock: seed from the RTC's cached string so a freshly-built/rebuilt header shows the
     * current time immediately (not "--:--"), then register it so the one header-tick timer
     * keeps it current, update-on-change (P4.2). */
    lv_obj_t *clk = lv_label_create(h);
    lv_label_set_text(clk, nocsif_rtc_clock_str());
    lv_obj_add_style(clk, &nocsif_style_clock, 0);
    nocsif_nav_register_live_label(clk, nocsif_rtc_clock_str);

    /* Battery: seed from the PMU's cached "NN%" string so a freshly-built/rebuilt header shows
     * the current level immediately (not "--%"), then register it so the one header-tick timer
     * keeps it current, update-on-change at the battery's slow cadence (P4.3). Plain "NN%" in
     * steel, no charge glyph — matches the mockup's .bat. */
    lv_obj_t *bat = lv_label_create(h);
    lv_label_set_text(bat, nocsif_power_batt_str());
    lv_obj_add_style(bat, &nocsif_style_battery, 0);
    nocsif_nav_register_live_label(bat, nocsif_power_batt_str);
}

/* ---- screen scaffold ------------------------------------------------------- */

/* Section caption: mono `// text` with a violet `//` (mockup .sub / .sub b). */
static void add_caption(lv_obj_t *parent, const char *caption)
{
    lv_obj_t *cap = lv_obj_create(parent);
    lv_obj_remove_style_all(cap);
    lv_obj_set_width(cap, NOCSIF_DISP_W);
    lv_obj_set_height(cap, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(cap, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(cap, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(cap, 6, 0);
    lv_obj_set_style_pad_left(cap, LIST_INSET, 0);
    lv_obj_set_style_pad_top(cap, 9, 0);
    lv_obj_set_style_pad_bottom(cap, 2, 0);
    lv_obj_remove_flag(cap, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(cap, LV_OBJ_FLAG_GESTURE_BUBBLE);

    lv_obj_t *slash = lv_label_create(cap);
    lv_label_set_text(slash, "//");
    lv_obj_add_style(slash, &nocsif_style_caption, 0);
    lv_obj_set_style_text_color(slash, NOCSIF_VIOLET, 0);

    lv_obj_t *cx = lv_label_create(cap);
    lv_label_set_text(cx, caption);
    lv_obj_add_style(cx, &nocsif_style_caption, 0);
}

lv_obj_t *nocsif_screen_scaffold(const char *title, const char *caption, lv_obj_t **content_out)
{
    lv_obj_t *scr = lv_obj_create(NULL);             /* detached screen root */
    lv_obj_remove_style_all(scr);
    lv_obj_set_size(scr, NOCSIF_DISP_W, NOCSIF_DISP_H);
    lv_obj_set_style_bg_opa(scr, LV_OPA_TRANSP, 0);  /* orrery (bottom layer) shows through */
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);   /* the content column scrolls, not the screen */
    lv_obj_add_event_cb(scr, screen_gesture_cb, LV_EVENT_GESTURE, NULL);   /* swipe-to-back */

    build_header(scr, title);       /* fixed */
    build_dashed_rule(scr);         /* fixed */

    /* Scrollable content column: fills the space under the header (fixed header, scrolling
     * body). Home's three bands or an 8-row submenu overflow 502px — this scrolls; the
     * header/clock stay put. full_refresh repaints the whole frame while scrolling (a
     * user-driven motion, accepted per the P3.1 flush ceiling). */
    lv_obj_t *content = lv_obj_create(scr);
    lv_obj_remove_style_all(content);
    lv_obj_set_width(content, NOCSIF_DISP_W);
    lv_obj_set_flex_grow(content, 1);                /* fill remaining height */
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(content, LV_DIR_VER);      /* vertical scroll only (horizontal = swipe-back) */
    lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_top(content, 4, 0);
    lv_obj_set_style_pad_bottom(content, 22, 0);     /* breathing room past the last row */
    lv_obj_add_flag(content, LV_OBJ_FLAG_GESTURE_BUBBLE);

    if (caption != NULL) {
        add_caption(content, caption);
    }
    if (content_out != NULL) {
        *content_out = content;
    }
    return scr;
}

/* --- P8 v2.4g: elastic "play" on a menu list that FITS the screen -------------------------------
 * LVGL only bounces a list that actually overflows (lv_indev_find_scroll_obj requires scroll_top or
 * scroll_bottom > 0), so a list that exactly fills the screen feels dead at the ends. This adds a
 * small damped rubber-band: while the parent content has NO native scroll room, a vertical drag tugs
 * the list a little and springs back on release, so a swipe always signals "this is a list / you're
 * at the end" (USER). Overflowing lists are left entirely to LVGL's native scroll + elastic — we
 * defer while there's scroll room. The tug transforms the LIST, which the parent content clips, so it
 * never overlaps the fixed header. Row presses reach this handler via LV_OBJ_FLAG_EVENT_BUBBLE. */
#define MENU_OS_DAMP      3      /* finger travel / this = tug distance (elastic slowness) */
#define MENU_OS_MAX      44      /* px cap on the tug */
#define MENU_OS_DZ        6      /* px dead-zone before a tug engages (keeps taps as taps) */
#define MENU_OS_SPRING_MS 200    /* ease-out spring-back duration */

static int32_t s_os_x0, s_os_y0;
static int32_t s_os_off;
static bool    s_os_active;

static void menu_os_spring_cb(void *list, int32_t v)
{
    lv_obj_set_style_translate_y((lv_obj_t *)list, v, 0);
}

static void menu_overscroll_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *list = lv_event_get_current_target(e);
    lv_indev_t *indev = lv_indev_active();
    if (indev == NULL) return;
    lv_point_t pt;
    lv_indev_get_point(indev, &pt);

    if (code == LV_EVENT_PRESSED) {
        s_os_x0 = pt.x; s_os_y0 = pt.y; s_os_off = 0; s_os_active = false;
        lv_anim_delete(list, menu_os_spring_cb);        /* take over any running spring-back */
        lv_obj_set_style_translate_y(list, 0, 0);
        return;
    }
    if (code == LV_EVENT_PRESSING) {
        lv_obj_t *content = lv_obj_get_parent(list);
        if (content == NULL) return;
        /* overflowing list -> LVGL's own scroll + elastic own the gesture; don't tug */
        if (lv_obj_get_scroll_top(content) > 0 || lv_obj_get_scroll_bottom(content) > 0) {
            if (s_os_off != 0) { s_os_off = 0; lv_obj_set_style_translate_y(list, 0, 0); }
            return;
        }
        int32_t dy = pt.y - s_os_y0, dx = pt.x - s_os_x0;
        if (!s_os_active && (LV_ABS(dy) <= MENU_OS_DZ || LV_ABS(dx) > LV_ABS(dy))) return;   /* not a vertical drag */
        s_os_active = true;
        int32_t off = dy / MENU_OS_DAMP;
        if (off >  MENU_OS_MAX) off =  MENU_OS_MAX;
        if (off < -MENU_OS_MAX) off = -MENU_OS_MAX;
        s_os_off = off;
        lv_obj_set_style_translate_y(list, off, 0);
        return;
    }
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (s_os_active || s_os_off != 0) {
            lv_anim_t a; lv_anim_init(&a);
            lv_anim_set_var(&a, list);
            lv_anim_set_exec_cb(&a, menu_os_spring_cb);
            lv_anim_set_values(&a, s_os_off, 0);
            lv_anim_set_duration(&a, MENU_OS_SPRING_MS);
            lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
            lv_anim_start(&a);
        }
        s_os_active = false; s_os_off = 0;
    }
}

lv_obj_t *nocsif_menu_list(lv_obj_t *content)
{
    lv_obj_t *list = lv_obj_create(content);
    lv_obj_remove_style_all(list);
    lv_obj_set_width(list, NOCSIF_DISP_W);
    lv_obj_set_height(list, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_left(list, LIST_INSET, 0);
    lv_obj_set_style_pad_right(list, LIST_INSET, 0);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(list, LV_OBJ_FLAG_SCROLLABLE);   /* the content column owns the scroll */
    lv_obj_add_flag(list, LV_OBJ_FLAG_GESTURE_BUBBLE);
    /* P8 v2.4g — elastic tug for a fitting list (see menu_overscroll_cb). Row presses bubble here. */
    lv_obj_add_event_cb(list, menu_overscroll_cb, LV_EVENT_PRESSED,    NULL);
    lv_obj_add_event_cb(list, menu_overscroll_cb, LV_EVENT_PRESSING,   NULL);
    lv_obj_add_event_cb(list, menu_overscroll_cb, LV_EVENT_RELEASED,   NULL);
    lv_obj_add_event_cb(list, menu_overscroll_cb, LV_EVENT_PRESS_LOST, NULL);
    return list;
}

void nocsif_band(lv_obj_t *content, const char *text)
{
    lv_obj_t *b = lv_obj_create(content);
    lv_obj_remove_style_all(b);
    lv_obj_set_width(b, NOCSIF_DISP_W);
    lv_obj_set_height(b, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(b, 28, 0);
    lv_obj_set_style_pad_right(b, 28, 0);
    lv_obj_set_style_pad_top(b, 16, 0);
    lv_obj_set_style_pad_bottom(b, 4, 0);
    lv_obj_set_style_pad_column(b, 9, 0);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(b, LV_OBJ_FLAG_GESTURE_BUBBLE);

    lv_obj_t *lbl = lv_label_create(b);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, &nocsif_mono_11, 0);
    lv_obj_set_style_text_color(lbl, NOCSIF_ASH, 0);
    lv_obj_set_style_text_letter_space(lbl, 2, 0);   /* caps tracking (mockup .band .28em) */

    lv_obj_t *rule = lv_obj_create(b);               /* trailing hairline (mockup .band::after) */
    lv_obj_remove_style_all(rule);
    lv_obj_set_height(rule, 1);
    lv_obj_set_flex_grow(rule, 1);
    lv_obj_set_style_bg_color(rule, NOCSIF_EDGE, 0);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
    lv_obj_remove_flag(rule, LV_OBJ_FLAG_CLICKABLE);
}

/* ---- menu row -------------------------------------------------------------- */

lv_obj_t *nocsif_menu_add_row(lv_obj_t *list, const char *icon, const char *name,
                              lv_obj_t *target, const char *tag, nocsif_tag_kind_t tag_kind,
                              bool disabled, nocsif_live_getter_t tag_getter)
{
    /* Invariant layout + hairline + pressed wash come from shared styles (two add_style
     * calls, not ~15 per-row set_style calls — the difference that made building the whole
     * menu at once trip the task watchdog). Only per-row dynamic bits are set locally. */
    lv_obj_t *row = lv_obj_create(list);
    lv_obj_remove_style_all(row);
    lv_obj_add_style(row, &nocsif_style_row, 0);
    lv_obj_add_style(row, &nocsif_style_row_press, LV_STATE_PRESSED);
    lv_obj_set_scrollbar_mode(row, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_GESTURE_BUBBLE);   /* bubble swipes to the screen */
    lv_obj_add_flag(row, LV_OBJ_FLAG_EVENT_BUBBLE);     /* P8 v2.4g: bubble press events to the list (elastic tug) */

    /* Left icon column (menu icon font, steel; dimmed on a stub). The icon font's advance is
     * uniform and each glyph's ink is centered in it (gen_icons.py lsb=xMin), so centering
     * the advance in this fixed-width column lines every icon up and names start on one x. */
    if (icon != NULL) {
        lv_obj_t *ic = lv_label_create(row);
        lv_label_set_text(ic, icon);
        lv_obj_add_style(ic, &nocsif_style_row_icon, 0);
        lv_obj_set_width(ic, ROW_ICON_W);
        lv_obj_set_style_text_align(ic, LV_TEXT_ALIGN_CENTER, 0);
        if (disabled) {
            lv_obj_set_style_text_color(ic, NOCSIF_EDGE2, 0);   /* dimmed stub icon */
        }
    }

    /* Name (flex-grow so tag/chevron align right). */
    lv_obj_t *nm = lv_label_create(row);
    lv_label_set_text(nm, name);
    lv_obj_add_style(nm, &nocsif_style_row_name, 0);
    lv_obj_set_flex_grow(nm, 1);
    if (disabled) {
        lv_obj_set_style_text_color(nm, NOCSIF_ASH, 0);   /* dimmed stub */
    }

    /* Optional status tag (spec §6 — only when there's something to say). */
    if (tag != NULL && tag_kind != NOCSIF_TAG_NONE) {
        lv_obj_t *tg = lv_label_create(row);
        /* Live tag (P4.3): seed from the getter so it shows the current value at build time,
         * then register it so the header tick keeps it current update-on-change. Else static. */
        lv_label_set_text(tg, tag_getter ? tag_getter() : tag);
        lv_obj_add_style(tg, &nocsif_style_row_tag, 0);
        lv_color_t c = NOCSIF_ASH;
        if (tag_kind == NOCSIF_TAG_READY) c = NOCSIF_STEEL;
        else if (tag_kind == NOCSIF_TAG_RUN) c = NOCSIF_VIOLET;
        if (disabled) c = NOCSIF_ASH;   /* a stub's tag is a dimmed hint, not a live state */
        lv_obj_set_style_text_color(tg, c, 0);
        if (tag_getter != NULL) {
            nocsif_nav_register_live_label(tg, tag_getter);
        }
    }

    /* Chevron (menu icon font i-chev, shared style). Always shown — the P3.2 skeleton is all
     * drill-in rows; chevron-less action rows (mockup's data-act) come with later milestones. */
    lv_obj_t *chev = lv_label_create(row);
    lv_label_set_text(chev, NOCSIF_ICON_CHEV);
    lv_obj_add_style(chev, &nocsif_style_chevron, 0);

    if (disabled) {
        lv_obj_add_state(row, LV_STATE_DISABLED);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);   /* non-navigating stub */
        lv_obj_set_style_text_color(chev, NOCSIF_EDGE2, 0);
    } else if (target != NULL) {
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, row_click_cb, LV_EVENT_CLICKED, target);
    }
    return row;
}
