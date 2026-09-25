/*
 * NocSif — PDM microphone (I2S PDM RX) worker (M11 watch-core, slice E1·2). See mic.h.
 *
 * Architecture (mirrors imu.c's producer/cache shape):
 *   - A dedicated worker task owns the I2S PDM RX channel and, while capture is ACTIVE, reads 16-bit
 *     PCM blocks, reduces each to an AC RMS + peak, and publishes a smoothed 0..100 level into a
 *     spinlock-guarded cache. The LVGL-side getters read only that cache (no I2S), so a live
 *     level-meter screen can poll them from an lv_timer.
 *   - Capture is GATED by nocsif_mic_set_active / _set_pitch / a recording: the worker parks on a task
 *     notification (no spinning) until wanted, ENABLES the PDM RX channel while capturing and DISABLES
 *     it when parked — so the mic listens only while a mic screen is open (power/privacy). The channel
 *     itself (its ~2.3 KB int-DMA ring) is created lazily on the first capture and DELETED on park, so
 *     an idle mic holds no internal-DMA (the mic is non-critical; see nocsif_mic_boot_reserve). It stays
 *     put across an active mic session (park only fires when nothing wants the mic) and reopens fine
 *     under BLE + WiFi + LoRa — each DMA descriptor is its own 512 B block, no contiguous 2.3 KB claim.
 *
 * Signal path: on the ESP32-S3 the I2S peripheral does PDM->PCM in hardware
 * (PCM default slot config) with the high-pass filter on, so DC is largely
 * removed on-chip; the RMS is still computed about the per-block mean
 * (AC-coupled) for robustness, a fixed gain applied, and the result clamped
 * to 0..100. MIC_GAIN is the sensitivity knob — raise it if the bar barely
 * moves on-device (the E1·2 proof is "blow -> bar moves").
 */
#include "mic.h"

#include <math.h>
#include <stdlib.h>              /* qsort — rec_loudness percentiles */
#include <string.h>
#include <stdio.h>
#include <unistd.h>              /* write() — the memo flush bypasses stdio (see mic_write_wav) */
#include <sys/stat.h>            /* mkdir (voice-memo output dir) */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"   /* xTaskCreateWithCaps — PSRAM worker stack */
#include "esp_memory_utils.h"         /* esp_ptr_external_ram — PSRAM-stack self-test probe */

#include "esp_log.h"
#include "esp_timer.h"           /* esp_timer_get_time — YIN pass timing telemetry */
#include "esp_heap_caps.h"       /* heap_caps_malloc(MALLOC_CAP_SPIRAM) — the record buffer */
#include "driver/i2s_pdm.h"

#include "reliability.h"          /* nocsif_reliability_safe_mode() */
#include "sdcard.h"               /* nocsif_sdcard_lock/unlock — /sd access for the WAV flush */
#include "usb_gadget.h"           /* nocsif_usb_gadget_claim_sd — own /sd away from USB-MSC while writing */
#include "coex.h"                 /* nocsif_log_dma_free — the boot reserve's int-DMA telemetry */

static const char *TAG = "mic";

/* ---- hardware / tunables ---------------------------------------------------------- */
#define MIC_I2S_PORT      I2S_NUM_0    /* PDM RX only lives on I2S0 on the S3; the amp is I2S_NUM_1 */
#define MIC_CLK_GPIO      17           /* PDM clock (lilygo_twatch_ultra pins_arduino.h MIC_SCK)    */
#define MIC_DAT_GPIO      18           /* PDM data  (... MIC_DAT)                                   */
#define MIC_SAMPLE_RATE   16000        /* 16 kHz mono — plenty for a level meter / voice memo       */
#define MIC_READ_FRAMES   256          /* samples per read (~16 ms at 16 kHz) -> responsive meter   */
#define MIC_READ_MS       100          /* i2s read timeout: bounds how fast a stop is noticed       */
/* RX DMA ring: the channel is created lazily on the first capture and freed on park (transient int-DMA
 * cost, only while a mic screen is active) — trimmed anyway so the transient claim stays small:
 * from the driver default 6 x 240 frames (2,880 B) to 4 x 256 frames x 2 B = 2,048 B (+ ~50 B of
 * descriptors). One i2s_channel_read of MIC_READ_FRAMES drains exactly one descriptor; 4 buffers =
 * 64 ms of slack for the worker to be late. 3 was tried (1,536 B) and DROPPED CHUNKS under LVGL repaints:
 * a dropped 256-sample chunk splices the pitch window and skews the estimate by the phase jump across the
 * splice (330 Hz read 336, 196 read 198.8; 440/880 stayed exact because 256 samples is ~a whole number of
 * their cycles). Every descriptor's buffer is its own heap block, so only 512 B of contiguity is ever
 * needed. Measured whole channel (ring + descriptors + channel/queue/semaphore objects + GDMA): 2,328 B. */
#define MIC_DMA_DESC_NUM  4
#define MIC_DMA_FRAME_NUM MIC_READ_FRAMES
/* The late reserve only claims the channel while the pool can afford it: the LoRa Signal-Alerts watch
 * (ui.c lora_alert_tick) refuses to arm the radio under `largest < 4096`, so the reserve must leave at
 * least that plus WiFi association (~0.5 KB) plus margin after its own ~2.3 KB. Measured 2026-09-24:
 * boot-end largest 6,656 on main -> a 6144 gate claimed and left only 256 B over the LoRa gate, so this
 * sits at 7168: today's main stays LAZY (the tuner opens the channel on first use and then holds it —
 * ~2.3 KB total, 512 B contiguous pieces, which fits even beside LoRa), and once the statics->PSRAM work
 * lifts the pool the reserve returns by itself. Below the gate a boot logs "mic reserve: SKIPPED". */
#define MIC_RESERVE_MIN_LARGEST  7168
/* Level mapping: pct = clamp( level * gain / 32768 * 100 ). A PDM MEMS mic outputs modest PCM for
 * speech; the gain lifts normal sound into a visible range while blowing pins the bar. The gain is now
 * a RUNTIME value (s_gain, adjustable from the Microphone screen + persisted by the UI) — the default
 * was raised from the E1·2-initial 12 because speech barely registered at that level. */
