/*
 * NocSif — CO5300 QSPI AMOLED display driver (M1). See display.h.
 *
 * Built on the espressif/esp_lcd_co5300 managed component (v2.x). For QSPI, vendor_config MUST set
 * flags.use_qspi_interface; passing init_cmds=NULL uses the driver's built-in init sequence (which
 * sets brightness to 0xFF, so a fill becomes visible right away). The 22px column offset is applied
 * via esp_lcd_panel_set_gap rather than by patching CASET directly — draw_bitmap adds the gap to
 * every window on its own.
 */
#include "display.h"

#include <stdint.h>
#include <string.h>

#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_co5300.h"
#include "display_io.h"     /* the NocSif QSPI panel IO — DMAs straight from PSRAM */
#include "esp_heap_caps.h"
#include "esp_log.h"

#define DISP_PIN_CS    41
#define DISP_PIN_SCK   40
#define DISP_PIN_D0    38
#define DISP_PIN_D1    39
#define DISP_PIN_D2    42
#define DISP_PIN_D3    45
#define DISP_PIN_RST   37
#define DISP_X_GAP     22
#define DISP_Y_GAP     0
#define DISP_QSPI_HOST SPI2_HOST

/* CO5300 QSPI DCS write-command wrapping — mirrors the component's own static tx_param(): in QSPI
 * mode the 8-bit DCS command sits in bits [15:8] with the 0x02 write opcode in bits [31:24], sent
 * as one 32-bit command (lcd_cmd_bits=32). Needed here to send a raw DCS command (the brightness
 * ramp) through the panel IO directly, since the driver's own tx_param helper is static. */
#define CO5300_QSPI_CMD(c)  (((uint32_t)0x02 << 24) | ((uint32_t)((c) & 0xFF) << 8))
/* The pixel-phase command word: the 0x32 write-colour opcode wrapping RAMWR (0x2C) — what the
 * component's static tx_color() builds. Sent as one line with CS held, then pixels follow on 4
 * lines. */
#define CO5300_QSPI_PIXELS  (((uint32_t)0x32 << 24) | ((uint32_t)0x2C << 8))

/* QSPI pixel clock. The component's own macro defaults to 40 MHz, but LilyGo runs this panel at
 * 80 MHz. Bumped to 80 MHz in UI-shell P3.1 once clean 40 MHz fills had been confirmed since M1 —
 * the full-frame nav-slide flush (~617 KB/frame) is QSPI-bound, so doubling the clock roughly
 * halves flush time (~30ms -> ~15ms/frame), which roughly doubles the slide animation's frame rate.
 * If tearing or artifacts ever show up at 80 MHz, drop back to 40 MHz. */
#define DISP_PCLK_HZ  (80 * 1000 * 1000)

static const char *TAG = "co5300";

static esp_lcd_panel_handle_t s_panel;
/* Kept as a module global (rather than local to init) so LVGL/esp_lvgl_port can bind to it. */
static esp_lcd_panel_io_handle_t s_io;

#define TRY(expr) do {                                        \
        esp_err_t _e = (expr);                                \
        if (_e != ESP_OK) {                                   \
            ESP_LOGE(TAG, "%s -> %s", #expr, esp_err_to_name(_e)); \
            return _e;                                        \
        }                                                     \
    } while (0)

/* Wraps an optional panel op: some ops (mirror / disp_on_off) may be unsupported or a no-op on this
 * driver, and the built-in init sequence has already turned the display on, so this just warns and
 * lets bring-up continue rather than failing the whole init. */
static void try_optional(const char *what, esp_err_t e)
{
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "%s -> %s (continuing)", what, esp_err_to_name(e));
    }
}

/* P5 fix: the component's default vendor_specific_init_default ends with DISPON (0x29), which
 * turns the panel on while GRAM is still uninitialized (white), and that white stays visible until
 * the first draw_bitmap. This table is that exact init sequence with the trailing DISPON REMOVED,
 * passed in as vendor_config.init_cmds so esp_lcd_panel_init leaves the panel sleep-out but
 * display-off. nocsif_display_init then clears GRAM to black and sends DISPON itself, so the first
 * light the panel ever emits is black, eliminating the boot flash. Kept byte-identical to the
 * vendor's default otherwise (MADCTL/COLMOD are sent by the driver before this list runs; TE on,
 * CASET/RASET, and the 60 ms SLPOUT delay are preserved) except that brightness (0x51) is held at 0
 * here and ramped up after the black clear, so nothing emits the white GRAM during boot. If a
 * future component bump changes the vendor default, re-sync this list against it. */
