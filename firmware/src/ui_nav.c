/*
 * Menu-first navigation shell implementation: screen stack plus reusable widgets
 * (UI-shell P3). See ui_nav.h.
 *
 * This file only implements the navigation mechanism — it has no knowledge of any
 * particular app or screen; ui.c builds those. The screen stack animates with a
 * custom transition (slide_in): lv_screen_load() swaps the active screen instantly,
 * then a single lv_anim drives lv_obj_set_x (a small ~30px directional slide with
 * ease-out) plus a plain LV_STYLE_OPA fade over NAV_ANIM_MS. This replaces
 * lv_screen_load_anim's built-in full-screen MOVE transition for two reasons:
 * translate_x has no effect on a screen root, and a full 410px MOVE looks coarse at
 * this panel's full-frame flush rate, whereas a small ~30px set_x plus fade reads as
 * smooth (spec section 5). Screens have transparent roots, so the P2 orrery on
 * lv_layer_bottom() shows through and is never invalidated; the corner masks stay on
 * lv_layer_top().
 *
 * Every entry point here runs on the LVGL task — screen building happens under the
 * port lock inside nocsif_ui_init, and nav_push/back are called from LVGL event
 * callbacks that already hold the lock — so no extra locking is needed in this file.
 *
 * The LVGL 9.3 APIs used here were checked against the pinned
 * managed_components/lvgl__lvgl headers: lv_screen_load / lv_screen_load_anim (plus
 * LV_SCR_LOAD_ANIM_MOVE_LEFT/RIGHT), the detached-screen pattern lv_obj_create(NULL),
 * LV_STATE_DISABLED, lv_canvas_set_px/fill_bg, and lv_draw_buf_width_to_stride.
 */
#include "ui_nav.h"

#include <stdint.h>
#include <string.h>

#include "display.h"        /* for NOCSIF_DISP_W / NOCSIF_DISP_H */
#include "ui_theme.h"       /* for the color tokens, fonts, and shared styles */
#include "rtc.h"            /* for nocsif_rtc_clock_str, the header clock's live data source (P4.2) */
#include "power.h"          /* for nocsif_power_batt_str, the header battery's live data source (P4.3) */
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "ui_nav";

#define NAV_STACK_MAX   8               /* Home plus its drilldowns; boot heap is roughly 8.5 MB, so this is cheap */
#define HEADER_INSET    20              /* inset of the dashed rule from each edge (spec section 4) */
#define LIST_INSET      26              /* horizontal safe inset for list content (spec section 4) */
/* Header content sits in the top row, inside the arcs of the rounded glass corners
 * — the normal 24-26px safe inset isn't enough there. On-device testing showed the
 * title's top edge, the clock/battery, and the back arrow all getting clipped by the
 * physical curve. So header content is pushed further in and down to clear a roughly
 * 56px corner radius (matching the mockup's .screen border-radius). */
/* Corner clearance rationale: the physical panel clips a roughly 56px rounded
 * corner (matching the mockup's .screen border-radius, and clipping noticeably more
 * than the software CORNER_R=40 mask does). The header's corner-adjacent elements —
 * the top-left back arrow and the top-right battery — sit right at these insets, so
 * anything less than a full corner radius still nips them (the battery was still
 * getting clipped even at 48/42). Both dimensions get a full corner-radius worth of
 * clearance so the whole corner cluster clears the arc. This applies to every
 * screen's header. See docs/LESSONS.md for the rounded-corner clipping writeup. */
#define HEADER_HINSET   56              /* header horizontal inset; must be at least the corner radius */
#define HEADER_TOP_PAD  56              /* header top padding, enough to clear the corner arc */
#define ROW_MIN_H       44              /* the minimum tap target size (spec section 4) */
#define ROW_VPAD        15              /* row vertical padding, matching the mockup's .row 15px */
#define ROW_ICON_GAP    16              /* gap between a row's icon and its name (spec section 4) */
#define ROW_ICON_W      24              /* fixed width of the icon column, so names all line up */
/* Directional slide duration (spec section 5). Currently 0, disabling the
 * animation entirely as a jank fix: a full-screen frame takes roughly 105ms to
 * composite (about 8-9 fps, bound by re-blitting the orrery — measured
 * 2026-08-12), and slide_in pre-sets the root to its dimmed, offset starting state
 * before loading it, so the very first rendered frame always shows that
 * dim/offset state for one ~105ms frame before snapping into place — which reads
 * as a laggy fade-in no matter how short the animation duration is set to. So for
 * now the animation is skipped entirely and each screen loads already at rest, at
 * full opacity. Re-enable this with a value above 0 once a background rework
 * raises the full-frame rate enough for a slide to actually look smooth instead of
 * stepping. Safe to tune on-device. */
