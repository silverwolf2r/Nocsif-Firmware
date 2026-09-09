/*
 * NocSif — CO5300 QSPI panel IO: streaming pixel path + never-allocate-internal-DMA guarantee. See
 * display_io.h.
 *
 * Mirrors esp_lcd_panel_io_spi.c (ESP-IDF 5.5.4, esp_lcd/spi) in quad mode with lcd_cmd_bits=32,
 * lcd_param_bits=8, and no D/C GPIO — the same configuration CO5300_PANEL_IO_QSPI_CONFIG produces —
 * with these deliberate differences:
 *   - a streaming API (set the window once, then send chunks with CS held) built for the LVGL flush;
 *   - every pixel transaction is flagged SPI_TRANS_DMA_USE_PSRAM (an internal source is unaffected;
 *     a PSRAM source is DMA'd in place instead of being bounced through an internal malloc);
 *   - the 32-bit command word and short parameter lists go out via SPI_TRANS_USE_TXDATA (embedded,
 *     no separate buffer at all);
 *   - a free-slot bitmask over the descriptor pool. esp_lcd's own stack-style pool (where entries
 *     0..inflight-1 are always the in-flight ones) only works when recycling and reuse happen
 *     together; with a separate reserve() step, the slot spi_master hands back is not necessarily
 *     pool[inflight], so slots are tracked explicitly here instead;
 *   - DMA-fail-tolerant bookkeeping. spi_device_get_trans_result DEQUEUES a transaction that
 *     underran/overran and then returns ESP_ERR_INVALID_STATE (see spi_master.c) — that chunk's
 *     pixels were corrupt but the transfer itself completed and its slot is free, so it's booked as
 *     done (the ISR already counted it toward s_tx_fail). Treating that as "not dequeued" is what
 *     wedged the very first version of this code.
 * A polled command (tx_param, the command phase of tx_color, or stream_begin) always drains every
 * queued chunk first: a CASET/RASET must never interleave with in-flight RAMWR pixel data.
 */
#include "display_io.h"

#include <string.h>
#include <sys/cdefs.h>            /* __containerof */
#include "esp_check.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_io_interface.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "disp_io";

#define DIO_CMD_BITS      32               /* CO5300 QSPI command word: opcode<<24 | dcs<<8 */
#define DIO_PARAM_SCRATCH 32               /* parameter lists are tiny (<=4 B: CASET/RASET/MADCTL/0x51); cap */
#define DIO_MAX_DEPTH     32               /* width of the free-slot bitmask */

typedef struct {
    spi_transaction_t base;
    struct { unsigned last : 1; } flags;   /* set on the last pixel chunk of a tx_color / stream -> fires the callback */
} dio_trans_t;

typedef struct {
    esp_lcd_panel_io_t   base;             /* vtable — must be first so the handle == &base */
    spi_device_handle_t  dev;
    uint32_t             depth;            /* pool size, equal to the SPI device's queue size */
    uint32_t             free_mask;        /* bit i set means pool[i] is free (not currently queued) */
    esp_lcd_panel_io_color_trans_done_cb_t on_color_done;
    void                *user_ctx;
    dio_trans_t          pool[];           /* `depth` transaction descriptors */
} dio_t;

static volatile uint32_t s_tx_fail;        /* count of SPI_TRANS_DMA_TX_FAIL underruns (a starved PSRAM-sourced chunk) */
static volatile uint32_t s_color_chunks;   /* count of completed pixel chunks */
/* Internal, DMA-capable scratch buffer for the rare parameter list longer than 4 bytes (filled
 * while the bus lock is held). */
static DRAM_ATTR uint8_t s_param_scratch[DIO_PARAM_SCRATCH];

uint32_t nocsif_display_io_tx_fail(void)      { return s_tx_fail; }
uint32_t nocsif_display_io_color_chunks(void) { return s_color_chunks; }

/* SPI ISR, called after every transaction, polled or queued alike; only a pixel chunk flagged
 * `last` triggers the user callback. lv_display_flush_ready (the usual on_color_done) is safe to
 * call from an ISR context. */
IRAM_ATTR static void dio_post_cb(spi_transaction_t *trans)
{
    dio_t       *d = (dio_t *)trans->user;
    dio_trans_t *t = __containerof(trans, dio_trans_t, base);
    if (trans->flags & SPI_TRANS_DMA_TX_FAIL) {
        s_tx_fail++;
    }
    if (trans->flags & SPI_TRANS_MODE_QIO) {
        s_color_chunks++;
    }
    if (t->flags.last && d->on_color_done) {
        d->on_color_done(&d->base, NULL, d->user_ctx);
    }
}

