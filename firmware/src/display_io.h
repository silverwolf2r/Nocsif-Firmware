/*
 * NocSif — CO5300 QSPI panel IO with a streaming pixel path and a guarantee that it never
 * allocates internal DMA memory. Part of the RAM remediation work (docs/RAM-BUDGET.md remake #15;
 * docs/DMA-COEXISTENCE-VERIFICATION.md).
 *
 * A drop-in esp_lcd_panel_io_handle_t for the espressif/esp_lcd_co5300 driver that reproduces
 * esp_lcd_panel_io_spi's QSPI framing exactly, while adding two things the stock IO can't do:
 *
 *  1. STREAMING for the LVGL flush. The stock esp_lcd tx_color API re-sends the CASET/RASET/RAMWR
 *     write-window commands on every draw_bitmap call, so an earlier fix (PR #95) that staged
 *     flushes through internal memory had to use big bands (2 x 12 lines = 29.5 KB of boot-
 *     permanent internal DMA) just to amortize that per-band command cost. This IO's stream API
 *     sets the write window ONCE, then pushes pixels as consecutive chunks with CS held — one
 *     continuous RAMWR transfer, no repeated commands — so a small internal stage (2 x 4 lines =
 *     9.8 KB) can stream a whole frame at the memcpy-bound rate, returning ~19.7 KB to the
 *     BLE/WiFi/USB memory pool.
 *
 *  2. NO INTERNAL-DMA ALLOCATION, EVER. Every pixel transaction is flagged SPI_TRANS_DMA_USE_PSRAM:
 *     an internal (staging) source is unaffected, while a PSRAM source is DMA'd directly (the S3's
 *     GDMA can read external RAM; spi_master handles the cache write-back itself) instead of being
 *     bounced through an internal buffer that spi_master would otherwise malloc per chunk — that
 *     allocation is exactly what could wedge the LVGL task under memory pressure
 *     (gotcha-display-dma-hang). Commands and short parameters travel embedded in the transaction
 *     itself, so no display path ever asks the heap for internal DMA memory.
 *     Measured (2026-09-07): a PSRAM-sourced pixel DMA can underrun at the 80 MHz pixel clock (4 of
 *     19 chunks underran on a 617 KB boot-time frame clear — quad PSRAM through the cache tops out
 *     around 30 MB/s, while the panel wants 40 MB/s). So in practice pixels must come from an
 *     internal staging buffer at 80 MHz; the PSRAM-DMA flag is kept purely as a safety net (a
 *     corrupt chunk is preferable to a failed allocation). The IO counts these underruns
 *     (nocsif_display_io_tx_fail) — it should read 0 in normal use.
 *
 * Framing (identical to esp_lcd in quad mode, lcd_cmd_bits=32, lcd_param_bits=8, no D/C line):
 *   tx_param : [32-bit cmd, 1 line, MSB first] (CS held) [params, 1 line, 8-bit each]
 *   tx_color : [32-bit cmd, 1 line] (CS held) [pixels on 4 lines, <=32 KB chunks, CS held between]
 *   stream   : stream_begin = the cmd with CS held; stream_chunk = one pixel chunk (CS held unless last)
 * The CO5300 driver wraps the DCS byte into the 32-bit word itself (0x02<<24|cmd<<8 for params,
 * 0x32<<24|0x2C<<8 for pixels); this IO just puts those 4 bytes on the wire, big-endian. The
 * on_color_trans_done callback fires from the SPI ISR on the LAST chunk of a tx_color or stream —
 * the same contract as esp_lcd / esp_lvgl_port expect.
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

/* The largest single SPI transaction the S3 can DMA in one go (SPI_LL_DMA_MAX_BIT_LEN = 1<<18
 * bits). Pixel data is chunked to this size; set the QSPI bus's max_transfer_sz to match. */
#define NOCSIF_DISPLAY_IO_MAX_TRANSFER  32768

typedef struct {
    int    cs_gpio_num;   /* panel chip-select */
    int    pclk_hz;       /* QSPI pixel clock */
    int    spi_mode;      /* SPI mode (CO5300: 0) */
    size_t queue_depth;   /* pixel chunks in flight (1..32). For the stream path, set this equal to
                           * the number of stage buffers: a stream_reserve() call then returns
                           * exactly once the buffer about to be reused has landed. */
} nocsif_display_io_cfg_t;

/* Creates the IO on an already-initialized QSPI bus (spi_bus_initialize with the 4 data lines).
 * The returned handle plugs directly into esp_lcd_new_panel_co5300(). Never freed by NocSif. */
esp_err_t nocsif_display_io_qspi_new(spi_host_device_t host, const nocsif_display_io_cfg_t *cfg,
                                     esp_lcd_panel_io_handle_t *ret_io);

/* ---- streaming pixel path ------------------------------------------------------------------
 * Caller sequence per frame/rect: set the window (CASET/RASET via esp_lcd_panel_io_tx_param), then
 * stream_begin(RAMWR word), then per band: stream_reserve(); fill the band; stream_chunk(band, n,
 * last). stream_begin drains any already-queued chunks first, since a window change must never
 * interleave with pixel data. stream_reserve blocks until a chunk slot frees up. stream_chunk
 * queues one chunk (<=32 KB, internal or PSRAM); on the `last` chunk, CS releases and
 * on_color_trans_done fires from the ISR once it lands. stream_finish blocks until every queued
 * chunk has actually landed — needed before FREEING a band buffer (a following tx_param/
 * stream_begin drains implicitly, so the LVGL flush path never needs to call it directly). */
esp_err_t nocsif_display_io_stream_begin(esp_lcd_panel_io_handle_t io, int lcd_cmd);
esp_err_t nocsif_display_io_stream_reserve(esp_lcd_panel_io_handle_t io);
esp_err_t nocsif_display_io_stream_chunk(esp_lcd_panel_io_handle_t io, const void *buf, size_t len, bool last);
esp_err_t nocsif_display_io_stream_finish(esp_lcd_panel_io_handle_t io);

/* Telemetry. tx_fail counts chunks spi_master flagged SPI_TRANS_DMA_TX_FAIL (a DMA underrun — that
 * chunk's pixels were corrupted, but the transfer still completed). Should read 0 in normal use;
 * only a PSRAM-sourced chunk can underrun, never one from the internal stage. color_chunks counts
 * completed pixel chunks overall. */
uint32_t nocsif_display_io_tx_fail(void);
uint32_t nocsif_display_io_color_chunks(void);

#ifdef __cplusplus
}
#endif