#define NAV_ANIM_MS     0
/* Slide distance. The design mockup's transition is a subtle directional
 * translate (translateX of about +-26px, plus a fade) — not a full-screen slide.
 * On this panel, every animation frame re-flushes the entire 410x502 frame over
 * QSPI (roughly a 30-50 fps ceiling), so a full-screen 410px slide steps coarsely
 * and looks janky, while a small ~30px translate only steps about 5px per frame
 * and reads as smooth at that same frame rate. This matches the mockup's intent. */
#define NAV_SLIDE_PX    30

/* A screen root carrying this flag is pinned: nocsif_nav_back will not free it
 * when it's popped. Set via nocsif_nav_pin, for screens that own lv_timers or other
 * live state that needs to survive being navigated away from. */
#define NAV_FLAG_PIN    LV_OBJ_FLAG_USER_1

/* The nav stack of screen roots; s_stack[0] is always Home, and s_sp indexes the current top. */
static lv_obj_t *s_stack[NAV_STACK_MAX];
static int       s_sp = -1;

/* The starting x offset for whatever slide transition is currently in flight (only one runs at a time, on the LVGL task). */
static int32_t   s_slide_from;

/* ---- live-data label registry (P4 header clock/battery + Time readout) ---- *
 * Labels showing data that changes periodically register here along with a getter
 * function; the single header-tick timer refreshes them, only writing when the
 * value actually changed. Sized generously for the live set: each of up to
 * NAV_STACK_MAX stacked headers can hold a clock, a battery, and the P4.5.4 status
 * badge, plus the Time screen's own readout — this cap never grows unbounded since a
 * label frees its slot when deleted. Bumped from *2 to *3 once the badge became a
 * third per-header live label. */
#define LIVE_LABELS_MAX  (NAV_STACK_MAX * 3 + 4)
typedef struct { lv_obj_t *label; nocsif_live_getter_t getter; } live_label_t;
static live_label_t s_live[LIVE_LABELS_MAX];

/* Optional app-supplied getter for the header status badge (P4.5.4). Stays NULL
 * until nocsif_nav_set_header_badge is called; once set, build_header adds a badge
 * label bound to it, hidden while the getter returns "". */
static nocsif_live_getter_t s_badge_getter;

/* (P8 v2.3/2.7) Header swipe-down opens the Control Center. header_drag_cb fires
 * the registered "begin" callback exactly once, when a downward, vertical-dominant
 * drag that started on the (non-scrolling) header crosses the threshold; that
 * callback (cc_open in ui.c) snaps the shade open. The move/end callbacks are
 * currently unused — the earlier finger-follow behavior was dropped because it felt
 * glitchy. Since there's only ever one active touch, a single module-static start
 * point is all that's needed. */
#define NAV_PULL_START  30            /* pixels of downward travel required before the swipe-open gesture commits */
static nocsif_drag_begin_cb_t s_drag_begin;
static nocsif_drag_move_cb_t  s_drag_move;
static nocsif_drag_end_cb_t   s_drag_end;
static int32_t                s_pull_x0, s_pull_y0;
static bool                   s_pull_active;

/* Clears a label's registry slot when it's deleted (its screen was freed on
 * nav-back), so the tick timer never writes into a dangling widget. Runs on the
 * LVGL task, right before the label is actually torn down. */
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
    s_badge_getter = getter;   /* takes effect starting with the next header that's built (call before nav_init) */
}

