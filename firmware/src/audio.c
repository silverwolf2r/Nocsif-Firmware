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
#include <strings.h>        /* strcasecmp — file-extension sniff */
#include <stdio.h>          /* fopen/fread — WAV header parse, MP3 stream */
#include <unistd.h>         /* read/lseek — the WAV sample stream bypasses stdio (see AUD_FILE_SECTOR) */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"   /* xTaskCreateWithCaps — places the worker stack in PSRAM */
#include "freertos/queue.h"
#include "freertos/semphr.h"          /* the WAV reader task's slot semaphores */
#include "esp_memory_utils.h"         /* esp_ptr_external_ram — PSRAM-stack self-test probe */
#include "coex.h"                     /* nocsif_log_dma_free — I2S TX boot-reserve telemetry */

#include "esp_log.h"
#include "esp_timer.h"      /* esp_timer_get_time — I2S feed-gap telemetry (did the DMA ring run dry?) */
#include "esp_heap_caps.h"  /* heap_caps_malloc(MALLOC_CAP_SPIRAM) — the playback buffer */
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
/* TX DMA ring: AUD_DMA_DESC_NUM descriptors x AUD_DMA_FRAME_NUM stereo frames (internal-DMA only, boot-
 * resident — see i2s_lazy_open for why 4 and not the driver default 6). 4 x 240 = 60 ms at 16 kHz: the
 * time the worker may be away between two writes before the ring runs dry (tx_write counts those). */
#define AUD_DMA_DESC_NUM  4
#define AUD_DMA_FRAME_NUM 240
/* §4.13 fix #5 / loudness pass: file playback runs through a PEAK LIMITER — instant attack, ~8 ms
 * release, ceiling AUD_WAV_CEIL — so a boosted peak is turned down for a few ms rather than bent. The
 * §4.13 version was a static soft-knee waveshaper: harmless on the odd peak, but driven hard (the reverted
 * 3.0x memo make-up put most of every word above its knee) it was a fuzz box — "static". A gain-riding
 * limiter has no such regime: it costs a little punch on the loudest syllables and nothing else, which is
 * what lets the memo make-up sit at 3x. A hard saturating clamp still backstops it. Cue/boot tones never
 * pass here. */
#define AUD_WAV_CEIL      0.95f
#define AUD_WAV_LIM_REL   0.992f        /* limiter envelope release per sample: ~8 ms at 16 kHz           */
/* Phase B file engine: streamed read block (PSRAM, per play) — a multiple of every WAV block_align
 * (1..8 B), and the accepted sample-rate window (the I2S clock follows the file). 8 KB = 16 sectors
 * through the staged diskio path: short enough that one read fits inside the DMA ring's 60 ms with room
 * (16 KB did not — see the FIFO note), fast enough to outrun 48 kHz stereo (192 KB/s) at one read per
 * ~16 ms pass. */
#define AUD_FILE_BLOCK    8192
#define AUD_FILE_RATE_MIN 8000
#define AUD_FILE_RATE_MAX 48000
/* WAV read-ahead FIFO (PSRAM, per play): AUD_FILE_FIFO_SLOTS blocks of AUD_FILE_BLOCK = 256 KB, i.e. 8 s
 * of 16 kHz mono (a memo) or 1.3 s of 48 kHz stereo. The feed used to read a block only when the
 * previous one was spent, waiting up to 3 s for the card lock each time — and the DMA ring covers 60 ms.
 * Any longer lock holder (a FAT free-space walk on a 64 GB card, a logbook flush, a directory listing)
 * therefore cut the audio: measured 2026-09-24 as 18 dry runs and a 2999 ms worst gap on one 14 s memo
 * while the bridge polled `status`. The FIFO is filled by a READER TASK of its own (wav_reader_task,
 * PSRAM stack, below the audio worker) so the worker only ever emits: reading in the feeding task cannot
 * work on this card, whose fixed ~45 ms command latency makes even a direct 8 KB read (77 ms avg) longer
 * than the ring, so every read was a dropout no matter how far ahead the FIFO was. The reader waits for
 * the card lock in short slices for as long as it takes (the FIFO carries the sound), so a long lock
 * holder now costs nothing until the FIFO itself runs out. */