static inline uint32_t dio_full_mask(const dio_t *d)
{
    return (d->depth >= 32) ? 0xFFFFFFFFu : ((1u << d->depth) - 1u);
}

/* Blocks for one completed chunk and frees its slot. A flagged DMA failure still counts as a
 * completed transfer (see the header comment) and is never treated as an error here. */
static esp_err_t dio_collect(dio_t *d)
{
    spi_transaction_t *done = NULL;
    esp_err_t r = spi_device_get_trans_result(d->dev, &done, portMAX_DELAY);
    if (r == ESP_ERR_INVALID_STATE) {
        r = ESP_OK;                          /* dequeued and completed, pixels corrupted; already counted by the ISR */
    }
    if (r != ESP_OK || done == NULL) {
        return (r == ESP_OK) ? ESP_FAIL : r;
    }
    dio_trans_t *t = __containerof(done, dio_trans_t, base);
    d->free_mask |= 1u << (uint32_t)(t - d->pool);
    return ESP_OK;
}

/* Blocks until every queued chunk has landed (the panel has clocked all of them in). */
static esp_err_t dio_drain(dio_t *d)
{
    const uint32_t full = dio_full_mask(d);
    while (d->free_mask != full) {
        ESP_RETURN_ON_ERROR(dio_collect(d), TAG, "collect spi transaction failed");
    }
    return ESP_OK;
}

/* Grabs a free transaction descriptor, collecting the oldest in-flight one first if none is
 * currently free (blocks if needed). */
static esp_err_t dio_alloc(dio_t *d, dio_trans_t **out)
{
    if (d->free_mask == 0) {
        ESP_RETURN_ON_ERROR(dio_collect(d), TAG, "collect spi transaction failed");
    }
    const uint32_t idx = (uint32_t)__builtin_ctz(d->free_mask);
    d->free_mask &= ~(1u << idx);
    *out = &d->pool[idx];
    memset(*out, 0, sizeof **out);
    (*out)->base.user = d;
    return ESP_OK;
}

/* Sends the 32-bit command word as one line, MSB byte first on the wire (the LCD expects
 * big-endian, and esp_lcd byte-reverses an int to get the same ordering). keep_cs holds CS active
 * for whatever phase follows. This is a polled transfer: the pool must already be drained, and its
 * slot is free again by the time this returns. */
static esp_err_t dio_send_cmd(dio_t *d, int lcd_cmd, bool keep_cs)
{
    dio_trans_t *t = &d->pool[0];            /* pool is drained here, so every slot is free — use slot 0 transiently */
    memset(t, 0, sizeof *t);
    t->base.user       = d;
    t->base.flags      = SPI_TRANS_USE_TXDATA | (keep_cs ? SPI_TRANS_CS_KEEP_ACTIVE : 0);
    t->base.length     = DIO_CMD_BITS;
    t->base.tx_data[0] = (uint8_t)((uint32_t)lcd_cmd >> 24);
    t->base.tx_data[1] = (uint8_t)((uint32_t)lcd_cmd >> 16);
    t->base.tx_data[2] = (uint8_t)((uint32_t)lcd_cmd >> 8);
    t->base.tx_data[3] = (uint8_t)((uint32_t)lcd_cmd);
    return spi_device_polling_transmit(d->dev, &t->base);
}

/* Queues one pixel chunk from either an internal or PSRAM source. `last` releases CS and arms the
 * user callback once it lands. */
static esp_err_t dio_queue_chunk(dio_t *d, const void *buf, size_t len, bool last)
{
    dio_trans_t *t = NULL;
    ESP_RETURN_ON_ERROR(dio_alloc(d, &t), TAG, "no chunk slot");
    t->base.tx_buffer = buf;
    t->base.length    = len * 8;
    t->base.flags     = SPI_TRANS_MODE_QIO              /* 4 data lines for the pixel phase              */
                      | SPI_TRANS_DMA_USE_PSRAM         /* a PSRAM source is DMA'd in place, never bounced */
                      | (last ? 0 : SPI_TRANS_CS_KEEP_ACTIVE);
    t->flags.last     = last;
    esp_err_t r = spi_device_queue_trans(d->dev, &t->base, portMAX_DELAY);
    if (r != ESP_OK) {
        d->free_mask |= 1u << (uint32_t)(t - d->pool);   /* never actually queued, so free the slot back up */
    }
    return r;
}

