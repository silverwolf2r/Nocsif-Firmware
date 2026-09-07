/*
 * NocSif — PDM microphone (I2S PDM RX) worker (M11 watch-core, slice E1·2). See mic.h.
 *
 * Architecture (mirrors imu.c's producer/cache shape):
 *   - A dedicated worker task owns the I2S PDM RX channel and, while capture is ACTIVE, reads 16-bit
 *     PCM blocks, reduces each to an AC RMS + peak, and publishes a smoothed 0..100 level into a
 *     spinlock-guarded cache. The LVGL-side getters read only that cache (no I2S), so a live
 *     level-meter screen can poll them from an lv_timer.
 *   - Capture is GATED by nocsif_mic_set_active: the worker parks on a task notification (no spinning,
 *     channel freed) until activated, opens the PDM RX channel lazily on first activation, and closes
 *     it on deactivation — so the mic listens only while the Microphone screen is open (power/privacy)
 *     and internal DMA returns to the pool (the radio) in between.
 *
 * Signal path: on the ESP32-S3 the I2S peripheral does PDM->PCM in hardware (PCM default slot config)
 * with the high-pass filter on, so DC is largely removed on-chip; we still compute the RMS about the
 * per-block mean (AC-coupled) for robustness, apply a fixed gain, and clamp to 0..100. MIC_GAIN is the
 * sensitivity knob — raise it if the bar barely moves on-device (the E1·2 proof is "blow -> bar moves").
 */
#include "mic.h"

#include <math.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>            /* mkdir (voice-memo output dir) */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"   /* xTaskCreateWithCaps — PSRAM worker stack (RAM-BUDGET remake #7) */
#include "esp_memory_utils.h"         /* esp_ptr_external_ram — PSRAM-stack self-test probe */

#include "esp_log.h"
#include "esp_heap_caps.h"       /* heap_caps_malloc(MALLOC_CAP_SPIRAM) — the record buffer */
#include "driver/i2s_pdm.h"

#include "reliability.h"          /* nocsif_reliability_safe_mode() */
#include "sdcard.h"               /* nocsif_sdcard_lock/unlock — /sd access for the WAV flush */
#include "usb_gadget.h"           /* nocsif_usb_gadget_claim_sd — own /sd away from USB-MSC while writing */

static const char *TAG = "mic";

/* ---- hardware / tunables ---------------------------------------------------------- */
#define MIC_I2S_PORT      I2S_NUM_0    /* PDM RX only lives on I2S0 on the S3; the amp is I2S_NUM_1 */
#define MIC_CLK_GPIO      17           /* PDM clock (lilygo_twatch_ultra pins_arduino.h MIC_SCK)    */
#define MIC_DAT_GPIO      18           /* PDM data  (... MIC_DAT)                                   */
#define MIC_SAMPLE_RATE   16000        /* 16 kHz mono — plenty for a level meter / voice memo       */
#define MIC_READ_FRAMES   256          /* samples per read (~16 ms at 16 kHz) -> responsive meter   */
#define MIC_READ_MS       100          /* i2s read timeout: bounds how fast a stop is noticed       */
/* Level mapping: pct = clamp( level * gain / 32768 * 100 ). A PDM MEMS mic outputs modest PCM for
 * speech; the gain lifts normal sound into a visible range while blowing pins the bar. The gain is now
 * a RUNTIME value (s_gain, adjustable from the Microphone screen + persisted by the UI) — the default
 * was raised from the E1·2-initial 12 because speech barely registered at that level. */
#define MIC_GAIN_DEFAULT  200.0f
#define MIC_GAIN_MIN      2.0f
#define MIC_GAIN_MAX      600.0f
/* Voice memo (E1·3): record 16 kHz mono 16-bit PCM into a PSRAM buffer, capped at MIC_REC_MAX_SECS,
 * then flush to a WAV on /sd. 30 s * 16000 * 2 B ≈ 960 KB of PSRAM (plenty free). */