void nocsif_nav_set_top_drag_cbs(nocsif_drag_begin_cb_t begin,
                                 nocsif_drag_move_cb_t move,
                                 nocsif_drag_end_cb_t end)
{
    s_drag_begin = begin; s_drag_move = move; s_drag_end = end;   /* takes effect starting with the next header that's built */
}

/* Header PRESSED/PRESSING/RELEASED event router: a downward, vertical-dominant
 * drag that starts on the header drives the Control Center open, with the card
 * following the finger. Horizontal swipe-back is left to the screen's own gesture
 * handler — this only engages when dy > |dx|, so the two never conflict. */
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
        /* An empty getter means there's nothing to show, so the label is hidden entirely
         * and takes no header space (this is what lets the badge collapse away when nothing
         * is armed; clock/battery never return "", so they're unaffected). Toggling hidden
         * only triggers one invalidation, on the arm/disarm transition — acceptable cost. */
        if (s[0] == '\0') {
            if (!lv_obj_has_flag(lbl, LV_OBJ_FLAG_HIDDEN)) {
                lv_obj_add_flag(lbl, LV_OBJ_FLAG_HIDDEN);
            }
            continue;
        }
        if (lv_obj_has_flag(lbl, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_remove_flag(lbl, LV_OBJ_FLAG_HIDDEN);
        }
        /* Update only when the value actually changed: lv_label_set_text always
         * reallocates and invalidates, and under full_refresh every invalidation repaints the
         * entire 410x502 frame — so writing the same value every tick would burn a full flush
         * for nothing. Invalidations on a screen that's loaded but not currently shown (e.g. a
         * pinned Home screen sitting under an open submenu) are simply dropped by LVGL until
         * it becomes active again, so idle labels cost nothing either way. */
        if (strcmp(lv_label_get_text(lbl), s) != 0) {
            lv_label_set_text(lbl, s);
        }
    }
}

/* ---- navigation ---- */

/* One animation drives both the position (via set_x) and a subtle opacity fade,
 * keyed on progress `p` from 0 to 256. Critically, on a screen root (parent==NULL),
 * lv_obj_set_style_translate_x has no effect — only lv_obj_set_x actually moves a
 * screen (see lv_obj_pos.c line 621; LVGL's own built-in MOVE animation uses set_x
 * too). The fade uses a plain LV_STYLE_OPA property rather than a layered one
 * (calculate_layer_type only creates a layer for LAYERED opacity, transforms, masks,
 * or blend modes), so it costs the same every frame with no 617KB intermediate
 * layer, while still masking the positional stepping visible at this panel's
 * roughly 25-35 fps full-frame flush rate. */
static void slide_exec_cb(void *var, int32_t p)
{
    lv_obj_t *root = (lv_obj_t *)var;
    lv_obj_set_x(root, s_slide_from - (s_slide_from * p) / 256);       /* interpolates from_x down to 0 */
    lv_obj_set_style_opa(root, (lv_opa_t)(150 + (105 * p) / 256), 0);  /* interpolates opacity 150 up to 255 */
}

/* Locks in the settled state (x=0, full opacity) and removes the local opacity override. */
static void slide_completed_cb(lv_anim_t *a)
{
    lv_obj_t *root = (lv_obj_t *)lv_anim_get_user_data(a);
    lv_obj_set_x(root, 0);
    lv_obj_remove_local_style_prop(root, LV_STYLE_OPA, 0);
}

/* Swaps to `root` instantly (unloading whatever screen was active), then slides
 * it in over a small directional distance (matching the mockup's translateX of
 * about +-30px plus a fade). A full-screen 410px slide looks coarse at this panel's
 * frame rate; a ~30px set_x plus fade reads smoothly at that same rate. */