/* ---- esp_lcd_panel_io_t vtable ------------------------------------------------------------- */

static esp_err_t dio_tx_param(esp_lcd_panel_io_t *io, int lcd_cmd, const void *param, size_t param_size)
{
    dio_t *d = __containerof(io, dio_t, base);
    const bool have_param = (param != NULL && param_size > 0);
    ESP_RETURN_ON_FALSE(!have_param || param_size <= DIO_PARAM_SCRATCH, ESP_ERR_INVALID_SIZE, TAG,
                        "param list too long (%u)", (unsigned)param_size);

    ESP_RETURN_ON_ERROR(spi_device_acquire_bus(d->dev, portMAX_DELAY), TAG, "acquire spi bus failed");
    esp_err_t ret = dio_drain(d);            /* a command must never interleave with still-queued pixels */
    if (ret == ESP_OK && lcd_cmd >= 0) {
        ret = dio_send_cmd(d, lcd_cmd, have_param);
    }
    if (ret == ESP_OK && have_param) {
        dio_trans_t *t = &d->pool[0];
        memset(t, 0, sizeof *t);
        t->base.user   = d;
        t->base.length = param_size * 8;
        if (param_size <= sizeof t->base.tx_data) {
            t->base.flags = SPI_TRANS_USE_TXDATA;            /* embedded — no separate buffer needed */
            memcpy(t->base.tx_data, param, param_size);
        } else {
            memcpy(s_param_scratch, param, param_size);      /* internal and DMA-capable */
            t->base.tx_buffer = s_param_scratch;
        }
        ret = spi_device_polling_transmit(d->dev, &t->base);
    }
    spi_device_release_bus(d->dev);
    return ret;
}

static esp_err_t dio_rx_param(esp_lcd_panel_io_t *io, int lcd_cmd, void *param, size_t param_size)
{
    (void)io; (void)lcd_cmd; (void)param; (void)param_size;
    return ESP_ERR_NOT_SUPPORTED;            /* the CO5300 driver never reads anything back */
}

static esp_err_t dio_tx_color(esp_lcd_panel_io_t *io, int lcd_cmd, const void *color, size_t color_size)
{
    dio_t *d = __containerof(io, dio_t, base);
    ESP_RETURN_ON_ERROR(spi_device_acquire_bus(d->dev, portMAX_DELAY), TAG, "acquire spi bus failed");
    esp_err_t ret = ESP_OK;
    if (lcd_cmd >= 0) {
        ret = dio_drain(d);
        if (ret == ESP_OK) {
            ret = dio_send_cmd(d, lcd_cmd, color != NULL && color_size > 0);
        }
    }
    /* Queues the pixel data from wherever it lives, split into chunks of at most 32 KB with CS
     * held active between them. */
    const uint8_t *p    = (const uint8_t *)color;
    size_t         left = color_size;
    while (ret == ESP_OK && left > 0) {
        const size_t n = (left > NOCSIF_DISPLAY_IO_MAX_TRANSFER) ? NOCSIF_DISPLAY_IO_MAX_TRANSFER : left;
        ret   = dio_queue_chunk(d, p, n, n == left);
        p    += n;
        left -= n;
    }
    spi_device_release_bus(d->dev);
    return ret;
}

static esp_err_t dio_register_cbs(esp_lcd_panel_io_t *io, const esp_lcd_panel_io_callbacks_t *cbs, void *user_ctx)
{
    dio_t *d = __containerof(io, dio_t, base);
    ESP_RETURN_ON_FALSE(cbs != NULL, ESP_ERR_INVALID_ARG, TAG, "no callbacks");
    /* esp_lvgl_port registers its own default callback first, then ui.c later replaces it with the
     * NocSif flush callback (companion mirror capture / casting / timing) — this overwrite is
     * expected, not a bug. */
    d->on_color_done = cbs->on_color_trans_done;
    d->user_ctx      = user_ctx;
    return ESP_OK;
}

static esp_err_t dio_del(esp_lcd_panel_io_t *io)
{
    dio_t *d = __containerof(io, dio_t, base);
    esp_err_t ret = dio_drain(d);
    spi_bus_remove_device(d->dev);
    heap_caps_free(d);
    return ret;
}