#define MIC_GAIN_DEFAULT  200.0f
#define MIC_GAIN_MIN      2.0f
#define MIC_GAIN_MAX      600.0f
/* Voice memo (E1·3): record 16 kHz mono 16-bit PCM into a PSRAM buffer,
 * capped at MIC_REC_MAX_SECS, then flush to a WAV on /sd. 30 s * 16000 *
 * 2 B is roughly 960 KB of PSRAM (plenty free). */
#define MIC_REC_MAX_SECS  NOCSIF_MIC_REC_MAX_SECS
#define MIC_REC_CAP_BYTES ((size_t)MIC_REC_MAX_SECS * MIC_SAMPLE_RATE * 2)
#define MIC_WAV_HDR_BYTES 44
/* The memo flush writes through an internal DMA buffer on the descriptor, sector-aligned, so FatFs hands
 * the card whole multi-sector writes. Through stdio a PSRAM source reaches the diskio one staged sector
 * at a time, and this card charges ~45 ms per command: a 30 s memo took 88.8 s to save (11 KB/s) with
 * the card lock held throughout (audio.c has the read-side twin of this, AUD_FILE_SECTOR). 8 KB is a
 * transient internal claim for the flush only. */
#define MIC_WAV_WRITE_BUF 8192
#define MIC_WAV_SECTOR    512
/* Record-side loudness (voice memos) — see rec_loudness. The raw PDM PCM of wrist-distance speech sits
 * at ~0.1-0.5 % of full scale (the level meter looks healthy only because it applies a x200 DISPLAY gain
 * that never touches the stored samples), so a memo needs 30-60 dB of gain to be heard on the small
 * speaker. A plain peak normalize (the E1·3 pass, kept below as the no-memory fallback) took that gain
 * from the single loudest sample, so the BODY of the speech stayed ~15 dB under the peaks: quiet. The
 * reverted RMS-target pass with a 64x cap and no gate turned the pauses of a quiet take into loud hiss.
 * rec_loudness does it the honest way, once over the whole take in PSRAM before the WAV flush:
 *   1. analysis, per 10 ms block: RMS and the peak envelope (instant attack, ~80 ms release). Percentiles
 *      give the loud-speech level (90th of the block RMS -> a static make-up gain lifts it to
 *      MIC_LOUD_SPEECH_FS, capped at MIC_LOUD_PRE_MAX) and the noise floor in the ENVELOPE domain (20th
 *      of the block envelope — the gate below compares like with like; an RMS noise figure against a
 *      peak envelope never closes, since the envelope of noise rides ~3x its RMS);
 *   2. per sample: the same envelope drives a NOISE GATE — opens fast above noise x OPEN, closes slowly
 *      once under noise x CLOSE after a hold, down to a -16 dB hush (dead silence between words sounds
 *      broken) — and a COMPRESSOR (MIC_LOUD_COMP_R:1 above MIC_LOUD_COMP_T, gain recomputed every 16
 *      samples) that flattens the peak-to-body spread;
 *   3. post make-up: the compressed peak is scanned and lifted to MIC_LOUD_POST_PEAK (capped).
 * The words come up together, the pauses stay hushed, and audio.c's peak limiter then only rides the
 * top of the loudest syllables on playback. Float on the mic worker at save time: ~0.1 s for a 30 s take.
 * On the gain cap: the meta-prompt for this pass asked for ~8-16x. The offline model of a wrist-level
 * take (speech RMS ~90 raw = -52 dBFS, the level the meter's x200 display gain was tuned for) shows that
 * cap leaving the words at -25 dBFS — quieter than the old pass, and inaudible on this driver. The words
 * need ~40 dB; what the low cap was meant to prevent (a blown-up noise floor in the pauses) is the gate's
 * job, so the cap is 64x with the gate carrying the pauses at -16 dB under the words. */
#define MIC_LOUD_DC_R        0.99f                   /* one-pole DC blocker, ~25 Hz: the PDM path leaves an
                                                        offset of several hundred raw units that WANDERS
                                                        (means of 400-1300 measured on 30 s takes, on-chip
                                                        35.5 Hz HPF notwithstanding); under every RMS and
                                                        envelope figure below it kept the gate shut 98 % of
                                                        a take whose words were 3x the floor            */
#define MIC_LOUD_BLK         160                     /* 10 ms analysis blocks at 16 kHz                    */
#define MIC_LOUD_MIN_SAMPLES (MIC_LOUD_BLK * 20)     /* under 200 ms there is nothing to analyse            */
#define MIC_LOUD_NOISE_PCT   20                      /* block percentile taken as the noise floor           */
#define MIC_LOUD_SPEECH_PCT  90                      /* block-RMS percentile taken as loud speech           */
#define MIC_LOUD_NOISE_MIN   8.0f                    /* floor for the noise-envelope estimate (raw units)   */
#define MIC_LOUD_SPEECH_FS   0.25f                   /* loud speech lands here (of FS) before compression   */
#define MIC_LOUD_PRE_MAX     64.0f                   /* static make-up cap (36 dB); the gate owns pauses    */
#define MIC_LOUD_REL_COEF    0.99922f                /* envelope release: exp(-1 / (0.08 s x 16 kHz))       */
#define MIC_LOUD_GATE_OPEN   1.8f                    /* gate opens above the noise envelope x this (2.2 kept a
                                                        quiet voice shut; 1.8 lifts it and a silent take still
                                                        stays 97 % shut in the model)                        */
#define MIC_LOUD_GATE_CLOSE  1.3f                    /* ... and starts closing under it x this              */
#define MIC_LOUD_GATE_HOLD   (MIC_SAMPLE_RATE / 10)  /* 100 ms hold before it closes (word gaps are 100-300) */
#define MIC_LOUD_GATE_FLOOR  0.14f                   /* closed gate = -17 dB                                */
#define MIC_LOUD_GATE_ATT    0.01f                   /* gate gain smoothing per sample: opening (~6 ms)     */
#define MIC_LOUD_GATE_REL    0.0012f                 /* ... closing (~50 ms)                                */
/* Leveler (the operator's memo-006: loud voice -9 dBFS, normal -27, quiet -36 in one take — a static gain
 * plus a peak compressor leaves the quiet passages 20 dB down). A slow mean-|x| envelope (50 ms attack,
 * 500 ms release) of the pre-gained signal drives a per-passage gain toward MIC_LOUD_LEV_TARGET, bounded
 * to MIC_LOUD_LEV_MAX either way; it only moves while the gate is open (speech present) and holds through
 * pauses, so the noise between words is not pumped up. The peak compressor + knee then sit on top. */
