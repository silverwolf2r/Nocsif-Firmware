/*
 * LVGL v9 UI layer (M3).
 *
 * Brings up espressif/esp_lvgl_port on top of the CO5300 esp_lcd panel and
 * the CST9217 touch controller, then shows a minimal demo — a button plus a
 * label tracking live touch coordinates — to prove the display and input
 * stack end-to-end.
 *
 * Preconditions (call order in main): nocsif_display_init() and
 * nocsif_touch_init() must both have already succeeded — this binds LVGL to
 * the display panel/IO handles and makes the LVGL input device the sole
 * reader of the CST9217 (there must be no other touch-polling task, or
 * reports will race).
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Initializes esp_lvgl_port (tick/task/mutex), registers the CO5300 display and a pointer input device fed by nocsif_touch_read, and builds the menu-first shell. Safe to call more than once. Returns ESP_OK once the UI is running on the LVGL port task. */
esp_err_t nocsif_ui_init(void);

/* Brings the shell to a screen by its stable app id (e.g. "wifi",
 * "usb", "system"). The single launch-by-id entry point, so later bindings
 * — the FN side-button, peek-screen planets, gestures — can target any
 * app; menu rows already resolve through it too. A disabled stub or
 * unknown id is a logged no-op. Must run on the LVGL task, holding the
 * port lock — today it's only called from LVGL row callbacks. */
void nocsif_app_launch(const char *id);

/* ---- physical-button actions (UI-shell P4.4) ---- *
 * Called from the button worker task (buttons.c) when it decodes an
 * event. Each of these marshals the work onto the LVGL task
 * (lvgl_port_lock plus lv_async_call) and returns immediately — buttons.c
 * must never touch LVGL widgets directly, since LVGL is single-threaded
 * behind esp_lvgl_port. Safe to call before the UI is up; they no-op
 * until nocsif_ui_init completes. Named by the button event; the handler
 * in ui.c maps each to an action.
 *   pwr_short : dismisses the power menu, wakes if asleep, pops to Home
 *               in a submenu, or turns the screen off at Home
 *   pwr_long  : opens the power menu (Power off / Restart)
 *   pwr_double: launches the bound PWR double-press shortcut (from
 *               settings; a no-op if unbound)
 *   fn_short  : goes back one screen (a user-requested FN mapping, 2026-08-10)
 *   fn_double : launches the FN shortcut app (from settings; default "wifi") */
void nocsif_ui_btn_pwr_short(void);
void nocsif_ui_btn_pwr_long(void);
void nocsif_ui_btn_pwr_double(void);
void nocsif_ui_btn_fn_short(void);
void nocsif_ui_btn_fn_double(void);

/* ---- boot splash bring-up log (UI-shell P5) ---- *
 * The boot splash reveals an honest bring-up log; two of its lines —
 * storage and usb — reflect results that aren't known yet when
 * nocsif_ui_init returns. app_main calls these after init, once each
 * result is in, to resolve those lines. Called off the LVGL task; they
 * only publish a small buffer and flag it for the reveal timer to poll.
 * Safe to call even if the UI never came up, or the splash already
 * finished — both are no-ops. `detail` is a short, honest token, e.g.
 * "ready" / "no card" for storage, or "detached" for usb; pass NULL to
 * let storage fall back to ok?"ready":"no card". */
void nocsif_ui_boot_report_storage(bool ok, const char *detail);
void nocsif_ui_boot_report_usb(const char *label);

/* ---- section 4.15 desktop bridge: one complete frame of the
 * active screen ---- * Fills `out` (RGB565, little-endian; *w x *h is
 * the panel's own 410x502, 411,640 bytes) by forcing a full repaint
 * through the mirror's flush tap. Runs on the caller's task, under the
 * LVGL port lock, roughly 30-60ms; safe from any task once the UI is
 * up. Returns false if the UI isn't ready, `out` is too small, or the
 * lock can't be taken within 500ms. */
#include <stddef.h>
#include <stdint.h>
#define NOCSIF_UI_MIRROR_W 410
#define NOCSIF_UI_MIRROR_H 502
bool nocsif_ui_screenshot(uint8_t *out, size_t out_len, int *w, int *h);

/* ---- section 4.15 live view over USB: polls the changed part
 * of the mirror ---- * Copies the rectangle of the mirror buffer that
 * changed since the previous poll — the union of every flushed region,
 * as RGB565-LE rows of `*w` pixels, `*h` rows — into `out`, resets the
 * dirty box, bumps *seq, and re-arms the flush tap for roughly 2s, so
 * polling keeps it alive at zero cost when idle. `full` — or a tap that
 * had lapsed — forces a complete repaint and returns the whole frame.
 * `scale` 1 means the panel's own pixels (*x/*y/*w/*h in panel
 * coordinates); 2 means every other pixel and row, with coordinates
 * halved, for a slower link. Returns false when nothing changed (no
 * bytes written) or the UI is busy/not ready. Runs on the caller's task
 * under the port lock; safe from any task once the UI is up. */
bool nocsif_ui_mirror_poll(bool full, int scale, uint8_t *out, size_t out_len, int *x, int *y, int *w,
                           int *h, uint32_t *seq);

#ifdef __cplusplus
}
#endif
