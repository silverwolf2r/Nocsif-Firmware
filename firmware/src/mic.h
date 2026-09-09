/*
 * NocSif — PDM microphone (I2S PDM RX) — M11 watch-core, slice E1·2.
 *
 * The T-Watch Ultra carries a PDM MEMS microphone on I2S: PDM clock =
 * GPIO17, PDM data = GPIO18. It reads on I2S_NUM_0 (the MAX98357A amp uses
 * I2S_NUM_1, so the two compose freely), 16 kHz mono LEFT slot. On the
 * ESP32-S3 the I2S peripheral does PDM->PCM in hardware (with a high-pass
 * filter), so this driver reads 16-bit signed PCM directly. The mic sits on
 * the always-on 3.3 V domain — not the BLDO2 amp rail — so no power-rail
 * toggle is needed (deliberately independent of the speaker).
 *
 * E1·2 scope: a proof-of-life level meter — capture audio, reduce it to a
 * smoothed RMS/peak level, and expose that as a cached 0..100 value the
 * Microphone test screen renders as a live bar (blow on the mic -> the bar
 * moves). Voice memo (record -> WAV on /sd -> playback via the E1·1 amp) is E1·3.
 *
 * Threading (mirrors imu.c's producer/cache shape, not audio.c's command
 * queue): a dedicated worker task owns the I2S channel and, while capture
 * is active, continuously reads PCM blocks and publishes a level into a
 * spinlock-guarded cache; the LVGL-side getters read only that cache (no
 * I2S), so a live meter can poll them from an lv_timer. Capture is gated by
 * nocsif_mic_set_active: the worker is parked (no capture, channel freed)
 * until activated, opens the PDM RX channel lazily on first activation, and
 * closes it on deactivation — so the mic listens only while the Microphone
 * screen is open (good for power, privacy, and leaving internal DMA free
 * for the radio). Independent of the BLE/WiFi single radio (its own I2S
 * port), so it composes freely.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Coarse capture state, for the status string / a screen header. */
typedef enum {
    NOCSIF_MIC_OFF = 0,   /* worker idle — capture not active (or skipped in safe mode) */
    NOCSIF_MIC_ACTIVE,    /* capturing: reading PCM + updating the level cache */
    NOCSIF_MIC_FAILED,    /* the PDM RX channel could not be opened */
} nocsif_mic_state_t;

/* Creates the mic worker task. Cheap — no I2S yet (the PDM RX channel is
 * opened lazily when capture is first activated). No-op returning ESP_OK in
 * reliability safe mode (stays OFF). Idempotent — a second call is ignored.
 * Call after nocsif_audio_init() at boot. */
esp_err_t nocsif_mic_init(void);

/* True once the worker exists (so a status row can show "n/a" if the mic never came up). */
bool nocsif_mic_available(void);

/* Self-test: true once the worker has scheduled and confirmed its task
 * stack lives in PSRAM. Used only by the compile-gated
 * NOCSIF_PSRAM_STACK_SELFTEST hook. */
bool nocsif_mic_stack_is_psram(void);

/* Requests capture on/off. Turning it on wakes the worker, which lazily
 * opens + enables the PDM RX channel and starts reading; turning it off
 * stops reading and frees the channel (so its internal DMA returns to the
 * pool). The Microphone test screen calls (true) on build and (false) on
 * exit. Non-blocking; safe to call from the LVGL task. */
void nocsif_mic_set_active(bool active);

/* Cached input level 0..100 (a smoothed RMS of the most recent audio), 0 when not capturing.
 * No I2S — LVGL-safe. */
uint8_t nocsif_mic_level(void);

/* Cached recent peak level 0..100 (a decaying peak-hold), 0 when not capturing. LVGL-safe. */
uint8_t nocsif_mic_peak(void);

/* Coarse capture state (cached; no I2S). */
nocsif_mic_state_t nocsif_mic_state(void);

