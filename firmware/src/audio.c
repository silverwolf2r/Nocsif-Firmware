/*
 * NocSif audio output worker (MAX98357A I2S speaker, M11 watch-core, slice E1·1). See audio.h.
 *
 * Design (same pattern as wifi.c / imu.c):
 *   - A worker task owns every blocking call — I2S writes and the BLDO2 amp rail. Callers (LVGL
 *     callbacks) only push a tone/cue command onto its queue, so LVGL never blocks on audio and
 *     only one task ever touches the I2S channel.
 *   - Bring-up is lazy: the I2S TX channel is created and configured the first time something
 *     plays (i2s_lazy_open), keeping boot fast. The amp rail (AXP2101 BLDO2) powers up only while
 *     something is playing and drops right after, keeping idle draw low.
 *
 * Signal path: tones are synthesized on the worker as 16-bit signed PCM sine waves, duplicated to
 * both I2S slots (L==R) so the amp produces sound no matter how its channel-select pin is wired. A
 * short linear fade in/out on each segment avoids an audible click at the edges. The amp is mono,
 * so 16 kHz is plenty for chimes and alerts.
 */
#include "audio.h"

#include <math.h>
#include <string.h>
#include <strings.h>        /* strcasecmp for sniffing the file extension */
#include <stdio.h>          /* fopen/fread for WAV/voice-memo playback    */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"   /* xTaskCreateWithCaps — places the worker stack in PSRAM */
#include "freertos/queue.h"
#include "esp_memory_utils.h"         /* esp_ptr_external_ram — checks the PSRAM-stack self-test */
#include "coex.h"                     /* nocsif_log_dma_free — logs DMA headroom at boot reserve */

#include "esp_log.h"
#include "esp_heap_caps.h"  /* heap_caps_malloc(MALLOC_CAP_SPIRAM) for playback buffers */
#include "driver/i2s_std.h"

/* minimp3 (components/minimp3, CC0): this is the one translation unit that includes the actual
 * decoder implementation. MP3-only (no SIMD on Xtensa) keeps the code footprint small. Decoder
 * state and buffers live on the PSRAM heap; its ~19 KB of decode scratch lives on this worker's
 * (also PSRAM) stack, sized below. */
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_SIMD
#include "minimp3.h"

#include "power.h"          /* nocsif_power_speaker_rail — the BLDO2 amp rail          */
#include "settings.h"       /* persists mute / volume / boot+usb sound flags           */
#include "sdcard.h"         /* nocsif_sdcard_lock/unlock — /sd access for WAV playback */
#include "usb_gadget.h"     /* nocsif_usb_gadget_claim_sd — keeps /sd away from USB-MSC while reading */

static const char *TAG = "audio";

/* ---- hardware / tunables ---------------------------------------------------------- */
#define AUD_I2S_PORT      I2S_NUM_1     /* the amp's port (I2S_NUM_0 is reserved for the PDM mic) */
#define AUD_BCLK_GPIO     9             /* MAX98357A BCLK  (per lilygo_twatch_ultra pins_arduino.h) */
#define AUD_WS_GPIO       10            /* MAX98357A WCLK / LRCK                                    */
#define AUD_DOUT_GPIO     11            /* MAX98357A DIN                                             */
#define AUD_SAMPLE_RATE   16000         /* mono amp; plenty for chimes / alert tones                */
#define AUD_RAIL_SETTLE_MS 8            /* delay to let BLDO2 settle before clocking the amp        */
#define AUD_MAX_MS        2000          /* per-segment duration cap (guards a misbehaving caller)   */
#define AUD_CMD_QLEN      6             /* queued cues/tones before the oldest is dropped           */
#define AUD_MAX_SEG       4             /* max tone segments in one cue, played back-to-back        */
#define AUD_CHUNK_FRAMES  256           /* synth/write chunk size (stereo int16 -> 1 KB scratch)    */
#define AUD_VOL_DEFAULT   255           /* master speaker volume 0..255 (full, matches shipped level) */
#define AUD_WAV_MAX_BYTES (2u*1024*1024)/* cap on a WAV read into PSRAM (memos stay under 1 MB)     */
#define AUD_PATH_MAX      80
/* Voice-memo playback make-up gain. mic.c peak-normalizes recordings to ~0.9 FS, but speech is
 * peaky so its perceived loudness (RMS) still comes out quiet, so memos play back too soft. This
 * applies an extra gain > 1 on the WAV path only (cues are already near full scale), then a
 * soft-knee limiter so a boosted peak compresses instead of wrapping into a crackle (a hard clamp
 * backstops it). AUD_WAV_MAKEUP sets the loudness boost; AUD_WAV_KNEE sets where the limiter
 * kicks in (as a fraction of full scale). */
#define AUD_WAV_MAKEUP    2.4f
#define AUD_WAV_KNEE      0.80f
/* Phase B file engine: the streamed read block size (a multiple of every WAV block_align, 1..8 B)
 * and the accepted sample-rate range (the I2S clock follows whatever the file specifies). */
#define AUD_FILE_BLOCK    16384
#define AUD_FILE_RATE_MIN 8000
#define AUD_FILE_RATE_MAX 48000
#define AUD_MEMO_MAKEUP_X100 240        /* nocsif_audio_play_wav's fixed voice-memo make-up gain (2.4x) */
#define AUD_MP3_INBUF     16384         /* MP3 input buffer (PSRAM); refilled below AUD_MP3_LOWWATER */
#define AUD_MP3_LOWWATER  4096          /* > the largest MP3 frame (1441 B) plus ID3 slack           */
#define AUD_TASK_STACK    28672         /* PSRAM stack: mp3dec_decode_frame keeps ~19 KB of scratch on it */