#define AUD_FILE_FIFO_SLOTS   32
#define AUD_FILE_LOCK_MS      100       /* the reader's per-attempt lock wait; it simply asks again      */
#define AUD_FILE_READER_STACK 6144      /* PSRAM: read() -> FatFs -> SDSPI is the deep chain             */
#define AUD_FILE_READER_PRIO  4         /* under the audio worker (5): the feed always wins the CPU       */
/* READ INTO INTERNAL MEMORY, SECTOR-ALIGNED. A PSRAM destination goes through the staged diskio path
 * (sdcard.c sd_disk_read): one single-sector CMD17 per 512 B, and this 64 GB card charges ~40 ms of
 * command latency on every one — measured 2026-09-24 as 56 reads of 8 KB averaging 655 ms (max 901):
 * ~12 KB/s, so a 16 kHz memo (32 KB/s) could not stream at half real time and every block ran the ring
 * dry ("static that cuts out"). An internal, 4-aligned buffer takes the direct path, where FatFs hands
 * a whole run of sectors to one multi-sector CMD18 — provided the FILE OFFSET is sector-aligned too
 * (a partial sector goes through FatFs' window, one CMD17 each), so the first read is trimmed to reach
 * alignment and every later read is a whole number of sectors — AND provided the read reaches FatFs with
 * OUR buffer: stdio's fread refills the FILE's own small buffer and copies out, so an 8 KB fread still
 * arrived at the diskio as 16 staged single-sector calls (measured: 886 staged calls, 0 direct, 41 s for
 * 886 sectors). The sample stream therefore uses the raw VFS read() on the file descriptor; stdio is only
 * used for the header parse. The 8 KB is a TRANSIENT internal claim for the duration of one play (pool
 * largest ~22 KB today; the LoRa arming gate is 4 KB); when it cannot be had the play falls back to the
 * PSRAM path and says so. */
#define AUD_FILE_SECTOR     512
/* nocsif_audio_play_wav: the voice-memo make-up (3.0x). A memo leaves mic.c leveled, with its loud
 * syllables at ~-16 dBFS RMS and their peaks at ~0.85 FS (measured on the speaker->mic loopback; more
 * record-side compression buys < 1 dB there and lifts the floor 5-8 dB). x3.0 x the master volume
 * (170/255 by default) puts those syllables at ~-10 dBFS, next to the alert cue's ~-9, with the peak
 * limiter riding the top of each syllable for a few ms instead of a waveshaper bending it. */
#define AUD_MEMO_MAKEUP_X100 300
#define AUD_MP3_INBUF     16384         /* MP3 input window (PSRAM); refilled when < AUD_MP3_LOWWATER remain */
#define AUD_MP3_LOWWATER  4096          /* > the largest MP3 frame (1441 B) + ID3 slack                    */
#define AUD_TASK_STACK    28672         /* PSRAM: mp3dec_decode_frame keeps ~19 KB of scratch on the stack */
#define AUD_TASK_PRIO     5             /* above LVGL (4) + the mic worker (3): see nocsif_audio_init       */

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
static volatile bool       s_shake_snd = false;      /* shake wake/sleep blips (first-boot default off) */
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
    chan.auto_clear = true;                 /* emit zeros on underrun instead of stale noise */
    /* Shrink the TX DMA reserve from the default 6 descriptors to 4 (RAM Phase 2 — IMU int-DMA margin).
     * These descriptors are internal-DMA-only and boot-resident, so 6 x 240 x 2ch x 2B = 5760 B pinned
     * the steady-state largest hole down to ~1664 B, at which the BHI260 FIFO I2C read intermittently
     * starved (`imu: fifo process err -3`, rare task-WDT). 4 x 240 frames = 60 ms of stereo buffering at
     * AUD_SAMPLE_RATE — ample for the short cue/boot tones and adequate for streamed WAV playback (on
     * underrun auto_clear emits a brief silence, never a fault) — and hands ~1.9 KB back to the pool
     * (largest ~1664 -> ~3600), clearing the IMU margin. */
    chan.dma_desc_num  = AUD_DMA_DESC_NUM;
    chan.dma_frame_num = AUD_DMA_FRAME_NUM;  /* = the driver default; pinned so the feed-gap maths is honest */
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