#define MIC_REC_MAX_SECS  NOCSIF_MIC_REC_MAX_SECS
#define MIC_REC_CAP_BYTES ((size_t)MIC_REC_MAX_SECS * MIC_SAMPLE_RATE * 2)
#define MIC_WAV_HDR_BYTES 44
/* Normalize a saved memo toward full-scale so playback is AUDIBLE: the raw PDM mic PCM is only a few %
 * of full-scale (the level meter looks healthy only because it applies a big DISPLAY gain that never
 * touches the stored samples), which is inaudible on the small speaker. Target the peak to ~80% of
 * int16; cap the gain so a near-silent recording isn't amplified to a roar. */
#define MIC_REC_NORM_TARGET 30000
#define MIC_REC_NORM_MAX    64.0f
/* Display smoothing: the shown level is an EMA of per-block RMS so the bar glides; the peak-hold jumps
 * up instantly and decays each block, for a classic level-meter feel. */
#define MIC_LEVEL_EMA     0.35f        /* new-block weight (higher = snappier)                       */
#define MIC_PEAK_DECAY    6            /* pct subtracted from the held peak each block               */
/* 6 KB: the level-meter loop is shallow, but the voice-memo flush (rec_finalize -> fopen/fwrite ->
 * FatFs -> SDSPI -> spi_master) is a deep chain run on this same worker — the headroom avoids the
 * 4 KB-worker overflow class (same rationale as imu.c / the BLE-PCAP writer). */
#define MIC_TASK_STACK    6144
#define MIC_TASK_PRIO     3

/* ---- module state --------------------------------------------------------- */
/* The value cache is shared between the worker (writer) and the LVGL-side getters (readers); a short
 * spinlock keeps the read/write coherent without a heavier mutex (imu.c pattern). */
static portMUX_TYPE        s_lock = portMUX_INITIALIZER_UNLOCKED;
static nocsif_mic_state_t  s_state = NOCSIF_MIC_OFF;
static uint8_t             s_level;   /* smoothed 0..100 (spinlock)        */
static uint8_t             s_peak;    /* decaying peak 0..100 (spinlock)   */

static TaskHandle_t        s_task;
static volatile bool       s_active;                  /* desired capture state, set by the UI     */
static float               s_gain = MIC_GAIN_DEFAULT; /* RMS->level gain (aligned 32-bit: torn-read-safe) */
/* Touched only by the worker task. */
static i2s_chan_handle_t   s_rx;                      /* NULL unless capturing                     */
static int16_t             s_buf[MIC_READ_FRAMES];    /* PCM read scratch                          */
static volatile bool       s_stack_ext;               /* worker stack in PSRAM (set on 1st schedule) */
static float               s_level_f;                 /* EMA accumulator                          */

/* ---- voice memo recording (E1·3) ------------------------------------------ */
static nocsif_mic_rec_state_t s_rec_state;            /* spinlock          */
static volatile nocsif_mic_rec_err_t s_rec_err;       /* why the last one failed (worker writes, UI reads an int) */
static uint32_t            s_rec_secs;                 /* spinlock          */
static volatile bool       s_rec_start_req;            /* UI -> worker      */
static volatile bool       s_rec_stop_req;             /* UI -> worker      */
static char                s_rec_path[96];             /* set before start_req; worker-read */
/* Worker-only. */
static bool                s_recording;
static uint8_t            *s_rec_buf;                  /* PSRAM PCM buffer  */
static size_t              s_rec_len;                  /* bytes captured    */

static void set_state(nocsif_mic_state_t s)
{
    taskENTER_CRITICAL(&s_lock);
    s_state = s;
    taskEXIT_CRITICAL(&s_lock);
}