static void slide_in(lv_obj_t *root, int32_t from_x)
{
    lv_anim_delete(NULL, slide_exec_cb);            /* cancel any slide animation already in flight */
    if (NAV_ANIM_MS <= 0) {
        /* Animation is disabled (the jank fix described above): load the screen already
         * at rest, with no dim/offset starting frame and so no laggy fade-in at the current
         * ~8-9 fps. Clear out any leftover slide state first. */
        lv_obj_set_x(root, 0);
        lv_obj_remove_local_style_prop(root, LV_STYLE_OPA, 0);
        lv_screen_load(root);
        return;
    }
    s_slide_from = from_x;
    lv_obj_set_x(root, from_x);                     /* pre-offset and pre-dim it before it's ever shown on screen */
    lv_obj_set_style_opa(root, 150, 0);
    lv_screen_load(root);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, root);
    lv_anim_set_user_data(&a, root);               /* read back by slide_completed_cb */
    lv_anim_set_exec_cb(&a, slide_exec_cb);
    lv_anim_set_values(&a, 0, 256);
    lv_anim_set_duration(&a, NAV_ANIM_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);   /* decelerate into rest rather than stopping abruptly */
    lv_anim_set_completed_cb(&a, slide_completed_cb);
    lv_anim_start(&a);
}

void nocsif_nav_init(lv_obj_t *home_root)
{
    s_stack[0] = home_root;
    s_sp = 0;
    lv_screen_load(home_root);           /* the active screen, with no animation */
}

void nocsif_nav_push(lv_obj_t *root)
{
    if (root == NULL || s_sp < 0 || s_sp + 1 >= NAV_STACK_MAX) {
        return;
    }
    s_stack[++s_sp] = root;
    slide_in(root, NAV_SLIDE_PX);        /* pushing forward: enter from the right */
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
        return;                          /* already at Home, nothing to do */
    }
    /* Loads Home (making it active) and slides it in from the left first, then frees
     * the screens above it — this mirrors nocsif_nav_back's ordering of loading the parent
     * before deleting the child, so no currently-active screen is ever deleted. Pinned
     * screens (with live timers or state) are kept just like in nav_back; their registry
     * cache pointer survives so relaunching reuses the same screen. */
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
        return;                          /* Home has no back destination */
    }
    lv_obj_t *leaving = s_stack[s_sp];        /* the screen that's about to be popped */
    s_sp--;
    slide_in(s_stack[s_sp], -NAV_SLIDE_PX);   /* going back: load and slide the parent in from the left */

    /* Free the popped screen (unless it's pinned) so cached widgets don't accumulate
     * in LVGL's fixed-size object pool, which is separate from the regular system heap
     * and quite small (48 KB). Caching everything unboundedly used to exhaust that pool
     * after just a few screens, causing the next render's glyph or draw-buffer allocation
     * to fail and the LVGL task to spin forever inside lv_draw_label — a hang, not a
     * crash. By this point the parent has already been loaded above, so `leaving` is no
     * longer active and is safe to delete; slide_in() already cancelled any slide
     * animation still running on it. Its LV_EVENT_DELETE handler clears the app registry
     * cache in ui.c so relaunching rebuilds it fresh, and frees its dashed-rule buffer.
     * Only the pinned Home screen (stack[0]) is ever kept around. */
    if (leaving != NULL && !lv_obj_has_flag(leaving, NAV_FLAG_PIN)) {
        s_stack[s_sp + 1] = NULL;
        lv_obj_delete(leaving);
    }
}

/* (section 4.14) The optional back-gesture interceptor described in ui_nav.h: one
 * hook at a time, bound to whichever screen installed it, and cleared automatically
 * when that screen is deleted. Only the user-initiated back paths below consult it. */
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

/* A user-initiated back press: the active screen's hook gets first refusal, then the normal pop happens. */
static void nav_back_user(void)
{
    if (s_back_hook != NULL && s_sp >= 0 && s_stack[s_sp] == s_back_hook_scr && s_back_hook()) {
        return;                                            /* the back action was consumed by the screen's hook */
    }
    nocsif_nav_back();
}

/* ---- event callbacks ---- */

static void back_click_cb(lv_event_t *e)
{
    (void)e;
    nav_back_user();
}

/* A row tap pushes the target screen captured in the event's user_data. */
static void row_click_cb(lv_event_t *e)
{
    lv_obj_t *target = (lv_obj_t *)lv_event_get_user_data(e);
    nocsif_nav_push(target);
}

