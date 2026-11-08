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
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Start the worker task if it isn't already running. Idempotent. Under safe mode this
 * does nothing (no task created), so nocsif_nfc_available() stays false. */
esp_err_t nocsif_nfc_init(void);

/* Boot-reserve the ST25R3916 SPI DMA bounce (~520 B int-DMA) from the pristine pool, before
 * esp_wifi_init fragments it. Call once from app_main's reserve block (beside the ble/audio/usb/
 * mic reserves). Cheap, no hardware access — just claims the buffer NFC's SPI transfers stage
 * through, so a later Read never fails its lazy claim under fragmentation. See docs/RAM-BUDGET.md. */
void nocsif_nfc_boot_reserve(void);

/* Request one NFC-A discovery cycle: non-blocking and LVGL-callback-safe (only signals the
 * worker). The first request also performs the lazy chip bring-up. Ignored while a read is
 * already in progress. Results land in the status/readout strings within ~a scan. */
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
 * VERIFY the NFC RF path. The verdict is driven by RFAL's own field-on handshake (APON+CAT) and by a
 * live tag read, NOT by the AUX tx_on bit / amplitude delta (those read flat even on a known-good unit
 * — see hw_selftest). A cached int — no hardware access, safe from any task. */
typedef enum {
    NOCSIF_NFC_TEST_PENDING = 0,  /* requested but no verdict yet (or never run)                        */
    NOCSIF_NFC_TEST_OK,           /* field-on handshake completed (or a tag was read) — front-end radiates */
    NOCSIF_NFC_TEST_SHORTED,      /* over-current while driving — shorted antenna / matching (HW fault)  */
    NOCSIF_NFC_TEST_OPEN,         /* (legacy; not emitted) open antenna / broken match                   */
    NOCSIF_NFC_TEST_TX_DEAD,      /* field-on handshake never completed (no APON/CAT) — TX driver fault  */
    NOCSIF_NFC_TEST_DIGITAL,      /* chip not answering on SPI (wiring / power)                          */
} nocsif_nfc_test_t;
nocsif_nfc_test_t nocsif_nfc_selftest_result(void);

/* ---- Universal reader (M6-P2): clone-grade read result + auto-detect scan ----------
 *
 * The read flow is a small state machine the UI drives: nocsif_nfc_scan_start() turns the
 * field on and auto-detects a tag (WAITING "place tag" -> READING -> DONE); the worker then
 * gathers EVERYTHING it can and publishes it as a clone-grade snapshot (identity bytes +
 * geometry + raw memory image) for the on-screen detail view and the later save/copy step.
 * The ST25R3916 is 13.56 MHz HF only — 125 kHz LF tags cannot be read on this hardware.
 * All getters are module-owned and take no hardware access (safe on the LVGL task). */

typedef enum {
    NOCSIF_NFC_FAM_NONE = 0,
    NOCSIF_NFC_FAM_A,       /* ISO14443A (NFC-A)          */
    NOCSIF_NFC_FAM_B,       /* ISO14443B (NFC-B)          */
    NOCSIF_NFC_FAM_F,       /* FeliCa / ISO18092 (NFC-F)  */
    NOCSIF_NFC_FAM_V,       /* ISO15693 (NFC-V)           */
    NOCSIF_NFC_FAM_ST25TB,  /* ST25TB / ISO14443-2B'      */
} nocsif_nfc_family_t;

typedef enum {
    NOCSIF_NFC_SCAN_IDLE = 0, /* not scanning                                   */
    NOCSIF_NFC_SCAN_WAITING,  /* field on, waiting for a tag ("place tag")      */
    NOCSIF_NFC_SCAN_READING,  /* tag detected, gathering its contents           */
    NOCSIF_NFC_SCAN_DONE,     /* read complete; nocsif_nfc_last_info() is valid */
    NOCSIF_NFC_SCAN_ERROR,    /* bring-up / bus error                           */
} nocsif_nfc_scan_state_t;

#define NOCSIF_NFC_UID_MAX   10
#define NOCSIF_NFC_VER_MAX   8

typedef struct {
    bool                present;      /* a tag was activated on the last read            */
    nocsif_nfc_family_t family;
    char                tech[8];      /* "NFC-A" / "NFC-B" / "NFC-F" / "NFC-V"            */
    char                standard[48]; /* human protocol, e.g. "ISO14443-3A / Type 2"     */
    char                model[48];    /* best-effort chip model, e.g. "NTAG215"          */

    uint8_t  uid[NOCSIF_NFC_UID_MAX]; /* active device UID / NFCID                        */
    uint8_t  uid_len;

    bool     has_atqa_sak;            /* NFC-A only                                       */
    uint8_t  atqa[2];                 /* SENS_RES                                         */
    uint8_t  sak;                     /* SEL_RES                                          */

    uint8_t  version[NOCSIF_NFC_VER_MAX]; /* raw GET_VERSION (NFC-A), if the tag answered */
    uint8_t  version_len;

    bool     has_dsfid;               /* NFC-V only                                       */
    uint8_t  dsfid;

    uint32_t mem_bytes;               /* total user memory, 0 = unknown                   */
    uint16_t block_count;             /* 0 = unknown                                      */
    uint8_t  block_size;              /* bytes per block/page, 0 = unknown                */
    uint16_t read_blocks;             /* blocks actually read into the dump               */

    int8_t   writable;                /* -1 unknown, 0 locked/no, 1 yes/partial           */

    /* EMV (contactless payment card) read-out — set only when a Type-4 / ISO-DEP card answered the EMV
     * application-select flow (SELECT PPSE -> SELECT AID -> GET PROCESSING OPTIONS -> READ RECORD). This
     * is the Flipper-style card data pulled straight off the card's own records; the detail screen shows
     * these instead of the generic UID line when emv_present is true. Module-owned, no hardware access. */
    bool     emv_present;
    char     emv_scheme[16];          /* "Visa" / "Mastercard" / "Amex" / ... from the AID (RID)      */
    char     emv_label[24];           /* Application Label (TLV 50), e.g. "VISA CREDIT"               */
    char     emv_pan[24];             /* PAN grouped in 4s, e.g. "4111 1111 1111 1111"                */
    char     emv_expiry[6];           /* "MM/YY"                                                      */
    char     emv_name[28];            /* cardholder name (TLV 5F20) if the card exposes it            */
} nocsif_nfc_info_t;

/* Begin / cancel the auto-detect read flow. Idempotent; no-ops in safe mode. */
void nocsif_nfc_scan_start(void);
void nocsif_nfc_scan_stop(void);

/* Current read-flow state (the UI drives its screen off this). No hardware access. */
nocsif_nfc_scan_state_t nocsif_nfc_scan_state(void);

/* Clone-grade snapshot of the last completed read. Never NULL; .present stays false until
 * the first successful activation. No hardware access — safe on the LVGL task. */
const nocsif_nfc_info_t *nocsif_nfc_last_info(void);

/* Raw memory image captured on the last read (T2T pages / NFC-V blocks), for the hex view
 * and the upcoming save/copy step. Returns the byte count; *buf is module-owned and valid
 * while the result screen is up. */
size_t nocsif_nfc_last_dump(const uint8_t **buf);

#ifdef __cplusplus
}
#endif