/* A queued command is either a tone cue (up to AUD_MAX_SEG segments played back-to-back with the
 * amp rail held up) or a request to stream a WAV/MP3 file from /sd. */
typedef enum { AUD_CMD_TONES = 0, AUD_CMD_WAV } audio_cmd_kind_t;
typedef struct {
    uint8_t  kind;                /* audio_cmd_kind_t                    */
    uint16_t freq[AUD_MAX_SEG];   /* Hz (0 = silence for ms[i])          */
    uint16_t ms[AUD_MAX_SEG];     /* segment duration                    */
    uint8_t  nseg;
    uint8_t  vol;                 /* 0..100                              */
    uint16_t makeup_x100;         /* AUD_CMD_WAV: extra gain in percent (100 = unity) */
    char     path[AUD_PATH_MAX];  /* AUD_CMD_WAV: the /sd path            */
} audio_cmd_t;

static TaskHandle_t        s_task;
static QueueHandle_t       s_q;
static i2s_chan_handle_t   s_tx;                     /* NULL until the first play (lazy open)      */
static volatile bool       s_tx_failed;              /* a prior open attempt failed; stay silently dead */
static volatile bool       s_muted;
static volatile uint8_t    s_vol = AUD_VOL_DEFAULT;  /* master speaker volume 0..255          */
static volatile bool       s_boot_snd  = true;       /* boot chime enabled                    */
static volatile bool       s_usb_snd   = true;       /* USB-plug cue enabled                  */
static volatile bool       s_shake_snd = true;       /* shake wake/sleep blips enabled        */
static volatile bool       s_playing;                /* a file is currently playing            */
static volatile bool       s_stack_ext;              /* worker task stack landed in PSRAM      */
static int16_t             s_chunk[AUD_CHUNK_FRAMES * 2];   /* stereo interleaved scratch (worker-only) */
/* Phase B file-engine state. s_stop_req can be set from any task (an explicit Stop, or a new play
 * request pre-empting the current file) and is checked by the worker at the top of every chunk.
 * The playing path is kept as a spinlock-guarded copy for the UI's "now playing" line. s_cur_rate
 * tracks the I2S clock's current rate (worker-only). */
static volatile bool       s_stop_req;
static uint32_t            s_cur_rate;
static char                s_play_path[AUD_PATH_MAX];
static portMUX_TYPE        s_path_lock = portMUX_INITIALIZER_UNLOCKED;

static void set_play_path(const char *path)
{
    taskENTER_CRITICAL(&s_path_lock);
    strncpy(s_play_path, path ? path : "", sizeof s_play_path - 1);
    s_play_path[sizeof s_play_path - 1] = '\0';
    taskEXIT_CRITICAL(&s_path_lock);
}

/* ---- cue table -------------------------------------------------------------------- *
 * Frequencies are round musical-ish pitches; durations are kept short so a cue never feels laggy. */
static const audio_cmd_t s_cues[] = {
    [NOCSIF_AUDIO_BOOT]  = { .nseg = 2, .vol = 70, .freq = { 880, 1319 }, .ms = { 110, 150 } },  /* A5 -> E6 rising */
    [NOCSIF_AUDIO_WAKE]  = { .nseg = 1, .vol = 60, .freq = { 1319 },      .ms = { 70 } },        /* short high blip */
    [NOCSIF_AUDIO_SLEEP] = { .nseg = 2, .vol = 55, .freq = { 880, 587 },  .ms = { 70, 100 } },   /* falling         */
    [NOCSIF_AUDIO_TICK]  = { .nseg = 1, .vol = 45, .freq = { 1047 },      .ms = { 28 } },        /* brief tick      */
    [NOCSIF_AUDIO_ALERT] = { .nseg = 3, .vol = 85, .freq = { 1319, 988, 1319 }, .ms = { 120, 110, 150 } }, /* attention */
    [NOCSIF_AUDIO_USB]   = { .nseg = 2, .vol = 60, .freq = { 1047, 1568 }, .ms = { 80, 110 } },  /* USB connected: C6 -> G6 */
};

/* Creates and configures the standard-mode TX channel the first time it's needed, leaving it
 * disabled (playback enables it per command). Returns ESP_OK once s_tx is valid. */