/* A rightward swipe anywhere on a screen goes back one level, matching the
 * natural edge-back gesture. Children set LV_OBJ_FLAG_GESTURE_BUBBLE so a swipe
 * starting on a row, header, or button still bubbles up to this handler on the
 * screen root. A plain tap (no travel) still fires LV_EVENT_CLICKED instead, so
 * normal row navigation is unaffected. */
static void screen_gesture_cb(lv_event_t *e)
{
    (void)e;
    lv_indev_t *indev = lv_indev_active();
    if (indev != NULL && lv_indev_get_gesture_dir(indev) == LV_DIR_RIGHT) {
        nav_back_user();
    }
}

/* ---- dashed header rule ---- *
 * LVGL v9 only supports solid borders, so the single antique dashed rule (spec
 * sections 4/6) is drawn as a 1px ARGB8888 canvas with a 2-on/4-off dash pattern
 * written pixel by pixel. Each screen gets its own small persistent PSRAM buffer
 * (about 1.5 KB); since screens are cached for the app's lifetime, it's never freed
 * until the screen itself is deleted. A failure here is purely cosmetic — the rule
 * just doesn't appear — never fatal. */
/* Frees the canvas's PSRAM buffer when the canvas object is deleted. lv_canvas
 * does not own the buffer itself, so without this a screen freed on nav-back would
 * leak that buffer every time it's rebuilt. */
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
    lv_canvas_fill_bg(cv, NOCSIF_EDGE, LV_OPA_TRANSP);   /* clear the canvas to fully transparent first */
    for (int32_t x = 0; x < w; x++) {
        if ((x % 6) < 2) {                               /* a 2-pixel-on, 4-pixel-off dash pattern */
            lv_canvas_set_px(cv, x, 0, NOCSIF_EDGE, LV_OPA_COVER);
        }
    }
    lv_obj_set_style_margin_left(cv, HEADER_INSET, 0);
    lv_obj_set_style_margin_right(cv, HEADER_INSET, 0);

    /* Ties the buffer's lifetime to this canvas: it's freed when the canvas is
     * deleted (i.e. when a screen is freed on nav-back). Safe to do, since LVGL's canvas
     * teardown never reads the buffer's contents. */
    lv_obj_add_event_cb(cv, dashed_rule_deleted_cb, LV_EVENT_DELETE, buf);
}

/* ---- header ---- *
 * A 26px top pad and roughly 64px total height (spec section 4): [back arrow? +
 * serif title?] ... [mono clock] [battery]. title==NULL means Home, with no back
 * arrow or title shown. The clock is bound to the RTC (P4.2) and the battery to the
 * PMU gauge (P4.3); both are seeded with their current value at build time and then
 * kept current, update-on-change, by the single header tick — so a freshly built or
 * rebuilt header always shows the live value immediately, never a placeholder. */