#define MIC_LOUD_LEV_TARGET  0.22f                   /* mean-|x| target (of FS) ~ -11 dBFS RMS speech: 0.12
                                                        leveled a take to -19 dBFS windows, under the loud
                                                        voice the operator could already hear; the peak
                                                        compressor + knee carry the crest above this        */
#define MIC_LOUD_LEV_MAX     8.0f                    /* leveler boost / cut bound (18 dB)                   */
#define MIC_LOUD_LEV_ATT     0.00125f                /* slow envelope attack per sample (~50 ms)            */
#define MIC_LOUD_LEV_REL     0.000125f               /* ... release (~500 ms)                               */
#define MIC_LOUD_COMP_T      0.35f                   /* compressor threshold on the peak envelope (of FS)   */
#define MIC_LOUD_COMP_R      4.0f                    /* ratio above the threshold                           */
#define MIC_LOUD_COMP_STEP   16                      /* gain recomputed every 16 samples (1 ms); power of 2 */
#define MIC_LOUD_KNEE        0.85f                   /* soft knee on the gain stage's output: a transient the 1 ms
                                                        compressor update missed is bent, not clipped         */
#define MIC_LOUD_POST_PEAK   0.85f                   /* final peak target (of FS)                           */
#define MIC_LOUD_POST_MAX    2.0f                    /* post make-up cap                                    */
#define MIC_LOUD_POST_PCT    995                     /* the post make-up targets the 99.5th percentile of the
                                                        10 ms block peaks, not the single loudest sample:
                                                        one click (a tap, the amp rail) otherwise pins the
                                                        whole take 6 dB down; what is above bends in the knee */
/* The E1·3 peak normalize, now only the fallback when the analysis scratch cannot be allocated. */
#define MIC_REC_NORM_TARGET 30000
#define MIC_REC_NORM_MAX    64.0f
/* Display smoothing: the shown level is an EMA of per-block RMS so the bar
 * glides; the peak-hold jumps up instantly and decays each block, for a
 * classic level-meter feel. */
#define MIC_LEVEL_EMA     0.35f        /* new-block weight (higher = snappier) */
#define MIC_PEAK_DECAY    6            /* pct subtracted from the held peak each block */
/* 6 KB: the level-meter loop is shallow, but the voice-memo flush
 * (rec_finalize -> fopen/fwrite -> FatFs -> SDSPI -> spi_master) is a deep
 * chain run on this same worker — the headroom avoids the 4 KB-worker
 * overflow class (same rationale as imu.c / the BLE-PCAP writer). */
#define MIC_TASK_STACK    6144
/* Prio 3 — the same tier as the audio worker and BELOW the LVGL port task (4). Prio 5 was tried to stop
 * LVGL repaints starving the worker: the YIN pass is heavy enough that at 5 it starved the audio worker
 * instead (choppy tones, sparse pitch). Dropped chunks are handled by the 4-descriptor ring plus the
 * overflow guard (a spliced window is discarded, never analysed), so the worker can stay polite. */
#define MIC_TASK_PRIO     3
/* Pitch detection (YIN) — grab-bag batch. A ~96 ms window (1536 samples @16 kHz) resolves down to
 * ~21 Hz; recompute on a 50%-overlap slide (~20 Hz update). Window + YIN scratch live in PSRAM,
 * allocated only while a tuner screen has pitch enabled. */
#define MIC_PITCH_BUF     1536
#define MIC_PITCH_HALF    (MIC_PITCH_BUF / 2)
#define MIC_PITCH_THRESH  0.15f         /* YIN absolute threshold                        */
#define MIC_PITCH_MINCLR  0.55f         /* below this confidence -> report "no pitch"    */
#define MIC_PITCH_MINHZ   40.0f
#define MIC_PITCH_MAXHZ   1500.0f

/* ---- module state --------------------------------------------------------- */
/* The value cache is shared between the worker (writer) and the LVGL-side
 * getters (readers); a short spinlock keeps the read/write coherent
 * without a heavier mutex (imu.c pattern). */
static portMUX_TYPE        s_lock = portMUX_INITIALIZER_UNLOCKED;
static nocsif_mic_state_t  s_state = NOCSIF_MIC_OFF;
static uint8_t             s_level;   /* smoothed 0..100 (spinlock) */
static uint8_t             s_peak;    /* decaying peak 0..100 (spinlock) */

static TaskHandle_t        s_task;
static volatile bool       s_active;                  /* desired capture state, set by the UI */
static float               s_gain = MIC_GAIN_DEFAULT; /* RMS->level gain (aligned 32-bit: torn-read-safe) */
/* The channel handle is created lazily by the worker on the first capture and DELETED on park
 * (mic_close), so an idle mic holds no internal-DMA — see nocsif_mic_boot_reserve for why the old
 * session-long reserve was dropped. Held only for the duration of an active mic screen. */
static i2s_chan_handle_t   s_rx;                      /* NULL until the channel claims its DMA     */
/* Touched only by the worker task. */
static bool                s_rx_enabled;              /* channel enabled = capturing               */
static int64_t             s_yin_max_us;              /* longest YIN pass this capture (telemetry) */
/* PCM read scratch — PSRAM (lazily allocated): i2s_channel_read memcpy's out of the DMA ring into it,
 * so it needs no DMA capability; keeping it out of internal SRAM saves 512 B of the scarce pool. */
static int16_t            *s_buf;
static volatile bool       s_stack_ext;               /* worker stack in PSRAM (set on 1st schedule) */
static float               s_level_f;                 /* EMA accumulator */