/* ---- I2S PDM RX channel (worker task context only) ------------------------- */
/* Open + configure + enable the PDM RX channel. Returns ESP_OK once s_rx is valid and started. */
static esp_err_t mic_open(void)
{
    if (s_rx != NULL) {
        return ESP_OK;
    }
    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(MIC_I2S_PORT, I2S_ROLE_MASTER);
    esp_err_t err = i2s_new_channel(&chan, NULL, &s_rx);   /* RX handle is the 3rd arg */
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(err));
        s_rx = NULL;
        return err;
    }
    i2s_pdm_rx_config_t pdm = {
        .clk_cfg  = I2S_PDM_RX_CLK_DEFAULT_CONFIG(MIC_SAMPLE_RATE),
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .clk = MIC_CLK_GPIO,
            .din = MIC_DAT_GPIO,
            .invert_flags = { .clk_inv = 0 },
        },
    };
    err = i2s_channel_init_pdm_rx_mode(s_rx, &pdm);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_pdm_rx_mode failed: %s", esp_err_to_name(err));
        i2s_del_channel(s_rx);
        s_rx = NULL;
        return err;
    }
    err = i2s_channel_enable(s_rx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_enable failed: %s", esp_err_to_name(err));
        i2s_del_channel(s_rx);
        s_rx = NULL;
        return err;
    }
    ESP_LOGI(TAG, "PDM RX ready (CLK=%d DAT=%d, %d Hz mono LEFT) — MEMS mic",
             MIC_CLK_GPIO, MIC_DAT_GPIO, MIC_SAMPLE_RATE);
    return ESP_OK;
}

static void mic_close(void)
{
    if (s_rx == NULL) {
        return;
    }
    i2s_channel_disable(s_rx);
    i2s_del_channel(s_rx);
    s_rx = NULL;
    ESP_LOGI(TAG, "PDM RX closed");
}

/* Reduce one PCM block to a 0..100 RMS level (AC-coupled: RMS about the block mean) + a peak. */
static void process_block(const int16_t *pcm, size_t n, uint8_t *rms_pct, uint8_t *peak_pct)
{
    if (n == 0) {
        *rms_pct = 0;
        *peak_pct = 0;
        return;
    }
    int64_t sum = 0;
    for (size_t i = 0; i < n; i++) {
        sum += pcm[i];
    }
    float mean = (float)sum / (float)n;
    double acc = 0.0;
    int32_t peak = 0;
    for (size_t i = 0; i < n; i++) {
        float d = (float)pcm[i] - mean;
        acc += (double)d * (double)d;
        int32_t a = (int32_t)(d < 0 ? -d : d);
        if (a > peak) peak = a;
    }
    float rms = sqrtf((float)(acc / (double)n));
    float g   = s_gain;                          /* aligned 32-bit read — no tearing on Xtensa */
    float rp = rms  * g / 32768.0f * 100.0f;
    float pp = (float)peak * g / 32768.0f * 100.0f;
    if (rp > 100.0f) rp = 100.0f;
    if (pp > 100.0f) pp = 100.0f;
    *rms_pct  = (uint8_t)(rp + 0.5f);
    *peak_pct = (uint8_t)(pp + 0.5f);
}

/* Write a canonical 16 kHz mono 16-bit PCM WAV (44-byte header + samples) to `path` on /sd. Claims
 * /sd away from USB-MSC + takes the FAT lock (the PCAP-writer idiom). ESP32 is little-endian, so the
 * multi-byte header fields memcpy out in the WAV LE order directly. Returns true on success. */