static const co5300_lcd_init_cmd_t nocsif_co5300_init_no_dispon[] = {
    {0xFE, (uint8_t []){0x00}, 0, 0},
    {0xC4, (uint8_t []){0x80}, 1, 0},
    {0x35, (uint8_t []){0x00}, 0, 10},
    {0x53, (uint8_t []){0x20}, 1, 10},
    {0x51, (uint8_t []){0x00}, 1, 10},   /* brightness held at 0 through boot; ramped up after the black clear */
    {0x63, (uint8_t []){0xFF}, 1, 10},
    {0x2A, (uint8_t []){0x00, 0x06, 0x01, 0xDD}, 4, 0},
    {0x2B, (uint8_t []){0x00, 0x00, 0x01, 0xD1}, 4, 0},
    {0x11, (uint8_t []){0x00}, 0, 60},
    /* {0x29, ...} DISPON intentionally left out here — sent after the black clear below instead. */
};

esp_err_t nocsif_display_init(void)
{
    if (s_panel != NULL) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "QSPI bus init (SCK=%d D0=%d D1=%d D2=%d D3=%d)",
             DISP_PIN_SCK, DISP_PIN_D0, DISP_PIN_D1, DISP_PIN_D2, DISP_PIN_D3);
    /* max_transfer_sz caps a single SPI transaction; a draw_bitmap is streamed in chunks of this
     * size with CS held active between them, forming one continuous write window with no seams.
     * Sized to the S3's hardware per-transaction DMA limit (32 KB), so a full frame is roughly 19
     * chunks. The panel IO below DMAs the LVGL PSRAM draw buffers directly
     * (SPI_TRANS_DMA_USE_PSRAM), so there's no per-flush internal bounce buffer and no persistent
     * internal staging band to allocate — the flush needs zero internal DMA memory itself. The
     * bus's own DMA descriptors (roughly max_transfer_sz / 4092, about 9 x 12 B) are the only
     * internal-DMA cost here, claimed from the pristine boot pool. The boot-time black clear and
     * band test also draw straight from PSRAM the same way. */
    const spi_bus_config_t bus_cfg = CO5300_PANEL_BUS_QSPI_CONFIG(
        DISP_PIN_SCK, DISP_PIN_D0, DISP_PIN_D1, DISP_PIN_D2, DISP_PIN_D3,
        (int)NOCSIF_DISPLAY_IO_MAX_TRANSFER);   /* one 32 KB pixel chunk == one DMA transaction */
    TRY(spi_bus_initialize(DISP_QSPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    /* Uses the NocSif panel IO (display_io.c) in place of esp_lcd_new_panel_io_spi: byte-identical
     * QSPI framing to the component's own config (32-bit cmd, 8-bit params, quad pixel phase, mode
     * 0), plus the streaming window-once path the LVGL flush uses and the guarantee that it never
     * allocates internal DMA. queue_depth = 2 matches the number of stage bands, so the stream's
     * reserve() call blocks exactly until the band about to be reused has landed (the ping-pong
     * invariant). No callback is registered here — esp_lvgl_port registers its own default on this
     * handle, and ui.c later swaps in the NocSif flush callback under the port lock. */
    const nocsif_display_io_cfg_t io_cfg = {
        .cs_gpio_num = DISP_PIN_CS,
        .pclk_hz     = DISP_PCLK_HZ,    /* the component's macro defaults to 40MHz; bumped to 80MHz (see note above) */
        .spi_mode    = 0,
        .queue_depth = 2,               /* matches the stage bands used by ui.c and stream_frame below */
    };
    TRY(nocsif_display_io_qspi_new(DISP_QSPI_HOST, &io_cfg, &s_io));

    /* QSPI interface selection lives in the vendor config flags; init_cmds=NULL would fall back to
     * the built-in init: bits_per_pixel=24 sends COLMOD 0x3A=0x77 (RGB888), plus sleep-out,
     * display-on, and brightness 0xFF. */
    const co5300_vendor_config_t vendor_cfg = {
        .init_cmds = nocsif_co5300_init_no_dispon,   /* the vendor default minus DISPON — avoids the white flash */
        .init_cmds_size = sizeof(nocsif_co5300_init_no_dispon) / sizeof(nocsif_co5300_init_no_dispon[0]),
        .flags = {
            .use_qspi_interface = 1,
        },
    };
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = DISP_PIN_RST,
        /* BGR element order: LVGL v9's lv_color_t stores {blue,green,red} in memory, so its RGB888
         * draw buffers are laid out B,G,R. Setting the panel's MADCTL BGR bit lets lvgl_port hand
         * its pixel buffer straight to draw_bitmap with zero CPU-side swap. The bare test patterns
         * below compensate by writing B,G,R too. */
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 24,                 /* RGB888 -> COLMOD 0x3A=0x77 (256 levels per channel) */
        .vendor_config = (void *)&vendor_cfg,
    };
    TRY(esp_lcd_new_panel_co5300(s_io, &panel_cfg, &s_panel));

    TRY(esp_lcd_panel_reset(s_panel));
    TRY(esp_lcd_panel_init(s_panel));   /* the custom init_cmds omit DISPON, so the panel ends sleep-out but display-off */
    /* Sets MADCTL/gap, pre-clears GRAM to black, then sends DISPON. The panel is on but brightness
     * is still 0 (held from the init sequence), so nothing is emitted yet; the black clear matters
     * for the moment brightness is later raised (by the UI after its splash flush, or the main.c
     * fallback) — at that point it shows black rather than the uninitialized white GRAM. Uses the
     * raw disp_on_off call (not nocsif_display_sleep) so s_asleep stays "awake". */
    try_optional("mirror", esp_lcd_panel_mirror(s_panel, false, false)); /* sets MADCTL 0x00 */
    TRY(esp_lcd_panel_set_gap(s_panel, DISP_X_GAP, DISP_Y_GAP));         /* applies the 22px x-offset */
    if (nocsif_display_fill(0x000000) != ESP_OK) {
        ESP_LOGW(TAG, "initial black clear failed — a brief white flash may show before the UI");
    }
    try_optional("disp_on", esp_lcd_panel_disp_on_off(s_panel, true));   /* turns the panel on, brightness still 0 */

    /* Brightness (WRDISBV) is deliberately left at 0 here: the panel is on over black GRAM but
     * emits no light yet. It's only raised (via nocsif_display_set_brightness) once the UI has
     * flushed its first real frame — the boot splash — to the panel, so nothing shown between here
     * and that frame (LVGL's default blank screen, an early flush of uninitialized buffers) can
     * ever appear as a white flash. If the UI never comes up, main.c has its own fallback so a
     * display-OK-but-UI-failed boot doesn't stay stuck dark. */

    ESP_LOGI(TAG, "CO5300 panel up: %dx%d, x-gap=%d (BGR ele order for LVGL)",
             NOCSIF_DISP_W, NOCSIF_DISP_H, DISP_X_GAP);
    return ESP_OK;
}

