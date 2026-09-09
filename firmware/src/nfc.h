/*
 * NocSif — public API for the NFC (ST25R3916) worker task.
 *
 * C-callable wrapper around the C++ RFAL driver. A background worker task owns all
 * SPI traffic; callers only post a request and poll cached status/readout strings —
 * safe to call from the LVGL task since nothing here touches hardware directly.
 * Bring-up is lazy (deferred to the first read) and is skipped entirely in safe mode.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Start the worker task if it isn't already running. Idempotent. Under safe mode this
 * does nothing (no task created), so nocsif_nfc_available() stays false. */
esp_err_t nocsif_nfc_init(void);

/* Ask the worker for one NFC-A scan cycle. Returns immediately; the first call also
 * triggers chip bring-up. Has no effect if a scan is already running. */
void nocsif_nfc_request_read(void);

/* Ask the worker to run the antenna/RF hardware self-test (no tag required) before its
 * next scan, then log a pass/fail verdict over serial. Returns immediately. */
void nocsif_nfc_request_selftest(void);

/* True once the chip has been successfully brought up; false before that, in safe mode,
 * or after a failed init. */
bool nocsif_nfc_available(void);

/* Short status string for the NFC row (e.g. "tap", "scan", a UID, "none", "err", "off").
 * Cached, hardware-free — safe to call from the LVGL task. */
const char *nocsif_nfc_status_str(void);

/* Longer status line with the last scan's detail (UID, or the current error/state).
 * Cached, hardware-free — safe to call from the LVGL task. */
const char *nocsif_nfc_readout_str(void);

/* RF front-end / antenna self-test verdict, set by the worker's hw_selftest (triggered by
 * nocsif_nfc_request_selftest). PENDING until a self-test produces a verdict; the rest are the
 * plain-English outcomes the serial log already prints. The desktop-bridge hardware check reads this to
 * VERIFY the NFC RF path (this reference unit's TX path is dead → TX_DEAD). A cached int — no hardware
 * access, safe from any task. */
typedef enum {
    NOCSIF_NFC_TEST_PENDING = 0,  /* requested but no verdict yet (or never run)                        */
    NOCSIF_NFC_TEST_OK,           /* TX on + antenna amplitude rose — RF front-end + antenna radiate OK  */
    NOCSIF_NFC_TEST_SHORTED,      /* over-current while driving — shorted antenna / matching (HW fault)  */
    NOCSIF_NFC_TEST_OPEN,         /* TX asserted, amplitude flat — open antenna / broken match (HW fault) */
    NOCSIF_NFC_TEST_TX_DEAD,      /* TX never engaged (tx_on stays 0) — driver output stage / chip TX fault */
    NOCSIF_NFC_TEST_DIGITAL,      /* chip not answering on SPI (wiring / power)                          */
} nocsif_nfc_test_t;
nocsif_nfc_test_t nocsif_nfc_selftest_result(void);

#ifdef __cplusplus
}
#endif