static bool mic_write_wav(const char *path, const uint8_t *pcm, size_t len)
{
    esp_err_t ce = nocsif_usb_gadget_claim_sd(2000);
    if (ce == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "record: File Share owns the card — cannot save");
        s_rec_err = NOCSIF_MIC_ERR_CARD_USB;
        return false;
    }
    if (ce != ESP_OK) {
        ESP_LOGW(TAG, "record: claim_sd -> %s", esp_err_to_name(ce));
    }
    if (!nocsif_sdcard_lock(3000)) {
        ESP_LOGW(TAG, "record: /sd lock timeout");
        s_rec_err = NOCSIF_MIC_ERR_CARD_BUSY;
        return false;
    }
    mkdir("/sd/nocsif", 0777);          /* ignore EEXIST */
    mkdir("/sd/nocsif/voice", 0777);
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "record: fopen(%s) failed", path);
        s_rec_err = NOCSIF_MIC_ERR_FILE_OPEN;
        nocsif_sdcard_unlock();
        return false;
    }
    uint32_t data_len  = (uint32_t)len;
    uint32_t byte_rate = (uint32_t)MIC_SAMPLE_RATE * 1 * 2;
    uint8_t  h[MIC_WAV_HDR_BYTES];
    memcpy(h + 0, "RIFF", 4);
    uint32_t riff = 36 + data_len;      memcpy(h + 4, &riff, 4);
    memcpy(h + 8, "WAVE", 4);
    memcpy(h + 12, "fmt ", 4);
    uint32_t fmt_len = 16;              memcpy(h + 16, &fmt_len, 4);
    uint16_t fmt = 1, ch = 1;           memcpy(h + 20, &fmt, 2);   memcpy(h + 22, &ch, 2);
    uint32_t sr = MIC_SAMPLE_RATE;      memcpy(h + 24, &sr, 4);
    memcpy(h + 28, &byte_rate, 4);
    uint16_t block = 2, bits = 16;      memcpy(h + 32, &block, 2); memcpy(h + 34, &bits, 2);
    memcpy(h + 36, "data", 4);          memcpy(h + 40, &data_len, 4);
    bool ok = (fwrite(h, 1, sizeof h, f) == sizeof h);
    if (ok && data_len) {
        ok = (fwrite(pcm, 1, data_len, f) == data_len);
    }
    fclose(f);
    nocsif_sdcard_unlock();
    if (!ok) {
        ESP_LOGE(TAG, "record: short write to %s", path);
        s_rec_err = NOCSIF_MIC_ERR_FILE_WRITE;
    } else {
        ESP_LOGI(TAG, "record: saved %s (%u bytes, %us)", path, (unsigned)data_len,
                 (unsigned)(byte_rate ? data_len / byte_rate : 0));
    }
    return ok;
}

/* Normalize a recorded PCM buffer toward full-scale so the memo is audible on playback. Scans the peak,
 * applies a capped gain (never attenuates), clamps to int16. See MIC_REC_NORM_* above. */
static void rec_normalize(int16_t *pcm, size_t nsamp)
{
    if (nsamp == 0) return;
    int32_t peak = 1;
    for (size_t i = 0; i < nsamp; i++) {
        int32_t a = pcm[i] < 0 ? -(int32_t)pcm[i] : pcm[i];
        if (a > peak) peak = a;
    }
    float g = (float)MIC_REC_NORM_TARGET / (float)peak;
    if (g > MIC_REC_NORM_MAX) g = MIC_REC_NORM_MAX;
    if (g <= 1.0f) return;                 /* already loud enough — leave it */
    for (size_t i = 0; i < nsamp; i++) {
        int32_t v = (int32_t)((float)pcm[i] * g);
        if (v > 32767)  v = 32767;
        if (v < -32768) v = -32768;
        pcm[i] = (int16_t)v;
    }
    ESP_LOGI(TAG, "record: normalized (peak %d, gain %.1fx)", (int)peak, (double)g);
}

/* Stop recording + flush the WAV (worker context). Frees the PSRAM buffer, publishes DONE/ERROR. */
static void rec_finalize(void)
{
    s_recording = false;
    taskENTER_CRITICAL(&s_lock);
    s_rec_state = NOCSIF_MIC_REC_SAVING;
    taskEXIT_CRITICAL(&s_lock);
    if (s_rec_buf != NULL && s_rec_len > 0) {
        rec_normalize((int16_t *)s_rec_buf, s_rec_len / sizeof(int16_t));   /* make it audible */
    }
    bool ok;
    if (s_rec_buf != NULL && s_rec_len > 0) {
        ok = mic_write_wav(s_rec_path, s_rec_buf, s_rec_len);   /* sets s_rec_err on its failure paths */
    } else {
        ok = false;
        s_rec_err = NOCSIF_MIC_ERR_EMPTY;
    }
    if (ok) s_rec_err = NOCSIF_MIC_ERR_NONE;
    if (s_rec_buf) {
        heap_caps_free(s_rec_buf);
        s_rec_buf = NULL;
    }
    uint32_t secs = (uint32_t)(s_rec_len / ((uint32_t)MIC_SAMPLE_RATE * 2));
    s_rec_len = 0;
    taskENTER_CRITICAL(&s_lock);
    s_rec_secs  = secs;
    s_rec_state = ok ? NOCSIF_MIC_REC_DONE : NOCSIF_MIC_REC_ERROR;
    taskEXIT_CRITICAL(&s_lock);
}