/* (section 4.8a companion) The current screen's title, mirrored to the phone via
 * nocsif_companion_state_json. Updated every time a header is built; a NULL title
 * (Home) reads back as "Home". This is always a fresh copy, never the caller's own
 * pointer (which might point at a stack-local string). Read from the HTTP/timer
 * task — a plain scalar snapshot read, so any race is harmless. */
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
    lv_obj_add_flag(h, LV_OBJ_FLAG_GESTURE_BUBBLE);   /* lets a swipe-to-back gesture starting here reach the screen */

    /* (P8 v2.3) The header doubles as the surface you pull down to open the Control
     * Center. It's made clickable so it receives PRESSED/PRESSING/RELEASED events; a
     * downward, vertical-dominant drag pulls the shade down, following the finger, via
     * header_drag_cb. It carries no visual styles of its own, so this adds nothing to look
     * at, and the bubbled-gesture swipe-back path is untouched. */
    if (s_drag_begin != NULL) {
        lv_obj_add_flag(h, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(h, header_drag_cb, LV_EVENT_PRESSED,  NULL);
        lv_obj_add_event_cb(h, header_drag_cb, LV_EVENT_PRESSING, NULL);
        lv_obj_add_event_cb(h, header_drag_cb, LV_EVENT_RELEASED, NULL);
    }

    if (title != NULL) {
        /* Back arrow, rendered violet, using the menu icon font's back glyph. The label itself is the tap target. */
        lv_obj_t *bk = lv_label_create(h);
        lv_label_set_text(bk, NOCSIF_ICON_BACK);
        lv_obj_set_style_text_font(bk, &nocsif_icons, 0);
        lv_obj_set_style_text_color(bk, NOCSIF_VIOLET, 0);
        lv_obj_add_flag(bk, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(bk, LV_OBJ_FLAG_GESTURE_BUBBLE);   /* a swipe starting on the arrow still bubbles up for swipe-back */
        lv_obj_set_ext_click_area(bk, 12);           /* enlarges the touch target for comfort */
        lv_obj_add_event_cb(bk, back_click_cb, LV_EVENT_CLICKED, NULL);

        lv_obj_t *t = lv_label_create(h);
        lv_label_set_text(t, title);
        lv_obj_add_style(t, &nocsif_style_title, 0);
    }

    /* A spacer pushes the badge/clock/battery cluster over to the right. */
    lv_obj_t *sp = lv_obj_create(h);
    lv_obj_remove_style_all(sp);
    lv_obj_set_height(sp, 1);
    lv_obj_set_flex_grow(sp, 1);
    lv_obj_remove_flag(sp, LV_OBJ_FLAG_CLICKABLE);

    /* Optional status badge (P4.5.4), in the right cluster just left of the clock: a
     * small violet hairline pill bound to the app's getter. It's seeded here immediately
     * so a header built while something is already armed shows it right away; the header
     * tick then keeps it current and hides it whenever the getter returns "" (so there's
     * no visible gap while idle). ui.c binds this to the HID-armed indicator, naming the
     * HID keyboard whenever it's the currently active USB class. */
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

    /* Clock: seeded from the RTC's cached string so a freshly built or rebuilt header
     * shows the current time right away instead of "--:--", then registered so the single
     * header-tick timer keeps it current, update-on-change (P4.2). */
    lv_obj_t *clk = lv_label_create(h);
    lv_label_set_text(clk, nocsif_rtc_clock_str());
    lv_obj_add_style(clk, &nocsif_style_clock, 0);
    nocsif_nav_register_live_label(clk, nocsif_rtc_clock_str);

    /* Battery: seeded from the PMU's cached "NN%" string so a freshly built or
     * rebuilt header shows the current level right away instead of "--%", then registered
     * so the header-tick timer keeps it current, update-on-change, at the battery's own
     * slow update cadence (P4.3). Rendered as plain "NN%" in steel with no charge glyph,
     * matching the mockup's .bat. */
    lv_obj_t *bat = lv_label_create(h);
    lv_label_set_text(bat, nocsif_power_batt_str());
    lv_obj_add_style(bat, &nocsif_style_battery, 0);
    nocsif_nav_register_live_label(bat, nocsif_power_batt_str);
}

/* ---- screen scaffold ---- */

/* The section caption: mono "// text", with a violet "//" prefix (matching the mockup's .sub / .sub b). */
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
    lv_obj_t *scr = lv_obj_create(NULL);             /* a detached screen root */
    lv_obj_remove_style_all(scr);
    lv_obj_set_size(scr, NOCSIF_DISP_W, NOCSIF_DISP_H);
    lv_obj_set_style_bg_opa(scr, LV_OPA_TRANSP, 0);  /* transparent so the orrery on the bottom layer shows through */
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);   /* the content column below scrolls, not the screen itself */
    lv_obj_add_event_cb(scr, screen_gesture_cb, LV_EVENT_GESTURE, NULL);   /* enables the swipe-to-back gesture */

    build_header(scr, title);       /* fixed, non-scrolling */
    build_dashed_rule(scr);         /* fixed, non-scrolling */

    /* Scrollable content column, filling the space below the fixed header. Home's
     * three bands, or an 8-row submenu, can overflow the 502px screen height — this
     * column scrolls while the header and clock stay put. full_refresh repaints the
     * whole frame while scrolling, which is accepted here as a user-driven motion within
     * the P3.1 flush-rate budget. */
    lv_obj_t *content = lv_obj_create(scr);
    lv_obj_remove_style_all(content);
    lv_obj_set_width(content, NOCSIF_DISP_W);
    lv_obj_set_flex_grow(content, 1);                /* grows to fill the remaining vertical space */
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(content, LV_DIR_VER);      /* vertical scroll only; horizontal is reserved for swipe-back */
    lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_top(content, 4, 0);
    lv_obj_set_style_pad_bottom(content, 22, 0);     /* a little breathing room past the last row */
    lv_obj_add_flag(content, LV_OBJ_FLAG_GESTURE_BUBBLE);

    if (caption != NULL) {
        add_caption(content, caption);
    }
    if (content_out != NULL) {
        *content_out = content;
    }
    return scr;
}

