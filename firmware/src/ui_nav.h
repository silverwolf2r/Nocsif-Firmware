/*
 * Menu-first navigation shell: screen stack plus reusable widgets (UI-shell P3).
 *
 * This is just the navigation MECHANISM (spec section 5, plan
 * docs/research/ui-shell-2026-08-09.md section P3) — it knows nothing about specific
 * apps or screens; ui.c owns those:
 *   - a screen STACK (not tabs) of lv_obj_t* roots, animated with a custom set_x plus
 *     opacity slide-in (a small ~30px directional slide with a fade and ease-out):
 *     pushing slides forward, popping slides back.
 *   - reusable builders: a transparent-root screen scaffold (a header, a dashed rule,
 *     and an inset scrollable list column) and a flat menu row layout
 *     ([name] ... [tag?] [chevron]).
 *
 * COMPOSITING (from the P2 model, load-bearing): every screen root is transparent, so
 * the orrery on lv_layer_bottom() shows through and stays visible across screen swaps;
 * the rounded-corner masks live on lv_layer_top(). Screens are built once and cached
 * by the caller (auto_del=false), so going back is instant and any running state on a
 * screen survives.
 *
 * THREADING: everything here runs on the LVGL task — screen building happens under
 * the port lock inside nocsif_ui_init, and nav_push/back are called from LVGL event
 * callbacks that already hold the lock. Never call these from another task without
 * holding lvgl_port_lock() yourself.
 *
 * P3.1 note: real icons and the larger type scale arrive in P3.2 — for now rows use a
 * plain text chevron placeholder and the P1 font styles, and the only press feedback
 * is the pit-on background wash.
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Status tag kind (spec section 6). A tag is only ever drawn when there's actually
 * something to say — there is no idle tag. READY renders steel, RUN renders violet,
 * VALUE renders ash and holds a short value or count. */
typedef enum {
    NOCSIF_TAG_NONE = 0,   /* no tag: idle, nothing to show */
    NOCSIF_TAG_READY,      /* "ready", rendered steel */
    NOCSIF_TAG_RUN,        /* "running" or otherwise live, rendered violet */
    NOCSIF_TAG_VALUE,      /* a short value or count, rendered ash */
} nocsif_tag_kind_t;

/* Initializes the nav stack with `home_root` and loads it as the active screen with
 * no animation. Home always occupies stack index 0 and never gets a back arrow. */
void nocsif_nav_init(lv_obj_t *home_root);

/* Pushes a new screen onto the stack, sliding it in from the right over 240ms.
 * No-op if the stack is already full or `root` is NULL. `root` must be a screen the
 * caller keeps cached (built with auto_del=false). */
void nocsif_nav_push(lv_obj_t *root);

/* Pops one level off the stack, sliding the parent screen back in from the left
 * over 240ms. No-op if already at Home (index 0). The popped screen is deleted and
 * freed unless it was pinned via nocsif_nav_pin — this keeps cached widgets from
 * piling up in LVGL's fixed-size object pool. Anything that caches a screen root must
 * clear its own cache entry on that root's LV_EVENT_DELETE so a later launch rebuilds
 * it from scratch. */
void nocsif_nav_back(void);

/* (section 4.14) A screen can intercept the user's back gesture (swipe-right or the
 * header arrow): its hook runs first, and returning true consumes the back action
 * instead of popping — e.g. Signal Hunt drops its pinned target and shows a pick list
 * instead of actually navigating back; the next back press after that pops normally.
 * Only one hook is active at a time, tied to the screen that installed it, and it's
 * cleared automatically when that screen is deleted. Calls to nocsif_nav_back() made
 * directly from code are never intercepted by this hook. LVGL task only. */
typedef bool (*nocsif_back_hook_t)(void);
void nocsif_nav_set_back_hook(lv_obj_t *scr, nocsif_back_hook_t hook);

