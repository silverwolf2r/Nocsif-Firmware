/*
 * NocSif — BHI260AP inertial sensor hub (M11 watch-core, slice A1). See imu.h.
 *
 * Bring-up mirrors LilyGo's proven SensorBHI260AP::initImpl() path (vendored Bosch BHY2
 * API in components/bhy2): probe 0x28, configure the host interrupt/interface, upload the
 * ~117 KB firmware to the hub's RAM (this board boots from RAM, not flash), boot it, then
 * enable the accelerometer passthrough virtual sensor and drain the FIFO.
 *
 * A dedicated worker task owns the sensor and does the slow (~2-3 s) upload + FIFO polling
 * off the LVGL task; a short spinlock guards the tiny value cache the LVGL-side getters read.
 * Polling (no GPIO8 IRQ) is deliberate for this first slice — the interrupt-driven read and
 * the wrist-wear-wakeup / step-counter virtual sensors come in later M11 slices.
 */
#include "imu.h"

#include <string.h>
#include <stdio.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"          /* esp_rom_delay_us */
#include "driver/i2c_master.h"

#include "i2c_scan.h"             /* nocsif_i2c_bus() */
#include "power.h"                /* nocsif_power_sensor_rail() */
#include "reliability.h"          /* nocsif_reliability_safe_mode() */
#include "freertos/idf_additions.h" /* xTaskCreateWithCaps — PSRAM worker stack (RAM Phase A2) */
#include "esp_heap_caps.h"          /* MALLOC_CAP_SPIRAM — fifo work buffer + stack */
#include "esp_memory_utils.h"       /* esp_ptr_external_ram — PSRAM placement probe */
#include "coex.h"                   /* nocsif_int_dma_free/_largest — the I2C-failure decode gauge */

#include "bhy2.h"
#include "bhy2_parse.h"
#include "bhy2_defs.h"
#include "bhi260_fw.h"

static const char *TAG = "imu";

/* Set to 1 to emit a ~1 Hz accelerometer line to the log while streaming (bring-up / debug aid;
 * also surfaces in System > Diagnostics via the ESP_LOG tee). Off in the shipped build — the
 * getters (nocsif_imu_accel_g / _status_str) are the real interface. A1 on-device proof captured
 * with this at 1 (accelerometer tracked tilt to ±1 g per axis, clean coexistence, zero WDT). */
#ifndef NOCSIF_IMU_LOG_ACCEL
#define NOCSIF_IMU_LOG_ACCEL   0
#endif

#define BHI260AP_I2C_ADDR      0x28
#define IMU_I2C_HZ             400000
#define IMU_I2C_TIMEOUT_MS     100
/* ESP32-S3 I2C max read/write per transaction is 64 B (SensorLib's tested value); the BHY2
 * host-interface layer chunks the firmware upload to this. */
#define IMU_MAX_RW             64
#define IMU_ACCEL_RATE_HZ      50.0f
/* F2 heading: game-rotation-vector (accel+gyro fusion, no mag). 25 Hz is plenty for a hand rotation
 * sweep and lighter than the accel rate; it only runs while Signal Hunt asks for it. */
#define IMU_HEADING_RATE_HZ    25.0f
#define IMU_ACCEL_SCALE_G      (1.0f / 4096.0f)   /* raw count -> g (Bosch default accel scaling) */
#define IMU_POLL_MS            20
/* 6 KB: the firmware-upload path (bhy2 hif -> I2C driver) plus ESP_LOG formatting is a moderately
 * deep chain run thousands of times; headroom is cheap and avoids the 4 KB-worker overflow class. */
#define IMU_TASK_STACK         6144
#define IMU_TASK_PRIO          3
#define IMU_FIFO_WORK_SIZE     2048
/* B1 shake-to-wake (accel-based, host-side). A deliberate wrist twist back-and-forth produces large
 * sample-to-sample accel changes; we integrate that with a leaky accumulator and wake only when the
 * motion stays vigorous long enough — so a single bump, a slow raise, or an arm swing while walking
 * doesn't trigger, only a real shake. Tunable: MOVE = the per-sample L1 accel delta (g) that counts as
 * "vigorous"; FIRE = net vigorous samples needed (~20 ms each → ~FIRE*20 ms of sustained shaking);
 * CAP bounds the accumulator so it decays quickly once the shake stops. */