/* --- (P8 v2.4g) elastic "play" on a menu list that already fits the screen ---
 * LVGL only bounces a list that actually overflows its container
 * (lv_indev_find_scroll_obj requires scroll_top or scroll_bottom to be > 0), so a
 * list short enough to fit the whole screen feels completely dead at its ends. This
 * adds a small damped rubber-band effect instead: whenever the parent content has no
 * native scroll room left, a vertical drag tugs the list slightly and it springs back
 * on release, so a swipe always signals "this is a scrollable list, and you've hit
 * the end" even when there's technically nothing to scroll. Lists that do overflow
 * are left entirely to LVGL's own native scroll and elastic behavior — this defers
 * whenever there's real scroll room available. The tug transforms the list object,
 * which the parent content clips, so it can never visually overlap the fixed header.
 * Row presses reach this handler by bubbling via LV_OBJ_FLAG_EVENT_BUBBLE. */
#define MENU_OS_DAMP      3      /* divide finger travel by this to get the (slower) tug distance */
#define MENU_OS_MAX      44      /* maximum tug distance, in pixels */
#define MENU_OS_DZ        6      /* dead zone in pixels before a tug engages, so ordinary taps still register as taps */
#define MENU_OS_SPRING_MS 200    /* duration of the ease-out spring-back animation */

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
        lv_anim_delete(list, menu_os_spring_cb);        /* take over from any spring-back animation that's already running */
        lv_obj_set_style_translate_y(list, 0, 0);
        return;
    }
    if (code == LV_EVENT_PRESSING) {
        lv_obj_t *content = lv_obj_get_parent(list);
        if (content == NULL) return;
        /* the list actually overflows, so let LVGL's native scroll and elastic own the gesture instead */
        if (lv_obj_get_scroll_top(content) > 0 || lv_obj_get_scroll_bottom(content) > 0) {
            if (s_os_off != 0) { s_os_off = 0; lv_obj_set_style_translate_y(list, 0, 0); }
            return;
        }
        int32_t dy = pt.y - s_os_y0, dx = pt.x - s_os_x0;
        if (!s_os_active && (LV_ABS(dy) <= MENU_OS_DZ || LV_ABS(dx) > LV_ABS(dy))) return;   /* this isn't a vertical-dominant drag, so ignore it */
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
    lv_obj_remove_flag(list, LV_OBJ_FLAG_SCROLLABLE);   /* the content column, not the list, owns the actual scroll */
    lv_obj_add_flag(list, LV_OBJ_FLAG_GESTURE_BUBBLE);
    /* (P8 v2.4g) the elastic tug for a list that fits the screen — see menu_overscroll_cb; row presses bubble up to here */
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
    lv_obj_set_style_text_letter_space(lbl, 2, 0);   /* letter spacing for the caps tracking look (matching the mockup's .28em) */

    lv_obj_t *rule = lv_obj_create(b);               /* the trailing hairline, matching the mockup's .band::after */
    lv_obj_remove_style_all(rule);
    lv_obj_set_height(rule, 1);
    lv_obj_set_flex_grow(rule, 1);
    lv_obj_set_style_bg_color(rule, NOCSIF_EDGE, 0);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
    lv_obj_remove_flag(rule, LV_OBJ_FLAG_CLICKABLE);
}

/* ---- menu row ---- */

