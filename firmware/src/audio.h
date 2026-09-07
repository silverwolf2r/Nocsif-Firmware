/*
 * NocSif — audio output (MAX98357A I2S speaker) — M11 watch-core, slice E1·1.
 *
 * The T-Watch Ultra carries a MAX98357A mono Class-D amplifier on I2S (BCLK=GPIO9, WCLK/LRCK=GPIO10,
 * DOUT=GPIO11), powered from the AXP2101 BLDO2 rail. This module owns an I2S standard-mode TX channel
 * and plays short synthesized tones / named cues — the watch's on-wrist sound. With the DRV2605 haptic
 * hardware-dead on this unit, audio is also the ALERTING channel (a chime replaces the buzz).
 *
 * THREADING (mirrors wifi.c / imu.c): a dedicated worker task owns the blocking I2S writes and the
 * BLDO2 rail. LVGL/callers only POST a tone/cue to the worker's queue (never touch I2S), so the LVGL
 * task never blocks on audio. All the public functions below are non-blocking and safe to call from
 * the LVGL task. Bring-up is LAZY (the I2S channel is allocated on the first play), and the amp rail
 * is powered only while a sound plays (dropped in between), so boot stays light and idle draw is low.
 *
 * The mic (PDM RX, slice E1·2) will live on a separate I2S port and rail, so the two compose freely.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Named sound cues. Each expands to one or more back-to-back tone segments played with the amp rail
 * up for the whole cue (so a multi-tone chime is one clean power cycle). Cues are gated by the mute
 * flag (nocsif_audio_set_muted); the raw nocsif_audio_tone below is NOT (it is the hardware test). */
typedef enum {
    NOCSIF_AUDIO_BOOT = 0,   /* rising two-tone boot chime (heard right after boot)      */
    NOCSIF_AUDIO_WAKE,       /* short high blip — panel woke (shake / double-tap)         */
    NOCSIF_AUDIO_SLEEP,      /* short falling blip — panel slept (shake-to-sleep / PWR)   */
    NOCSIF_AUDIO_TICK,       /* brief tick — a toggle flipped                              */
    NOCSIF_AUDIO_ALERT,      /* attention cue — notification / alarm / Diagnostics test   */
    NOCSIF_AUDIO_USB,        /* short two-tone — USB power connected (gated by _usb_sound) */
} nocsif_audio_cue_t;

/* Create the audio worker task + its command queue. Cheap (no I2S, no rail yet — the channel is
 * allocated lazily on the first play). Reads the persisted "sound_en" setting into the mute flag.
 * Idempotent. Safe to call before the UI is up. */
esp_err_t nocsif_audio_init(void);

/* True once the worker exists (so a caller / status row can show "n/a" if audio never came up). */
bool nocsif_audio_available(void);

/* Self-test probe (RAM-BUDGET remake #7): true once the worker has scheduled and confirmed its task
 * stack lives in PSRAM. Used only by the compile-gated NOCSIF_PSRAM_STACK_SELFTEST hook. */
bool nocsif_audio_stack_is_psram(void);

/* Boot-reserve the I2S TX DMA (RAM-BUDGET remake #12 / conflict C7). Claims the ~5.7 KB of I2S TX
 * descriptors from the pristine boot pool, mirroring nocsif_ble_boot_reserve, so the Signal-Hunt cue /
 * boot chime can play at steady-state fragmentation (the descriptors are internal-DMA-only — no PSRAM
 * route). Call once from app_main AFTER nocsif_ble_boot_reserve() and BEFORE nocsif_ui_init() (the
 * first esp_wifi_init). Safe-mode callers skip it. Returns ESP_OK if the channel claimed its DMA. */
esp_err_t nocsif_audio_boot_reserve(void);

/* The honest audio gate: true only once the I2S TX channel has actually claimed its DMA (via the boot
 * reserve or a successful lazy open). A UI toggle that plays a cue should gate on THIS, not on
 * nocsif_audio_available() (which is true whenever the worker exists, even if I2S never opened). */
bool nocsif_audio_tx_ready(void);

/* Queue a single synthesized tone: sine at freq_hz for ms milliseconds at volume_pct (0..100).
 * Non-blocking (played on the worker). NOT gated by mute — this is the on-demand hardware test.
 * Duration is clamped to a sane ceiling; a zero freq/ms/volume is dropped. */
