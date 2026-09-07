/*
 * NocSif — NFC (ST25R3916 / RFAL) worker + UI glue (M6-P1).
 *
 * Thin extern-"C" shim over the C++ `rfal` component. All SPI I/O runs on a dedicated
 * worker task (the LVGL callback only *requests* a read, like ducky.c / usb_gadget.c):
 *   - Lazy bring-up on the first read request (NFC rail on -> SPI3 2nd device -> RFAL
 *     rfalNfcInitialize), so boot stays fast and the risk is isolated to first use.
 *   - Safe-mode gated (nocsif_reliability_safe_mode): NFC is skipped after a boot loop.
 *   - Discovery polls NFC-A and reports the tag UID.
 *
 * The status/readout getters return cached, module-owned strings (no I2C/SPI) so they
 * are safe to call from the LVGL task via the P4.2 live-label hook.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Create the idle worker task. Idempotent; safe to call from build_nfc() (LVGL task) as
 * the lazy trigger on first NFC-screen entry. In reliability safe mode this is a no-op and
 * nocsif_nfc_available() stays false. Returns ESP_OK once the task exists (or in safe mode). */
esp_err_t nocsif_nfc_init(void);

/* Request one NFC-A discovery cycle: non-blocking and LVGL-callback-safe (only signals the
 * worker). The first request also performs the lazy chip bring-up. Ignored while a read is
 * already in progress. Results land in the status/readout strings within ~a scan. */
void nocsif_nfc_request_read(void);

/* Request the ST25R3916 RF front-end / antenna hardware self-test (no tag needed): brings the
 * chip up, reads the internal supply rails, drives the carrier via the real read path + a raw
 * register poke, and logs a plain-English verdict (radiates OK / shorted / open / TX won't
 * start) over serial. Then runs a normal discovery. This is the M6-P1 grab-and-run bring-up
 * test — trigger it at boot with -DNOCSIF_NFC_BOOT_SELFTEST=1 (see main.c) on a new unit.
 * Non-blocking / LVGL-callback-safe. See docs/RESUME.md for the build+flash+capture recipe. */
void nocsif_nfc_request_selftest(void);

/* False in safe mode or if the chip failed to initialise; true once rfalNfcInitialize has
 * succeeded. (Before the first read it is false — bring-up is lazy.) No hardware access. */
bool nocsif_nfc_available(void);

/* Compact live status for the Read Tag row's right-side tag ("tap" / "scan" / a short UID /
 * "none" / "err" / "off"). Module-owned buffer, stable between updates. No hardware access —
 * safe on the LVGL task (this is the getter for the live-label hook). */
const char *nocsif_nfc_status_str(void);

/* Full live readout line for the detail label under the NFC rows (UID + type, or the current
 * state / error). Module-owned buffer. No hardware access — safe on the LVGL task. */
const char *nocsif_nfc_readout_str(void);

#ifdef __cplusplus
}
#endif