/* ---- I2S feed telemetry: did the DMA ring ever run dry? ------------------------------------------ *
 * i2s_channel_write blocks until its chunk is queued, so when it returns the ring is (near) full and
 * plays unattended for AUD_DMA_DESC_NUM x AUD_DMA_FRAME_NUM frames (60 ms at 16 kHz). If the worker then
 * takes longer than that to come back with the next chunk — starved by a higher-priority task (LVGL
 * repaint, the mic's YIN pass), or parked behind the /sd lock on a streamed file — the DMA runs dry and
 * auto_clear emits silence: a dropout, with a click at each edge. "It glitched" is a symptom of exactly
 * that OR of the analogue side (rail / driver), and the two need different fixes, so every play counts
 * its gaps: a dry ring is logged as a warning, the worst gap of the last play and the session total are
 * readable through nocsif_audio_feed_stats (bridge `status` -> audio{dry,worst_gap_ms}). Zero dry
 * runs on a play that still sounded wrong = look at the amplitude / rail, not the software. */
static int64_t           s_feed_last_us;       /* worker-only: when the previous write returned (0 = none) */
static uint32_t          s_feed_ring_us;       /* the ring's play time at the current I2S clock            */
static uint32_t          s_feed_dry;           /* this play: gaps longer than the ring                     */
static uint32_t          s_feed_worst_us;      /* this play: the longest gap                               */
static volatile uint32_t s_feed_dry_total;     /* session total (bridge status)                            */
static volatile uint32_t s_feed_worst_ms_last; /* the last play's worst gap (bridge status)                */

/* Enable the (disabled) channel and reset the per-play gap counters. */
static esp_err_t tx_enable(void)
{
    s_feed_last_us  = 0;
    s_feed_ring_us  = (uint32_t)((uint64_t)AUD_DMA_DESC_NUM * AUD_DMA_FRAME_NUM * 1000000ULL /
                                 (s_cur_rate ? s_cur_rate : AUD_SAMPLE_RATE));
    s_feed_dry      = 0;
    s_feed_worst_us = 0;
    return i2s_channel_enable(s_tx);
}

/* Write `bytes` of s_chunk to the ring, counting a dry run when the worker was away longer than the ring
 * could cover. */
static void tx_write(size_t bytes)
{
    int64_t now = esp_timer_get_time();
    if (s_feed_last_us != 0) {
        uint32_t gap = (uint32_t)(now - s_feed_last_us);
        if (gap > s_feed_worst_us) s_feed_worst_us = gap;
        if (gap > s_feed_ring_us)  { s_feed_dry++; s_feed_dry_total++; }
    }
    size_t wrote = 0;
    i2s_channel_write(s_tx, s_chunk, bytes, &wrote, portMAX_DELAY);
    s_feed_last_us = esp_timer_get_time();
}

/* End of a play: publish the gap figures; warn only when the ring actually ran dry. */
static void tx_feed_report(const char *what)
{
    s_feed_worst_ms_last = (s_feed_worst_us + 500) / 1000;
    if (s_feed_dry) {
        ESP_LOGW(TAG, "%s: DMA ring ran dry %u times (worst gap %u ms > ring %u ms) — audible dropouts",
                 what, (unsigned)s_feed_dry, (unsigned)s_feed_worst_ms_last, (unsigned)(s_feed_ring_us / 1000));
    }
}

/* Synthesize + stream one sine segment to the (already-enabled) channel. A linear fade over the
 * first/last few ms tames the click at the tone edges. freq 0 => silence (still consumes ms). */
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
        tx_write((size_t)n * 2 * sizeof(int16_t));
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
    if (tx_enable() != ESP_OK) {
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
    tx_feed_report("tone");
}

