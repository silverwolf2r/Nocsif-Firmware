/*
 * NocSif — menu-first navigation shell: screen stack + reusable widgets (UI-shell P3)
 *
 * The navigation MECHANISM (spec §5, plan docs/research/ui-shell-2026-08-09.md §P3),
 * kept free of app knowledge so ui.c owns the actual screens:
 *   - a screen STACK (not tabs) of lv_obj_t* roots, animated with a custom set_x + opacity
 *     slide-in (small ±30px directional slide + fade, ease-out): forward=push, back=pop.
 *   - reusable builders: a transparent-root screen scaffold (header + dashed rule +
 *     inset list column) and a flat menu row ([name] … [tag?] [chevron]).
 *
 * COMPOSITING (P2 model, load-bearing): every screen root is TRANSPARENT so the
 * orrery on lv_layer_bottom() shows through and persists across swaps; the corner
 * masks live on lv_layer_top(). Screens are built once and cached by the caller
 * (auto_del=false), so back is instant and any running state survives.
 *
 * THREADING: all of these run on the LVGL task (screen build under the port lock in
 * nocsif_ui_init; nav_push/back from LVGL event callbacks with the lock already held).
 * Do NOT call them from another task without holding lvgl_port_lock().
 *
 * P3.1 note: icons + the larger type scale land in P3.2 — rows here use a text
 * chevron placeholder and the P1 font styles; press feedback is the pit-on wash.
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Status tag kind (spec §6). A tag renders ONLY when there is something to say —
 * never an idle tag. READY=steel, RUN=violet, VALUE=ash count/short value. */
typedef enum {
    NOCSIF_TAG_NONE = 0,   /* no tag (idle) */
    NOCSIF_TAG_READY,      /* "ready"  — steel  */
    NOCSIF_TAG_RUN,        /* "running"/live    — violet */
    NOCSIF_TAG_VALUE,      /* a count/short value — ash   */
} nocsif_tag_kind_t;

/* Initialise the nav stack with the Home screen root and load it as the active
 * screen (no animation). Home is stack index 0 and has no back arrow. */
void nocsif_nav_init(lv_obj_t *home_root);

/* Push a screen: slide it in from the right (MOVE_LEFT), 240 ms. No-op if the stack
 * is full or root is NULL. The root must be a cached screen (auto_del=false). */
void nocsif_nav_push(lv_obj_t *root);

/* Pop one level: slide the parent back in from the left (MOVE_RIGHT), 240 ms.
 * No-op at Home (index 0). The popped screen is DELETED (freed) unless it was
 * pinned (nocsif_nav_pin) — so cached widgets don't accumulate in LVGL's fixed
 * object pool. A registry that caches roots must clear its cache on the root's
 * LV_EVENT_DELETE so a later launch rebuilds it. */
void nocsif_nav_back(void);

/* §4.14 — a screen may INTERCEPT the user's back (swipe-right / header arrow): the hook runs first and
 * returns true to consume it — e.g. Signal Hunt drops its pinned target and shows the pick list instead of
 * popping; the next back pops as usual. One hook at a time, bound to the screen that installed it and
 * cleared automatically when that screen is deleted. Programmatic nocsif_nav_back callers are never
 * intercepted. LVGL task only. */
typedef bool (*nocsif_back_hook_t)(void);
void nocsif_nav_set_back_hook(lv_obj_t *scr, nocsif_back_hook_t hook);

/* Pin a screen root so nocsif_nav_back does NOT free it when popped. Use for
 * screens that own lv_timers or live state (e.g. the USB action screen), which
 * must survive being navigated away from. Home (stack index 0) is never popped,
 * so pinning it is belt-and-suspenders. */
void nocsif_nav_pin(lv_obj_t *root);

/* True when Home (stack index 0) is the active screen — i.e. not in any submenu.
 * The PWR short-press uses this to choose screen-off (at Home) vs pop-to-Home (in a
 * submenu). LVGL task only. */
bool nocsif_nav_at_root(void);

/* The currently active screen root (top of the stack), or NULL before nav_init. Used by
 * launch-by-id to avoid rebuilding/re-pushing the app that is already showing. LVGL task only. */
lv_obj_t *nocsif_nav_top(void);

/* §4.8a Companion — the current screen's title ("Home" at the root), seeded on each header build so the
 * companion state push can mirror where the watch is. A cached string; safe to read from any task. */
const char *nocsif_nav_current_title(void);

/* Pop every screen above Home in one step: load Home (slide it in from the left) and
 * free each intervening screen (respecting pins, like nocsif_nav_back). No-op at Home.
 * The PWR short-press "back toward the watchface" — scoped to Home until the peek/lock
 * screen lands (P4.6). LVGL task only. */
void nocsif_nav_pop_to_root(void);

/* Build a transparent screen root (detached, lv_obj_create(NULL)) laid out as a
 * vertical flex column: a FIXED header (back arrow + serif title + mono clock/battery
 * placeholders) + dashed edge rule, then a SCROLLABLE content column that fills the rest
 * (so long screens — Home's three bands, an 8-row submenu — scroll under a fixed header).
 * Pass title=NULL for Home (no back arrow/title — just the right-aligned clock/battery);
 * pass caption=NULL to omit the mono `// ...` section caption.
 * *content_out receives the scroll container; add sections to it with nocsif_menu_list()
 * (a single list) and/or nocsif_band() (a labelled group, for Home). May be NULL. */