static esp_err_t i2s_lazy_open(void)
{
    if (s_tx != NULL) {
        return ESP_OK;
    }
    if (s_tx_failed) {
        /* An earlier attempt (the boot reserve, or a prior lazy open) couldn't claim the ~5.7 KB of
         * internal-DMA descriptors, and runtime teardown never gives that memory back defragmented,
         * so retrying would just re-log a failure on every beep — return the cached failure quietly. */
        return ESP_ERR_NO_MEM;
    }
    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(AUD_I2S_PORT, I2S_ROLE_MASTER);
    chan.auto_clear = true;                 /* emit silence on underrun instead of stale audio */
    /* Shrinks the TX DMA reserve from the default 6 descriptors to 4 (freeing internal-DMA
     * headroom for the IMU). At 6 descriptors x 240 frames x 2 ch x 2 B = 5760 B pinned, the
     * steady-state largest free block dropped to ~1664 B, which occasionally starved the BHI260
     * FIFO's I2C read. 4 descriptors x 240 frames still gives 60 ms of stereo buffering at
     * AUD_SAMPLE_RATE — plenty for short cues and adequate for streamed playback (an underrun just
     * emits brief silence) — and returns ~1.9 KB to the pool. */
    chan.dma_desc_num = 4;
    esp_err_t err = i2s_new_channel(&chan, &s_tx, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(err));
        s_tx = NULL;
        s_tx_failed = true;
        return err;
    }
    i2s_std_config_t std = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(AUD_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = AUD_BCLK_GPIO,
            .ws   = AUD_WS_GPIO,
            .dout = AUD_DOUT_GPIO,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = { 0 },
        },
    };
    err = i2s_channel_init_std_mode(s_tx, &std);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode failed: %s", esp_err_to_name(err));
        i2s_del_channel(s_tx);
        s_tx = NULL;
        s_tx_failed = true;
        return err;
    }
    s_cur_rate = AUD_SAMPLE_RATE;
    ESP_LOGI(TAG, "I2S TX ready (BCLK=%d WS=%d DOUT=%d, %d Hz) — MAX98357A", AUD_BCLK_GPIO,
             AUD_WS_GPIO, AUD_DOUT_GPIO, AUD_SAMPLE_RATE);
    return ESP_OK;
}

/* Reconfigures the I2S clock to a new sample rate (files set their own rate; tones/cues restore
 * AUD_SAMPLE_RATE the same way). The channel must already be disabled, which it always is between
 * plays. DMA descriptors are rate-independent, so this touches no internal memory. */
static void ensure_rate(uint32_t rate)
{
    if (s_tx == NULL || rate == s_cur_rate) {
        return;
    }
    i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(rate);
    esp_err_t e = i2s_channel_reconfig_std_clock(s_tx, &clk);
    if (e == ESP_OK) {
        s_cur_rate = rate;
    } else {
        ESP_LOGW(TAG, "i2s clock %u Hz -> %s (keeping %u Hz)", (unsigned)rate, esp_err_to_name(e), (unsigned)s_cur_rate);
    }
}

/* Synthesizes and streams one sine-wave segment to the already-enabled channel. A short linear
 * fade at the start/end masks the click at the tone edges. freq 0 produces silence for ms
 * milliseconds. */
static void play_segment(uint16_t freq, uint16_t ms, uint8_t vol)
{
    if (ms == 0) {
        return;
    }
    if (ms > AUD_MAX_MS) {
        ms = AUD_MAX_MS;
    }
    uint32_t total   = (uint32_t)AUD_SAMPLE_RATE * ms / 1000;   /* total frames to emit */
    uint32_t fade    = AUD_SAMPLE_RATE * 4 / 1000;              /* ~4 ms fade ramp      */
    if (fade > total / 2) {
        fade = total / 2;
    }
    float    amp     = (vol > 100 ? 100 : vol) / 100.0f * 0.9f * 32767.0f;  /* leaves headroom below full scale */
    double   phase   = 0.0;
    double   step    = 2.0 * M_PI * (double)freq / (double)AUD_SAMPLE_RATE;

    uint32_t done = 0;
    while (done < total) {
        uint32_t n = total - done;
        if (n > AUD_CHUNK_FRAMES) {
            n = AUD_CHUNK_FRAMES;
        }
        for (uint32_t i = 0; i < n; i++) {
            uint32_t g = done + i;                              /* index within the whole segment */
            float env = 1.0f;
            if (g < fade) {
                env = (float)g / (float)fade;                   /* fading in  */
            } else if (g >= total - fade) {
                env = (float)(total - g) / (float)fade;         /* fading out */
            }
            int16_t s = (freq == 0) ? 0 : (int16_t)(sin(phase) * amp * env);
            phase += step;
            if (phase >= 2.0 * M_PI) {
                phase -= 2.0 * M_PI;
            }
            s_chunk[i * 2]     = s;                              /* left channel  */
            s_chunk[i * 2 + 1] = s;                              /* right channel (duplicated)  */
        }
        size_t wrote = 0;
        i2s_channel_write(s_tx, s_chunk, (size_t)n * 2 * sizeof(int16_t), &wrote, portMAX_DELAY);
        done += n;
    }
}

/* Plays one queued tone command: powers the amp, clocks out each segment, then drops the amp. */
static void do_play(const audio_cmd_t *c)
{
    if (i2s_lazy_open() != ESP_OK) {
        return;
    }
    ensure_rate(AUD_SAMPLE_RATE);           /* a prior file may have left the clock at another rate */
    nocsif_power_speaker_rail(true);
    vTaskDelay(pdMS_TO_TICKS(AUD_RAIL_SETTLE_MS));
    if (i2s_channel_enable(s_tx) != ESP_OK) {
        nocsif_power_speaker_rail(false);
        return;
    }
    for (uint8_t i = 0; i < c->nseg && i < AUD_MAX_SEG; i++) {
        play_segment(c->freq[i], c->ms[i], c->vol);
    }
    /* A brief silence tail flushes the last DMA frames before the clock/rail cut, avoiding a
     * clipped final sample. */
    play_segment(0, 6, 0);
    i2s_channel_disable(s_tx);
    nocsif_power_speaker_rail(false);
}