/* ---- voice memo recording (E1·3) ------------------------------------------ */
static nocsif_mic_rec_state_t s_rec_state;            /* spinlock */
static volatile nocsif_mic_rec_err_t s_rec_err;       /* why the last one failed (worker writes, UI reads an int) */
static uint32_t            s_rec_secs;                 /* spinlock */
static volatile bool       s_rec_start_req;            /* UI -> worker */
static volatile bool       s_rec_stop_req;             /* UI -> worker */
static char                s_rec_path[96];             /* set before start_req; worker-read */
/* Worker-only. */
static bool                s_recording;
static uint8_t            *s_rec_buf;                  /* PSRAM PCM buffer */
static size_t              s_rec_len;                  /* bytes captured */

/* ---- pitch detection (grab-bag batch) ------------------------------------- */
static volatile bool       s_pitch_on;                 /* UI -> worker: analyze pitch          */
static float               s_pitch_hz;                  /* published fundamental (spinlock)     */
static float               s_pitch_clar;                /* published clarity 0..1 (spinlock)    */
/* Worker-only. */
static int16_t            *s_pw;                        /* PSRAM window buffer (MIC_PITCH_BUF)  */
static float              *s_yin;                       /* PSRAM YIN scratch (MIC_PITCH_HALF)   */
static int                 s_pw_n;                      /* samples currently in the window      */
static uint32_t            s_pw_ovf_mark;               /* s_rx_ovf when this window started    */
/* RX ring overflows (the driver dropped a finished chunk because we were late): counted from the I2S
 * ISR. A pitch window that spans a drop is spliced — its estimate is garbage with decent clarity — so
 * the worker discards it instead of publishing (compare against s_pw_ovf_mark). Logged on park. */
static volatile uint32_t   s_rx_ovf;
static uint32_t            s_rx_ovf_logged;

static bool IRAM_ATTR mic_rx_ovf_cb(i2s_chan_handle_t handle, i2s_event_data_t *event, void *user_ctx)
{
    (void)handle; (void)event; (void)user_ctx;
    s_rx_ovf++;
    return false;                                       /* no task woken */
}

static void set_state(nocsif_mic_state_t s)
{
    taskENTER_CRITICAL(&s_lock);
    s_state = s;
    taskEXIT_CRITICAL(&s_lock);
}

/* ---- I2S PDM RX channel ------------------------------------------------------ */
/* Create + configure the PDM RX channel, left DISABLED (no PDM clock on the mic until a capture
 * enables it). Called from the worker on the first capture. Returns ESP_OK once s_rx holds its DMA; the
 * channel is released again on park (mic_close), so an idle mic gives its ~2.3 KB back to the pool. */
static esp_err_t mic_create_channel(void)
{
    if (s_rx != NULL) {
        return ESP_OK;
    }
    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(MIC_I2S_PORT, I2S_ROLE_MASTER);
    chan.dma_desc_num  = MIC_DMA_DESC_NUM;     /* lean ring — see the tunables above */
    chan.dma_frame_num = MIC_DMA_FRAME_NUM;
    i2s_chan_handle_t rx = NULL;
    esp_err_t err = i2s_new_channel(&chan, NULL, &rx);   /* RX handle is the 3rd arg */
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %s (int-dma free=%u largest=%u)", esp_err_to_name(err),
                 (unsigned)nocsif_int_dma_free(), (unsigned)nocsif_int_dma_largest());
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
    err = i2s_channel_init_pdm_rx_mode(rx, &pdm);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_pdm_rx_mode failed: %s", esp_err_to_name(err));
        i2s_del_channel(rx);
        return err;
    }
    i2s_event_callbacks_t cbs = { .on_recv_q_ovf = mic_rx_ovf_cb };   /* count dropped chunks */
    if (i2s_channel_register_event_callback(rx, &cbs, NULL) != ESP_OK) {
        ESP_LOGW(TAG, "overflow callback not registered (drops will go uncounted)");
    }
    s_rx = rx;
    ESP_LOGI(TAG, "PDM RX channel ready (CLK=%d DAT=%d, %d Hz mono LEFT, %d x %d-frame DMA) — MEMS mic",
             MIC_CLK_GPIO, MIC_DAT_GPIO, MIC_SAMPLE_RATE, MIC_DMA_DESC_NUM, MIC_DMA_FRAME_NUM);
    return ESP_OK;
}

/* Worker context: start capturing — enable the held channel (creating it first if the boot reserve
 * never ran / failed). Returns ESP_OK once PCM is flowing. */
static esp_err_t mic_open(void)
{
    if (s_rx_enabled) {
        return ESP_OK;
    }
    esp_err_t err = mic_create_channel();       /* no-op when boot-reserved */
    if (err != ESP_OK) {
        return err;
    }
    err = i2s_channel_enable(s_rx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_enable failed: %s", esp_err_to_name(err));
        return err;
    }
    s_rx_enabled = true;
    ESP_LOGI(TAG, "PDM RX capturing");
    return ESP_OK;
}

/* Worker context: stop capturing — disable the channel (PDM clock off) and DELETE it so its ~2.3 KB of
 * internal-DMA (ring + descriptors + GDMA/queue objects) goes back to the pool the radios contend for
 * while no mic screen is up. The mic is non-critical and reopens fine later — each descriptor is its own
 * 512 B block, so no 2.3 KB contiguous run is needed even under BLE+WiFi+LoRa fragmentation. Called on
 * park (nothing wants the mic), so within an active tuner/recorder session the channel stays put. */
static void mic_close(void)
{
    if (!s_rx_enabled) {
        return;
    }
    i2s_channel_disable(s_rx);
    s_rx_enabled = false;
    i2s_del_channel(s_rx);
    s_rx = NULL;
    ESP_LOGI(TAG, "PDM RX closed (channel released); longest YIN pass %lld us; int-dma free=%u largest=%u",
             (long long)s_yin_max_us, (unsigned)nocsif_int_dma_free(), (unsigned)nocsif_int_dma_largest());
    s_yin_max_us = 0;
}

/* Reduces one PCM block to a 0..100 RMS level (AC-coupled: RMS about the block mean) + a peak. */
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