/* Cached one-line status string for a screen header, e.g. "idle",
 * "listening 42%", "no mic". Static buffer, stable between calls; LVGL-safe. */
const char *nocsif_mic_status_str(void);

/* ---- adjustable sensitivity (E1·2) ---------------------------------------- *
 * The displayed level is RMS * gain / 32768 * 100 (clamped 0..100). A PDM
 * MEMS mic gives modest PCM for speech, so the gain lifts it into a visible
 * range; raise it if speech barely registers, lower it if quiet-room noise
 * pins the bar. The UI persists the chosen value and calls the setter. */
void  nocsif_mic_set_gain(float gain);   /* clamped to a sane range; takes effect on the next block */
float nocsif_mic_gain(void);             /* current gain (cached) */

/* ---- voice memo recording (E1·3) ------------------------------------------ *
 * Recording rides the same worker/I2S RX as the level meter: while active
 * it also appends the 16 kHz mono PCM into a PSRAM buffer (capped at this
 * many seconds), then on stop the worker writes it to a WAV on /sd.
 * Playback is nocsif_audio_play_wav() (audio.c, the E1·1 amp). */
#define NOCSIF_MIC_REC_MAX_SECS 30
typedef enum {
    NOCSIF_MIC_REC_IDLE = 0,   /* not recording */
    NOCSIF_MIC_REC_RECORDING,  /* capturing PCM into the PSRAM buffer */
    NOCSIF_MIC_REC_SAVING,     /* writing the WAV to /sd */
    NOCSIF_MIC_REC_DONE,       /* last recording saved OK (path via _record_path) */
    NOCSIF_MIC_REC_ERROR,      /* alloc / SD / write failed */
} nocsif_mic_rec_state_t;

/* Starts recording to `path` (a WAV on /sd). Ensures capture is active,
 * allocates the PSRAM buffer, and records until _record_stop() or the ~cap
 * is hit (auto-stop). Non-blocking; the worker does the capture + the WAV
 * flush. Copies `path`. No-op if already recording. */
void nocsif_mic_record_start(const char *path);

/* Requests stop: the worker finalizes and writes the WAV (state -> SAVING -> DONE/ERROR). */
void nocsif_mic_record_stop(void);

/* Cached recording state / elapsed (or last-recorded) whole seconds. LVGL-safe. */
nocsif_mic_rec_state_t nocsif_mic_record_state(void);
uint32_t               nocsif_mic_record_secs(void);

/* True while recording or saving (the UI blocks a second record + shows "recording.../saving..."). */
bool nocsif_mic_recording(void);

/* Why the last recording ended in NOCSIF_MIC_REC_ERROR. The state alone
 * hides six different causes behind one "check the card" line; the worker
 * records the cause at each failure site and the screen prints the
 * matching short reason (nocsif_mic_rec_error_str). LVGL-safe (an int read). */
typedef enum {
    NOCSIF_MIC_ERR_NONE = 0,
    NOCSIF_MIC_ERR_MIC_OPEN,    /* the PDM RX channel would not open (I2S / DMA) */
    NOCSIF_MIC_ERR_NO_MEMORY,   /* the PSRAM capture buffer could not be allocated */
    NOCSIF_MIC_ERR_EMPTY,       /* nothing was captured (stopped at once) */
    NOCSIF_MIC_ERR_CARD_USB,    /* USB File Share owns the card */
    NOCSIF_MIC_ERR_CARD_BUSY,   /* /sd lock timed out (another writer / WiFi contention) */
    NOCSIF_MIC_ERR_FILE_OPEN,   /* fopen failed (no card / read-only / bad folder) */
    NOCSIF_MIC_ERR_FILE_WRITE,  /* short write (card full / removed mid-save) */
} nocsif_mic_rec_err_t;
nocsif_mic_rec_err_t nocsif_mic_rec_error(void);
const char          *nocsif_mic_rec_error_str(void);   /* a short, honest line for the Voice Memos status */

#ifdef __cplusplus
}
#endif