/* Marks a screen root as pinned, so nocsif_nav_back() will not free it when it's
 * popped. Use this for screens that own lv_timers or other live state (e.g. the USB
 * action screen) that needs to keep running after the user navigates away. Home
 * (stack index 0) is never popped anyway, so pinning it too is just extra safety. */
void nocsif_nav_pin(lv_obj_t *root);

/* True when Home (stack index 0) is the currently active screen — i.e. the user
 * isn't inside any submenu. The PWR short-press action uses this to decide between
 * turning the screen off (already at Home) or popping back to Home (inside a
 * submenu). LVGL task only. */
bool nocsif_nav_at_root(void);

/* Returns the currently active screen root (the top of the stack), or NULL before
 * nav_init has run. Used by launch-by-id so it can skip rebuilding or re-pushing an
 * app that's already showing. LVGL task only. */
lv_obj_t *nocsif_nav_top(void);

/* (section 4.8a companion) The current screen's title ("Home" at the root), updated
 * every time a header is built so the companion phone app's state push can mirror
 * where the watch currently is. This is a cached string, safe to read from any task. */
const char *nocsif_nav_current_title(void);

/* Pops every screen above Home in a single step: loads Home (sliding it in from the
 * left) and frees each screen above it in turn (respecting pins, just like
 * nocsif_nav_back). No-op if already at Home. This backs the PWR short-press's "go
 * back toward the watchface" behavior, currently scoped to just Home until the
 * peek/lock screen (P4.6) lands. LVGL task only. */
void nocsif_nav_pop_to_root(void);

/* Builds a detached, transparent screen root (lv_obj_create(NULL)) laid out as a
 * vertical flex column: a fixed header (back arrow, serif title, and mono
 * clock/battery placeholders) plus a dashed edge rule, followed by a scrollable
 * content column that fills the rest of the screen — so long screens (Home's three
 * bands, an 8-row submenu) scroll underneath a header that stays put. Pass
 * title=NULL for Home, which omits the back arrow and title and shows only the
 * right-aligned clock/battery; pass caption=NULL to skip the mono "// ..." section
 * caption. *content_out receives the scrollable container, to which sections can be
 * added via nocsif_menu_list() (a single list) or nocsif_band() (a labelled group,
 * used on Home). content_out may be NULL if the caller doesn't need it. */
lv_obj_t *nocsif_screen_scaffold(const char *title, const char *caption, lv_obj_t **content_out);

/* Adds an inset menu list (a flex column with a 26px inset and no dividers of its
 * own) to a scaffold's content container. Append rows to it with
 * nocsif_menu_add_row(). Returns the list object. */
lv_obj_t *nocsif_menu_list(lv_obj_t *content);

/* Adds a band heading (mono caps text, ash color, trailing hairline — matching the
 * mockup's .band) to a scaffold's content container, placed above the list it
 * labels (Home's operations/watch/system groupings use this). */
void nocsif_band(lv_obj_t *content, const char *text);

/* ---- live-data hook (UI-shell P4) ---- *
 * The header clock/battery, and the Time screen's readout, all display data that
 * changes over time, on screens that get freed and rebuilt every time the user
 * navigates back. So a single periodic tick needs to reach whichever labels are
 * currently alive, and a freshly rebuilt label needs to show the current value right
 * away. A label registers a getter function that returns its current cached string
 * (e.g. nocsif_rtc_clock_str); nocsif_nav_header_tick() then walks the set of
 * registered labels and updates each one only when its text actually changed (this
 * matters under full_refresh, where any label write repaints the entire frame). A
 * label removes itself from the set automatically on its own LV_EVENT_DELETE, so
 * freeing a screen also drops its labels (the live set stays small — roughly the
 * nav stack depth). All of this happens on the LVGL task. */
typedef const char *(*nocsif_live_getter_t)(void);