lv_obj_t *nocsif_screen_scaffold(const char *title, const char *caption, lv_obj_t **content_out);

/* Add an inset menu list (flex column, 26px inset, no dividers of its own) to a scaffold
 * content container; append rows to it with nocsif_menu_add_row(). Returns the list. */
lv_obj_t *nocsif_menu_list(lv_obj_t *content);

/* Add a band heading (mono caps, ash, trailing hairline — mockup .band) to a scaffold
 * content container, above the list it labels (Home's operations/watch/system groups). */
void nocsif_band(lv_obj_t *content, const char *text);

/* ---- live-data hook (UI-shell P4) ------------------------------------------ *
 * The header clock/battery (and the Time screen's readout) show data that changes over
 * time on screens that are freed/rebuilt on nav-back, so a single periodic tick must reach
 * whichever labels are currently alive and a rebuilt label must show the current value at
 * once. A label registers a GETTER that returns its current cached string (e.g.
 * nocsif_rtc_clock_str); nocsif_nav_header_tick() walks the live set and writes each label
 * ONLY when its text changed (mandatory under full_refresh — any label write repaints the
 * whole frame). A label auto-unregisters on its own LV_EVENT_DELETE, so freeing a screen
 * drops its labels from the set (live set = nav depth, a handful). All of this runs on the
 * LVGL task. */
typedef const char *(*nocsif_live_getter_t)(void);

/* Register `label` to be refreshed by nocsif_nav_header_tick() using `getter` (the getter
 * must return a stable cached string and touch no hardware — it is called on the LVGL task).
 * Seed the label's initial text from getter() yourself at build time so it never flashes a
 * placeholder. Idempotent per label; the slot is cleared automatically on the label's delete. */
void nocsif_nav_register_live_label(lv_obj_t *label, nocsif_live_getter_t getter);

/* Refresh every registered live label, update-on-change (strcmp before lv_label_set_text).
 * Call from the ONE header-tick timer (ui.c), after refreshing the underlying caches. A live
 * label whose getter returns "" is HIDDEN (takes no header space) until it has something to say —
 * this is what lets the optional header badge (below) collapse when nothing is armed. */
void nocsif_nav_header_tick(void);

/* Register an OPTIONAL app-supplied header status badge (UI-shell P4.5.4). When set (once, before
 * the first screen is built), every scaffold header gains a small right-aligned badge label bound
 * to `getter`: seeded at build, kept current by the header tick update-on-change, and hidden while
 * the getter returns "" (so it costs nothing when idle). ui.c uses this for the HID-armed indicator
 * — the badge names the HID keyboard whenever it is the live USB class. Keeps ui_nav app-agnostic:
 * ui_nav owns the badge's look + lifecycle, the app owns what it says (or nothing). */
void nocsif_nav_set_header_badge(nocsif_live_getter_t getter);

/* P8 v2.3 — register finger-follow drag callbacks for a DOWNWARD pull on a scaffold header. The
 * header is the fixed, non-scrolling top band, so a downward drag that STARTS on it drives the
 * callbacks WITHOUT fighting the scrollable content (no scroll hijack) or the horizontal swipe-back
 * (this only engages on a vertical-dominant drag). ui.c binds these to the Control Center: begin()
 * shows it, move(reveal_px) tracks the finger, end(reveal_px) snaps open/closed. Set once before the
 * first screen is built; NULL disables. */
typedef void (*nocsif_drag_begin_cb_t)(void);
typedef void (*nocsif_drag_move_cb_t)(int32_t reveal_px);
typedef void (*nocsif_drag_end_cb_t)(int32_t reveal_px);
void nocsif_nav_set_top_drag_cbs(nocsif_drag_begin_cb_t begin,
                                 nocsif_drag_move_cb_t move,
                                 nocsif_drag_end_cb_t end);

/* Append a flat menu row to a scaffold list: [icon] name … [tag?] [chevron].
 * - icon: a NOCSIF_ICON_* glyph (menu icon font) for the left column (NULL = no icon).
 * - target: the screen root to push on tap (NULL = non-navigating).
 * - tag/tag_kind: a right-aligned status tag before the chevron (NONE = omit).
 * - disabled: present-but-dimmed stub (LV_STATE_DISABLED + ash) that never navigates.
 * - tag_getter: OPTIONAL live-data getter (P4.3). When non-NULL AND a tag is drawn, the tag
 *   label is seeded from getter() and registered with nocsif_nav_register_live_label so the
 *   header tick keeps it current (e.g. the System>Power battery %). NULL = a static tag.
 * Returns the row object. (P3.2: icon column + icon-font chevron + larger type.) */
lv_obj_t *nocsif_menu_add_row(lv_obj_t *list, const char *icon, const char *name,
                              lv_obj_t *target, const char *tag, nocsif_tag_kind_t tag_kind,
                              bool disabled, nocsif_live_getter_t tag_getter);

#ifdef __cplusplus
}
#endif