static void mic_task(void *arg)
{
    (void)arg;
    volatile uint8_t probe;                          /* an on-stack byte: is the stack in PSRAM? */
    s_stack_ext = esp_ptr_external_ram((void *)&probe);
    for (;;) {
        /* Keep capturing while EITHER a screen wants the live meter (s_active) OR a recording is in
         * flight (s_recording / a pending start). This is what lets recording continue in the
         * BACKGROUND after the Voice Memos screen is closed — the memo is finalized only on an explicit
         * stop or the cap, never on set_active(false). */
        if (!s_active && !s_recording && !s_rec_start_req) {
            /* Park: nothing wants the mic. Free the channel, zero the meter, sleep until notified. */
            mic_close();
            taskENTER_CRITICAL(&s_lock);
            s_level = 0;
            s_peak = 0;
            if (s_state != NOCSIF_MIC_FAILED) s_state = NOCSIF_MIC_OFF;
            taskEXIT_CRITICAL(&s_lock);
            s_level_f = 0.0f;
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        if (s_rx == NULL) {
            if (mic_open() != ESP_OK) {
                set_state(NOCSIF_MIC_FAILED);
                s_active = false;          /* park instead of hammering a broken channel */
                if (s_rec_start_req || s_recording) {   /* fail any recording that needed the mic */
                    s_rec_start_req = false;
                    s_recording = false;
                    s_rec_err = NOCSIF_MIC_ERR_MIC_OPEN;
                    taskENTER_CRITICAL(&s_lock);
                    s_rec_state = NOCSIF_MIC_REC_ERROR;
                    taskEXIT_CRITICAL(&s_lock);
                }
                continue;
            }
            set_state(NOCSIF_MIC_ACTIVE);
            s_level_f = 0.0f;
        }
        /* Begin a recording if requested — the PSRAM buffer is allocated here, on the worker. */
        if (s_rec_start_req && !s_recording) {
            s_rec_start_req = false;
            s_rec_buf = heap_caps_malloc(MIC_REC_CAP_BYTES, MALLOC_CAP_SPIRAM);
            if (s_rec_buf == NULL) {
                ESP_LOGE(TAG, "record: PSRAM alloc %u failed", (unsigned)MIC_REC_CAP_BYTES);
                s_rec_err = NOCSIF_MIC_ERR_NO_MEMORY;
                taskENTER_CRITICAL(&s_lock);
                s_rec_state = NOCSIF_MIC_REC_ERROR;
                taskEXIT_CRITICAL(&s_lock);
            } else {
                s_rec_len = 0;
                s_rec_err = NOCSIF_MIC_ERR_NONE;
                s_recording = true;
                taskENTER_CRITICAL(&s_lock);
                s_rec_secs  = 0;
                s_rec_state = NOCSIF_MIC_REC_RECORDING;
                taskEXIT_CRITICAL(&s_lock);
                ESP_LOGI(TAG, "record: start -> %s", s_rec_path);
            }
        }
        size_t got = 0;
        esp_err_t err = i2s_channel_read(s_rx, s_buf, sizeof(s_buf), &got, pdMS_TO_TICKS(MIC_READ_MS));
        if (err == ESP_ERR_TIMEOUT) {
            if (s_rec_stop_req && s_recording) { s_rec_stop_req = false; rec_finalize(); }
            continue;                      /* no data yet — loop back and re-check s_active */
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "i2s_channel_read: %s", esp_err_to_name(err));
            continue;
        }
        uint8_t rms_pct, peak_pct;
        process_block(s_buf, got / sizeof(int16_t), &rms_pct, &peak_pct);
        s_level_f = s_level_f + MIC_LEVEL_EMA * ((float)rms_pct - s_level_f);   /* glide the level */
        taskENTER_CRITICAL(&s_lock);
        s_level = (uint8_t)(s_level_f + 0.5f);
        int p = (int)s_peak - MIC_PEAK_DECAY;                                    /* decay the peak */
        if (p < 0) p = 0;
        if ((int)peak_pct > p) p = peak_pct;
        s_peak = (uint8_t)p;
        taskEXIT_CRITICAL(&s_lock);
        /* Append this block to the recording buffer + auto-stop at the cap. */
        if (s_recording && s_rec_buf) {
            size_t bytes = got;
            if (s_rec_len + bytes > MIC_REC_CAP_BYTES) {
                bytes = MIC_REC_CAP_BYTES - s_rec_len;
            }
            if (bytes) {
                memcpy(s_rec_buf + s_rec_len, s_buf, bytes);
                s_rec_len += bytes;
            }
            taskENTER_CRITICAL(&s_lock);
            s_rec_secs = (uint32_t)(s_rec_len / ((uint32_t)MIC_SAMPLE_RATE * 2));
            taskEXIT_CRITICAL(&s_lock);
            if (s_rec_len >= MIC_REC_CAP_BYTES) {   /* hit the cap → auto-stop */
                rec_finalize();
                continue;
            }
        }
        if (s_rec_stop_req && s_recording) {
            s_rec_stop_req = false;
            rec_finalize();
        }
    }
}