/* ---- Phase B file engine: any PCM WAV, streamed ------------------------------------ *
 * Replaces the E1·3 player (16 kHz mono 16-bit only, whole clip read into PSRAM, capped at 2 MB): the
 * RIFF chunks are walked for "fmt " + "data" (LIST/fact/etc. skipped, WAVE_FORMAT_EXTENSIBLE unwrapped),
 * the file's rate/bits/channels drive the conversion, the I2S clock follows the file, and the samples are
 * STREAMED in AUD_FILE_BLOCK reads under short /sd locks — no whole-file buffer, any length. Each frame is
 * downmixed to mono float, scaled (master volume x make-up), peak-limited (§4.13 fix #5), and written
 * to both I2S slots (the MAX98357A is channel-agnostic). s_stop_req ends a file within one chunk. */
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

/* ---- shared sample emitter: mono float in [-1,1] -> gain -> peak limiter -> both I2S slots ---------- *
 * Fills s_chunk and writes it to I2S every AUD_CHUNK_FRAMES frames (worker-only). */
static size_t   s_emit_n;
static float    s_emit_gain;
static uint32_t s_emit_limited;
static float    s_emit_env;                 /* limiter envelope, normalized (reset per play) */

static void emit_flush(void)
{
    if (s_emit_n > 0) {
        tx_write(s_emit_n * 2 * sizeof(int16_t));
        s_emit_n = 0;
    }
}

static inline void emit_mono(float x)
{
    x *= s_emit_gain;
    const float ax = fabsf(x);
    s_emit_env = (ax > s_emit_env) ? ax : s_emit_env * AUD_WAV_LIM_REL;
    if (s_emit_env > AUD_WAV_CEIL) {                        /* limiter: ride the gain down for a few ms */
        s_emit_limited++;
        x *= AUD_WAV_CEIL / s_emit_env;
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
    s_emit_env     = 0.0f;

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
            if (tx_enable() != ESP_OK) break;
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
        tx_feed_report("play");
        ESP_LOGI(TAG, "play: %s %s (mp3 %u Hz %s, %u frames, gain %.2fx, %u limited, ring dry %u, worst gap %u ms)",
                 stopped ? "stopped" : "done", path, (unsigned)rate, channels == 2 ? "stereo" : "mono",
                 (unsigned)frames, (double)s_emit_gain, (unsigned)s_emit_limited,
                 (unsigned)s_feed_dry, (unsigned)s_feed_worst_ms_last);
    } else {
        ESP_LOGW(TAG, "play: %s — no decodable MP3 frames", path);
    }
}

static void do_play_wav_stream(const char *path, uint16_t makeup_x100);

/* ---- WAV reader task (see the AUD_FILE_FIFO_SLOTS note) ---------------------------------------------- *
 * Fills FIFO slots under the card lock; the audio worker only emits. Two counting semaphores hand slots
 * across — free_s (reader takes one per block, the emitter gives it back once played) and full_s (the
 * reverse). A slot of length 0 is the end-of-stream marker. The worker sets `abort` to stop the reader,
 * waits for `exited`, then deletes the (suspended) task. */
typedef struct {
    int               fd;
    wav_info_t        w;
    uint8_t          *fifo, *rdbuf;
    size_t            want;
    size_t            slot_len[AUD_FILE_FIFO_SLOTS];
    SemaphoreHandle_t free_s, full_s, exited;
    uint32_t          left, pos;
    volatile bool     abort;
    uint32_t          reads, rd_us, rd_max_us, busy;          /* telemetry for the play line */
    char              holder[configMAX_TASK_NAME_LEN + 1];    /* who had the card on the first miss */
} wav_reader_t;

