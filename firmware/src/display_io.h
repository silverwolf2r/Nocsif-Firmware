/*
 * NocSif — CO5300 QSPI panel IO with a STREAMING pixel path and a never-allocate-internal-DMA guarantee.
 * RAM remediation Phase A1 (docs/RAM-BUDGET.md remake #15; docs/DMA-COEXISTENCE-VERIFICATION.md).
 *
 * A drop-in esp_lcd_panel_io_handle_t for the espressif/esp_lcd_co5300 driver that replicates
 * esp_lcd_panel_io_spi's QSPI framing byte-for-byte, plus two things the stock IO cannot do:
 *
 *  1. STREAMING (the LVGL flush). esp_lcd's tx_color API re-sends CASET/RASET/RAMWR for every
 *     draw_bitmap, so PR #95's stage-band flush paid three polled transactions per band and had to use
 *     big bands (2 x 12 lines = 29.5 KB of boot-permanent internal DMA) to amortise them. The stream API
 *     sets the write window ONCE, then pushes the pixels as successive chunks with CS held — one
 *     continuous RAMWR window, no per-band commands — so a SMALL internal stage (2 x 4 lines = 9.8 KB)
 *     streams a full frame at the memcpy-bound rate. ~19.7 KB returns to the BLE/WiFi/USB pool.
 *
 *  2. NO INTERNAL-DMA ALLOCATION, EVER. Every pixel transaction carries SPI_TRANS_DMA_USE_PSRAM: an
 *     internal (stage) source is unaffected; a PSRAM source is DMA'd in place (the S3 GDMA reads
 *     external RAM, SOC_PSRAM_DMA_CAPABLE; spi_master write-backs the cache itself) instead of being
 *     bounced through an internal buffer spi_master mallocs PER CHUNK — the allocation that wedged
 *     taskLVGL under memory pressure (gotcha-display-dma-hang). Commands and short params travel
 *     embedded in the transaction (USE_TXDATA), so no display path can ask the heap for internal DMA.
 *     ⚠ MEASURED (2026-09-07): a PSRAM-sourced pixel DMA UNDERRUNS at the 80 MHz pixel clock — 4 of 19
 *     chunks on the 617 KB boot clear (quad PSRAM through the cache tops out ~30 MB/s; the panel pulls
 *     40 MB/s). So pixels must come from an INTERNAL stage at 80 MHz; the flag is kept purely as the
 *     safety net (a corrupt chunk beats a failed allocation). The IO counts those underruns
 *     (nocsif_display_io_tx_fail): it must read 0 in steady use.
 *
 * Framing (identical to esp_lcd in quad_mode, lcd_cmd_bits=32, lcd_param_bits=8, no D/C line):
 *   tx_param : [32-bit cmd, 1 line, MSB first] (CS held) [params, 1 line, 8-bit each]
 *   tx_color : [32-bit cmd, 1 line] (CS held) [pixels on 4 lines, <=32 KB chunks, CS held between]
 *   stream   : stream_begin = the cmd with CS held; stream_chunk = one pixel chunk (CS held unless last)
 * The CO5300 driver wraps the DCS byte into the 32-bit word itself (0x02<<24|cmd<<8 for params,
 * 0x32<<24|0x2C<<8 for pixels); this IO just puts those 4 bytes on the wire big-endian. The user's
 * on_color_trans_done fires from the SPI ISR on the LAST chunk (tx_color or stream) — the same contract
 * as esp_lcd / esp_lvgl_port.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_lcd_types.h"        /* esp_lcd_panel_io_handle_t */
#include "driver/spi_master.h"    /* spi_host_device_t */

#ifdef __cplusplus
extern "C" {
#endif

/* Largest single SPI transaction the S3 can DMA (SPI_LL_DMA_MAX_BIT_LEN = 1<<18 bits). Pixel data
 * is chunked to this; size the QSPI bus max_transfer_sz to it. */
#define NOCSIF_DISPLAY_IO_MAX_TRANSFER  32768

typedef struct {
    int    cs_gpio_num;   /* panel chip-select */
    int    pclk_hz;       /* QSPI pixel clock */
    int    spi_mode;      /* SPI mode (CO5300: 0) */
    size_t queue_depth;   /* pixel chunks in flight (1..32). For the stream path make this EQUAL to the
                           * number of stage buffers: stream_reserve() returning then means the buffer
                           * about to be reused is free. */
} nocsif_display_io_cfg_t;

/* Create the IO on an already-initialised QSPI bus (spi_bus_initialize with the 4 data lines). The
 * returned handle plugs straight into esp_lcd_new_panel_co5300(). Never freed in NocSif. */
esp_err_t nocsif_display_io_qspi_new(spi_host_device_t host, const nocsif_display_io_cfg_t *cfg,
                                     esp_lcd_panel_io_handle_t *ret_io);

/* ---- streaming pixel path ------------------------------------------------------------------
 * Caller sequence per frame/rect: set the window (CASET/RASET via esp_lcd_panel_io_tx_param) ->
 * stream_begin(RAMWR word) -> per band { stream_reserve(); fill the band; stream_chunk(band, n, last) }.
 * stream_begin drains any queued chunks first (a window change must never interleave with pixel
 * data). stream_reserve blocks until a chunk slot is free. stream_chunk queues one chunk (<=32 KB,
 * internal or PSRAM); on `last` CS releases and on_color_trans_done fires from the ISR when it lands.
 * stream_finish blocks until every queued chunk has landed (needed before FREEING a band buffer — a
 * following tx_param/stream_begin drains implicitly, so the LVGL flush never calls it). */
esp_err_t nocsif_display_io_stream_begin(esp_lcd_panel_io_handle_t io, int lcd_cmd);
esp_err_t nocsif_display_io_stream_reserve(esp_lcd_panel_io_handle_t io);
esp_err_t nocsif_display_io_stream_chunk(esp_lcd_panel_io_handle_t io, const void *buf, size_t len, bool last);
esp_err_t nocsif_display_io_stream_finish(esp_lcd_panel_io_handle_t io);

/* Telemetry. tx_fail = chunks spi_master flagged SPI_TRANS_DMA_TX_FAIL (DMA underrun: the chunk's
 * pixels were corrupt; the transfer still completed). Must read 0 in steady use — the internal stage
 * cannot underrun, only a PSRAM-sourced chunk can. color_chunks = completed pixel chunks. */
uint32_t nocsif_display_io_tx_fail(void);
uint32_t nocsif_display_io_color_chunks(void);

#ifdef __cplusplus
}
#endif