/* Writes a canonical 16 kHz mono 16-bit PCM WAV (44-byte header + samples)
 * to `path` on /sd. Claims /sd away from USB-MSC + takes the FAT lock (the
 * PCAP-writer idiom). ESP32 is little-endian, so the multi-byte header
 * fields memcpy out in the WAV LE order directly. Returns true on success. */
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
    const int64_t t0 = esp_timer_get_time();
    bool ok = (fwrite(h, 1, sizeof h, f) == sizeof h) && (fflush(f) == 0);
    if (ok && data_len) {
        uint8_t *wb = heap_caps_malloc(MIC_WAV_WRITE_BUF, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
        if (wb != NULL) {
            const int fd = fileno(f);
            size_t off = 0;
            while (ok && off < data_len) {
                size_t n = data_len - off;
                const uint32_t mis = (uint32_t)(MIC_WAV_HDR_BYTES + off) % MIC_WAV_SECTOR;
                if (mis != 0 && n > MIC_WAV_SECTOR - mis) n = MIC_WAV_SECTOR - mis;   /* reach alignment first */
                if (n > MIC_WAV_WRITE_BUF) n = MIC_WAV_WRITE_BUF;
                memcpy(wb, pcm + off, n);
                ok = (write(fd, wb, n) == (ssize_t)n);
                off += n;
            }
            heap_caps_free(wb);
        } else {
            ESP_LOGW(TAG, "record: no internal write buffer — stdio path, expect a slow save");
            ok = (fwrite(pcm, 1, data_len, f) == data_len);
        }
    }
    fclose(f);
    nocsif_sdcard_unlock();
    if (!ok) {
        ESP_LOGE(TAG, "record: short write to %s", path);
        s_rec_err = NOCSIF_MIC_ERR_FILE_WRITE;
    } else {
        ESP_LOGI(TAG, "record: saved %s (%u bytes, %us) in %u ms", path, (unsigned)data_len,
                 (unsigned)(byte_rate ? data_len / byte_rate : 0), (unsigned)((esp_timer_get_time() - t0) / 1000));
    }
    return ok;
}

/* Fallback: normalize a recorded PCM buffer toward full-scale by its peak. Scans the peak, applies a
 * capped gain (never attenuates), clamps to int16. See MIC_REC_NORM_* above. */
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

static int cmp_float(const void *a, const void *b)
{
    const float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}

static inline int16_t clamp16(float v)
{
    int32_t s = (int32_t)lrintf(v);
    if (s > 32767)  s = 32767;
    if (s < -32768) s = -32768;
    return (int16_t)s;
}

/* Soft knee (the audio.c emit_mono curve): |v| above MIC_LOUD_KNEE of full scale maps (knee..inf) ->
 * (knee..1) on 1/(1+t), so an over-shoot bends instead of flat-topping. */
static inline int16_t knee16(float v)
{
    const float a = fabsf(v) * (1.0f / 32768.0f);
    if (a > MIC_LOUD_KNEE) {
        const float range = 1.0f - MIC_LOUD_KNEE;
        const float over  = a - MIC_LOUD_KNEE;
        const float c     = (MIC_LOUD_KNEE + range * (over / (over + range))) * 32767.0f;
        v = (v < 0.0f) ? -c : c;
    }
    return clamp16(v);
}