#define SHAKE_MOVE   0.30f   /* per-20 ms |dx|+|dy|+|dz| above this = a vigorous sample */
#define SHAKE_FIRE   8       /* accumulator level that fires a wake (higher = less sensitive) */
#define SHAKE_CAP    12      /* accumulator clamp (bounds post-shake decay) */

/* Stillness (M11 F1 power): the per-20 ms L1 accel delta BELOW which the sample counts as "still".
 * Small enough that a worn wrist's micro-motion usually clears it, so the watch reads "still" mainly
 * when set down. The UI pairs it with a flat-orientation check before it sleeps early on stillness. */
#define STILL_MOVE   0.05f

/* Accel axis remap for the T-Watch Ultra's mounting (LilyGo TOP_LAYER_BOTTOM_RIGHT_CORNER =
 * P2, 180° about Z): X = right, Y = up/12-o'clock, Z = out of the screen. So face-up rests at
 * z ~= +1 g. */
static const struct bhy2_orient_matrix IMU_REMAP_ACCEL = { .c = { -1, 0, 0, 0, -1, 0, 0, 0, 1 } };

/* ---- module state --------------------------------------------------------- */
/* The value cache is shared between the worker (writer) and the LVGL-side getters (readers);
 * a short spinlock keeps the read/write of a few floats coherent without a heavier mutex. */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static nocsif_imu_state_t s_state = NOCSIF_IMU_OFF;
static float  s_ax, s_ay, s_az;      /* latest accel, g, watch frame */
static bool   s_have;                /* a sample has landed */
static uint16_t s_kernel_ver;        /* hub firmware kernel version (0 until booted) */

/* Wrist-raise auto-wake (B1). Detection is accel-based and runs on the worker; the UI setter only
 * records intent (s_wrist_want). s_wrist_events is a rising-edge latch the LVGL side drains. */
static bool     s_wrist_ready;       /* raise detection is available (IMU online) — spinlock */
static bool     s_wrist_want;        /* desired detection enable, set by the UI — spinlock    */
static uint32_t s_wrist_events;      /* pending raise count, drained by the UI — spinlock      */

/* Touched only by the worker task (bhy2 is single-threaded here). */
static bool                    s_started;
static i2c_master_dev_handle_t s_dev;
static struct bhy2_dev         s_bhy2;
static uint8_t                 s_wbuf[IMU_MAX_RW + 1];   /* I2C write scratch (reg + payload) */
/* RAM Phase A2: the 2 KB FIFO work buffer lives in PSRAM (allocated once in nocsif_imu_init). The S3's I2C
 * peripheral has no DMA — the i2c_master driver copies the hardware FIFO into this buffer from its (non-
 * IRAM) ISR, which cannot run while the flash cache is off — so a PSRAM target is safe; bhy2 parses it on
 * this worker. Frees 2,048 B of boot-permanent internal BSS from the contended pool. */
static uint8_t                *s_fifo_work;
/* RAM Phase A2: decode the I2C failures behind `fifo process err -3` (BHY2_E_IO). The new i2c_master driver
 * allocates nothing per call and the S3 I2C has no DMA, so a failure here is a BUS event (timeout / NACK),
 * not memory — log the real esp_err_t next to the int-DMA gauge so the correlation can be settled
 * on-device. Rate-limited: the first IMU_I2C_ERR_LOG_FIRST failures, then every IMU_I2C_ERR_LOG_EVERY-th. */
#define IMU_I2C_ERR_LOG_FIRST  8
#define IMU_I2C_ERR_LOG_EVERY  64
static uint32_t                s_i2c_err_count;   /* failures that survived the retry (worker-only)   */
static uint32_t                s_i2c_retry_ok;    /* transient failures cured by the retry (worker-only) */
/* DECODED on-device (2026-09-07, docs/DMA-COEXISTENCE-VERIFICATION.md "Phase A2 addendum"): the failure
 * behind `fifo process err -3` is ESP_ERR_INVALID_STATE with NO i2c_master timeout log — in the driver that
 * combination is a NACK (a timeout logs ESP_LOGE; a NACK only ESP_LOGD): the BHI260 refused the INT_STATUS
 * (0x2D) read once, ~1 s after WiFi `assoc -> run`, with 37 KB int-DMA free. Hub-side and transient — the
 * next 20 ms poll always succeeded. So: retry ONCE after a tick. One tick (10 ms at 100 Hz) is well inside
 * the 20 ms poll period. */