/* ---- Phase B file engine: any PCM WAV, streamed ------------------------------------ *
 * Replaces the older E1·3 player (16 kHz mono 16-bit only, whole clip buffered in PSRAM, capped at
 * 2 MB): walks the RIFF chunks for "fmt " and "data" (skipping LIST/fact/etc, unwrapping
 * WAVE_FORMAT_EXTENSIBLE), converts using the file's actual rate/bits/channels, follows the file's
 * rate with the I2S clock, and streams samples in AUD_FILE_BLOCK reads under short /sd locks — no
 * whole-file buffer, no length limit. Each frame is downmixed to mono float, scaled by the master
 * volume and make-up gain, soft-knee limited, and written to both I2S slots. s_stop_req interrupts
 * a file within one chunk. */
typedef struct {
    uint16_t fmt;        /* 1 = PCM integer, 3 = IEEE float (32-bit only) */
    uint16_t ch;         /* 1 | 2 */
    uint32_t rate;
    uint16_t bits;       /* 8 | 16 | 24 | 32 */
    uint16_t block;      /* bytes per frame, all channels */
    long     data_off;   /* file offset of the sample data */
    uint32_t data_len;   /* bytes of sample data (0 = to EOF) */
} wav_info_t;

static inline uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static inline uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

/* Walks the RIFF chunks (caller already holds the /sd lock), leaving the stream positioned at the
 * sample data on success. */
static bool wav_parse(FILE *f, wav_info_t *w)
{
    uint8_t hdr[12];
    if (fread(hdr, 1, 12, f) != 12 || memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) {
        return false;
    }
    memset(w, 0, sizeof *w);
    bool have_fmt = false;
    for (int guard = 0; guard < 32; guard++) {          /* bounded so a malformed file can't loop us forever */
        uint8_t ch[8];
        if (fread(ch, 1, 8, f) != 8) break;
        const uint32_t sz = le32(ch + 4);
        if (memcmp(ch, "fmt ", 4) == 0) {
            uint8_t fm[40];
            const size_t take = sz < sizeof fm ? sz : sizeof fm;
            if (take < 16 || fread(fm, 1, take, f) != take) return false;
            if (sz > take) fseek(f, (long)(sz - take), SEEK_CUR);
            w->fmt   = le16(fm);
            w->ch    = le16(fm + 2);
            w->rate  = le32(fm + 4);
            w->block = le16(fm + 12);
            w->bits  = le16(fm + 14);
            if (w->fmt == 0xFFFE && take >= 26) w->fmt = le16(fm + 24);   /* WAVE_FORMAT_EXTENSIBLE: real sub-format tag */
            have_fmt = true;
        } else if (memcmp(ch, "data", 4) == 0) {
            w->data_off = ftell(f);
            w->data_len = sz;
            break;                                        /* "fmt " always precedes "data" in a valid file */
        } else {
            fseek(f, (long)sz, SEEK_CUR);                 /* skip LIST / fact / cue / etc. */
        }
        if (sz & 1) fseek(f, 1, SEEK_CUR);                /* chunks are word-padded */
    }
    if (!have_fmt || w->data_off == 0) return false;
    if (!(w->fmt == 1 || (w->fmt == 3 && w->bits == 32))) return false;
    if (!(w->ch == 1 || w->ch == 2)) return false;
    if (!(w->bits == 8 || w->bits == 16 || w->bits == 24 || w->bits == 32)) return false;
    if (w->rate < AUD_FILE_RATE_MIN || w->rate > AUD_FILE_RATE_MAX) return false;
    const uint16_t frame = (uint16_t)(w->ch * (w->bits / 8));
    if (w->block == 0) w->block = frame;
    return w->block == frame;
}

/* Converts one sample frame to mono float in [-1, 1). */
static inline float wav_frame_mono(const uint8_t *p, const wav_info_t *w)
{
    float acc = 0.0f;
    for (int c = 0; c < w->ch; c++, p += w->bits / 8) {
        float s;
        switch (w->bits) {
        case 8:  s = ((int)p[0] - 128) / 128.0f; break;
        case 16: s = (int16_t)le16(p) / 32768.0f; break;
        case 24: {
            int32_t v = (int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24) >> 8;
            s = v / 8388608.0f;
            break;
        }
        default:
            if (w->fmt == 3) { float fv; memcpy(&fv, p, 4); s = fv; }
            else            { s = (int32_t)le32(p) / 2147483648.0f; }
            break;
        }
        acc += s;
    }
    return (w->ch == 2) ? acc * 0.5f : acc;
}

/* ---- shared sample emitter: mono float in [-1,1] -> gain -> soft-knee limiter -> both I2S slots ---- *
 * Buffers into s_chunk and writes it to I2S every AUD_CHUNK_FRAMES frames (worker-only). */
static size_t   s_emit_n;
static float    s_emit_gain;
static uint32_t s_emit_limited;

static void emit_flush(void)
{
    if (s_emit_n > 0) {
        size_t wrote = 0;
        i2s_channel_write(s_tx, s_chunk, s_emit_n * 2 * sizeof(int16_t), &wrote, portMAX_DELAY);
        s_emit_n = 0;
    }
}