/* Record-side loudness: noise gate + compressor + make-up over the whole take (see MIC_LOUD_* above). */
static void rec_loudness(int16_t *pcm, size_t n)
{
    if (n < MIC_LOUD_MIN_SAMPLES) return;
    /* 0. DC blocker, in place (see MIC_LOUD_DC_R). */
    {
        float xp = 0.0f, yp = 0.0f;
        for (size_t i = 0; i < n; i++) {
            const float x = (float)pcm[i];
            const float y = x - xp + MIC_LOUD_DC_R * yp;
            xp = x; yp = y;
            pcm[i] = clamp16(y);
        }
    }
    /* 1. Analysis: per-block RMS + peak envelope, then the levels as percentiles. */
    const size_t nblk = n / MIC_LOUD_BLK;
    float *brms = heap_caps_malloc(2 * nblk * sizeof(float), MALLOC_CAP_SPIRAM);
    if (brms == NULL) {
        rec_normalize(pcm, n);
        return;
    }
    float *benv = brms + nblk;
    float  env  = 0.0f;
    for (size_t b = 0; b < nblk; b++) {
        const int16_t *p = pcm + b * MIC_LOUD_BLK;
        float acc = 0.0f;
        for (int i = 0; i < MIC_LOUD_BLK; i++) {
            const float x = (float)p[i], a = fabsf(x);
            acc += x * x;
            env = (a > env) ? a : env * MIC_LOUD_REL_COEF;
        }
        brms[b] = sqrtf(acc / (float)MIC_LOUD_BLK);
        benv[b] = env;
    }
    qsort(brms, nblk, sizeof(float), cmp_float);
    qsort(benv, nblk, sizeof(float), cmp_float);
    const float noise_rms = brms[(nblk - 1) * MIC_LOUD_NOISE_PCT  / 100];
    float       speech    = brms[(nblk - 1) * MIC_LOUD_SPEECH_PCT / 100];
    float       noise_env = benv[(nblk - 1) * MIC_LOUD_NOISE_PCT  / 100];
    float      *bpk       = brms;                    /* pass 2 reuses the scratch for the block peaks */
    if (noise_env < MIC_LOUD_NOISE_MIN)  noise_env = MIC_LOUD_NOISE_MIN;
    if (speech    < noise_rms * 1.5f)    speech    = noise_rms * 1.5f;   /* no real speech: the gate does the work */
    float pre = MIC_LOUD_SPEECH_FS * 32768.0f / speech;
    if (pre > MIC_LOUD_PRE_MAX) pre = MIC_LOUD_PRE_MAX;
    if (pre < 1.0f)             pre = 1.0f;

    /* 2. Gate + compressor, per sample, in place. */
    const float open_thr = noise_env * MIC_LOUD_GATE_OPEN, close_thr = noise_env * MIC_LOUD_GATE_CLOSE;
    float    gate = MIC_LOUD_GATE_FLOOR, gate_tgt = MIC_LOUD_GATE_FLOOR, comp = 1.0f;
    float    env_s = 0.0f, lev = 1.0f, lev_min = MIC_LOUD_LEV_MAX, lev_max = 1.0f / MIC_LOUD_LEV_MAX;
    uint32_t hold = 0, shut = 0, squeezed = 0, steps = 0;
    int32_t  peak = 1;
    env = 0.0f;
    for (size_t i = 0; i < n; i++) {
        const float x = (float)pcm[i];
        const float a = fabsf(x);
        env = (a > env) ? a : env * MIC_LOUD_REL_COEF;
        if (env > open_thr) {
            gate_tgt = 1.0f;
            hold = MIC_LOUD_GATE_HOLD;
        } else if (env < close_thr) {
            if (hold) hold--; else gate_tgt = MIC_LOUD_GATE_FLOOR;
        }
        gate += (gate_tgt - gate) * (gate_tgt > gate ? MIC_LOUD_GATE_ATT : MIC_LOUD_GATE_REL);
        env_s += (a - env_s) * (a > env_s ? MIC_LOUD_LEV_ATT : MIC_LOUD_LEV_REL);
        if ((i & (MIC_LOUD_COMP_STEP - 1)) == 0) {
            if (gate_tgt == 1.0f) {                          /* level only while speech is present */
                const float m = env_s * pre / 32768.0f;
                float g = (m > 1e-6f) ? MIC_LOUD_LEV_TARGET / m : MIC_LOUD_LEV_MAX;
                if (g > MIC_LOUD_LEV_MAX)        g = MIC_LOUD_LEV_MAX;
                if (g < 1.0f / MIC_LOUD_LEV_MAX) g = 1.0f / MIC_LOUD_LEV_MAX;
                lev = g;
                if (g < lev_min) lev_min = g;
                if (g > lev_max) lev_max = g;
            }
            const float e = env * pre * lev / 32768.0f;
            comp = (e > MIC_LOUD_COMP_T) ? powf(MIC_LOUD_COMP_T / e, 1.0f - 1.0f / MIC_LOUD_COMP_R) : 1.0f;
            steps++;
            if (comp < 1.0f)     squeezed++;
            if (gate_tgt < 1.0f) shut++;
        }
        const int16_t v = knee16(x * pre * gate * lev * comp);
        pcm[i] = v;
        const int32_t av = v < 0 ? -(int32_t)v : v;
        if (av > peak) peak = av;
        const size_t blk = i / MIC_LOUD_BLK;
        if (blk < nblk) {
            if ((i % MIC_LOUD_BLK) == 0) bpk[blk] = 0.0f;
            if ((float)av > bpk[blk]) bpk[blk] = (float)av;
        }
    }

    /* 3. Post make-up: the 99.5th-percentile block peak to MIC_LOUD_POST_PEAK (see MIC_LOUD_POST_PCT),
     * through the knee so the rare sample above it bends. */
    qsort(bpk, nblk, sizeof(float), cmp_float);
    float ref = bpk[(nblk - 1) * MIC_LOUD_POST_PCT / 1000];
    heap_caps_free(brms);
    if (ref < 1.0f) ref = (float)peak;
    float post = MIC_LOUD_POST_PEAK * 32767.0f / ref;
    if (post > MIC_LOUD_POST_MAX) post = MIC_LOUD_POST_MAX;
    if (post > 1.0f) {
        for (size_t i = 0; i < n; i++) pcm[i] = knee16((float)pcm[i] * post);
    } else {
        post = 1.0f;
    }
    if (steps == 0) steps = 1;
    ESP_LOGI(TAG, "record: loudness noise %u/%u speech %u (raw rms/env) -> pre %.1fx, leveler %.2f..%.2fx, gate shut %u%%, compressed %u%%, post %.2fx, peak %d",
             (unsigned)noise_rms, (unsigned)noise_env, (unsigned)speech, (double)pre, (double)lev_min, (double)lev_max,
             (unsigned)(shut * 100 / steps), (unsigned)(squeezed * 100 / steps), (double)post,
             (int)fminf((float)peak * post, 32767.0f));
}