#define IMU_I2C_RETRY_TICKS    1
#define IMU_I2C_RETRY_LOG_FIRST 3
static bool                    s_wrist_active;  /* detector running (mirrors want; edge-logs enable/disable) */
static float                   s_sh_px, s_sh_py, s_sh_pz; /* previous accel sample (g) for the shake delta */
static bool                    s_sh_have_prev;  /* a previous sample is loaded */
static int                     s_sh_acc;        /* leaky vigorous-motion accumulator */
static float                   s_st_px, s_st_py, s_st_pz; /* previous sample for the stillness delta */
static bool                    s_st_have_prev;
static uint32_t                s_still_polls;   /* consecutive still polls (worker-local) */
static uint32_t                s_still_ms;      /* cached stillness duration, ms — spinlock */

/* F2 relative heading (GAMERV). Enabled on demand by the UI; the worker toggles the hub sensor. */
static bool                    s_head_want;     /* desired heading-sensor enable (UI) — spinlock  */
static bool                    s_head_active;   /* hub sensor currently enabled (worker-local)     */
static float                   s_heading;       /* latest yaw, deg [0,360) — spinlock              */
static bool                    s_have_heading;  /* a heading sample has landed — spinlock          */

static void set_state(nocsif_imu_state_t s)
{
    taskENTER_CRITICAL(&s_lock);
    s_state = s;
    taskEXIT_CRITICAL(&s_lock);
}

/* ---- BHY2 platform callbacks (worker task context only) -------------------- */
static void imu_i2c_fail(const char *op, uint8_t reg, uint32_t len, esp_err_t e)
{
    uint32_t n = ++s_i2c_err_count;
    if (n <= IMU_I2C_ERR_LOG_FIRST || (n % IMU_I2C_ERR_LOG_EVERY) == 0) {
        ESP_LOGW(TAG, "i2c %s reg=0x%02x len=%u -> %s (#%u); int-dma free=%u largest=%u", op, reg, (unsigned)len,
                 esp_err_to_name(e), (unsigned)n, (unsigned)nocsif_int_dma_free(), (unsigned)nocsif_int_dma_largest());
    }
}

/* A transient failure the retry cured: visible the first few times so the mechanism stays observable. */
static void imu_i2c_retried(const char *op, uint8_t reg, esp_err_t first)
{
    uint32_t n = ++s_i2c_retry_ok;
    if (n <= IMU_I2C_RETRY_LOG_FIRST) {
        ESP_LOGW(TAG, "i2c %s reg=0x%02x -> %s, recovered by retry (#%u)", op, reg, esp_err_to_name(first), (unsigned)n);
    }
}

static int8_t imu_i2c_read(uint8_t reg, uint8_t *data, uint32_t len, void *intf)
{
    (void)intf;
    if (s_dev == NULL) return -1;
    esp_err_t e = i2c_master_transmit_receive(s_dev, &reg, 1, data, len, IMU_I2C_TIMEOUT_MS);
    if (e != ESP_OK) {
        const esp_err_t first = e;
        vTaskDelay(IMU_I2C_RETRY_TICKS);                     /* worker task only — never an ISR */
        e = i2c_master_transmit_receive(s_dev, &reg, 1, data, len, IMU_I2C_TIMEOUT_MS);
        if (e == ESP_OK) { imu_i2c_retried("read", reg, first); return 0; }
        imu_i2c_fail("read", reg, len, e);
        return -1;
    }
    return 0;
}

static int8_t imu_i2c_write(uint8_t reg, const uint8_t *data, uint32_t len, void *intf)
{
    (void)intf;
    if (s_dev == NULL || len > IMU_MAX_RW) return -1;
    s_wbuf[0] = reg;
    memcpy(&s_wbuf[1], data, len);
    esp_err_t e = i2c_master_transmit(s_dev, s_wbuf, len + 1, IMU_I2C_TIMEOUT_MS);
    if (e != ESP_OK) {
        const esp_err_t first = e;
        vTaskDelay(IMU_I2C_RETRY_TICKS);
        e = i2c_master_transmit(s_dev, s_wbuf, len + 1, IMU_I2C_TIMEOUT_MS);
        if (e == ESP_OK) { imu_i2c_retried("write", reg, first); return 0; }
        imu_i2c_fail("write", reg, len, e);
        return -1;
    }
    return 0;
}