/* Registers `label` to be refreshed by nocsif_nav_header_tick() using `getter`.
 * The getter must return a stable cached string and must not touch hardware, since
 * it's called on the LVGL task. Callers should seed the label's initial text from
 * getter() themselves at build time, so it never briefly shows a placeholder. Safe
 * to call more than once per label; the registration is automatically cleared when
 * the label is deleted. */
void nocsif_nav_register_live_label(lv_obj_t *label, nocsif_live_getter_t getter);

/* Refreshes every registered live label, but only actually writes a label's text
 * when it changed (comparing with strcmp before calling lv_label_set_text). Called
 * from the single header-tick timer in ui.c, after the underlying data caches have
 * been refreshed. A label whose getter currently returns "" is hidden entirely, so
 * it takes no space in the header — this is what lets the optional header badge
 * below collapse away when there's nothing to show. */
void nocsif_nav_header_tick(void);

/* Registers an optional, app-supplied header status badge (UI-shell P4.5.4). Once
 * set (call this before the first screen is built), every scaffold header gains a
 * small right-aligned badge label bound to `getter`: it's seeded when the header is
 * built, kept current by the header tick's update-on-change logic, and hidden
 * whenever the getter returns "" (so it costs nothing while idle). ui.c uses this
 * for the HID-armed indicator, naming the HID keyboard whenever it's the active USB
 * class. This keeps ui_nav itself app-agnostic: ui_nav owns the badge's appearance
 * and lifecycle, while the app decides what it says, if anything. */
void nocsif_nav_set_header_badge(nocsif_live_getter_t getter);

/* (P8 v2.3) Registers finger-follow drag callbacks for a downward pull gesture that
 * starts on a scaffold header. Since the header is the fixed, non-scrolling top band,
 * a downward drag beginning there can drive these callbacks without fighting the
 * scrollable content below it (no scroll hijack) or the horizontal swipe-back gesture
 * (this only triggers on a vertical-dominant drag). ui.c wires these up to the
 * Control Center: begin() reveals it, move(reveal_px) tracks the finger's position,
 * and end(reveal_px) snaps it fully open or closed. Set once before the first screen
 * is built; passing NULL disables the feature. */
typedef void (*nocsif_drag_begin_cb_t)(void);
typedef void (*nocsif_drag_move_cb_t)(int32_t reveal_px);
typedef void (*nocsif_drag_end_cb_t)(int32_t reveal_px);
void nocsif_nav_set_top_drag_cbs(nocsif_drag_begin_cb_t begin,
                                 nocsif_drag_move_cb_t move,
                                 nocsif_drag_end_cb_t end);

/* Appends a flat menu row to a scaffold's list: [icon] name ... [tag?] [chevron].
 * - icon: a NOCSIF_ICON_* glyph from the menu icon font for the left column, or NULL
 *   for no icon.
 * - target: the screen to push when the row is tapped, or NULL for a non-navigating
 *   row.
 * - tag/tag_kind: an optional right-aligned status tag shown before the chevron;
 *   pass NOCSIF_TAG_NONE to omit it.
 * - disabled: renders the row as a dimmed, present-but-inert stub (LV_STATE_DISABLED
 *   plus ash coloring) that never navigates on tap.
 * - tag_getter: an optional live-data getter (P4.3). When it's non-NULL and a tag is
 *   being drawn, the tag label is seeded from getter() and registered with
 *   nocsif_nav_register_live_label so the header tick keeps it current (e.g. the
 *   System > Power battery percentage). Pass NULL for a static, unchanging tag.
 * Returns the row object. (P3.2 adds the icon column, icon-font chevron, and larger
 * type.) */
lv_obj_t *nocsif_menu_add_row(lv_obj_t *list, const char *icon, const char *name,
                              lv_obj_t *target, const char *tag, nocsif_tag_kind_t tag_kind,
                              bool disabled, nocsif_live_getter_t tag_getter);

#ifdef __cplusplus
}
#endif