esp_lcd_panel_handle_t nocsif_display_panel(void)    { return s_panel; }
esp_lcd_panel_io_handle_t nocsif_display_io(void)    { return s_io; }

static bool s_asleep;

esp_err_t nocsif_display_sleep(bool sleep)
{
    if (s_panel == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (sleep == s_asleep) {
        return ESP_OK;   /* already in that state */
    }
    /* disp_on_off(on) sends DISPON (0x29) when turning on and DISPOFF (0x28) when turning off — the
     * CO5300 managed driver maps this straight to those MIPI commands (confirmed in the component
     * source). DISPOFF blanks the emissive AMOLED with no re-init needed to wake it later. */
    esp_err_t err = esp_lcd_panel_disp_on_off(s_panel, !sleep);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "disp_on_off(%d) -> %s", !sleep, esp_err_to_name(err));
        return err;
    }
    s_asleep = sleep;
    ESP_LOGI(TAG, "screen %s", sleep ? "OFF (DISPOFF)" : "ON (DISPON)");
    return ESP_OK;
}

bool nocsif_display_is_asleep(void) { return s_asleep; }

esp_err_t nocsif_display_set_brightness(uint8_t level)
{
    if (s_panel == NULL || s_io == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Sends WRDISBV (0x51), QSPI-wrapped the same way the driver's own tx_param would. Held at 0
     * from display_init to suppress the power-on white flash; the UI ramps it to full right after
     * it flushes the boot splash. */
    return esp_lcd_panel_io_tx_param(s_io, CO5300_QSPI_CMD(0x51), &level, 1);
}

esp_err_t nocsif_display_window_begin(int x1, int y1, int x2, int y2)
{
    if (s_io == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Same window math the component's own draw_bitmap uses (gap added, end coordinates inclusive). */
    x1 += DISP_X_GAP; x2 += DISP_X_GAP;
    y1 += DISP_Y_GAP; y2 += DISP_Y_GAP;
    const uint8_t ca[4] = { (uint8_t)(x1 >> 8), (uint8_t)x1, (uint8_t)(x2 >> 8), (uint8_t)x2 };
    const uint8_t ra[4] = { (uint8_t)(y1 >> 8), (uint8_t)y1, (uint8_t)(y2 >> 8), (uint8_t)y2 };
    TRY(esp_lcd_panel_io_tx_param(s_io, CO5300_QSPI_CMD(0x2A), ca, sizeof ca));   /* CASET */
    TRY(esp_lcd_panel_io_tx_param(s_io, CO5300_QSPI_CMD(0x2B), ra, sizeof ra));   /* RASET */
    return nocsif_display_io_stream_begin(s_io, (int)CO5300_QSPI_PIXELS);        /* RAMWR, CS held */
}

/* Streams a whole PSRAM frame through the streaming path in internal bands (two
 * NOCSIF_FLUSH_STAGE_BYTES buffers allocated just for this call — boot only, so memory is
 * plentiful; freed once the last band lands). One write window means no per-strip seams. A
 * PSRAM-sourced DMA can underrun at the 80 MHz pixel clock (measured 4 of 19 chunks on this exact
 * frame, 2026-09-07), so even the boot-time clear is bounced through the internal stage. */
static esp_err_t stream_frame(const uint8_t *src, size_t nbytes)
{
    uint8_t *band[2] = {
        heap_caps_malloc(NOCSIF_FLUSH_STAGE_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        heap_caps_malloc(NOCSIF_FLUSH_STAGE_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
    };
    esp_err_t err = (band[0] != NULL && band[1] != NULL) ? ESP_OK : ESP_ERR_NO_MEM;
    if (err == ESP_OK) {
        err = nocsif_display_window_begin(0, 0, NOCSIF_DISP_W - 1, NOCSIF_DISP_H - 1);
    }
    int b = 0;
    for (size_t off = 0; err == ESP_OK && off < nbytes; b ^= 1) {
        const size_t n = (nbytes - off > NOCSIF_FLUSH_STAGE_BYTES) ? NOCSIF_FLUSH_STAGE_BYTES : nbytes - off;
        err = nocsif_display_io_stream_reserve(s_io);          /* waits until the band we're about to overwrite has landed */
        if (err != ESP_OK) {
            break;
        }
        memcpy(band[b], src + off, n);
        err = nocsif_display_io_stream_chunk(s_io, band[b], n, off + n >= nbytes);
        off += n;
    }
    nocsif_display_io_stream_finish(s_io);      /* the bands must outlive their in-flight DMA before we free them */
    heap_caps_free(band[0]);
    heap_caps_free(band[1]);
    return err;
}

/* Renders a whole frame into one PSRAM buffer and streams it through a single write window (rather
 * than many small row-windows, which left a faint seam at every boundary — visible on a smooth
 * gradient). The framebuffer lives in PSRAM since 410*502*3 ~= 617 KB is far too big for internal
 * RAM. */
static esp_err_t draw_full(uint32_t (*color_of)(int y))
{
    if (s_panel == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    const size_t npx = (size_t)NOCSIF_DISP_W * NOCSIF_DISP_H;
    const size_t nbytes = npx * 3;   /* RGB888: 3 bytes per pixel */
    uint8_t *fb = heap_caps_malloc(nbytes, MALLOC_CAP_SPIRAM);
    if (fb == NULL) {
        ESP_LOGE(TAG, "framebuffer alloc (%u B PSRAM) failed", (unsigned)nbytes);
        return ESP_ERR_NO_MEM;
    }
    for (int y = 0; y < NOCSIF_DISP_H; y++) {
        const uint32_t c = color_of(y);              /* 0x00RRGGBB */
        const uint8_t r = (uint8_t)(c >> 16);
        const uint8_t g = (uint8_t)(c >> 8);
        const uint8_t b = (uint8_t)c;
        uint8_t *px = fb + (size_t)y * NOCSIF_DISP_W * 3;
        for (int x = 0; x < NOCSIF_DISP_W; x++) {
            px[0] = b; px[1] = g; px[2] = r;         /* byte order matches rgb_ele_order = BGR */
            px += 3;
        }
    }
    esp_err_t err = stream_frame(fb, nbytes);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "stream_frame(full) -> %s", esp_err_to_name(err));
    }
    heap_caps_free(fb);
    return err;
}

static uint32_t s_fill_color;
static uint32_t fill_cb(int y) { (void)y; return s_fill_color; }

esp_err_t nocsif_display_fill(uint32_t rgb888)
{
    s_fill_color = rgb888;
    return draw_full(fill_cb);
}

static uint32_t bands_cb(int y)
{
    /* red / green / blue / white quarters (RGB888). */
    static const uint32_t band[4] = { 0xFF0000, 0x00FF00, 0x0000FF, 0xFFFFFF };
    int idx = y / (NOCSIF_DISP_H / 4);
    if (idx > 3) idx = 3;
    return band[idx];
}

esp_err_t nocsif_display_test_bands(void)
{
    ESP_LOGI(TAG, "drawing test bands (R/G/B/W)");
    return draw_full(bands_cb);
}

static uint32_t gradient_cb(int y)
{
    /* Black-to-white ramp using the full 256 levels per RGB888 channel, so there's no visible
     * banding (RGB565 only had 32 levels, giving ~16-row steps). */
    uint32_t level = (uint32_t)((y * 255) / (NOCSIF_DISP_H - 1));   /* 0..255 */
    return (level << 16) | (level << 8) | level;
}

esp_err_t nocsif_display_test_gradient(void)
{
    ESP_LOGI(TAG, "drawing smooth grayscale gradient (RGB888)");
    return draw_full(gradient_cb);
}