static void imu_delay_us(uint32_t period_us, void *intf)
{
    (void)intf;
    if (period_us >= 10000) vTaskDelay(pdMS_TO_TICKS(period_us / 1000));
    else if (period_us)     esp_rom_delay_us(period_us);
}

/* ---- FIFO parse callbacks -------------------------------------------------- */
static void imu_accel_cb(const struct bhy2_fifo_parse_data_info *info, void *priv)
{
    (void)priv;
    if (info->data_ptr == NULL) return;
    struct bhy2_data_xyz v;
    bhy2_parse_xyz(info->data_ptr, &v);
    taskENTER_CRITICAL(&s_lock);
    s_ax = (float)v.x * IMU_ACCEL_SCALE_G;
    s_ay = (float)v.y * IMU_ACCEL_SCALE_G;
    s_az = (float)v.z * IMU_ACCEL_SCALE_G;
    s_have = true;
    taskEXIT_CRITICAL(&s_lock);
}

/* Game-rotation-vector -> tilt-robust relative heading (F2). A raw euler yaw only equals a turn-heading
 * when the watch is FLAT; tilted (in the hand / on the wrist) it mixes tilt into the angle. Instead we
 * rotate the watch's 12-o'clock axis (body +Y) into the world frame by the fusion quaternion and take its
 * AZIMUTH in the horizontal plane: turning about vertical moves it 1:1 at any tilt, while tilting the
 * watch to read it leaves it put. Relative only (game rotation vector has no magnetometer reference), so
 * the sweep zeroes it at start. Normalized to [0,360). */
static void imu_gamerv_cb(const struct bhy2_fifo_parse_data_info *info, void *priv)
{
    (void)priv;
    if (info->data_ptr == NULL) return;
    struct bhy2_data_quaternion q;
    bhy2_parse_quaternion(info->data_ptr, &q);
    float w = q.w / 16384.0f, x = q.x / 16384.0f, y = q.y / 16384.0f, z = q.z / 16384.0f;
    /* World X/Y components of body +Y = columns of the quaternion rotation matrix (the Z part, straight
     * up/down, is discarded by the horizontal projection). */
    float wx = 2.0f * (x * y - w * z);
    float wy = 1.0f - 2.0f * (x * x + z * z);
    float hdg = atan2f(wx, wy) * (180.0f / (float)M_PI);
    if (hdg < 0.0f) hdg += 360.0f;
    taskENTER_CRITICAL(&s_lock);
    s_heading = hdg;
    s_have_heading = true;
    taskEXIT_CRITICAL(&s_lock);
}

static void imu_meta_cb(const struct bhy2_fifo_parse_data_info *info, void *priv)
{
    (void)priv;
    if (info->data_ptr == NULL) return;
    uint8_t ev = info->data_ptr[0];
    uint8_t d  = info->data_size > 1 ? info->data_ptr[1] : 0;
    ESP_LOGD(TAG, "meta sid=%u ev=%u data=%u", info->sensor_id, ev, d);
}