static inline void emit_mono(float x)
{
    x *= s_emit_gain;
    float ax = fabsf(x);
    if (ax > AUD_WAV_KNEE) {                                /* soft knee: compresses (knee..inf) into (knee..1) */
        s_emit_limited++;
        const float range = 1.0f - AUD_WAV_KNEE;
        float over = ax - AUD_WAV_KNEE;
        float comp = AUD_WAV_KNEE + range * (over / (over + range));
        x = (x < 0.0f) ? -comp : comp;
    }
    int32_t s = (int32_t)(x * 32767.0f);
    if (s > 32767)  s = 32767;
    if (s < -32768) s = -32768;
    s_chunk[s_emit_n * 2]     = (int16_t)s;                 /* left channel  */
    s_chunk[s_emit_n * 2 + 1] = (int16_t)s;                 /* right channel (duplicated) */
    if (++s_emit_n == AUD_CHUNK_FRAMES) {
        emit_flush();
    }
}

static bool has_ext(const char *path, const char *ext)
{
    const char *dot = strrchr(path, '.');
    return dot != NULL && strcasecmp(dot, ext) == 0;
}

/* ---- MP3 (Phase B, minimp3): streamed decode -------------------------------------------------- *
 * A 16 KB PSRAM input window is refilled from the card under short /sd locks whenever fewer than
 * 4 KB remain (a frame never exceeds 1441 B, and minimp3 only accepts a frame once the next header
 * is visible too); each decoded frame's PCM (up to 1152 samples x 2 ch, PSRAM) is downmixed through
 * emit_mono. Tags are trimmed before decoding starts: a leading ID3v2 block (which can carry 100s
 * of KB of album art the decoder would otherwise scan through for false frame syncs) is skipped by
 * its header length, and trailing ID3v1/APE tags are cut off the end, because minimp3 validates a
 * frame by chaining up to 10 following headers and a trailer would otherwise reject the last ~10
 * frames. Junk bytes between frames return 0 samples with frame_bytes > 0, meaning "skip and
 * continue". The I2S clock follows the first decoded frame's rate and is re-set if the stream's
 * rate ever changes. */
static void do_play_mp3(const char *path, uint16_t makeup_x100)
{
    esp_err_t ce = nocsif_usb_gadget_claim_sd(2000);
    if (ce == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "play: File Share owns the card — cannot play");
        return;
    }
    if (!nocsif_sdcard_lock(3000)) {
        ESP_LOGW(TAG, "play: /sd lock timeout");
        return;
    }
    FILE *f = fopen(path, "rb");
    long  start = 0, limit = 0;
    if (f != NULL) {
        uint8_t t[32];
        if (fseek(f, 0, SEEK_END) == 0) limit = ftell(f);
        if (limit >= 128 && fseek(f, limit - 128, SEEK_SET) == 0 && fread(t, 1, 3, f) == 3 && memcmp(t, "TAG", 3) == 0) {
            limit -= 128;                                                       /* trailing ID3v1 tag */
        }
        if (limit >= 32 && fseek(f, limit - 32, SEEK_SET) == 0 && fread(t, 1, 32, f) == 32 && memcmp(t, "APETAGEX", 8) == 0) {
            long ts = (long)((uint32_t)t[12] | ((uint32_t)t[13] << 8) | ((uint32_t)t[14] << 16) | ((uint32_t)t[15] << 24));
            limit -= 32;                                                        /* APEv2 footer plus its items */
            if (limit >= ts) limit -= ts;
        }
        if (fseek(f, 0, SEEK_SET) == 0 && fread(t, 1, 10, f) == 10 && memcmp(t, "ID3", 3) == 0 &&
            !((t[5] & 15) || ((t[6] | t[7] | t[8] | t[9]) & 0x80))) {
            start = (long)(((t[6] & 0x7f) << 21) | ((t[7] & 0x7f) << 14) | ((t[8] & 0x7f) << 7) | (t[9] & 0x7f)) + 10;
            if (t[5] & 16) start += 10;                                         /* ID3v2 footer present too */
        }
        if (start > limit) start = limit;
        fseek(f, start, SEEK_SET);
    }
    nocsif_sdcard_unlock();
    if (f == NULL) {
        ESP_LOGW(TAG, "play: fopen(%s) failed", path);
        return;
    }
    if (i2s_lazy_open() != ESP_OK) {
        fclose(f);
        return;
    }
    mp3dec_t *dec = heap_caps_malloc(sizeof *dec, MALLOC_CAP_SPIRAM);
    uint8_t  *in  = heap_caps_malloc(AUD_MP3_INBUF, MALLOC_CAP_SPIRAM);
    int16_t  *pcm = heap_caps_malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (dec == NULL || in == NULL || pcm == NULL) {
        ESP_LOGE(TAG, "play: PSRAM alloc failed (mp3)");
        heap_caps_free(dec); heap_caps_free(in); heap_caps_free(pcm);
        fclose(f);
        return;
    }
    mp3dec_init(dec);
    s_emit_gain    = ((float)makeup_x100 / 100.0f) * ((float)s_vol / 255.0f);
    s_emit_n       = 0;
    s_emit_limited = 0;

    size_t   have = 0, pos = 0;
    size_t   remain = (size_t)(limit - start);             /* remaining audio bytes to read */
    bool     eof = (remain == 0), started = false, stopped = false;
    uint32_t frames = 0, rate = 0;
    int      channels = 0;
    while (!stopped) {
        /* Refill: shift the unconsumed tail to the front, then top up from the card (short lock). */
        if (!eof && have - pos < AUD_MP3_LOWWATER) {
            if (pos > 0) { memmove(in, in + pos, have - pos); have -= pos; pos = 0; }
            size_t ask = AUD_MP3_INBUF - have;
            if (ask > remain) ask = remain;
            if (!nocsif_sdcard_lock(3000)) break;
            size_t rd = fread(in + have, 1, ask, f);
            nocsif_sdcard_unlock();
            have   += rd;
            remain -= rd;
            if (rd == 0 || remain == 0) eof = true;
        }
        if (have - pos == 0) break;                        /* nothing left to decode */
        mp3dec_frame_info_t info;
        int n = mp3dec_decode_frame(dec, in + pos, (int)(have - pos), pcm, &info);
        if (info.frame_bytes == 0) {                       /* (defensive: reports the span the decoder scanned) */
            if (eof) break;
            pos = have;                                    /* force a refill */
            continue;
        }
        pos += (size_t)info.frame_bytes;
        if (n <= 0) continue;                              /* junk skipped, or the decoder resynced */
        channels = info.channels;
        if (!started || (uint32_t)info.hz != rate) {
            if (started) { emit_flush(); i2s_channel_disable(s_tx); }
            rate = (uint32_t)info.hz;
            if (rate < AUD_FILE_RATE_MIN || rate > AUD_FILE_RATE_MAX) {
                ESP_LOGW(TAG, "play: mp3 rate %u Hz unsupported", (unsigned)rate);
                break;
            }
            ensure_rate(rate);
            if (!started) {
                nocsif_power_speaker_rail(true);
                vTaskDelay(pdMS_TO_TICKS(AUD_RAIL_SETTLE_MS));
            }
            if (i2s_channel_enable(s_tx) != ESP_OK) break;
            if (!started) { set_play_path(path); s_playing = true; started = true; }
        }
        if (channels == 2) {
            for (int i = 0; i < n; i++) emit_mono(((float)pcm[2 * i] + (float)pcm[2 * i + 1]) * (0.5f / 32768.0f));
        } else {
            for (int i = 0; i < n; i++) emit_mono((float)pcm[i] / 32768.0f);
        }
        frames += (uint32_t)n;
        if (s_stop_req) stopped = true;
    }
    if (started) {
        emit_flush();
        play_segment(0, 6, 0);                             /* silence tail flushes the last DMA frames */
        i2s_channel_disable(s_tx);
        nocsif_power_speaker_rail(false);
    }
    s_playing = false;
    set_play_path("");
    heap_caps_free(dec); heap_caps_free(in); heap_caps_free(pcm);
    fclose(f);
    if (started) {
        ESP_LOGI(TAG, "play: %s %s (mp3 %u Hz %s, %u frames, gain %.2fx, %u limited)",
                 stopped ? "stopped" : "done", path, (unsigned)rate, channels == 2 ? "stereo" : "mono",
                 (unsigned)frames, (double)s_emit_gain, (unsigned)s_emit_limited);
    } else {
        ESP_LOGW(TAG, "play: %s — no decodable MP3 frames", path);
    }
}