lv_obj_t *nocsif_menu_add_row(lv_obj_t *list, const char *icon, const char *name,
                              lv_obj_t *target, const char *tag, nocsif_tag_kind_t tag_kind,
                              bool disabled, nocsif_live_getter_t tag_getter)
{
    /* The row's invariant layout, hairline, and pressed wash all come from shared
     * styles — two add_style calls instead of roughly 15 individual set_style calls per
     * row, which is what made building the whole menu at once trip the task watchdog.
     * Only the row's per-instance dynamic bits are set directly here. */
    lv_obj_t *row = lv_obj_create(list);
    lv_obj_remove_style_all(row);
    lv_obj_add_style(row, &nocsif_style_row, 0);
    lv_obj_add_style(row, &nocsif_style_row_press, LV_STATE_PRESSED);
    lv_obj_set_scrollbar_mode(row, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_GESTURE_BUBBLE);   /* lets swipe gestures bubble up to the screen */
    lv_obj_add_flag(row, LV_OBJ_FLAG_EVENT_BUBBLE);     /* (P8 v2.4g) lets press events bubble up to the list, for the elastic tug */

    /* The left icon column, using the menu icon font in steel (dimmed for a disabled
     * stub). The icon font's advance width is uniform and each glyph's ink is centered
     * within it (gen_icons.py sets lsb=xMin), so centering that advance within this
     * fixed-width column lines every icon up consistently and makes every row's name
     * start at the same x position. */
    if (icon != NULL) {
        lv_obj_t *ic = lv_label_create(row);
        lv_label_set_text(ic, icon);
        lv_obj_add_style(ic, &nocsif_style_row_icon, 0);
        lv_obj_set_width(ic, ROW_ICON_W);
        lv_obj_set_style_text_align(ic, LV_TEXT_ALIGN_CENTER, 0);
        if (disabled) {
            lv_obj_set_style_text_color(ic, NOCSIF_EDGE2, 0);   /* dim the icon on a disabled stub row */
        }
    }

    /* The row name; flex-grow so the tag and chevron align to the right. */
    lv_obj_t *nm = lv_label_create(row);
    lv_label_set_text(nm, name);
    lv_obj_add_style(nm, &nocsif_style_row_name, 0);
    lv_obj_set_flex_grow(nm, 1);
    if (disabled) {
        lv_obj_set_style_text_color(nm, NOCSIF_ASH, 0);   /* dim the name on a disabled stub row */
    }

    /* An optional status tag (spec section 6) — only drawn when there's actually something to say. */
    if (tag != NULL && tag_kind != NOCSIF_TAG_NONE) {
        lv_obj_t *tg = lv_label_create(row);
        /* A live tag (P4.3): seeded from the getter so it shows the current value right
         * at build time, then registered so the header tick keeps it current, update-on-change.
         * Otherwise it's just a static, unchanging tag. */
        lv_label_set_text(tg, tag_getter ? tag_getter() : tag);
        lv_obj_add_style(tg, &nocsif_style_row_tag, 0);
        lv_color_t c = NOCSIF_ASH;
        if (tag_kind == NOCSIF_TAG_READY) c = NOCSIF_STEEL;
        else if (tag_kind == NOCSIF_TAG_RUN) c = NOCSIF_VIOLET;
        if (disabled) c = NOCSIF_ASH;   /* a stub's tag is just a dimmed hint, not a live status value */
        lv_obj_set_style_text_color(tg, c, 0);
        if (tag_getter != NULL) {
            nocsif_nav_register_live_label(tg, tag_getter);
        }
    }

    /* The chevron, using the menu icon font's chevron glyph and its shared style.
     * Always shown for now — the P3.2 skeleton consists entirely of drill-in rows;
     * chevron-less action rows (matching the mockup's data-act) arrive in a later
     * milestone. */
    lv_obj_t *chev = lv_label_create(row);
    lv_label_set_text(chev, NOCSIF_ICON_CHEV);
    lv_obj_add_style(chev, &nocsif_style_chevron, 0);

    if (disabled) {
        lv_obj_add_state(row, LV_STATE_DISABLED);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);   /* a disabled, non-navigating stub row */
        lv_obj_set_style_text_color(chev, NOCSIF_EDGE2, 0);
    } else if (target != NULL) {
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, row_click_cb, LV_EVENT_CLICKED, target);
    }
    return row;
}