/* Stop recording + flush the WAV (worker context). Frees the PSRAM buffer, publishes DONE/ERROR. */
static void rec_finalize(void)
{
    s_recording = false;
    taskENTER_CRITICAL(&s_lock);
    s_rec_state = NOCSIF_MIC_REC_SAVING;
    taskEXIT_CRITICAL(&s_lock);
    if (s_rec_buf != NULL && s_rec_len > 0) {
        rec_loudness((int16_t *)s_rec_buf, s_rec_len / sizeof(int16_t));   /* gate + compress + make-up */
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

/* YIN monophonic pitch estimate over `n` samples at `sr` Hz (uses the s_yin scratch, MIC_PITCH_HALF
 * floats). Returns the fundamental in Hz (0 if none) and *out_clar = 1 - d'(tau) (periodicity). The
 * three steps: squared-difference d(tau), cumulative-mean-normalized d'(tau), first-min-below-threshold
 * (else global min), then parabolic interpolation of tau. */
/* Raw YIN difference d(tau) = sum_i (x[i] - x[i+tau])^2 over the half window — the un-normalised curve,
 * recomputed at single lags for the unbiased parabolic vertex (see yin_pitch). ~768 MACs per call. */
static float yin_raw_d(const int16_t *buf, int half, int tau)
{
    float sum = 0.0f;
    for (int i = 0; i < half; i++) {
        float delta = (float)buf[i] - (float)buf[i + tau];
        sum += delta * delta;
    }
    return sum;
}

static float yin_pitch(const int16_t *buf, int n, int sr, float thresh, float *out_clar)
{
    int half = n / 2;
    float *d = s_yin;
    d[0] = 1.0f;
    float running = 0.0f;
    for (int tau = 1; tau < half; tau++) {
        float sum = 0.0f;
        for (int i = 0; i < half; i++) {
            float delta = (float)buf[i] - (float)buf[i + tau];
            sum += delta * delta;
        }
        running += sum;
        d[tau] = (running > 0.0f) ? (sum * (float)tau / running) : 1.0f;
    }
    int tau = 2;
    while (tau < half - 1 && d[tau] >= thresh) tau++;
    if (tau >= half - 1 || d[tau] >= thresh) {
        int best = 2;                                   /* nothing below threshold -> global min */
        for (int t = 3; t < half; t++) if (d[t] < d[best]) best = t;
        tau = best;
    } else {
        while (tau + 1 < half && d[tau + 1] < d[tau]) tau++;   /* descend to the local min */
    }
    if (out_clar) *out_clar = 1.0f - d[tau];
    /* Sub-sample interpolation on the RAW squared difference (recomputed at just the 3 lags needed), not
     * on d': the cumulative-mean normalisation tilts d' around a dip, and a parabola fitted to a tilted
     * dip lands a few tenths of a sample off — a steady 247 Hz tone read 246.0 (-7 c) that way. The raw
     * d(tau) is locally a clean parabola (∝ 1 - cos for a sinusoid), so its vertex is unbiased. */
    float better = (float)tau;
    if (tau > 1 && tau < half - 1) {
        float a = yin_raw_d(buf, half, tau - 1), b = yin_raw_d(buf, half, tau), c = yin_raw_d(buf, half, tau + 1);
        float denom = 2.0f * (2.0f * b - a - c);
        if (denom != 0.0f) better = (float)tau + (c - a) / denom;
    }
    if (better <= 0.0f) return 0.0f;
    /* Multi-period refinement (tuners batch). The parabolic estimate at the FIRST dip carries a few
     * hundredths of a sample of error, which at a short period is real money in cents: A4 is 36.4
     * samples @16 kHz (0.05 smp = 2.4 c), violin E5 24.3 (3.6 c). d'(tau) also dips at every multiple
     * k*tau of the period; the dip at the LARGEST k that fits in the window is a k-times finer period
     * estimate (its sub-sample error divides by k — A4 fits k = 21). Walk k down from the largest and
     * take the first dip that is a clean periodicity minimum (deep, and a true local min inside its
     * search band) and agrees with the first estimate; a decaying pluck / noisy tail fails the depth
     * test and we keep the plain estimate. Cost: a few hundred float compares — negligible. */
    if (tau >= 4) {
        int kmax = (half - 2) / tau;
        for (int k = kmax; k >= 2; k--) {
            int c    = (int)lroundf(better * (float)k);
            int band = tau / 4;
            int lo = c - band, hi = c + band;
            if (lo < 2) lo = 2;
            if (hi > half - 2) hi = half - 2;
            if (lo >= hi) continue;
            int m = c < lo ? lo : (c > hi ? hi : c);
            for (int t = lo; t <= hi; t++) if (d[t] < d[m]) m = t;
            if (m == lo || m == hi || d[m] > 0.5f) continue;    /* not a clean, interior dip */
            float a = yin_raw_d(buf, half, m - 1), b = yin_raw_d(buf, half, m), cc = yin_raw_d(buf, half, m + 1);
            float denom = 2.0f * (2.0f * b - a - cc);
            float fm = (float)m + ((denom != 0.0f) ? (cc - a) / denom : 0.0f);
            float cand = fm / (float)k;
            if (fabsf(cand - better) <= 0.5f) {                 /* sane: within half a sample of the first dip */
                better = cand;
                break;
            }
        }
    }
    return (float)sr / better;
}

static void mic_task(void *arg)
{
    (void)arg;
    volatile uint8_t probe;                          /* is this stack in PSRAM? */
    s_stack_ext = esp_ptr_external_ram((void *)&probe);
    while (s_buf == NULL) {                          /* the read scratch (PSRAM; internal as a last resort) */
        s_buf = heap_caps_malloc(MIC_READ_FRAMES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
        if (s_buf == NULL) s_buf = heap_caps_malloc(MIC_READ_FRAMES * sizeof(int16_t), MALLOC_CAP_DEFAULT);
        if (s_buf == NULL) {
            ESP_LOGE(TAG, "read scratch alloc failed — retrying");
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    for (;;) {
        /* Keep capturing while EITHER a screen wants the live meter (s_active) OR a recording is in
         * flight (s_recording / a pending start). This is what lets recording continue in the
         * BACKGROUND after the Voice Memos screen is closed — the memo is finalized only on an explicit
         * stop or the cap, never on set_active(false). */
        if (!s_active && !s_recording && !s_rec_start_req && !s_pitch_on) {
            /* Park: nothing wants the mic. Stop the channel (held, disabled), zero the meter, sleep
             * until notified. */
            mic_close();
            taskENTER_CRITICAL(&s_lock);
            s_level = 0;
            s_peak = 0;
            s_pitch_hz = 0.0f;
            s_pitch_clar = 0.0f;
            if (s_state != NOCSIF_MIC_FAILED) s_state = NOCSIF_MIC_OFF;
            taskEXIT_CRITICAL(&s_lock);
            s_level_f = 0.0f;
            if (s_pw)  { heap_caps_free(s_pw);  s_pw  = NULL; }   /* drop the pitch window/scratch */
            if (s_yin) { heap_caps_free(s_yin); s_yin = NULL; }
            s_pw_n = 0;
            if (s_rx_ovf != s_rx_ovf_logged) {                    /* honest telemetry: were we ever late? */
                ESP_LOGW(TAG, "RX ring overflows this session: %u (+%u since last park) — chunks dropped",
                         (unsigned)s_rx_ovf, (unsigned)(s_rx_ovf - s_rx_ovf_logged));
                s_rx_ovf_logged = s_rx_ovf;
            }
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        if (!s_rx_enabled) {
            if (mic_open() != ESP_OK) {
                set_state(NOCSIF_MIC_FAILED);
                /* Park instead of hammering a broken channel: drop EVERY wake reason. (The tuners' first
                 * cut cleared only s_active, so with a tuner open s_pitch_on kept this loop spinning on
                 * i2s_new_channel — CPU burn + log spam until the screen closed.) The next
                 * set_active / set_pitch / record_start retries the open once. */
                s_active = false;
                s_pitch_on = false;
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
        esp_err_t err = i2s_channel_read(s_rx, s_buf, MIC_READ_FRAMES * sizeof(int16_t), &got,
                                         pdMS_TO_TICKS(MIC_READ_MS));
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
        /* Pitch analysis (tuner screens): accumulate a window of the same PCM, run YIN on a full
         * window, then slide by half for the next estimate. Buffers are lazily allocated in PSRAM. */
        if (s_pitch_on) {
            if (!s_pw)  s_pw  = heap_caps_malloc(MIC_PITCH_BUF  * sizeof(int16_t), MALLOC_CAP_SPIRAM);
            if (!s_yin) s_yin = heap_caps_malloc(MIC_PITCH_HALF * sizeof(float),   MALLOC_CAP_SPIRAM);
            if (s_pw && s_yin) {
                size_t ns = got / sizeof(int16_t);
                if (s_pw_n == 0) s_pw_ovf_mark = s_rx_ovf;        /* a fresh window starts here */
                for (size_t i = 0; i < ns && s_pw_n < MIC_PITCH_BUF; i++) s_pw[s_pw_n++] = s_buf[i];
                if (s_pw_n >= MIC_PITCH_BUF) {
                    if (s_rx_ovf != s_pw_ovf_mark) {
                        /* The ring dropped a chunk somewhere inside this window: it is spliced, and a
                         * spliced sinusoid still yields a confident but WRONG period (the phase jump
                         * across the splice skews every dip). Throw the window away, start clean. */
                        s_pw_n = 0;
                        continue;
                    }
                    float clar = 0.0f;
                    int64_t t0 = esp_timer_get_time();
                    float hz = yin_pitch(s_pw, MIC_PITCH_BUF, MIC_SAMPLE_RATE, MIC_PITCH_THRESH, &clar);
                    int64_t dt = esp_timer_get_time() - t0;
                    if (dt > s_yin_max_us) s_yin_max_us = dt;
                    if (clar < MIC_PITCH_MINCLR || hz < MIC_PITCH_MINHZ || hz > MIC_PITCH_MAXHZ) hz = 0.0f;
                    taskENTER_CRITICAL(&s_lock);
                    s_pitch_hz = hz; s_pitch_clar = clar;
                    taskEXIT_CRITICAL(&s_lock);
                    memmove(s_pw, s_pw + MIC_PITCH_HALF, MIC_PITCH_HALF * sizeof(int16_t));
                    s_pw_n = MIC_PITCH_HALF;
                    s_pw_ovf_mark = s_rx_ovf;                      /* the retained half is clean */
                }
            }
        } else if (s_pw || s_yin) {          /* pitch turned off while the meter keeps capturing */
            if (s_pw)  { heap_caps_free(s_pw);  s_pw  = NULL; }
            if (s_yin) { heap_caps_free(s_yin); s_yin = NULL; }
            s_pw_n = 0;
            taskENTER_CRITICAL(&s_lock); s_pitch_hz = 0.0f; s_pitch_clar = 0.0f; taskEXIT_CRITICAL(&s_lock);
        }
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
            if (s_rec_len >= MIC_REC_CAP_BYTES) {   /* hit the cap -> auto-stop */
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
    /* Stack in PSRAM: the mic worker never DMAs from its own stack (I2S RX
     * lands in the static-internal s_buf; the record buffer is its own
     * PSRAM alloc) and never runs with the flash cache disabled (no
     * on-task NVS/flash writes), so its 6 KB no longer competes for the
     * scarce internal-DMA hole — a lazy first-use spawn under steady-state
     * fragmentation (largest ~3 KB) now succeeds. Never deleted -> no
     * vTaskDeleteWithCaps. */
    if (xTaskCreateWithCaps(mic_task, "nocsif_mic", MIC_TASK_STACK, NULL, MIC_TASK_PRIO, &s_task,
                            MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "worker task create failed");
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "mic worker ready (PDM RX opens lazily on first capture, released on park; idle now)");
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

esp_err_t nocsif_mic_boot_reserve(void)
{
    /* The mic is a NON-CRITICAL feature, so it must not hold internal-DMA while idle. The PDM RX channel
     * is created lazily on the first capture (mic_open) and released on park (mic_close) — its ~2.3 KB
     * goes back to the pool the radios contend for whenever no mic screen is up. It reopens fine even
     * under BLE + WiFi + LoRa fragmentation because each DMA descriptor is its own 512 B heap block (no
     * 2.3 KB contiguous claim is ever needed).
     *
     * A permanent boot reserve used to be GATED on `largest >= MIC_RESERVE_MIN_LARGEST` (7168) and, once
     * met, held the ring for the whole session. That gate was designed for a thin pool where it stayed
     * lazy; the cold-BSS -> PSRAM work (#235 / #238) then lifted the boot pool past 7168, so the reserve
     * began claiming and the ring sat resident with the mic OFF (status showed rx_ready while state=off)
     * — ~2.3 KB permanently denied to the radios for a feature that is idle almost always. Deliberately
     * disabled: keep the ring transient (see docs/RAM-BUDGET.md). No claim, no privacy surface at boot.
     * Reinstate only if a mic path is ever shown to fail to open post-fragmentation. */
    (void)MIC_RESERVE_MIN_LARGEST;   /* retained for context; the reserve is intentionally not taken */
    return ESP_OK;
}

bool nocsif_mic_rx_ready(void)
{
    return s_rx != NULL;            /* the honest gate: the channel actually holds its DMA */
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

/* ---- pitch detection (grab-bag batch) ------------------------------------- */
void nocsif_mic_set_pitch(bool on)
{
    if (s_task == NULL) return;
    s_pitch_on = on;
    if (on) xTaskNotifyGive(s_task);      /* wake the worker if parked (harmless if running) */
}

bool nocsif_mic_pitch(float *hz, float *clarity)
{
    taskENTER_CRITICAL(&s_lock);
    float h = s_pitch_hz, c = s_pitch_clar;
    taskEXIT_CRITICAL(&s_lock);
    if (h <= 0.0f) return false;
    if (hz)      *hz = h;
    if (clarity) *clarity = c;
    return true;
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
                                      * background, independent of the UI's set_active live meter */
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
