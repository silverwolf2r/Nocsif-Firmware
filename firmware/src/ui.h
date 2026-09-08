/*
 * NocSif — LVGL v9 UI layer (M3)
 *
 * Brings up espressif/esp_lvgl_port on top of the CO5300 esp_lcd panel and the
 * CST9217 touch controller, then shows a minimal demo (a button + a label that
 * tracks live touch coordinates) to prove the display + input stack end-to-end.
 *
 * PRECONDITIONS (call order in main): nocsif_display_init() and
 * nocsif_touch_init() must both have succeeded first — this binds LVGL to the
 * display panel/IO handles and makes the LVGL input device the SOLE reader of
 * the CST9217 (there must be no other touch-polling task, or reports race).
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Initialise esp_lvgl_port (tick/task/mutex), register the CO5300 display and a
 * pointer input device fed by nocsif_touch_read, and build the menu-first shell.
 * Idempotent. Returns ESP_OK once the UI is running on the LVGL port task. */
esp_err_t nocsif_ui_init(void);

/* Bring the shell to a screen by its stable app id (e.g. "wifi", "usb", "system").
 * The single launch-by-id entry point so later bindings (the FN side-button, peek-screen
 * planets, gestures) can target any app; menu rows already resolve through it. A disabled
 * stub or unknown id is a logged no-op. Must run on the LVGL task (holds the port lock) —
 * today it is only called from LVGL row callbacks. */
void nocsif_app_launch(const char *id);

/* ---- physical-button actions (UI-shell P4.4) --------------------------------- *
 * Called from the button worker task (buttons.c) when it decodes an event. Each MARSHALS the
 * work onto the LVGL task (lvgl_port_lock + lv_async_call) and returns immediately — buttons.c
 * must never touch LVGL widgets directly (LVGL is single-threaded behind esp_lvgl_port). Safe
 * to call before the UI is up (they no-op until nocsif_ui_init completes). Named by the button
 * EVENT; the handler in ui.c maps each to an action.
 *   pwr_short : dismiss power menu · wake (if asleep) · pop to Home (in a submenu) · screen off (at Home)
 *   pwr_long  : open the power menu (Power off / Restart)
 *   pwr_double: launch the bound PWR double-press shortcut (settings; no-op if unbound)
 *   fn_short  : back one screen (USER-REQUESTED FN mapping, 2026-08-10)
 *   fn_double : launch the FN shortcut app (settings; default "wifi") */
void nocsif_ui_btn_pwr_short(void);
void nocsif_ui_btn_pwr_long(void);
void nocsif_ui_btn_pwr_double(void);
void nocsif_ui_btn_fn_short(void);
void nocsif_ui_btn_fn_double(void);

/* ---- boot splash bring-up log (UI-shell P5) ---------------------------------- *
 * The boot splash reveals an honest bring-up log; two of its lines (storage, usb) reflect results
 * that aren't known when nocsif_ui_init returns. app_main calls these AFTER init, once each result
 * is in, to resolve those lines. Off the LVGL task (they only publish a small buffer + flag the
 * reveal timer polls). Safe to call even if the UI never came up or the splash already finished
 * (no-op). `detail` is a short honest token, e.g. "ready" / "no card" (storage) or "detached" (usb);
 * pass NULL to let storage fall back to ok?"ready":"no card". */
void nocsif_ui_boot_report_storage(bool ok, const char *detail);
void nocsif_ui_boot_report_usb(const char *label);

/* ---- §4.15 desktop bridge — one complete frame of the active screen ------------------------------ *
 * Fills `out` (RGB565 little-endian, *w × *h = the panel's own 410×502, 411,640 B) by forcing a full
 * repaint through the mirror's flush tap. Runs on the CALLER's task under the LVGL port lock
 * (~30–60 ms); safe from any task once the UI is up. False if the UI isn't ready, `out` is too small,
 * or the lock can't be taken within 500 ms. */
#include <stddef.h>
#include <stdint.h>
#define NOCSIF_UI_MIRROR_W 410
#define NOCSIF_UI_MIRROR_H 502
bool nocsif_ui_screenshot(uint8_t *out, size_t out_len, int *w, int *h);

/* ---- §4.15 live view over USB — poll the changed part of the mirror ------------------------------ *
 * Copies the rectangle of the mirror buffer that changed since the previous poll (union of every
 * flushed region, RGB565-LE rows of `*w` pixels, `*h` rows) into `out`, resets the dirty box, bumps
 * *seq, and re-arms the flush tap for ~2 s (so polling keeps it alive at zero cost when idle). `full`
 * (or a tap that had lapsed) forces a complete repaint and returns the whole frame. `scale` 1 = the
 * panel's pixels (*x/*y/*w/*h in panel coordinates); 2 = every other pixel and row (coordinates
 * halved) for a slower link. Returns false when nothing changed (no bytes written) or the UI is
 * busy/not ready. Runs on the caller's task under the port lock; safe from any task once the UI is up. */
bool nocsif_ui_mirror_poll(bool full, int scale, uint8_t *out, size_t out_len, int *x, int *y, int *w,
                           int *h, uint32_t *seq);

#ifdef __cplusplus
}
#endif