static void do_play_wav_stream(const char *path, uint16_t makeup_x100);

static void do_play_file(const char *path, uint16_t makeup_x100)
{
    s_stop_req = false;                                    /* a stale Stop must not cancel THIS new play */
    if (has_ext(path, ".mp3")) {
        do_play_mp3(path, makeup_x100);
    } else {
        do_play_wav_stream(path, makeup_x100);
    }
}

static void do_play_wav_stream(const char *path, uint16_t makeup_x100)
{
    esp_err_t ce = nocsif_usb_gadget_claim_sd(2000);
    if (ce == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "play: File Share owns the card — cannot play");
        return;
    }
    if (!nocsif_sdcard_lock(3000)) {
        ESP_LOGW(TAG, "play: /sd lock timeout");
        return;
    }
    FILE *f = fopen(path, "rb");
    wav_info_t w;
    const bool ok = (f != NULL) && wav_parse(f, &w);
    nocsif_sdcard_unlock();                                /* the file handle stays valid; the lock just guards the bus */
    if (!ok) {
        ESP_LOGW(TAG, "play: %s — %s", path,
                 f ? "not a playable WAV (PCM 8/16/24/32-bit or float, mono/stereo, 8-48 kHz)" : "fopen failed");
        if (f) fclose(f);
        return;
    }
    if (i2s_lazy_open() != ESP_OK) {
        fclose(f);
        return;
    }
    uint8_t *blk = heap_caps_malloc(AUD_FILE_BLOCK, MALLOC_CAP_SPIRAM);
    if (blk == NULL) {
        ESP_LOGE(TAG, "play: PSRAM block alloc failed");
        fclose(f);
        return;
    }
    ensure_rate(w.rate);
    nocsif_power_speaker_rail(true);
    vTaskDelay(pdMS_TO_TICKS(AUD_RAIL_SETTLE_MS));
    if (i2s_channel_enable(s_tx) != ESP_OK) {
        nocsif_power_speaker_rail(false);
        heap_caps_free(blk);
        fclose(f);
        return;
    }
    set_play_path(path);
    s_playing = true;

    /* Master volume (attenuation) times make-up gain, as normalized [-1,1] float; emit_mono applies
     * the soft-knee limiter and the final saturating clamp. */
    s_emit_gain    = ((float)makeup_x100 / 100.0f) * ((float)s_vol / 255.0f);
    s_emit_n       = 0;
    s_emit_limited = 0;
    const size_t want = (AUD_FILE_BLOCK / w.block) * w.block;    /* round down to whole frames */
    uint32_t left = w.data_len ? w.data_len : 0xFFFFFFFFu;
    uint32_t frames = 0;
    bool stopped = false;
    while (left > 0 && !stopped) {
        size_t ask = (want < left) ? want : (size_t)left;
        ask -= ask % w.block;
        if (ask == 0) break;
        if (!nocsif_sdcard_lock(3000)) break;
        size_t rd = fread(blk, 1, ask, f);                 /* PSRAM target: sdspi bounces via its own block buffer */
        nocsif_sdcard_unlock();
        if (rd < w.block) break;                           /* short read: end of file */
        rd  -= rd % w.block;
        left -= (uint32_t)rd;
        for (size_t off = 0; off < rd; off += w.block) {
            emit_mono(wav_frame_mono(blk + off, &w));
            if ((++frames % AUD_CHUNK_FRAMES) == 0 && s_stop_req) { stopped = true; break; }
        }
    }
    emit_flush();
    play_segment(0, 6, 0);                                 /* silence tail flushes the last DMA frames */
    i2s_channel_disable(s_tx);
    nocsif_power_speaker_rail(false);
    s_playing = false;
    set_play_path("");
    heap_caps_free(blk);
    fclose(f);
    ESP_LOGI(TAG, "play: %s %s (%u Hz %u-bit %s, %u frames, gain %.2fx, %u limited)",
             stopped ? "stopped" : "done", path, (unsigned)w.rate, (unsigned)w.bits,
             w.ch == 2 ? "stereo" : "mono", (unsigned)frames, (double)s_emit_gain, (unsigned)s_emit_limited);
}