/* ---- bring-up (worker task) ----------------------------------------------- */
static esp_err_t imu_bring_up(void)
{
    /* Sensor rail (ALDO4) + settle for the BHI260AP power-on. */
    esp_err_t e = nocsif_power_sensor_rail(true);
    if (e != ESP_OK) { ESP_LOGE(TAG, "ALDO4 sensor rail on failed: %s", esp_err_to_name(e)); return e; }
    vTaskDelay(pdMS_TO_TICKS(50));   /* rail settle + BHI260AP power-on before the first I2C */

    i2c_master_bus_handle_t bus = nocsif_i2c_bus();
    if (bus == NULL) { ESP_LOGE(TAG, "i2c bus not up"); return ESP_ERR_INVALID_STATE; }
    i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = BHI260AP_I2C_ADDR,
        .scl_speed_hz    = IMU_I2C_HZ,
    };
    e = i2c_master_bus_add_device(bus, &dc, &s_dev);
    if (e != ESP_OK) { ESP_LOGE(TAG, "i2c add 0x28 failed: %s", esp_err_to_name(e)); return e; }

    int8_t r = bhy2_init(BHY2_I2C_INTERFACE, imu_i2c_read, imu_i2c_write, imu_delay_us,
                         IMU_MAX_RW, NULL, &s_bhy2);
    if (r != BHY2_OK) { ESP_LOGE(TAG, "bhy2_init %d", r); return ESP_FAIL; }

    r = bhy2_soft_reset(&s_bhy2);
    if (r != BHY2_OK) { ESP_LOGE(TAG, "soft_reset %d", r); return ESP_FAIL; }

    uint8_t pid = 0;
    r = bhy2_get_product_id(&pid, &s_bhy2);
    if (r != BHY2_OK || pid != BHY2_PRODUCT_ID) {
        ESP_LOGE(TAG, "product id 0x%02X (want 0x%02X) r=%d", pid, BHY2_PRODUCT_ID, r);
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "BHI260AP found (product id 0x%02X)", pid);

    /* Host interrupt control (0x07): status FIFO on, debug off, level / active-high / push-pull;
     * and enable the async status channel (0x06). Matches LilyGo; sets the INT pin up for the
     * later IRQ-driven read even though this slice polls. */
    uint8_t hic = 0;
    bhy2_get_host_interrupt_ctrl(&hic, &s_bhy2);
    hic &= ~BHY2_ICTL_DISABLE_STATUS_FIFO;
    hic |=  BHY2_ICTL_DISABLE_DEBUG;
    hic &= ~BHY2_ICTL_EDGE;
    hic &= ~BHY2_ICTL_ACTIVE_LOW;
    hic &= ~BHY2_ICTL_OPEN_DRAIN;
    bhy2_set_host_interrupt_ctrl(hic, &s_bhy2);
    bhy2_set_host_intf_ctrl(BHY2_HIF_CTRL_ASYNC_STATUS_CHANNEL, &s_bhy2);

    /* Upload firmware to RAM + boot. ~117 KB over I2C ≈ a couple of seconds. */
    uint32_t fw_len = nocsif_bhi260_fw_size();
    ESP_LOGI(TAG, "uploading firmware to RAM (%u bytes)...", (unsigned)fw_len);
    int64_t t0 = esp_timer_get_time();
    r = bhy2_upload_firmware_to_ram(nocsif_bhi260_fw_image(), fw_len, &s_bhy2);
    if (r != BHY2_OK) { ESP_LOGE(TAG, "upload_firmware_to_ram %d", r); return ESP_FAIL; }
    r = bhy2_boot_from_ram(&s_bhy2);
    if (r != BHY2_OK) { ESP_LOGE(TAG, "boot_from_ram %d", r); return ESP_FAIL; }

    uint16_t kver = 0;
    r = bhy2_get_kernel_version(&kver, &s_bhy2);
    if (r != BHY2_OK || kver == 0) { ESP_LOGE(TAG, "kernel version %u r=%d", kver, r); return ESP_FAIL; }
    ESP_LOGI(TAG, "firmware booted in %lld ms, kernel version %u",
             (long long)((esp_timer_get_time() - t0) / 1000), kver);
    taskENTER_CRITICAL(&s_lock);
    s_kernel_ver = kver;
    taskEXIT_CRITICAL(&s_lock);

    /* System meta events (logged at debug) + the accelerometer data callback. */
    bhy2_register_fifo_parse_callback(BHY2_SYS_ID_META_EVENT,    imu_meta_cb, NULL, &s_bhy2);
    bhy2_register_fifo_parse_callback(BHY2_SYS_ID_META_EVENT_WU, imu_meta_cb, NULL, &s_bhy2);
    r = bhy2_register_fifo_parse_callback(BHY2_SENSOR_ID_ACC_PASS, imu_accel_cb, NULL, &s_bhy2);
    if (r != BHY2_OK) { ESP_LOGE(TAG, "register accel cb %d", r); return ESP_FAIL; }
    /* F2 heading: register the GAMERV parser now, but leave the sensor OFF (rate 0) — the worker turns
     * it on only when the UI requests heading, so the gyro is not powered at idle. */
    r = bhy2_register_fifo_parse_callback(BHY2_SENSOR_ID_GAMERV, imu_gamerv_cb, NULL, &s_bhy2);
    if (r != BHY2_OK) { ESP_LOGE(TAG, "register gamerv cb %d", r); return ESP_FAIL; }

    bhy2_get_and_process_fifo(s_fifo_work, IMU_FIFO_WORK_SIZE, &s_bhy2);   /* prime */
    bhy2_update_virtual_sensor_list(&s_bhy2);

    bhy2_set_orientation_matrix(BHY2_PHYS_SENSOR_ID_ACCELEROMETER, IMU_REMAP_ACCEL, &s_bhy2);
    /* F2 heading: the gyro must share the accel's mounting matrix, or the GAMERV fusion (which combines
     * BOTH) gets inconsistent axes and produces a wrong rotation vector. Same physical package, same map. */
    bhy2_set_orientation_matrix(BHY2_PHYS_SENSOR_ID_GYROSCOPE, IMU_REMAP_ACCEL, &s_bhy2);

    r = bhy2_set_virt_sensor_cfg(BHY2_SENSOR_ID_ACC_PASS, IMU_ACCEL_RATE_HZ, 0, &s_bhy2);
    if (r != BHY2_OK) { ESP_LOGE(TAG, "set accel cfg %d", r); return ESP_FAIL; }

    /* B1 shake-to-wake is accel-based (the detector lives in imu_task) — available whenever the
     * accelerometer streams, so mark it ready now. See the SHAKE_* rationale up top. */
    taskENTER_CRITICAL(&s_lock);
    s_wrist_ready = true;
    taskEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "shake-to-wake ready (accel-based)");

    return ESP_OK;
}

