/*
 * NocSif — CO5300 QSPI AMOLED display (M1)
 *
 * 2.06" 410x502 24-bit RGB888 AMOLED via the espressif/esp_lcd_co5300 managed component.
 * Pins (docs/HARDWARE.md): CS=41 SCK=40 D0=38 D1=39 D2=42 D3=45 TE=6 RESET=37.
 * PRECONDITION: the display rail must be powered first — AXP2101 ALDO2 = 3.3V
 * (nocsif_power_display_rail) and XL9555 IO7 high (nocsif_xl9555_display_power) —
 * or the panel stays dark regardless of init.
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

/* The flush stage band (ui.c nocsif_flush_cb + display.c stream_frame): LVGL's draw buffers live in
 * PSRAM (RGB888, buff_spiram); each flush is memcpy'd PSRAM->internal in bands of this many panel lines
 * and streamed to the panel from there (two bands ping-pong). History: esp_lcd's stock SPI panel-IO
 * bounced PSRAM->an INTERNAL DMA buffer allocated PER FLUSH; under memory pressure that alloc failed
 * and the flush wedged taskLVGL forever, so PR #95 introduced a persistent stage — but esp_lcd re-sends
 * CASET/RASET/RAMWR per draw, which forced BIG bands (2 x 12 lines = 29.5 KB) to amortise them. RAM
 * remediation Phase A1: the NocSif panel IO (display_io.c) streams a whole flush through ONE write
 * window (nocsif_display_window_begin + stream chunks with CS held), so bands cost nothing in panel
 * time and can be SMALL — 2 x 4 lines = 9.8 KB, returning ~19.7 KB to the BLE/WiFi/USB pool. EVEN
 * (each band boundary must stay 2-px aligned or the CO5300 shears — see ui_rounder_cb). PSRAM-direct
 * DMA (no stage at all) was measured to underrun at 80 MHz (display_io.h) — the stage stays. */
#define NOCSIF_FLUSH_STAGE_LINES  4
#define NOCSIF_FLUSH_STAGE_BYTES  ((size_t)NOCSIF_FLUSH_STAGE_LINES * NOCSIF_DISP_W * 3)

/* Bring up the QSPI bus, panel IO, and CO5300 panel (reset, init, gap=22, on).
 * Idempotent. Returns ESP_OK once the panel is ready to draw. */
esp_err_t nocsif_display_init(void);

/* Handles for LVGL (esp_lvgl_port) to bind to. Valid only after a successful
 * nocsif_display_init(); NULL otherwise. The panel already applies the 22px
 * x-gap inside draw_bitmap, and rgb_ele_order is BGR (matches LVGL's RGB888
 * B,G,R buffer) — so lvgl_port can hand px_map straight to draw_bitmap. */
esp_lcd_panel_handle_t    nocsif_display_panel(void);
esp_lcd_panel_io_handle_t nocsif_display_io(void);

/* Streaming write window (Phase A1): CASET/RASET for the INCLUSIVE panel rect [x1..x2]x[y1..y2]
 * (the 22-px x-gap applied here, like the component's draw_bitmap) + the RAMWR word with CS held.
 * Follow with nocsif_display_io_stream_reserve/_chunk(...) per band (display_io.h); the pixels of one
 * window must total (x2-x1+1)*(y2-y1+1)*3 bytes. Drains any previous window's tail first. */
esp_err_t nocsif_display_window_begin(int x1, int y1, int x2, int y2);

/* Screen on/off (UI-shell P4.4 — PWR short-press screen toggle). sleep=true sends the
 * panel DISPOFF (0x28), which blanks the emissive AMOLED (pixels off — true screen-off,
 * no re-init needed to wake); sleep=false sends DISPON (0x29). The display rail and panel
 * state are left up, so wake is instant. Idempotent; no-op before init. NOTE: this only
 * darkens the panel — LVGL/touch gating is the caller's job (ui.c disables the indev while
 * asleep). Deeper sleep (rail off / CPU / PSRAM) is a later power-management milestone. */
esp_err_t nocsif_display_sleep(bool sleep);

/* True while the panel is blanked by nocsif_display_sleep(true). */
bool nocsif_display_is_asleep(void);

/* Set panel brightness (CO5300 WRDISBV 0x51): 0x00 = off (no emission), 0xFF = full. UI-shell P5
 * holds this at 0 from nocsif_display_init() so the panel emits no light during bring-up (killing the
 * power-on white flash), and the UI ramps it to full immediately after flushing the first real frame
 * (the boot splash). No-op before init. */
esp_err_t nocsif_display_set_brightness(uint8_t level);

/* Fill the whole visible panel with one RGB888 colour (0x00RRGGBB). */
esp_err_t nocsif_display_fill(uint32_t rgb888);

/* Diagnostic: four horizontal bands red/green/blue/white — proves addressing,
 * the 22px x-gap, and RGB888 byte/colour order in one glance. M1 verified on-device
 * that R shows red with RGB element order; M3 flipped the panel to BGR element order
 * (to match LVGL's B,G,R RGB888 buffers) and the raw draw path now writes B,G,R to
 * compensate, so the bands must STILL read RED/GREEN/BLUE/WHITE top->bottom. This is
 * the red-fill sanity check before trusting the LVGL UI. */
esp_err_t nocsif_display_test_bands(void);

/* Diagnostic: smooth top-to-bottom grayscale ramp — no hard colour jumps, so a
 * visible horizontal line would mean a rendering seam, whereas a clean gradient
 * confirms the band lines were the panel's hard-transition artifact. */
esp_err_t nocsif_display_test_gradient(void);

#ifdef __cplusplus
}
#endif