static void audio_task(void *arg)
{
    (void)arg;
    volatile uint8_t probe;                          /* on-stack byte, used to check where the stack lives */
    s_stack_ext = esp_ptr_external_ram((void *)&probe);
    audio_cmd_t c;
    for (;;) {
        if (xQueueReceive(s_q, &c, portMAX_DELAY) == pdTRUE) {
            if (c.kind == AUD_CMD_WAV) {
                do_play_file(c.path, c.makeup_x100);
            } else {
                do_play(&c);
            }
        }
    }
}

/* ---- public API ------------------------------------------------------------------- */
esp_err_t nocsif_audio_init(void)
{
    if (s_task != NULL) {
        return ESP_OK;              /* already initialized */
    }
    s_muted = nocsif_settings_get_i32("sound_en", 1) == 0;
    int32_t v = nocsif_settings_get_i32("spk_vol", AUD_VOL_DEFAULT);
    if (v < 0)   v = 0;
    if (v > 255) v = 255;
    s_vol      = (uint8_t)v;
    s_boot_snd  = nocsif_settings_get_i32("boot_snd", 1) != 0;
    s_usb_snd   = nocsif_settings_get_i32("usb_snd", 1) != 0;
    s_shake_snd = nocsif_settings_get_i32("shake_snd", 1) != 0;
    s_q = xQueueCreate(AUD_CMD_QLEN, sizeof(audio_cmd_t));
    if (s_q == NULL) {
        ESP_LOGE(TAG, "failed to create command queue");
        return ESP_ERR_NO_MEM;
    }
    /* Tone synthesis is a shallow call chain, but voice-memo playback (fopen/fread -> FatFs ->
     * SDSPI) goes much deeper on this same worker, so the stack needs real headroom to avoid an
     * overflow. Low priority — audio must never preempt the UI or radio work, and it's not
     * Task-WDT-subscribed. The stack lives in PSRAM (xTaskCreateWithCaps + SPIRAM): this worker
     * never DMAs from its own stack (I2S TX copies via the static s_chunk buffer; file buffers are
     * their own PSRAM allocations) and never runs with the flash cache disabled, so it's safe to
     * take this stack out of the scarce internal-DMA pool. The task is never deleted. */
    /* AUD_TASK_STACK (28 KB, PSRAM, effectively free): the MP3 path's mp3dec_decode_frame keeps
     * ~19 KB of scratch on the stack; the WAV path and its FatFs/SDSPI chain fit within the older
     * 6 KB with room to spare. */
    if (xTaskCreateWithCaps(audio_task, "audio", AUD_TASK_STACK, NULL, 3, &s_task, MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "failed to create audio worker task");
        vQueueDelete(s_q);
        s_q = NULL;
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "audio worker ready (lazy I2S; sound %s)", s_muted ? "muted" : "on");
    return ESP_OK;
}

bool nocsif_audio_available(void)
{
    return s_task != NULL;
}

bool nocsif_audio_stack_is_psram(void)
{
    return s_stack_ext;             /* latched true once the worker first schedules on a PSRAM stack */
}

esp_err_t nocsif_audio_boot_reserve(void)
{
    /* Claims the I2S TX DMA descriptors now, from the pristine boot memory pool, before
     * esp_wifi_init fragments the internal-DMA heap. The ~3.8 KB of descriptors (4 x 240 frames x
     * 4 B — trimmed from the driver's default 6 for the IMU's benefit; see i2s_lazy_open) can only
     * live in internal DMA-capable SRAM, since the IDF I2S driver hard-codes an internal-DMA
     * allocation and the GDMA peripheral reads them directly — there's no PSRAM path here, unlike
     * the LoRa stack. Once opened, the channel is never torn down, so it stays claimed for the
     * whole session, mirroring nocsif_ble_boot_reserve. It's left disabled here — no clock toggling
     * and no amp rail powered until an actual play happens. Safe-mode boots skip this call
     * entirely, so tx_ready stays false and the UI can honestly report audio as unavailable. */
    esp_err_t e = i2s_lazy_open();
    if (e != ESP_OK) {
        s_tx_failed = true;
        nocsif_log_dma_free("audio reserve: I2S TX FAILED (cue honestly n/a)");
    } else {
        nocsif_log_dma_free("audio reserve: I2S TX claimed (~3.8KB int-DMA, 4 desc, held for session)");
    }
    return e;
}