static void wav_reader_task(void *arg)
{
    wav_reader_t *r = arg;
    size_t wi = 0;
    bool   marker = false;                                  /* the 0-length end marker has been posted */
    while (!r->abort && r->left > 0) {
        size_t ask = (r->want < r->left) ? r->want : (size_t)r->left;
        /* Reach sector alignment with the first (short) read, then every read starts on a sector. */
        const uint32_t mis = r->pos % AUD_FILE_SECTOR;
        if (mis != 0 && r->rdbuf != NULL && ask > AUD_FILE_SECTOR - mis) {
            const size_t trim = (AUD_FILE_SECTOR - mis) - ((AUD_FILE_SECTOR - mis) % r->w.block);
            if (trim > 0) ask = trim;                       /* else: frames never align — full read */
        }
        ask -= ask % r->w.block;
        if (ask == 0) break;
        while (!r->abort && xSemaphoreTake(r->free_s, pdMS_TO_TICKS(100)) != pdTRUE) { }
        if (r->abort) break;
        while (!r->abort && !nocsif_sdcard_lock(AUD_FILE_LOCK_MS)) {
            if (r->busy == 0) strlcpy(r->holder, nocsif_sdcard_lock_holder(), sizeof r->holder);
            r->busy++;
        }
        if (r->abort) break;
        const int64_t t0 = esp_timer_get_time();
        ssize_t got;
        if (r->rdbuf != NULL) {
            got = read(r->fd, r->rdbuf, ask);                /* internal + aligned: one multi-sector read */
            if (got > 0) memcpy(r->fifo + wi * r->want, r->rdbuf, (size_t)got);
        } else {
            got = read(r->fd, r->fifo + wi * r->want, ask);  /* PSRAM target: staged, a sector at a time */
        }
        nocsif_sdcard_unlock();
        const uint32_t dt = (uint32_t)(esp_timer_get_time() - t0);
        r->reads++; r->rd_us += dt; if (dt > r->rd_max_us) r->rd_max_us = dt;
        size_t rd = got > 0 ? (size_t)got : 0;
        r->pos += (uint32_t)rd;
        rd -= rd % r->w.block;
        r->left = (rd < r->left) ? r->left - (uint32_t)rd : 0;
        if (rd < ask) r->left = 0;                           /* short read = EOF / short file */
        r->slot_len[wi] = rd;                                /* rd == 0 doubles as the end marker */
        wi = (wi + 1) % AUD_FILE_FIFO_SLOTS;
        marker = (rd == 0);
        xSemaphoreGive(r->full_s);
    }
    if (!marker && !r->abort && xSemaphoreTake(r->free_s, pdMS_TO_TICKS(5000)) == pdTRUE) {
        r->slot_len[wi] = 0;
        xSemaphoreGive(r->full_s);
    }
    xSemaphoreGive(r->exited);
    vTaskSuspend(NULL);                                      /* the worker deletes us (vTaskDeleteWithCaps) */
}

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
    const size_t want = (AUD_FILE_BLOCK / w.block) * w.block;    /* whole frames per block read */
    uint8_t *fifo = heap_caps_malloc((size_t)AUD_FILE_FIFO_SLOTS * want, MALLOC_CAP_SPIRAM);
    if (fifo == NULL) {
        ESP_LOGE(TAG, "play: PSRAM FIFO alloc failed");
        fclose(f);
        return;
    }
    /* The internal read buffer (see AUD_FILE_SECTOR): heap_caps_malloc, not _aligned_alloc — the latter is
     * wrapped by sd_bounce.c for the SD driver's own small buffers. */
    uint8_t *rdbuf = heap_caps_malloc(AUD_FILE_BLOCK, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (rdbuf == NULL) {
        ESP_LOGW(TAG, "play: no internal read buffer (int-dma largest %u) — PSRAM path, expect a slow card",
                 (unsigned)nocsif_int_dma_largest());
    }
    ensure_rate(w.rate);
    nocsif_power_speaker_rail(true);
    vTaskDelay(pdMS_TO_TICKS(AUD_RAIL_SETTLE_MS));
    if (tx_enable() != ESP_OK) {
        nocsif_power_speaker_rail(false);
        heap_caps_free(fifo);
        heap_caps_free(rdbuf);
        fclose(f);
        return;
    }
    set_play_path(path);
    s_playing = true;

    /* Master volume (attenuation) x make-up, in normalized [-1,1] float; emit_mono applies the peak
     * limiter and the saturating clamp. */
    s_emit_gain    = ((float)makeup_x100 / 100.0f) * ((float)s_vol / 255.0f);
    s_emit_n       = 0;
    s_emit_limited = 0;
    s_emit_env     = 0.0f;
    /* The reader task fills the FIFO (a ring of AUD_FILE_FIFO_SLOTS block slots, slot_len[] = the whole-
     * frame byte count of each, 0 = end); this worker drains the oldest slot and hands it back. The
     * samples are read through the descriptor, not the FILE (see AUD_FILE_SECTOR): put the descriptor
     * where the parse left the stream first. */
    wav_reader_t r;
    memset(&r, 0, sizeof r);
    r.fd = fileno(f); r.w = w; r.fifo = fifo; r.rdbuf = rdbuf; r.want = want;
    r.left = w.data_len ? w.data_len : 0xFFFFFFFFu;
    r.pos  = (uint32_t)w.data_off;
    if (nocsif_sdcard_lock(3000)) {
        if (lseek(r.fd, (off_t)w.data_off, SEEK_SET) != (off_t)w.data_off) r.left = 0;
        nocsif_sdcard_unlock();
    } else {
        r.left = 0;
    }
    r.free_s = xSemaphoreCreateCounting(AUD_FILE_FIFO_SLOTS, AUD_FILE_FIFO_SLOTS);
    r.full_s = xSemaphoreCreateCounting(AUD_FILE_FIFO_SLOTS, 0);
    r.exited = xSemaphoreCreateBinary();
    TaskHandle_t reader = NULL;
    if (r.free_s == NULL || r.full_s == NULL || r.exited == NULL ||
        xTaskCreateWithCaps(wav_reader_task, "wavread", AUD_FILE_READER_STACK, &r, AUD_FILE_READER_PRIO,
                            &reader, MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "play: reader task / semaphores could not be created");
        reader = NULL;
        r.left = 0;
    }
    uint32_t sd_direct0, sd_staged0, sd_sect0, sd_us0;
    nocsif_sdcard_read_stats(&sd_direct0, &sd_staged0, &sd_sect0, &sd_us0);

    size_t      ri = 0;
    uint32_t    frames = 0;
    UBaseType_t fifo_low = AUD_FILE_FIFO_SLOTS;             /* the fewest filled slots seen once rolling */
    bool        stopped = false;
    while (reader != NULL && !stopped) {
        if (xSemaphoreTake(r.full_s, pdMS_TO_TICKS(4000)) != pdTRUE) {
            ESP_LOGW(TAG, "play: reader stalled 4 s (card busy %u, holder %s) — ending", (unsigned)r.busy, r.holder);
            break;
        }
        const UBaseType_t have = uxSemaphoreGetCount(r.full_s) + 1;
        if (frames > 0 && have < fifo_low) fifo_low = have;
        const size_t   len = r.slot_len[ri];
        const uint8_t *p   = fifo + ri * want;
        if (len == 0) break;                                /* end of stream */
        for (size_t off = 0; off < len; off += w.block) {
            emit_mono(wav_frame_mono(p + off, &w));
            if ((++frames % AUD_CHUNK_FRAMES) == 0 && s_stop_req) { stopped = true; break; }
        }
        ri = (ri + 1) % AUD_FILE_FIFO_SLOTS;
        xSemaphoreGive(r.free_s);
    }
    emit_flush();
    play_segment(0, 6, 0);                                 /* silence tail flushes the last DMA frames */
    i2s_channel_disable(s_tx);
    nocsif_power_speaker_rail(false);
    s_playing = false;
    set_play_path("");
    /* Retire the reader: it checks `abort` between every wait, so it is out within one read. */
    r.abort = true;
    if (reader != NULL) {
        if (xSemaphoreTake(r.exited, pdMS_TO_TICKS(6000)) == pdTRUE) {
            vTaskDeleteWithCaps(reader);
        } else {
            ESP_LOGE(TAG, "play: reader did not exit — leaking its task");   /* never expected: reads are bounded */
        }
    }
    if (r.free_s) vSemaphoreDelete(r.free_s);
    if (r.full_s) vSemaphoreDelete(r.full_s);
    if (r.exited) vSemaphoreDelete(r.exited);
    heap_caps_free(fifo);
    heap_caps_free(rdbuf);
    fclose(f);
    tx_feed_report("play");
    uint32_t sd_direct, sd_staged, sd_sect, sd_us;
    nocsif_sdcard_read_stats(&sd_direct, &sd_staged, &sd_sect, &sd_us);
    ESP_LOGI(TAG, "play: %s %s (%u Hz %u-bit %s, %u frames, gain %.2fx, %u limited, ring dry %u, worst gap %u ms, "
                  "card busy %u [%s], %u reads avg %u max %u ms via %s, fifo low %u/%u; "
                  "disk: %u direct + %u staged calls, %u sectors, %u ms)",
             stopped ? "stopped" : "done", path, (unsigned)w.rate, (unsigned)w.bits,
             w.ch == 2 ? "stereo" : "mono", (unsigned)frames, (double)s_emit_gain, (unsigned)s_emit_limited,
             (unsigned)s_feed_dry, (unsigned)s_feed_worst_ms_last, (unsigned)r.busy, r.holder,
             (unsigned)r.reads, (unsigned)(r.reads ? r.rd_us / r.reads / 1000 : 0), (unsigned)(r.rd_max_us / 1000),
             rdbuf ? "internal" : "psram", (unsigned)fifo_low, (unsigned)AUD_FILE_FIFO_SLOTS,
             (unsigned)(sd_direct - sd_direct0), (unsigned)(sd_staged - sd_staged0),
             (unsigned)(sd_sect - sd_sect0), (unsigned)((sd_us - sd_us0) / 1000));
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
    s_shake_snd = nocsif_settings_get_i32("shake_snd", 0) != 0;   /* first-boot default: off */
    s_q = xQueueCreate(AUD_CMD_QLEN, sizeof(audio_cmd_t));
    if (s_q == NULL) {
        ESP_LOGE(TAG, "failed to create command queue");
        return ESP_ERR_NO_MEM;
    }
    /* 6 KB stack: tone synth is shallow, but voice-memo playback (do_play_wav -> fopen/fread -> FatFs
     * -> SDSPI) is a deep chain on this worker — headroom avoids the 4 KB-worker overflow class.
     * Priority AUD_TASK_PRIO (5): ABOVE the LVGL port task (4) and the mic worker (3). This worker is
     * a real-time feed that sleeps in i2s_channel_write ~95% of the time (16 ms of audio per ~1 ms of
     * synth), so it cannot starve the UI — but at its old prio 3 the UI starved IT: the feed-gap
     * telemetry (tx_write) on the tuner screen measured 31-33 ms between chunk writes with the dial
     * repainting and the mic's YIN pass running, and a 76 ms gap (ring 60 ms -> dry -> dropout) on the
     * first tone after the screen opened. Not Task-WDT-subscribed.
     * Stack in PSRAM (xTaskCreateWithCaps + SPIRAM, RAM-BUDGET remake #7): this worker never DMAs
     * from its own stack (I2S TX copies via the static-internal s_chunk; WAV/record buffers are their
     * own PSRAM allocs) and never runs with the flash cache disabled (no on-task NVS/flash writes),
     * so its 6 KB comes out of the scarce internal-DMA pool. Never deleted -> no vTaskDeleteWithCaps. */
    /* AUD_TASK_STACK (28 KB, PSRAM — free): the Phase B MP3 path's mp3dec_decode_frame keeps ~19 KB of
     * scratch on the stack; the WAV path + FatFs/SDSPI chain fit the old 6 KB with room. */
    if (xTaskCreateWithCaps(audio_task, "audio", AUD_TASK_STACK, NULL, AUD_TASK_PRIO, &s_task, MALLOC_CAP_SPIRAM) != pdPASS) {
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

void nocsif_audio_feed_stats(uint32_t *dry_total, uint32_t *worst_gap_ms_last)
{
    if (dry_total)         *dry_total         = s_feed_dry_total;
    if (worst_gap_ms_last) *worst_gap_ms_last = s_feed_worst_ms_last;
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