/* ---- public API ----------------------------------------------------------- */
esp_err_t nocsif_mic_init(void)
{
    if (s_task != NULL) {
        return ESP_OK;                     /* idempotent */
    }
    if (nocsif_reliability_safe_mode()) {
        ESP_LOGW(TAG, "safe mode — mic worker skipped");
        set_state(NOCSIF_MIC_OFF);
        return ESP_OK;
    }
    /* Stack in PSRAM (xTaskCreateWithCaps + SPIRAM, RAM-BUDGET remake #7): the mic worker never DMAs
     * from its own stack (I2S RX lands in the static-internal s_buf; the record buffer is its own PSRAM
     * alloc) and never runs with the flash cache disabled (no on-task NVS/flash writes), so its 6 KB no
     * longer competes for the scarce internal-DMA hole — a lazy first-use spawn under steady-state
     * fragmentation (largest ~3 KB) now succeeds. Never deleted -> no vTaskDeleteWithCaps. */
    if (xTaskCreateWithCaps(mic_task, "nocsif_mic", MIC_TASK_STACK, NULL, MIC_TASK_PRIO, &s_task,
                            MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "worker task create failed");
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "mic worker ready (lazy PDM RX; idle until a capture screen opens)");
    return ESP_OK;
}

bool nocsif_mic_stack_is_psram(void)
{
    return s_stack_ext;             /* set true once the worker first schedules on a PSRAM stack */
}

bool nocsif_mic_available(void)
{
    return s_task != NULL;
}

void nocsif_mic_set_active(bool active)
{
    if (s_task == NULL) {
        return;
    }
    s_active = active;
    if (active) {
        xTaskNotifyGive(s_task);           /* wake the worker if it is parked (harmless if running) */
    }
}

uint8_t nocsif_mic_level(void)
{
    taskENTER_CRITICAL(&s_lock);
    uint8_t v = s_level;
    taskEXIT_CRITICAL(&s_lock);
    return v;
}

uint8_t nocsif_mic_peak(void)
{
    taskENTER_CRITICAL(&s_lock);
    uint8_t v = s_peak;
    taskEXIT_CRITICAL(&s_lock);
    return v;
}

