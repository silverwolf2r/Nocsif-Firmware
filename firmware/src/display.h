/*
 * NocSif — CO5300 QSPI AMOLED display driver (M1).
 *
 * Drives the 2.06" 410x502 24-bit RGB888 AMOLED panel over the espressif/esp_lcd_co5300 managed
 * component. Pins (docs/HARDWARE.md): CS=41 SCK=40 D0=38 D1=39 D2=42 D3=45 TE=6 RESET=37.
 * PRECONDITION: the display power rail must already be on — AXP2101 ALDO2 = 3.3V
 * (nocsif_power_display_rail) and XL9555 IO7 driven high (nocsif_xl9555_display_power) — otherwise
 * the panel stays dark no matter what init does.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_lcd_types.h"     /* esp_lcd_panel_handle_t / esp_lcd_panel_io_handle_t */

#ifdef __cplusplus
extern "C" {
#endif

#define NOCSIF_DISP_W  410
#define NOCSIF_DISP_H  502

/* Size of one flush stage band (see ui.c's flush callback and display.c's stream_frame). LVGL's
 * draw buffers live in PSRAM (RGB888); each flush is copied PSRAM->internal in bands of this many
 * panel lines, streamed out from there, with two bands ping-ponging. History: the stock esp_lcd SPI
 * panel IO bounced PSRAM through a freshly-malloc'd internal DMA buffer PER FLUSH, and under memory
 * pressure that allocation could fail and wedge the LVGL task forever — so an earlier fix (PR #95)
 * introduced a persistent staging buffer, but since esp_lcd re-sends the write-window commands on
 * every draw, it needed BIG bands (2 x 12 lines = 29.5 KB) to make that overhead worth paying. The
 * RAM remediation work replaced esp_lcd's panel IO with a NocSif one (display_io.c) that streams a
 * whole flush through ONE write window, so the per-band command cost disappears and bands can be
 * small — 2 x 4 lines = 9.8 KB, returning ~19.7 KB to the pool BLE/WiFi/USB share. Each band
 * boundary must stay 2-pixel aligned or the panel shears (see ui_rounder_cb) — DMA'ing straight
 * from PSRAM with no staging buffer at all was tried and measured to underrun at 80 MHz
 * (display_io.h), so the staging step stays. */
#define NOCSIF_FLUSH_STAGE_LINES  4
#define NOCSIF_FLUSH_STAGE_BYTES  ((size_t)NOCSIF_FLUSH_STAGE_LINES * NOCSIF_DISP_W * 3)

/* Brings up the QSPI bus, the panel IO, and the CO5300 panel itself (reset, init, gap=22, on).
 * Idempotent. Returns ESP_OK once the panel is ready to draw. */
esp_err_t nocsif_display_init(void);

/* Handles for LVGL (esp_lvgl_port) to attach to. Only valid after a successful
 * nocsif_display_init(); NULL otherwise. The panel already applies the 22px x-gap inside
 * draw_bitmap, and its element order is BGR (matching LVGL's RGB888 B,G,R buffer layout), so
 * lvgl_port can hand the pixel buffer straight to draw_bitmap with no conversion. */
esp_lcd_panel_handle_t    nocsif_display_panel(void);
esp_lcd_panel_io_handle_t nocsif_display_io(void);

/* Opens a streaming write window: sends CASET/RASET for the inclusive panel rect [x1..x2]x[y1..y2]
 * (with the 22-px x-gap applied, same as the component's own draw_bitmap), then the RAMWR command
 * with CS held. Follow with nocsif_display_io_stream_reserve/_chunk(...) per band (display_io.h);
 * the total pixel bytes across one window must equal (x2-x1+1)*(y2-y1+1)*3. Any previous window's
 * remaining chunks are drained first. */
esp_err_t nocsif_display_window_begin(int x1, int y1, int x2, int y2);

/* Turns the screen on/off (UI-shell P4.4's PWR short-press screen toggle). sleep=true sends DISPOFF
 * (0x28), blanking the emissive AMOLED (pixels simply off — a real screen-off with no re-init
 * needed to wake); sleep=false sends DISPON (0x29). The display rail and panel state stay up, so
 * waking is instant. Idempotent; a no-op before init. Note: this only darkens the panel — gating
 * LVGL/touch is the caller's job (ui.c disables the input device while asleep). Deeper sleep (rail
 * off, CPU, PSRAM) is a later power-management milestone. */
esp_err_t nocsif_display_sleep(bool sleep);

/* True while the panel is blanked by nocsif_display_sleep(true). */
bool nocsif_display_is_asleep(void);

/* Sets panel brightness via the CO5300's WRDISBV command (0x51): 0x00 = no emission, 0xFF = full.
 * UI-shell P5 holds this at 0 from nocsif_display_init() onward so nothing is emitted during
 * bring-up (avoiding the power-on white flash), then the UI ramps it to full right after flushing
 * the first real frame (the boot splash). A no-op before init. */
esp_err_t nocsif_display_set_brightness(uint8_t level);

/* Fills the whole visible panel with one RGB888 colour (0x00RRGGBB). */
esp_err_t nocsif_display_fill(uint32_t rgb888);

/* Diagnostic: draws four horizontal bands (red/green/blue/white) to sanity-check addressing, the
 * 22px x-gap, and RGB888 byte/colour order in one glance. M1 confirmed on-device that RGB element
 * order shows red as red; M3 switched the panel to BGR element order (matching LVGL's B,G,R
 * buffers) and compensates by writing B,G,R in the raw draw path, so the bands must still read
 * red/green/blue/white top-to-bottom. Run this before trusting the LVGL UI's colours. */
esp_err_t nocsif_display_test_bands(void);

/* Diagnostic: draws a smooth top-to-bottom grayscale ramp with no hard colour jumps — a visible
 * horizontal line here would mean a rendering seam, while a clean gradient confirms the band-test
 * lines above were a real transition rather than a rendering artifact. */
esp_err_t nocsif_display_test_gradient(void);

#ifdef __cplusplus
}
#endif
