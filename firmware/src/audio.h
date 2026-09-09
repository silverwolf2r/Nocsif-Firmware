/*
 * NocSif audio output — drives the MAX98357A I2S speaker (M11 watch-core, slice E1·1).
 *
 * The T-Watch Ultra has a MAX98357A mono Class-D amp on I2S (BCLK=GPIO9, WCLK/LRCK=GPIO10,
 * DOUT=GPIO11), powered from the AXP2101 BLDO2 rail. This module owns an I2S TX channel in
 * standard mode and plays short synthesized tones and named cues. Since the DRV2605 haptic driver
 * is dead on this unit, audio doubles as the alert channel — a chime stands in for the buzz.
 *
 * THREADING (same pattern as wifi.c / imu.c): a worker task owns every blocking action — the I2S
 * writes and the amp rail. Callers only post a tone/cue to its queue, so nothing blocks the LVGL
 * task. Bring-up is lazy (the I2S channel opens on first use) and the amp rail powers up only for
 * the duration of a sound, so boot stays fast and idle current stays low.
 *
 * The mic (PDM RX, slice E1·2) uses a separate I2S port and rail, so it composes freely with this.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Named sound cues. Each is one or more tone segments played back-to-back with the amp rail held
 * up for the whole cue. Cues respect the mute flag (nocsif_audio_set_muted); the raw
 * nocsif_audio_tone() call below does not, since it's the hardware test path. */
typedef enum {
    NOCSIF_AUDIO_BOOT = 0,   /* rising two-tone chime heard right after boot        */
    NOCSIF_AUDIO_WAKE,       /* short high blip when the panel wakes                */
    NOCSIF_AUDIO_SLEEP,      /* short falling blip when the panel sleeps            */
    NOCSIF_AUDIO_TICK,       /* brief tick for a toggled setting                    */
    NOCSIF_AUDIO_ALERT,      /* attention cue for notifications/alarms/self-tests   */
    NOCSIF_AUDIO_USB,        /* short two-tone cue for a USB power connect          */
} nocsif_audio_cue_t;

/* Creates the audio worker task and its command queue, and loads the persisted mute setting.
 * Doesn't touch I2S or the amp rail yet (both open lazily on first play). Idempotent; safe to call
 * before the UI exists. */
esp_err_t nocsif_audio_init(void);

/* True once the worker task exists, so a status display can report "n/a" if audio never came up. */
bool nocsif_audio_available(void);

/* Self-test hook: true once the worker has confirmed its own task stack lives in PSRAM. Only used
 * behind the compile-time NOCSIF_PSRAM_STACK_SELFTEST flag. */
bool nocsif_audio_stack_is_psram(void);

/* Claims the I2S TX DMA descriptors from the pristine boot memory pool before it fragments, so a
 * boot chime can still play later. Call once from app_main, after nocsif_ble_boot_reserve() and
 * before nocsif_ui_init(). Skipped by safe-mode boots. Returns ESP_OK if the channel got its DMA. */
esp_err_t nocsif_audio_boot_reserve(void);

/* True only once the I2S TX channel has actually claimed DMA (via the boot reserve or a later lazy
 * open). Gate a UI sound toggle on this rather than nocsif_audio_available(), which is true even if
 * I2S never came up. */
bool nocsif_audio_tx_ready(void);

/* Queues a single sine tone at freq_hz for ms milliseconds at volume_pct (0..100). Non-blocking;
 * ignores the mute flag since this is the on-demand hardware test. Duration is capped; a zero
 * freq/ms/volume is silently dropped. */
void nocsif_audio_tone(uint32_t freq_hz, uint32_t ms, uint8_t volume_pct);

/* Queues a named cue. Non-blocking; a no-op while muted. */
void nocsif_audio_cue(nocsif_audio_cue_t cue);

/* Mutes/unmutes cue playback and persists the setting. Doesn't stop a sound already queued, and
 * doesn't affect nocsif_audio_tone(). */
void nocsif_audio_set_muted(bool muted);

/* Current mute state, read from a cached flag. */
bool nocsif_audio_muted(void);

/* ---- master speaker volume (E1·2) ----------------------------------------- *
 * A 0..255 scale applied to cues and file playback (not to the raw nocsif_audio_tone test, which
 * always plays at its requested level). Persisted as "spk_vol". This is the watch's own volume,
 * separate from the phone-facing Control-Center media slider. Defaults to full. */
void    nocsif_audio_set_volume(uint8_t vol_0_255);
uint8_t nocsif_audio_volume(void);

/* ---- boot / USB-plug sound toggles (E1·2) --------------------------------- *
 * Independent on/off flags (persisted, default on) that sit under the master mute — a cue only
 * plays when unmuted AND its own flag is on. Use these gated helpers rather than calling
 * nocsif_audio_cue directly for the boot/USB/shake events. */
void nocsif_audio_set_boot_sound(bool on);
bool nocsif_audio_boot_sound(void);
void nocsif_audio_set_usb_sound(bool on);
bool nocsif_audio_usb_sound(void);
void nocsif_audio_set_shake_sound(bool on);   /* gates the shake wake/sleep blips */
bool nocsif_audio_shake_sound(void);

/* Plays the boot chime if unmuted and boot-sound is on. Called once at boot from main.c. */
void nocsif_audio_boot_cue(void);

/* Plays the USB-connect cue if unmuted and USB-sound is on. Called on a VBUS rising edge from
 * buttons.c. Non-blocking; safe to call from any task. */
void nocsif_audio_usb_cue(void);

/* Plays a shake wake/sleep blip if unmuted and shake-sound is on. Called from the shake gesture
 * handler in ui.c. */
void nocsif_audio_shake_cue(nocsif_audio_cue_t cue);

/* ---- file playback (Phase B: Carts; also voice memos) ---------------------- *
 * Plays a PCM WAV (8/16/24/32-bit int or float, mono/stereo, 8-48 kHz) or an MP3 (via minimp3, any
 * bitrate incl. VBR, ID3 tags skipped) from /sd through the amp, chosen by extension. The I2S clock
 * is reconfigured to match each file (tones/cues restore 16 kHz afterward). Streamed from the card
 * in 16 KB blocks under short /sd locks, so no whole-file buffer and no length limit. Scaled by the
 * master volume; makeup_x100 is an extra gain (percent) run through a soft-knee limiter (100 =
 * unity for normal tones; voice memos use 240 since raw PDM speech is quiet). Non-blocking — the
 * path string is copied and the worker plays it. Starting a new file while one plays stops the old
 * one first (tap-to-switch). Path length is bounded. */
void nocsif_audio_play_file(const char *path, uint16_t makeup_x100);

/* Convenience for voice memos: nocsif_audio_play_file(path, 240), the standard memo make-up gain. */
void nocsif_audio_play_wav(const char *path);

/* Stops whatever file is playing; a no-op if idle. Non-blocking — the worker finishes within one
 * chunk. */
void nocsif_audio_stop(void);

/* True while a file is playing, so the UI can show a "playing…" state. Cached. */
bool nocsif_audio_playing(void);

/* Copies the path of the currently-playing file into out ("" when idle); also returns
 * nocsif_audio_playing(). Copy is spinlock-guarded, so safe to call from the LVGL task (used for
 * the Carts "now playing" line). */
bool nocsif_audio_playing_path(char *out, size_t out_len);

#ifdef __cplusplus
}
#endif