nocsif_mic_state_t nocsif_mic_state(void)
{
    taskENTER_CRITICAL(&s_lock);
    nocsif_mic_state_t s = s_state;
    taskEXIT_CRITICAL(&s_lock);
    return s;
}

const char *nocsif_mic_status_str(void)
{
    static char buf[32];
    taskENTER_CRITICAL(&s_lock);
    nocsif_mic_state_t st = s_state;
    uint8_t lvl = s_level;
    taskEXIT_CRITICAL(&s_lock);

    switch (st) {
    case NOCSIF_MIC_OFF:    strcpy(buf, "idle");                                               break;
    case NOCSIF_MIC_ACTIVE: snprintf(buf, sizeof buf, "listening %u%%", (unsigned)lvl);        break;
    case NOCSIF_MIC_FAILED: strcpy(buf, "no mic");                                             break;
    default:                strcpy(buf, "?");                                                  break;
    }
    return buf;
}

/* ---- adjustable sensitivity (E1·2) ---------------------------------------- */
void nocsif_mic_set_gain(float gain)
{
    if (gain < MIC_GAIN_MIN) gain = MIC_GAIN_MIN;
    if (gain > MIC_GAIN_MAX) gain = MIC_GAIN_MAX;
    s_gain = gain;   /* aligned 32-bit store — the worker reads it torn-free on the next block */
}

float nocsif_mic_gain(void)
{
    return s_gain;
}

/* ---- voice memo recording (E1·3) ------------------------------------------ */
void nocsif_mic_record_start(const char *path)
{
    if (s_task == NULL || path == NULL || path[0] == '\0') {
        return;
    }
    if (s_recording || s_rec_start_req) {
        return;                      /* already recording / queued */
    }
    strncpy(s_rec_path, path, sizeof s_rec_path - 1);
    s_rec_path[sizeof s_rec_path - 1] = '\0';
    s_rec_stop_req  = false;
    s_rec_start_req = true;
    if (s_task) {
        xTaskNotifyGive(s_task);     /* wake the worker; s_rec_start_req keeps it capturing in the
                                      * BACKGROUND, independent of the UI's set_active live meter */
    }
}

void nocsif_mic_record_stop(void)
{
    if (s_task == NULL) {
        return;
    }
    s_rec_stop_req = true;
}

nocsif_mic_rec_state_t nocsif_mic_record_state(void)
{
    taskENTER_CRITICAL(&s_lock);
    nocsif_mic_rec_state_t s = s_rec_state;
    taskEXIT_CRITICAL(&s_lock);
    return s;
}

uint32_t nocsif_mic_record_secs(void)
{
    taskENTER_CRITICAL(&s_lock);
    uint32_t s = s_rec_secs;
    taskEXIT_CRITICAL(&s_lock);
    return s;
}

bool nocsif_mic_recording(void)
{
    nocsif_mic_rec_state_t s = nocsif_mic_record_state();
    return s == NOCSIF_MIC_REC_RECORDING || s == NOCSIF_MIC_REC_SAVING;
}

nocsif_mic_rec_err_t nocsif_mic_rec_error(void)
{
    return s_rec_err;
}

const char *nocsif_mic_rec_error_str(void)
{
    switch (s_rec_err) {
        case NOCSIF_MIC_ERR_MIC_OPEN:   return "mic didn't open";
        case NOCSIF_MIC_ERR_NO_MEMORY:  return "out of memory";
        case NOCSIF_MIC_ERR_EMPTY:      return "nothing recorded";
        case NOCSIF_MIC_ERR_CARD_USB:   return "File Share has the card";
        case NOCSIF_MIC_ERR_CARD_BUSY:  return "card busy \xC2\xB7 try again";
        case NOCSIF_MIC_ERR_FILE_OPEN:  return "couldn't create the file";
        case NOCSIF_MIC_ERR_FILE_WRITE: return "write failed (card full?)";
        default:                        return "save failed";
    }
}