/* Bring-up retries. An intermittent BHI260 firmware-upload glitch (a jostled I2C transfer, a rail not yet
 * settled) used to leave the IMU offline for the WHOLE session — the worker gave up after a single try
 * (vTaskDelete), so the motion sensor stayed dead until a full reboot. Retry fast a few times, then keep
 * retrying slowly forever, so it self-heals on its own. */
#define IMU_BRINGUP_TRIES 3

static void imu_task(void *arg)
{
    (void)arg;
    volatile uint8_t probe;                          /* an on-stack byte: is the stack in PSRAM? (Phase A2) */
    ESP_LOGI(TAG, "worker up: stack in %s, fifo work buffer in %s", esp_ptr_external_ram((void *)&probe) ? "PSRAM" : "INTERNAL",
             esp_ptr_external_ram(s_fifo_work) ? "PSRAM" : "INTERNAL");
    int tries = 0;
    while (imu_bring_up() != ESP_OK) {
        if (s_dev) { i2c_master_bus_rm_device(s_dev); s_dev = NULL; }   /* undo a partial add before the retry */
        int wait_ms = (++tries <= IMU_BRINGUP_TRIES) ? 200 : 5000;      /* fast first, then a slow heal loop */
        ESP_LOGW(TAG, "bring-up failed (try %d) — retrying in %d ms", tries, wait_ms);
        set_state(NOCSIF_IMU_BOOTING);                                  /* not "offline" — it is still coming up */
        vTaskDelay(pdMS_TO_TICKS(wait_ms));
    }
    set_state(NOCSIF_IMU_ONLINE);
    if (tries) ESP_LOGI(TAG, "bring-up recovered after %d retr%s", tries, tries == 1 ? "y" : "ies");
    ESP_LOGI(TAG, "online — accelerometer streaming at %d Hz", (int)IMU_ACCEL_RATE_HZ);

#if NOCSIF_IMU_LOG_ACCEL
    int log_div = 0;
#endif
    for (;;) {
        int8_t r = bhy2_get_and_process_fifo(s_fifo_work, IMU_FIFO_WORK_SIZE, &s_bhy2);
        if (r != BHY2_OK) ESP_LOGW(TAG, "fifo process err %d", r);
#if NOCSIF_IMU_LOG_ACCEL
        if (++log_div >= (1000 / IMU_POLL_MS)) {   /* ~1 s */
            log_div = 0;
            float x, y, z;
            if (nocsif_imu_accel_g(&x, &y, &z))
                ESP_LOGI(TAG, "accel  x%+5d  y%+5d  z%+5d  mg",
                         (int)lroundf(x * 1000.0f), (int)lroundf(y * 1000.0f), (int)lroundf(z * 1000.0f));
        }
#endif
        /* F2 heading: enable/disable the GAMERV virtual sensor to match the UI's request. Done here on
         * the worker (I2C-owning) task; toggling rate 0<->IMU_HEADING_RATE_HZ powers the gyro only while
         * a hunt actually needs a bearing. */
        taskENTER_CRITICAL(&s_lock);
        bool head_want = s_head_want;
        taskEXIT_CRITICAL(&s_lock);
        if (head_want != s_head_active) {
            int8_t hr = bhy2_set_virt_sensor_cfg(BHY2_SENSOR_ID_GAMERV,
                                                 head_want ? IMU_HEADING_RATE_HZ : 0.0f, 0, &s_bhy2);
            if (hr == BHY2_OK) {
                s_head_active = head_want;
                if (!head_want) {
                    taskENTER_CRITICAL(&s_lock);
                    s_have_heading = false;
                    taskEXIT_CRITICAL(&s_lock);
                }
                ESP_LOGI(TAG, "heading (GAMERV) %s", head_want ? "enabled" : "disabled");
            } else {
                ESP_LOGW(TAG, "set gamerv cfg %d", hr);
            }
        }

        /* B1 shake-to-wake detector (accel-based, worker task — never touches LVGL). Integrate the
         * per-sample accel change with a leaky accumulator; a sustained vigorous wrist shake climbs it
         * to SHAKE_FIRE and latches a wake, while a single bump or slow motion decays back to zero. */
        taskENTER_CRITICAL(&s_lock);
        bool want = s_wrist_want;
        bool have = s_have;
        float ax = s_ax, ay = s_ay, az = s_az;
        taskEXIT_CRITICAL(&s_lock);

        /* Stillness tracker (always on, independent of shake): count consecutive near-constant samples
         * so the UI can sleep the panel early when the watch is set down (F1 "sleep when still"). */
        if (have) {
            if (s_st_have_prev) {
                float dm = fabsf(ax - s_st_px) + fabsf(ay - s_st_py) + fabsf(az - s_st_pz);
                if (dm > STILL_MOVE) s_still_polls = 0;
                else if (s_still_polls < 0x7FFFFFFF) s_still_polls++;
            }
            s_st_px = ax; s_st_py = ay; s_st_pz = az;
            s_st_have_prev = true;
            uint32_t ms = s_still_polls * IMU_POLL_MS;
            taskENTER_CRITICAL(&s_lock);
            s_still_ms = ms;
            taskEXIT_CRITICAL(&s_lock);
        }

        if (want) {
            if (!s_wrist_active) { s_wrist_active = true; s_sh_have_prev = false; s_sh_acc = 0; ESP_LOGI(TAG, "shake-to-wake enabled"); }
            if (have) {
                if (s_sh_have_prev) {
                    float m = fabsf(ax - s_sh_px) + fabsf(ay - s_sh_py) + fabsf(az - s_sh_pz);
                    if (m > SHAKE_MOVE) { if (s_sh_acc < SHAKE_CAP) s_sh_acc++; }
                    else if (s_sh_acc > 0) { s_sh_acc--; }
                    if (s_sh_acc >= SHAKE_FIRE) {
                        s_sh_acc = 0;
                        taskENTER_CRITICAL(&s_lock);
                        if (s_wrist_events < 0xFFFF) s_wrist_events++;
                        taskEXIT_CRITICAL(&s_lock);
                        ESP_LOGD(TAG, "shake detected");
                    }
                }
                s_sh_px = ax; s_sh_py = ay; s_sh_pz = az;
                s_sh_have_prev = true;
            }
        } else if (s_wrist_active) {
            s_wrist_active = false;
            s_sh_acc = 0;
            ESP_LOGI(TAG, "shake-to-wake disabled");
        }
        vTaskDelay(pdMS_TO_TICKS(IMU_POLL_MS));
    }
}