/* ---- streaming pixel path ------------------------------------------------------------------ */

esp_err_t nocsif_display_io_stream_begin(esp_lcd_panel_io_handle_t io, int lcd_cmd)
{
    dio_t *d = __containerof(io, dio_t, base);
    ESP_RETURN_ON_ERROR(spi_device_acquire_bus(d->dev, portMAX_DELAY), TAG, "acquire spi bus failed");
    esp_err_t ret = dio_drain(d);            /* the previous rect's tail must land before a new window opens */
    if (ret == ESP_OK) {
        ret = dio_send_cmd(d, lcd_cmd, true);    /* holds CS for the chunks that follow */
    }
    spi_device_release_bus(d->dev);
    return ret;
}

esp_err_t nocsif_display_io_stream_reserve(esp_lcd_panel_io_handle_t io)
{
    dio_t *d = __containerof(io, dio_t, base);
    if (d->free_mask != 0) {
        return ESP_OK;
    }
    return dio_collect(d);                   /* blocks until the oldest chunk lands */
}

esp_err_t nocsif_display_io_stream_chunk(esp_lcd_panel_io_handle_t io, const void *buf, size_t len, bool last)
{
    dio_t *d = __containerof(io, dio_t, base);
    ESP_RETURN_ON_FALSE(buf != NULL && len > 0 && len <= NOCSIF_DISPLAY_IO_MAX_TRANSFER, ESP_ERR_INVALID_SIZE,
                        TAG, "bad chunk (%u B)", (unsigned)len);
    ESP_RETURN_ON_ERROR(spi_device_acquire_bus(d->dev, portMAX_DELAY), TAG, "acquire spi bus failed");
    esp_err_t ret = dio_queue_chunk(d, buf, len, last);
    spi_device_release_bus(d->dev);
    return ret;
}

esp_err_t nocsif_display_io_stream_finish(esp_lcd_panel_io_handle_t io)
{
    dio_t *d = __containerof(io, dio_t, base);
    return dio_drain(d);
}

/* ---- construction --------------------------------------------------------------------------- */

esp_err_t nocsif_display_io_qspi_new(spi_host_device_t host, const nocsif_display_io_cfg_t *cfg,
                                     esp_lcd_panel_io_handle_t *ret_io)
{
    ESP_RETURN_ON_FALSE(cfg != NULL && ret_io != NULL && cfg->queue_depth >= 1 && cfg->queue_depth <= DIO_MAX_DEPTH,
                        ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    /* The SPI ISR reads these descriptors directly, so they're kept internal (a few hundred bytes,
     * no DMA needed). */
    dio_t *d = heap_caps_calloc(1, sizeof(dio_t) + sizeof(dio_trans_t) * cfg->queue_depth,
                                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(d != NULL, ESP_ERR_NO_MEM, TAG, "no mem for panel io");

    /* Byte-identical to esp_lcd's own device config for CO5300_PANEL_IO_QSPI_CONFIG: half-duplex,
     * zero command/address bits (the "command" here is sent as a 32-bit data phase instead), no D/C
     * pre-callback. */
    const spi_device_interface_config_t devcfg = {
        .flags          = SPI_DEVICE_HALFDUPLEX,
        .clock_speed_hz = cfg->pclk_hz,
        .mode           = (uint8_t)cfg->spi_mode,
        .spics_io_num   = cfg->cs_gpio_num,
        .queue_size     = (int)cfg->queue_depth,
        .post_cb        = dio_post_cb,
    };
    esp_err_t ret = spi_bus_add_device(host, &devcfg, &d->dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device -> %s", esp_err_to_name(ret));
        heap_caps_free(d);
        return ret;
    }
    d->depth     = (uint32_t)cfg->queue_depth;
    d->free_mask = dio_full_mask(d);
    d->base.tx_param                 = dio_tx_param;
    d->base.rx_param                 = dio_rx_param;
    d->base.tx_color                 = dio_tx_color;
    d->base.del                      = dio_del;
    d->base.register_event_callbacks = dio_register_cbs;
    *ret_io = &d->base;
    ESP_LOGI(TAG, "QSPI panel IO up: pclk %d MHz, %u chunk slots x <=%u B, streaming window path, "
                  "no internal-DMA allocation on any path",
             cfg->pclk_hz / 1000000, (unsigned)cfg->queue_depth, (unsigned)NOCSIF_DISPLAY_IO_MAX_TRANSFER);
    return ESP_OK;
}