void nocsif_audio_tone(uint32_t freq_hz, uint32_t ms, uint8_t volume_pct);

/* Queue a named cue. Non-blocking. No-op when muted (except this is how the UI feedback sounds are
 * routed, so muting silences them). */
void nocsif_audio_cue(nocsif_audio_cue_t cue);

/* Mute/unmute the cue layer (persists to the "sound_en" NVS setting). Does not affect an
 * already-queued sound or the raw nocsif_audio_tone test. */
void nocsif_audio_set_muted(bool muted);

/* Current mute state (cached; no I2C/NVS). */
bool nocsif_audio_muted(void);

/* ---- master speaker volume (E1·2) ----------------------------------------- *
 * A 0..255 scalar applied to CUES and voice-memo playback (NOT to the raw nocsif_audio_tone test,
 * which stays at its requested level — it is the hardware test). Persisted to the "spk_vol" setting.
 * This is the watch's own speaker level; the Control-Center media slider is a SEPARATE, phone-facing
 * control. Default is full (no change to the shipped cue loudness). */
void    nocsif_audio_set_volume(uint8_t vol_0_255);
uint8_t nocsif_audio_volume(void);

/* ---- boot / USB-plug sound toggles (E1·2) --------------------------------- *
 * Independent on/off flags (persisted to "boot_snd" / "usb_snd", default on) layered UNDER the master
 * mute: a cue plays only when NOT muted AND its specific flag is on. Play the two event cues through
 * these gated helpers rather than nocsif_audio_cue directly. */
void nocsif_audio_set_boot_sound(bool on);
bool nocsif_audio_boot_sound(void);
void nocsif_audio_set_usb_sound(bool on);
bool nocsif_audio_usb_sound(void);
void nocsif_audio_set_shake_sound(bool on);   /* gates the shake wake/sleep blips ("shake_snd") */
bool nocsif_audio_shake_sound(void);

/* Play the boot chime, gated by (not muted) AND boot-sound-on. Called once at boot (main.c). */
void nocsif_audio_boot_cue(void);

/* Play the USB-connected cue, gated by (not muted) AND usb-sound-on. Called on a VBUS rising edge
 * (buttons.c). Non-blocking; safe from any task. */
void nocsif_audio_usb_cue(void);

/* Play a shake wake/sleep blip (cue must be NOCSIF_AUDIO_WAKE or _SLEEP), gated by (not muted) AND
 * shake-sound-on. Called from the shake gesture handler (ui.c). */
void nocsif_audio_shake_cue(nocsif_audio_cue_t cue);

/* ---- file playback (Phase B: Carts; also voice memos) ---------------------- *
 * Play ANY PCM WAV (8/16/24/32-bit integer or 32-bit float, mono or stereo (downmixed), 8–48 kHz) or an
 * MP3 (minimp3; MPEG-1/2 layer III, any bitrate incl. VBR, mono/stereo; ID3 tags skipped) from /sd
 * through the amp — by extension (.mp3 = MP3, anything else = WAV). The I2S clock is reconfigured per file
 * (tones/cues restore their own 16 kHz). STREAMED from the card in 16 KB blocks under short /sd locks: no
 * whole-file buffer, any length. Scaled by the master volume; `makeup_x100` is an extra gain in percent through the
 * soft-knee limiter (100 = unity for published/normalised tones; voice memos use 240 because raw PDM
 * speech is quiet). Non-blocking: the path is copied and the worker plays it. A request while another
 * file is playing STOPS that one and plays this (tap-to-switch). Path length is bounded. */
void nocsif_audio_play_file(const char *path, uint16_t makeup_x100);

/* Voice-memo convenience: nocsif_audio_play_file(path, 240) — the E1·3 make-up gain (§4.13 fix #5). */
void nocsif_audio_play_wav(const char *path);

/* Stop the file currently playing (no-op when idle). Non-blocking; the worker drains within one chunk. */
void nocsif_audio_stop(void);

/* True while a file is playing (the UI can show "playing…"). Cached. */
bool nocsif_audio_playing(void);

/* Copy the path of the file currently playing into `out` ("" when idle); returns nocsif_audio_playing().
 * Spinlock-guarded copy — LVGL-safe (used by the Carts "now playing" status line). */
bool nocsif_audio_playing_path(char *out, size_t out_len);

#ifdef __cplusplus
}
#endif