/* ---- public API ----------------------------------------------------------- */
esp_err_t nocsif_imu_init(void)
{
    if (s_started) return ESP_OK;
    if (nocsif_reliability_safe_mode()) {
        ESP_LOGW(TAG, "safe mode — IMU bring-up skipped");
        set_state(NOCSIF_IMU_OFF);
        return ESP_OK;
    }
    s_started = true;
    set_state(NOCSIF_IMU_BOOTING);
    if (s_fifo_work == NULL) {
        s_fifo_work = heap_caps_malloc(IMU_FIFO_WORK_SIZE, MALLOC_CAP_SPIRAM);   /* Phase A2: off the int pool */
        if (s_fifo_work == NULL) {
            s_started = false;
            set_state(NOCSIF_IMU_FAILED);
            ESP_LOGE(TAG, "fifo work buffer alloc failed");
            return ESP_ERR_NO_MEM;
        }
    }
    /* Stack in PSRAM (xTaskCreateWithCaps + SPIRAM, RAM Phase A2): this worker only talks I2C (no DMA on
     * the S3 I2C peripheral; the driver copies its hardware FIFO from a non-IRAM ISR) and never runs with
     * the flash cache disabled (no on-task NVS/flash writes — only nocsif_reliability_safe_mode() RAM
     * reads), so its 6 KB no longer competes for the scarce contiguous internal-DMA hole. Never deleted
     * (the retry loop lives INSIDE the task) -> no vTaskDeleteWithCaps needed. */
    if (xTaskCreateWithCaps(imu_task, "nocsif_imu", IMU_TASK_STACK, NULL, IMU_TASK_PRIO, NULL,
                            MALLOC_CAP_SPIRAM) != pdPASS) {
        s_started = false;
        set_state(NOCSIF_IMU_FAILED);
        ESP_LOGE(TAG, "worker task create failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

nocsif_imu_state_t nocsif_imu_state(void)
{
    taskENTER_CRITICAL(&s_lock);
    nocsif_imu_state_t s = s_state;
    taskEXIT_CRITICAL(&s_lock);
    return s;
}

bool nocsif_imu_online(void)
{
    return nocsif_imu_state() == NOCSIF_IMU_ONLINE;
}

bool nocsif_imu_accel_g(float *x, float *y, float *z)
{
    taskENTER_CRITICAL(&s_lock);
    bool have = s_have;
    float a = s_ax, b = s_ay, c = s_az;
    taskEXIT_CRITICAL(&s_lock);
    if (!have) return false;
    if (x) *x = a;
    if (y) *y = b;
    if (z) *z = c;
    return true;
}

const char *nocsif_imu_status_str(void)
{
    static char buf[48];
    taskENTER_CRITICAL(&s_lock);
    nocsif_imu_state_t st = s_state;
    float x = s_ax, y = s_ay, z = s_az;
    bool have = s_have;
    uint16_t kv = s_kernel_ver;
    taskEXIT_CRITICAL(&s_lock);

    switch (st) {
    case NOCSIF_IMU_OFF:     strcpy(buf, "off");        break;
    case NOCSIF_IMU_BOOTING: strcpy(buf, "booting");    break;
    case NOCSIF_IMU_FAILED:  strcpy(buf, "not found");  break;
    case NOCSIF_IMU_ONLINE:
        if (have) snprintf(buf, sizeof buf, "x%+d y%+d z%+d mg",
                           (int)lroundf(x * 1000.0f), (int)lroundf(y * 1000.0f), (int)lroundf(z * 1000.0f));
        else      snprintf(buf, sizeof buf, "online (k%u)", kv);
        break;
    default:                 strcpy(buf, "?");          break;
    }
    return buf;
}

/* ---- wrist-raise auto-wake (B1) ------------------------------------------- */
void nocsif_imu_set_wrist_wake(bool enable)
{
    taskENTER_CRITICAL(&s_lock);
    s_wrist_want = enable;
    taskEXIT_CRITICAL(&s_lock);
}

bool nocsif_imu_wrist_wake_available(void)
{
    taskENTER_CRITICAL(&s_lock);
    bool a = s_wrist_ready;
    taskEXIT_CRITICAL(&s_lock);
    return a;
}

unsigned nocsif_imu_take_wrist_raise(void)
{
    taskENTER_CRITICAL(&s_lock);
    uint32_t n = s_wrist_events;
    s_wrist_events = 0;
    taskEXIT_CRITICAL(&s_lock);
    return (unsigned)n;
}

uint32_t nocsif_imu_still_ms(void)
{
    taskENTER_CRITICAL(&s_lock);
    uint32_t v = s_still_ms;
    taskEXIT_CRITICAL(&s_lock);
    return v;
}

/* ---- relative heading (F2) ------------------------------------------------- */
void nocsif_imu_set_heading(bool enable)
{
    taskENTER_CRITICAL(&s_lock);
    s_head_want = enable;
    taskEXIT_CRITICAL(&s_lock);
}

bool nocsif_imu_heading_deg(float *deg)
{
    taskENTER_CRITICAL(&s_lock);
    bool have = s_have_heading;
    float h = s_heading;
    taskEXIT_CRITICAL(&s_lock);
    if (!have) return false;
    if (deg) *deg = h;
    return true;
}