bool nocsif_audio_tx_ready(void)
{
    return s_tx != NULL;            /* true only once the channel has actually claimed its DMA */
}

static void post(const audio_cmd_t *c)
{
    if (s_q == NULL) {
        return;
    }
    xQueueSend(s_q, c, 0);          /* non-blocking; a full queue just drops the request (best-effort) */
}

void nocsif_audio_tone(uint32_t freq_hz, uint32_t ms, uint8_t volume_pct)
{
    if (freq_hz == 0 || ms == 0 || volume_pct == 0) {
        return;
    }
    audio_cmd_t c = {
        .nseg = 1,
        .vol  = (uint8_t)(volume_pct > 100 ? 100 : volume_pct),
        .freq = { (uint16_t)(freq_hz > 20000 ? 20000 : freq_hz) },
        .ms   = { (uint16_t)(ms > AUD_MAX_MS ? AUD_MAX_MS : ms) },
    };
    post(&c);
}

void nocsif_audio_cue(nocsif_audio_cue_t cue)
{
    if (s_muted) {
        return;
    }
    if ((size_t)cue >= sizeof(s_cues) / sizeof(s_cues[0])) {
        return;
    }
    audio_cmd_t c = s_cues[cue];                       /* copy so it can be scaled by the master volume */
    c.kind = AUD_CMD_TONES;
    c.vol  = (uint8_t)((uint32_t)c.vol * s_vol / 255);
    post(&c);
}

void nocsif_audio_set_muted(bool muted)
{
    s_muted = muted;
    nocsif_settings_set_i32("sound_en", muted ? 0 : 1);
}

bool nocsif_audio_muted(void)
{
    return s_muted;
}

/* ---- master speaker volume (E1·2) ----------------------------------------- */
void nocsif_audio_set_volume(uint8_t vol_0_255)
{
    s_vol = vol_0_255;
    nocsif_settings_set_i32("spk_vol", vol_0_255);
}

uint8_t nocsif_audio_volume(void)
{
    return s_vol;
}

/* ---- boot / USB-plug sound toggles (E1·2) --------------------------------- */
void nocsif_audio_set_boot_sound(bool on)
{
    s_boot_snd = on;
    nocsif_settings_set_i32("boot_snd", on ? 1 : 0);
}

bool nocsif_audio_boot_sound(void)
{
    return s_boot_snd;
}

void nocsif_audio_set_usb_sound(bool on)
{
    s_usb_snd = on;
    nocsif_settings_set_i32("usb_snd", on ? 1 : 0);
}

bool nocsif_audio_usb_sound(void)
{
    return s_usb_snd;
}

void nocsif_audio_boot_cue(void)
{
    if (s_boot_snd) {
        nocsif_audio_cue(NOCSIF_AUDIO_BOOT);   /* nocsif_audio_cue still checks the master mute */
    }
}

void nocsif_audio_usb_cue(void)
{
    if (s_usb_snd) {
        nocsif_audio_cue(NOCSIF_AUDIO_USB);
    }
}

void nocsif_audio_set_shake_sound(bool on)
{
    s_shake_snd = on;
    nocsif_settings_set_i32("shake_snd", on ? 1 : 0);
}

bool nocsif_audio_shake_sound(void)
{
    return s_shake_snd;
}

void nocsif_audio_shake_cue(nocsif_audio_cue_t cue)
{
    if (s_shake_snd) {
        nocsif_audio_cue(cue);   /* nocsif_audio_cue still checks the master mute */
    }
}

/* ---- file playback (Phase B; voice memos ride the same path) ---------------- */
void nocsif_audio_play_file(const char *path, uint16_t makeup_x100)
{
    if (s_q == NULL || path == NULL || path[0] == '\0') {
        return;
    }
    if (s_playing) {
        s_stop_req = true;          /* tap-to-switch: the running file stops within a chunk, then this one plays */
    }
    audio_cmd_t c;
    memset(&c, 0, sizeof c);
    c.kind        = AUD_CMD_WAV;
    c.makeup_x100 = makeup_x100 ? makeup_x100 : 100;
    strncpy(c.path, path, sizeof c.path - 1);
    post(&c);
}

void nocsif_audio_play_wav(const char *path)
{
    nocsif_audio_play_file(path, AUD_MEMO_MAKEUP_X100);
}

void nocsif_audio_stop(void)
{
    if (s_playing) {
        s_stop_req = true;
    }
}

bool nocsif_audio_playing(void)
{
    return s_playing;
}

bool nocsif_audio_playing_path(char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return s_playing;
    }
    taskENTER_CRITICAL(&s_path_lock);
    strncpy(out, s_play_path, out_len - 1);
    taskEXIT_CRITICAL(&s_path_lock);
    out[out_len - 1] = '\0';
    return s_playing;
}
