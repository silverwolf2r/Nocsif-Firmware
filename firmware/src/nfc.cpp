/*
 * NocSif — NFC worker task implementation. See nfc.h.
 *
 * Built as C++ so it can call the vendored RFAL driver's classes directly; nfc.h exposes
 * a plain C API to the rest of the firmware.
 *
 * On first use: power the NFC rail, bring up SPI3 as a second device with manual chip
 * select, then run rfalNfcInitialize() to prove the chip is alive. A scan then calls
 * rfalNfcDiscover() and pumps the RFAL state machine until a tag activates or it times out.
 *
 * The ST25R3916 shares SPI3 with the microSD card, so the whole scan is done while
 * holding the SD lock to keep the two peripherals from interleaving on the bus.
 */
#include "nfc.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/spi_common.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_random.h"     /* esp_random — EMV unpredictable number in the PDOL */

#include "power.h"          /* NFC rail power */
#include "sdcard.h"         /* shared SPI3 lock */
#include "reliability.h"    /* safe-mode flag */
#include "freertos/idf_additions.h" /* xTaskCreateWithCaps, for a PSRAM-backed task stack */
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"       /* checks whether a pointer lands in PSRAM */

/* Vendored C++ RFAL driver. */
#include "rfal_rfst25r3916.h"
#include "rfal_nfc.h"

static const char *TAG = "nfc";

/* ---- hardware wiring (see docs/HARDWARE.md) -------------------------------------- */
#define NFC_SPI_HOST     SPI3_HOST
#define NFC_PIN_MOSI     34
#define NFC_PIN_MISO     33
#define NFC_PIN_SCK      35
#define NFC_PIN_CS       4        /* driven manually by RFAL, not by the SPI driver */
#define NFC_PIN_IRQ      5        /* active-high, polled rather than interrupt-driven */
#define NFC_PIN_LORA_CS  36       /* another device on the same bus; held high when idle */
#define NFC_SPI_HZ       5000000

#define NFC_DISCOVER_MS  2000U    /* how long each discovery poll cycle runs */
#define NFC_SCAN_TMO_MS  2600U    /* overall deadline for one scan attempt */
#define NFC_MAX_UID      10       /* longest NFC-A UID (triple NFCID1) */

/* ---- published state: worker writes one buffer, readers see the other (no locking) --- */
static char           s_status[2][16];
static char           s_readout[2][192];
static volatile int   s_status_i;
static volatile int   s_readout_i;

static void publish_status(const char *s)
{
    int n = s_status_i ^ 1;
    snprintf(s_status[n], sizeof s_status[n], "%s", s);
    s_status_i = n;
}
static void publish_readout(const char *s)
{
    int n = s_readout_i ^ 1;
    snprintf(s_readout[n], sizeof s_readout[n], "%s", s);
    s_readout_i = n;
}

/* ---- clone-grade read result (M6-P2 universal reader) ------------------------------
 * Double-buffered like the strings: the worker fills the back slot and flips the index
 * once the read is complete, so an LVGL getter always sees a consistent snapshot. */
static nocsif_nfc_info_t s_info[2];
static volatile int      s_info_i;

static nocsif_nfc_info_t *info_begin(void)   /* back slot, zeroed; the worker fills it */
{
    int n = s_info_i ^ 1;
    memset(&s_info[n], 0, sizeof s_info[n]);
    s_info[n].writable = -1;
    return &s_info[n];
}
static void info_commit(void) { s_info_i ^= 1; }   /* publish the back slot */

/* Raw memory image of the last read (T2T pages / NFC-V blocks). PSRAM, allocated lazily
 * at bring-up; the worker writes it during READING, the UI reads it only in DONE. */
static uint8_t        *s_dump;
static size_t          s_dump_cap;
static volatile size_t s_dump_len;

/* Read-flow state machine (nocsif_nfc_scan_state_t). */
static volatile int    s_scan_state;   /* NOCSIF_NFC_SCAN_* */
static volatile bool   s_scan_on;      /* UI wants the auto-detect loop running */
static void set_scan_state(nocsif_nfc_scan_state_t s) { s_scan_state = (int)s; }

/* ---- module state ----------------------------------------------------------------- */
static SPIClass               s_spi;              /* Arduino-style SPI shim RFAL talks to */
static spi_device_handle_t    s_spi_dev;
static RfalRfST25R3916Class  *s_reader;
static RfalNfcClass          *s_nfc;
static TaskHandle_t           s_task;
static bool                   s_brought_up;       /* chip init has succeeded */
static volatile bool          s_available;        /* public mirror of s_brought_up */
static volatile bool          s_selftest_req;     /* run the HW self-test on the next scan */
static volatile int           s_test_result;      /* nocsif_nfc_test_t verdict of the last self-test (PENDING=0) */

bool nocsif_nfc_available(void) { return s_available; }
const char *nocsif_nfc_status_str(void)  { return s_status[s_status_i]; }
const char *nocsif_nfc_readout_str(void) { return s_readout[s_readout_i]; }
nocsif_nfc_test_t nocsif_nfc_selftest_result(void) { return (nocsif_nfc_test_t)s_test_result; }

const nocsif_nfc_info_t *nocsif_nfc_last_info(void) { return &s_info[s_info_i]; }
size_t nocsif_nfc_last_dump(const uint8_t **buf) { if (buf) { *buf = s_dump; } return s_dump_len; }
nocsif_nfc_scan_state_t nocsif_nfc_scan_state(void) { return (nocsif_nfc_scan_state_t)s_scan_state; }

/* Boot-reserve the ST25R3916 SPI DMA bounce from the pristine int-DMA pool, before WiFi inits.
 * The ~520 B bounce routes every RFAL block/register transfer (the shared SPI3 bus runs DMA, so
 * RFAL's small unaligned stack buffers must be staged through it). Claimed lazily on first NFC
 * use it fails under steady-state fragmentation (largest int-DMA measured as low as 92 B), so the
 * chip-ID read comes back stale and rfalNfcInitialize returns ERR_HW_MISMATCH(36) every retry —
 * NFC never reads. Claimed here (main.c reserve block, beside ble/audio/usb/mic) it lands while
 * the pool is one unbroken run and is held for the session. See docs/RAM-BUDGET.md. */
void nocsif_nfc_boot_reserve(void)
{
    if (SPIClass::reserveBounce()) {
        ESP_LOGI(TAG, "NFC SPI DMA bounce reserved (int-DMA) — NFC read is fragmentation-proof");
    } else {
        ESP_LOGW(TAG, "NFC SPI DMA bounce reserve FAILED at boot — NFC may not read (int-DMA low)");
    }
}

/* Ensure the shared SPI3 bus is up (sdcard.c normally did this at boot) and add the
 * ST25R3916 as a 2nd device with manual CS. Returns false on a hard SPI error. */
static bool ensure_spi(void)
{
    if (s_spi_dev != NULL) {
        return true;
    }
    const spi_bus_config_t bus_cfg = {
        .mosi_io_num = NFC_PIN_MOSI,
        .miso_io_num = NFC_PIN_MISO,
        .sclk_io_num = NFC_PIN_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    esp_err_t err = spi_bus_initialize(NFC_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {   /* INVALID_STATE = already up */
        ESP_LOGE(TAG, "spi_bus_initialize(SPI3) failed: %s", esp_err_to_name(err));
        return false;
    }
    /* Hold the LoRa chip select high so it can't interfere with the shared bus. */
    gpio_config_t cs = {
        .pin_bit_mask = 1ULL << NFC_PIN_LORA_CS,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&cs);
    gpio_set_level((gpio_num_t)NFC_PIN_LORA_CS, 1);

    /* Register the ST25R3916 with no driver-managed CS — RFAL toggles GPIO4 itself. */
    spi_device_interface_config_t dev_cfg = {};
    dev_cfg.clock_speed_hz = NFC_SPI_HZ;
    dev_cfg.mode           = 1;
    dev_cfg.spics_io_num   = -1;
    dev_cfg.queue_size     = 3;
    err = spi_bus_add_device(NFC_SPI_HOST, &dev_cfg, &s_spi_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device(NFC) failed: %s", esp_err_to_name(err));
        s_spi_dev = NULL;
        return false;
    }
    s_spi.setHandle(s_spi_dev);
    ESP_LOGI(TAG, "ST25R3916 on SPI3 as 2nd device (CS=%d manual, IRQ=%d, %d Hz)",
             NFC_PIN_CS, NFC_PIN_IRQ, NFC_SPI_HZ);
    return true;
}

/* --- NFC antenna/RF hardware self-test ---------------------------------------------
 * Checks whether the RF front-end and antenna are healthy without needing a tag present,
 * using the chip's own measurement registers, and logs a plain verdict.
 *
 * It reads the internal supply rails, then drives the carrier both through RFAL's normal
 * field-on path and via a raw register write, measuring the antenna amplitude before and
 * after. A clear amplitude rise means the coil is driven and resonating correctly; TX
 * turning on with no amplitude change points to an antenna/matching fault; TX never turning
 * on points to a transmitter or configuration problem instead. */
static void hw_selftest(void)
{
    RfalRfST25R3916Class *r = s_reader;
    ESP_LOGW(TAG, "==== NFC HW SELF-TEST (ST25R3916, no tag needed) ====");

    /* 1) Confirm the chip answers on SPI at all. */
    uint8_t rev = 0xFF;
    bool idok = r->st25r3916CheckChipID(&rev);
    ESP_LOGW(TAG, "1) digital/SPI: chip_id_ok=%d rev=0x%02X  %s",
             idok, rev, idok ? "(SPI + chip alive)" : "(SPI/chip NOT answering)");

    /* 2) Start the oscillator and check the internal regulator came up. */
    r->st25r3916OscOn();
    uint16_t reg_mV = 0;
    ReturnCode ar = r->st25r3916AdjustRegulators(&reg_mV);
    ESP_LOGW(TAG, "2) regulators : adjust=%d reg=%u mV  %s",
             (int)ar, (unsigned)reg_mV, (reg_mV > 1500) ? "(LDO OK)" : "(LDO LOW/FAIL)");

    /* 3) Read all internal supply rails with the field off; a dead RF rail here points
     *    straight at a hardware fault rather than an antenna coupling issue. */
    uint16_t vdd    = r->st25r3916MeasureVoltage(ST25R3916_REG_REGULATOR_CONTROL_mpsv_vdd);
    uint16_t vdd_a  = r->st25r3916MeasureVoltage(ST25R3916_REG_REGULATOR_CONTROL_mpsv_vdd_a);
    uint16_t vdd_d  = r->st25r3916MeasureVoltage(ST25R3916_REG_REGULATOR_CONTROL_mpsv_vdd_d);
    uint16_t vdd_rf = r->st25r3916MeasureVoltage(ST25R3916_REG_REGULATOR_CONTROL_mpsv_vdd_rf);
    uint16_t vdd_am = r->st25r3916MeasureVoltage(ST25R3916_REG_REGULATOR_CONTROL_mpsv_vdd_am);
    ESP_LOGW(TAG, "3) supplies   : VDD=%u VDD_A=%u VDD_D=%u VDD_RF=%u VDD_AM=%u (mV)",
             vdd, vdd_a, vdd_d, vdd_rf, vdd_am);

    /* 4) Record baseline amplitude/phase/registers with the field still off. */
    uint8_t amp_off = 0, ph_off = 0, op_off = 0, aux_off = 0;
    r->st25r3916MeasureAmplitude(&amp_off);
    r->st25r3916MeasurePhase(&ph_off);
    r->st25r3916ReadRegister(ST25R3916_REG_OP_CONTROL, &op_off);
    r->st25r3916ReadRegister(ST25R3916_REG_AUX_DISPLAY, &aux_off);
    ESP_LOGW(TAG, "4) field OFF  : amp=0x%02X phase=0x%02X OP_CTRL=0x%02X AUX=0x%02X",
             amp_off, ph_off, op_off, aux_off);

    /* 5) Log the TX driver strength and antenna-tuning trim left by the earlier init. */
    uint8_t txdrv = 0, anta = 0, antb = 0;
    r->st25r3916ReadRegister(ST25R3916_REG_TX_DRIVER, &txdrv);
    r->st25r3916ReadRegister(ST25R3916_REG_ANT_TUNE_A, &anta);
    r->st25r3916ReadRegister(ST25R3916_REG_ANT_TUNE_B, &antb);
    ESP_LOGW(TAG, "5) driver cfg : TX_DRIVER=0x%02X ANT_TUNE_A=0x%02X ANT_TUNE_B=0x%02X", txdrv, anta, antb);

    /* 6) Turn the carrier on the same way a real scan would (through RFAL's field-on
     *    call, which needs poller mode set first). Poll the AUX register quickly to catch
     *    a brief tx_on, and check for an over-current trip and the loaded VDD_RF value. */
    s_nfc->rfalNfcaPollerInitialize();
    ReturnCode fon = r->rfalFieldOnAndStartGT();
    uint8_t aux_or_c = 0, aux_s = 0;
    bool tx_seen_c = false;
    for (int i = 0; i < 40; i++) {
        r->st25r3916ReadRegister(ST25R3916_REG_AUX_DISPLAY, &aux_s);
        aux_or_c |= aux_s;
        if (aux_s & ST25R3916_REG_AUX_DISPLAY_tx_on) { tx_seen_c = true; }
    }
    uint16_t vdd_rf_c = r->st25r3916MeasureVoltage(ST25R3916_REG_REGULATOR_CONTROL_mpsv_vdd_rf);
    uint8_t amp_c = 0, op_c = 0, aux_c = 0, regres_c = 0;
    r->st25r3916MeasureAmplitude(&amp_c);
    r->st25r3916ReadRegister(ST25R3916_REG_OP_CONTROL, &op_c);
    r->st25r3916ReadRegister(ST25R3916_REG_AUX_DISPLAY, &aux_c);
    r->st25r3916ReadRegister(ST25R3916_REG_REGULATOR_RESULT, &regres_c);
    bool tx_on_c = (aux_c & ST25R3916_REG_AUX_DISPLAY_tx_on) != 0;
    bool ilim_c  = (regres_c & ST25R3916_REG_REGULATOR_RESULT_i_lim) != 0;
    r->rfalFieldOff();
    ESP_LOGW(TAG, "6) RFAL field : fieldOn=%d amp=0x%02X OP_CTRL=0x%02X AUX=0x%02X(or=0x%02X) tx_on=%d tx_seen=%d i_lim=%d VDD_RF=%u",
             (int)fon, amp_c, op_c, aux_c, aux_or_c, tx_on_c, tx_seen_c, ilim_c, (unsigned)vdd_rf_c);

    /* 7) Turn the carrier on the bare-metal way — a direct register write that skips RFAL's
     *    collision avoidance entirely — and repeat the same amplitude/over-current checks. */
    uint8_t opon = (uint8_t)(ST25R3916_REG_OP_CONTROL_en |
                             ST25R3916_REG_OP_CONTROL_tx_en |
                             ST25R3916_REG_OP_CONTROL_rx_en);   /* en_fd = 00 = efd_off */
    r->st25r3916WriteRegister(ST25R3916_REG_OP_CONTROL, opon);
    uint8_t aux_or_d = 0;
    bool tx_seen_d = false;
    for (int i = 0; i < 40; i++) {
        r->st25r3916ReadRegister(ST25R3916_REG_AUX_DISPLAY, &aux_s);
        aux_or_d |= aux_s;
        if (aux_s & ST25R3916_REG_AUX_DISPLAY_tx_on) { tx_seen_d = true; }
    }
    uint8_t amp_d = 0, op_d = 0, regres_d = 0;
    r->st25r3916MeasureAmplitude(&amp_d);
    r->st25r3916ReadRegister(ST25R3916_REG_OP_CONTROL, &op_d);
    r->st25r3916ReadRegister(ST25R3916_REG_REGULATOR_RESULT, &regres_d);
    bool ilim_d = (regres_d & ST25R3916_REG_REGULATOR_RESULT_i_lim) != 0;
    r->st25r3916WriteRegister(ST25R3916_REG_OP_CONTROL, ST25R3916_REG_OP_CONTROL_en);
    ESP_LOGW(TAG, "7) direct TX  : amp=0x%02X OP_CTRL=0x%02X AUX(or)=0x%02X tx_seen=%d i_lim=%d REGRES=0x%02X",
             amp_d, op_d, aux_or_d, tx_seen_d, ilim_d, regres_d);

    /* 8) Verdict.
     *
     * The authoritative "does the front-end radiate" signal is RFAL's own field-on handshake, not the
     * register scrape below: rfalFieldOnAndStartGT() runs Initial RF Collision Avoidance and returns
     * ERR_NONE only after the chip raises APON (analog power-on) *and* CAT (field on, guard time met) —
     * i.e. the power amplifier actually started and the antenna field physically came up. On the dead
     * reference unit that handshake never completes.
     *
     * The legacy proxies — the AUX_DISPLAY tx_on bit and the amp(on)-amp(off) delta — do NOT track the
     * live field on this chip + RFAL build: on a KNOWN-GOOD unit (field-on OK, a tag read immediately
     * after) they read exactly as flat (tx_on=0, delta~1) as they did on the dead unit, so they never
     * discriminated. They are logged for diagnostics only and are no longer gated on. Finally, a
     * successful discovery right after (do_read) upgrades the verdict to OK unconditionally — energizing
     * and reading a tag is unanswerable proof the field radiates. */
    int  amp_delta  = (int)amp_c - (int)amp_off;              /* diagnostic only (unreliable on this HW) */
    bool tx_ever    = tx_seen_c || tx_on_c || tx_seen_d;      /* diagnostic only */
    bool over_curr  = ilim_c || ilim_d;
    bool field_up   = (fon == ERR_NONE);                      /* APON+CAT confirmed — carrier came up */
    ESP_LOGW(TAG, "---- VERDICT (fieldOn=%d field_up=%d over_current=%d | diag: amp_delta=%d tx_ever=%d) ----",
             (int)fon, field_up, over_curr, amp_delta, tx_ever);
    if (!idok) {
        s_test_result = NOCSIF_NFC_TEST_DIGITAL;
        ESP_LOGW(TAG, "  DIGITAL FAULT: chip not answering on SPI -> wiring/power; retest first.");
    } else if (over_curr) {
        s_test_result = NOCSIF_NFC_TEST_SHORTED;
        ESP_LOGW(TAG, "  Over-current tripped while driving the antenna ->");
        ESP_LOGW(TAG, "  SHORTED antenna / matching network: HARDWARE fault.");
    } else if (field_up) {
        s_test_result = NOCSIF_NFC_TEST_OK;
        ESP_LOGW(TAG, "  Field-on handshake (APON+CAT) completed -> RF front-end + antenna RADIATE OK.");
        ESP_LOGW(TAG, "  Hardware is fine; a no-read is discovery-logic / positioning / tag.");
    } else if (fon == ERR_RF_COLLISION) {
        /* Preliminary only — a tag read in discover_once() still upgrades this to OK. An external field
         * aborted our collision-avoidance, so our own carrier was not confirmed on this pass. */
        s_test_result = NOCSIF_NFC_TEST_TX_DEAD;
        ESP_LOGW(TAG, "  Collision avoidance saw an EXTERNAL field -> our field was not confirmed.");
        ESP_LOGW(TAG, "  Inconclusive: move away from other readers and retest (a tag read still proves OK).");
    } else {
        s_test_result = NOCSIF_NFC_TEST_TX_DEAD;
        ESP_LOGW(TAG, "  Field-on handshake did not complete (no APON/CAT, fieldOn=%d), no over-current ->", (int)fon);
        ESP_LOGW(TAG, "  TX driver / antenna / analog-config fault: no carrier came up.");
    }
    ESP_LOGW(TAG, "=====================================================");
}

/* Power the NFC rail, bring up SPI, and run RFAL init. Called from the worker task
 * while it holds the SD lock. */
static void bring_up(void)
{
    esp_err_t perr = nocsif_power_nfc_rail(true);
    if (perr != ESP_OK) {
        ESP_LOGE(TAG, "NFC rail enable failed: %s", esp_err_to_name(perr));
        publish_status("err");
        publish_readout("NFC rail failed — PMU not ready.");
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(25));   /* let DLDO1 + the ST25R3916 power-on settle before the first SPI read */

    if (!ensure_spi()) {
        publish_status("err");
        publish_readout("SPI bus init failed.");
        return;
    }

    if (s_reader == NULL) {
        s_reader = new RfalRfST25R3916Class(&s_spi, NFC_PIN_CS, NFC_PIN_IRQ, NFC_SPI_HZ);
        s_nfc    = new RfalNfcClass(s_reader);
    }

    /* The first IC_IDENTITY read right after the rail comes up is occasionally racy — the
     * ST25R3916 isn't fully settled (or a glitch on the shared SPI3 bus) — so rfalNfcInitialize
     * returns ERR_HW_MISMATCH (36) and the Read screen used to dead-end at "NFC error" until a
     * reboot. The chip is known-good (it reads tags), so re-attempt a few times with a short
     * settle (each rfalNfcInitialize re-issues SET_DEFAULT, so a retry is a clean restart). */
    ReturnCode err = ERR_NONE;
    for (int attempt = 0; attempt < 4; attempt++) {
        err = s_nfc->rfalNfcInitialize();
        if (err == ERR_NONE) {
            if (attempt > 0) ESP_LOGW(TAG, "rfalNfcInitialize OK on attempt %d", attempt + 1);
            break;
        }
        ESP_LOGW(TAG, "rfalNfcInitialize attempt %d failed: %d — retrying", attempt + 1, (int)err);
        vTaskDelay(pdMS_TO_TICKS(30));
    }
    if (err != ERR_NONE) {
        ESP_LOGE(TAG, "rfalNfcInitialize failed: %d (rail/SPI/IRQ/chip)", (int)err);
        publish_status("err");
        publish_readout("Init failed — check rail / SPI / IRQ.");
        /* Free the RFAL objects so the next attempt starts bring-up from scratch. */
        delete s_nfc;    s_nfc = NULL;
        delete s_reader; s_reader = NULL;
        return;
    }
    s_brought_up = true;
    s_available  = true;
    ESP_LOGI(TAG, "rfalNfcInitialize OK — ST25R3916 up");

    /* Clone-grade dump buffer (PSRAM): T2T pages / NFC-V blocks land here for the hex view
     * and the later save/copy step. 4 KB covers MIFARE 4K / NTAG216 / typical NFC-V. */
    if (s_dump == NULL) {
        s_dump     = (uint8_t *)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
        s_dump_cap = (s_dump != NULL) ? 4096 : 0;
    }

    publish_readout("Chip ready. Tap Read Tag to scan.");
}

/* ====================================================================================
 * M6-P2 — universal reader
 *
 * The read flow auto-detects any HF tag (NFC-A / -B / -F / -V) and gathers everything it
 * can: technology, chip model, memory geometry, identity bytes (clone-grade), and — for
 * the families wired so far (T2T and NFC-V) — the full memory image into s_dump. The
 * ST25R3916 is 13.56 MHz HF only; 125 kHz LF tags cannot be read on this hardware.
 * MIFARE Classic dictionary dump, NDEF decode, and the NFC-B/-F/-ST25TB deep reads are
 * the next increment; those tags are identified here but not yet block-dumped.
 * ==================================================================================== */

#define NFC_SCAN_CYCLE_MS  350U   /* one auto-detect discovery cycle while WAITING */

/* Generic activated-device transceive (used for GET_VERSION etc.). Pumps the RFAL worker
 * until the exchange finishes or ~600 ms elapse. Returns ERR_NONE + the byte count in rxOut. */
static ReturnCode nfc_xchg(const uint8_t *tx, uint16_t txLen,
                           uint8_t *rx, uint16_t rxCap, uint16_t *rxOut, uint32_t fwt)
{
    uint8_t  *rxData = NULL;
    uint16_t *rxLen  = NULL;
    if (rxOut != NULL) { *rxOut = 0; }
    ReturnCode e = s_nfc->rfalNfcDataExchangeStart((uint8_t *)tx, txLen, &rxData, &rxLen, fwt);
    if (e != ERR_NONE) { return e; }
    uint32_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(600);
    do {
        s_nfc->rfalNfcWorker();
        e = s_nfc->rfalNfcDataExchangeGetStatus();
        if (e == ERR_BUSY) { vTaskDelay(pdMS_TO_TICKS(1)); }
    } while (e == ERR_BUSY && (int32_t)(xTaskGetTickCount() - deadline) < 0);
    if (e != ERR_NONE) { return e; }
    uint16_t n = (rxLen != NULL) ? *rxLen : 0;
    if (n > rxCap) { n = rxCap; }
    if (rx != NULL && rxData != NULL && n > 0) { memcpy(rx, rxData, n); }
    if (rxOut != NULL) { *rxOut = n; }
    return ERR_NONE;
}

/* NFC-A GET_VERSION (0x60) → name NTAG / Ultralight-EV1 and their storage size. */
static void classify_t2t_version(const uint8_t *v, nocsif_nfc_info_t *in)
{
    uint8_t vendor = v[1], ptype = v[2], storage = v[6];
    if (vendor == 0x04 && ptype == 0x04) {          /* NXP NTAG */
        switch (storage) {
            case 0x0F: snprintf(in->model, sizeof in->model, "NTAG213"); in->mem_bytes = 144; in->block_count = 45;  break;
            case 0x11: snprintf(in->model, sizeof in->model, "NTAG215"); in->mem_bytes = 504; in->block_count = 135; break;
            case 0x13: snprintf(in->model, sizeof in->model, "NTAG216"); in->mem_bytes = 888; in->block_count = 231; break;
            default:   snprintf(in->model, sizeof in->model, "NTAG (storage 0x%02X)", storage);                      break;
        }
    } else if (vendor == 0x04 && ptype == 0x03) {   /* NXP MIFARE Ultralight EV1 */
        switch (storage) {
            case 0x0B: snprintf(in->model, sizeof in->model, "MIFARE Ultralight EV1 (MF0UL11)"); in->mem_bytes = 48;  in->block_count = 20; break;
            case 0x0E: snprintf(in->model, sizeof in->model, "MIFARE Ultralight EV1 (MF0UL21)"); in->mem_bytes = 128; in->block_count = 41; break;
            default:   snprintf(in->model, sizeof in->model, "MIFARE Ultralight EV1");                                                      break;
        }
    } else {
        snprintf(in->model, sizeof in->model, "Type 2 tag (vendor 0x%02X)", vendor);
    }
}

/* Name a MIFARE Classic variant and its geometry from the SAK (SEL_RES). */
static void classify_classic_sak(uint8_t sak, nocsif_nfc_info_t *in)
{
    in->block_size = 16;
    switch (sak) {
        case 0x08: case 0x01: case 0x28: snprintf(in->model, sizeof in->model, "MIFARE Classic 1K");  in->mem_bytes = 1024; in->block_count = 64;  break;
        case 0x09:                       snprintf(in->model, sizeof in->model, "MIFARE Classic Mini"); in->mem_bytes = 320;  in->block_count = 20;  break;
        case 0x18: case 0x38:            snprintf(in->model, sizeof in->model, "MIFARE Classic 4K");   in->mem_bytes = 4096; in->block_count = 256; break;
        case 0x10: case 0x11:            snprintf(in->model, sizeof in->model, "MIFARE Plus");         in->block_size = 0;                           break;
        default:                         snprintf(in->model, sizeof in->model, "MIFARE Classic (SAK 0x%02X)", sak);                                  break;
    }
}

/* T2T (NTAG / Ultralight) page dump via rfalT2TPollerRead (16 B = 4 pages per call). Reads pages
 * [startPage, endPage) and APPENDS to s_dump, continuing from whatever is already there — so the caller
 * can read the mandatory first pages BEFORE a best-effort GET_VERSION (which halts a plain Ultralight)
 * and then extend once GET_VERSION reports the real geometry. Stops at the first READ that NAKs/times
 * out, which is the normal end-of-memory / locked-page signal. */
static void t2t_dump_range(nocsif_nfc_info_t *in, uint16_t startPage, uint16_t endPage)
{
    uint16_t pos = (uint16_t)s_dump_len;
    for (uint16_t p = startPage; p < endPage && pos < s_dump_cap; p += 4) {
        uint8_t  rx[16];
        uint16_t rcv = 0;
        if (s_nfc->rfalT2TPollerRead((uint8_t)p, rx, sizeof rx, &rcv) != ERR_NONE || rcv < 4) { break; }
        uint16_t n = (rcv > 16) ? 16 : rcv;
        if (pos + n > s_dump_cap) { n = (uint16_t)(s_dump_cap - pos); }
        memcpy(s_dump + pos, rx, n);
        pos += n;
        in->read_blocks = (uint16_t)(pos / 4);
    }
    s_dump_len = pos;
    /* Writable hint from the static lock bytes (page 2, bytes 2-3): all-zero => unlocked. Set once. */
    if (pos >= 12 && in->writable < 0) { in->writable = (s_dump[10] == 0 && s_dump[11] == 0) ? 1 : 0; }
}

/* NFC-V (ISO15693) block dump via ReadSingleBlock until the tag stops answering. */
static void nfcv_dump(const rfalNfcDevice *dev, nocsif_nfc_info_t *in)
{
    const uint8_t *uid = dev->nfcid;          /* RFAL points nfcid at the NFC-V UID */
    uint16_t pos = 0, blk = 0;
    uint8_t  bs = 0;
    for (blk = 0; blk < 256 && pos < s_dump_cap; blk++) {
        uint8_t  rx[32];
        uint16_t rcv = 0;
        if (s_nfc->rfalNfcvPollerReadSingleBlock(0x02, uid, (uint8_t)blk, rx, sizeof rx, &rcv) != ERR_NONE || rcv < 2) { break; }
        uint8_t dlen = (uint8_t)(rcv - 1);    /* drop the leading status/flag byte */
        if (bs == 0) { bs = dlen; }
        if (pos + dlen > s_dump_cap) { dlen = (uint8_t)(s_dump_cap - pos); }
        memcpy(s_dump + pos, rx + 1, dlen);
        pos += dlen;
    }
    in->block_size  = bs;
    in->block_count = (uint16_t)blk;
    in->read_blocks = (uint16_t)blk;
    in->mem_bytes   = (uint32_t)blk * bs;
    s_dump_len      = pos;
}

/* ====================================================================================
 * EMV contactless (payment card) reader — Flipper-style card data.
 *
 * A Type-4 / ISO-DEP card (NFC-A with SAK bit 0x20, or an ISO-DEP NFC-B) is already activated for
 * ISO-DEP, so we push APDUs over the same rfalNfcDataExchange path nfc_xchg uses. Standard EMV
 * discovery: SELECT PPSE ("2PAY.SYS.DDF01") -> read the ADF AID -> SELECT AID -> GET PROCESSING
 * OPTIONS -> READ RECORD across the AFL, then lift the PAN / expiry / name out of the BER-TLV
 * records. No keys or PIN — this is the same public cardholder data any contactless terminal reads
 * to begin a transaction (owner's own card, owned hardware; see memory/neutral-framing).
 * ==================================================================================== */

/* Minimal BER-TLV search: find `tag` (1- or 2-byte) within [buf,len), recursing into constructed
 * templates. Returns the value pointer and sets *outLen, or NULL if not found. */
static const uint8_t *tlv_find(const uint8_t *buf, uint16_t len, uint16_t tag, uint16_t *outLen)
{
    uint16_t i = 0;
    while (i < len) {
        uint16_t t = buf[i++];
        bool constructed = (t & 0x20u) != 0u;
        if ((t & 0x1Fu) == 0x1Fu) {                        /* multi-byte tag */
            while (i < len) { uint8_t b = buf[i++]; t = (uint16_t)((t << 8) | b); if ((b & 0x80u) == 0u) { break; } }
        }
        if (i >= len) { break; }
        uint16_t l = buf[i++];
        if (l & 0x80u) {                                   /* long-form length */
            uint8_t nb = (uint8_t)(l & 0x7Fu);
            l = 0;
            while (nb-- && i < len) { l = (uint16_t)((l << 8) | buf[i++]); }
        }
        if ((uint32_t)i + l > len) { l = (uint16_t)(len - i); }
        if (t == tag) { if (outLen) { *outLen = l; } return &buf[i]; }
        if (constructed) {
            uint16_t sub = 0;
            const uint8_t *r = tlv_find(&buf[i], l, tag, &sub);
            if (r) { if (outLen) { *outLen = sub; } return r; }
        }
        i += l;
    }
    return NULL;
}

/* One ISO-DEP APDU (CLA INS P1 P2 [Lc data] [00=Le]). Returns the response body (minus SW1SW2) in
 * rx/*rxLen and the status word in *sw. ERR_NONE if the exchange completed (whatever the SW). */
static ReturnCode emv_apdu(uint8_t cla, uint8_t ins, uint8_t p1, uint8_t p2,
                           const uint8_t *data, uint8_t lc, bool le,
                           uint8_t *rx, uint16_t rxCap, uint16_t *rxLen, uint16_t *sw)
{
    uint8_t cmd[80]; uint16_t n = 0;
    cmd[n++] = cla; cmd[n++] = ins; cmd[n++] = p1; cmd[n++] = p2;
    if (lc) { cmd[n++] = lc; if (data) { memcpy(&cmd[n], data, lc); n = (uint16_t)(n + lc); } }
    if (le) { cmd[n++] = 0x00; }
    uint8_t buf[264]; uint16_t rl = 0;
    if (rxLen) { *rxLen = 0; }
    if (sw)    { *sw = 0; }
    ReturnCode e = nfc_xchg(cmd, n, buf, sizeof buf, &rl, RFAL_FWT_NONE);
    if (e != ERR_NONE) { return e; }
    if (rl < 2) { return ERR_PROTO; }
    if (sw) { *sw = (uint16_t)((buf[rl - 2] << 8) | buf[rl - 1]); }
    uint16_t body = (uint16_t)(rl - 2);
    if (rx && body) { uint16_t c = (body > rxCap) ? rxCap : body; memcpy(rx, buf, c); if (rxLen) { *rxLen = c; } }
    return ERR_NONE;
}

/* Fill a PDOL (tag+len list) with terminal defaults so GPO is accepted: known tags get real-ish
 * values (TTQ, country/currency, random UN, terminal caps), everything else is zero-filled. */
static uint8_t emv_build_pdol(const uint8_t *pdol, uint16_t pdolLen, uint8_t *out, uint8_t outCap)
{
    uint8_t  o = 0;
    uint16_t i = 0;
    while (i < pdolLen && o < outCap) {
        uint16_t t = pdol[i++];
        if ((t & 0x1Fu) == 0x1Fu) { while (i < pdolLen) { uint8_t b = pdol[i++]; t = (uint16_t)((t << 8) | b); if ((b & 0x80u) == 0u) { break; } } }
        if (i >= pdolLen) { break; }
        uint8_t l = pdol[i++];
        for (uint8_t k = 0; k < l && o < outCap; k++) {
            uint8_t val;
            switch (t) {
                case 0x9F66: val = (k == 0) ? 0x36 : 0x00; break;                       /* TTQ: qVSDC + contactless */
                case 0x9F1A: val = (k == l - 1) ? 0x40 : ((k == l - 2) ? 0x08 : 0x00); break; /* terminal country 0840 */
                case 0x5F2A: val = (k == l - 1) ? 0x40 : ((k == l - 2) ? 0x08 : 0x00); break; /* tx currency 0840     */
                case 0x9F37: val = (uint8_t)(esp_random() & 0xFFu); break;              /* unpredictable number */
                case 0x9F35: val = 0x22; break;                                         /* terminal type        */
                case 0x9F33: val = (k == 0) ? 0xE0 : 0x00; break;                       /* terminal capabilities */
                default:     val = 0x00; break;                                         /* amounts, date, TVR, ... */
            }
            out[o++] = val;
        }
    }
    return o;
}

/* RID (first 5 AID bytes) -> card scheme name. */
static void emv_scheme_from_aid(const uint8_t *aid, uint16_t len, char *out, size_t cap)
{
    out[0] = '\0';
    if (len < 5) { snprintf(out, cap, "EMV card"); return; }
    static const struct { uint8_t rid[5]; const char *name; } k[] = {
        {{0xA0,0x00,0x00,0x00,0x03}, "Visa"},
        {{0xA0,0x00,0x00,0x00,0x04}, "Mastercard"},
        {{0xA0,0x00,0x00,0x00,0x25}, "Amex"},
        {{0xA0,0x00,0x00,0x00,0x65}, "JCB"},
        {{0xA0,0x00,0x00,0x01,0x52}, "Discover"},
        {{0xA0,0x00,0x00,0x03,0x33}, "UnionPay"},
        {{0xA0,0x00,0x00,0x05,0x24}, "RuPay"},
        {{0xA0,0x00,0x00,0x02,0x77}, "Interac"},
    };
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++) {
        if (memcmp(aid, k[i].rid, 5) == 0) { snprintf(out, cap, "%s", k[i].name); return; }
    }
    snprintf(out, cap, "EMV card");
}

/* Format a BCD-packed PAN (tag 5A) into groups of 4; stops at an 'F' nibble pad. */
static void emv_format_pan(const uint8_t *bcd, uint16_t len, char *out, size_t cap)
{
    size_t o = 0; int digits = 0;
    for (uint16_t i = 0; i < len && o + 2 < cap; i++) {
        for (int hi = 1; hi >= 0; hi--) {
            uint8_t nib = hi ? (uint8_t)(bcd[i] >> 4) : (uint8_t)(bcd[i] & 0x0F);
            if (nib > 9) { out[o] = '\0'; return; }                 /* F pad = end of PAN */
            if (digits && (digits % 4) == 0 && o + 1 < cap) { out[o++] = ' '; }
            out[o++] = (char)('0' + nib);
            digits++;
        }
    }
    out[o] = '\0';
}

/* Parse a Track-2 Equivalent (tag 57): PAN nibbles up to the 'D' separator, then YYMM expiry.
 * Fills emv_pan / emv_expiry only if they are still empty. */
static void emv_parse_track2(const uint8_t *t2, uint16_t len, nocsif_nfc_info_t *in)
{
    char s[48]; size_t o = 0;
    for (uint16_t b = 0; b < len && o + 2 < sizeof s; b++) {
        for (int hi = 1; hi >= 0; hi--) {
            uint8_t nib = hi ? (uint8_t)(t2[b] >> 4) : (uint8_t)(t2[b] & 0x0F);
            s[o++] = (nib <= 9) ? (char)('0' + nib) : (nib == 0x0D ? 'D' : '=');
        }
    }
    s[o] = '\0';
    char *d = strchr(s, 'D');
    if (!d) { return; }
    if (in->emv_pan[0] == '\0') {                                   /* group the PAN digits in 4s */
        size_t pl = (size_t)(d - s), po = 0;
        for (size_t i = 0; i < pl && po + 2 < sizeof in->emv_pan; i++) {
            if (i && (i % 4) == 0) { in->emv_pan[po++] = ' '; }
            in->emv_pan[po++] = s[i];
        }
        in->emv_pan[po] = '\0';
    }
    if (in->emv_expiry[0] == '\0' && strlen(d + 1) >= 4) {          /* d+1 = YYMM -> MM/YY */
        snprintf(in->emv_expiry, sizeof in->emv_expiry, "%c%c/%c%c", d[3], d[4], d[1], d[2]);
    }
}

/* Run the EMV read flow on an already-ISO-DEP-activated card. Returns true if a PAN was recovered.
 * Logs each APDU step's SW so a failed read is diagnosable from a serial capture. */
static bool emv_read(nocsif_nfc_info_t *in)
{
    uint8_t rx[264]; uint16_t rl = 0, sw = 0;

    /* 1) SELECT PPSE */
    static const uint8_t PPSE[] = { '2','P','A','Y','.','S','Y','S','.','D','D','F','0','1' };
    if (emv_apdu(0x00, 0xA4, 0x04, 0x00, PPSE, sizeof PPSE, true, rx, sizeof rx, &rl, &sw) != ERR_NONE) {
        ESP_LOGW(TAG, "EMV: SELECT PPSE exchange failed (not ISO-DEP?)");
        return false;
    }
    ESP_LOGW(TAG, "EMV: PPSE SW=%04X len=%u", sw, rl);
    if (sw != 0x9000) { return false; }

    uint16_t aidLen = 0;
    const uint8_t *aidp = tlv_find(rx, rl, 0x4F, &aidLen);
    if (!aidp || aidLen < 5 || aidLen > 16) { ESP_LOGW(TAG, "EMV: no AID (4F) in PPSE"); return false; }
    uint8_t aid[16]; memcpy(aid, aidp, aidLen);
    emv_scheme_from_aid(aid, aidLen, in->emv_scheme, sizeof in->emv_scheme);
    ESP_LOGW(TAG, "EMV: AID %02X%02X%02X%02X%02X.. (%u B) scheme=%s",
             aid[0], aid[1], aid[2], aid[3], aid[4], aidLen, in->emv_scheme);

    /* 2) SELECT AID */
    if (emv_apdu(0x00, 0xA4, 0x04, 0x00, aid, (uint8_t)aidLen, true, rx, sizeof rx, &rl, &sw) != ERR_NONE || sw != 0x9000) {
        ESP_LOGW(TAG, "EMV: SELECT AID SW=%04X", sw);
        return false;
    }
    uint16_t labLen = 0;
    const uint8_t *lab = tlv_find(rx, rl, 0x50, &labLen);
    if (lab && labLen) {
        uint16_t c = (labLen < sizeof in->emv_label - 1) ? labLen : (uint16_t)(sizeof in->emv_label - 1);
        memcpy(in->emv_label, lab, c); in->emv_label[c] = '\0';
    }
    uint16_t pdolLen = 0;
    const uint8_t *pdol = tlv_find(rx, rl, 0x9F38, &pdolLen);

    /* 3) GET PROCESSING OPTIONS — command data is 83 <len> <PDOL-filled> (or 83 00 when no PDOL) */
    uint8_t gpo[80]; uint8_t gn = 0;
    gpo[gn++] = 0x83;
    if (pdol && pdolLen) {
        uint8_t filled[64];
        uint8_t fn = emv_build_pdol(pdol, pdolLen, filled, sizeof filled);
        gpo[gn++] = fn;
        memcpy(&gpo[gn], filled, fn); gn = (uint8_t)(gn + fn);
    } else {
        gpo[gn++] = 0x00;
    }
    uint8_t  afl[256]; uint16_t aflLen = 0;
    if (emv_apdu(0x80, 0xA8, 0x00, 0x00, gpo, gn, true, rx, sizeof rx, &rl, &sw) == ERR_NONE && sw == 0x9000) {
        if (rl >= 4 && rx[0] == 0x80) {                    /* format 1: 80 len AIP(2) AFL(..) */
            aflLen = (uint16_t)(rl - 4); if (aflLen > sizeof afl) { aflLen = sizeof afl; }
            memcpy(afl, &rx[4], aflLen);
        } else {                                           /* format 2: 77 template, AFL = tag 94 */
            uint16_t t = 0; const uint8_t *a = tlv_find(rx, rl, 0x94, &t);
            if (a && t) { aflLen = (t > sizeof afl) ? sizeof afl : t; memcpy(afl, a, aflLen); }
        }
        ESP_LOGW(TAG, "EMV: GPO ok, AFL %u B", aflLen);
    } else {
        ESP_LOGW(TAG, "EMV: GPO SW=%04X (scanning SFI 1..3 as a fallback)", sw);
    }

    /* 4) READ RECORD. Prefer the AFL; if GPO gave none, brute a few SFIs/records (common layout). */
    int reads = 0;
    for (uint16_t i = 0; (aflLen >= 4) ? (i + 4 <= aflLen) : (i < 12); i += 4) {
        uint8_t sfi, first, last;
        if (aflLen >= 4) { sfi = (uint8_t)(afl[i] >> 3); first = afl[i + 1]; last = afl[i + 2]; }
        else             { sfi = (uint8_t)(i / 4 + 1);   first = 1;          last = 4; }        /* fallback: scan SFI 1..3 recs 1..4 */
        if (sfi == 0 || first == 0) { continue; }
        for (uint8_t rec = first; rec <= last; rec++) {
            uint8_t p2 = (uint8_t)((sfi << 3) | 0x04);
            if (emv_apdu(0x00, 0xB2, rec, p2, NULL, 0, true, rx, sizeof rx, &rl, &sw) != ERR_NONE || sw != 0x9000) { continue; }
            reads++;
            if (in->emv_pan[0] == '\0') {
                uint16_t l = 0; const uint8_t *p = tlv_find(rx, rl, 0x5A, &l);
                if (p && l) { emv_format_pan(p, l, in->emv_pan, sizeof in->emv_pan); }
            }
            if (in->emv_expiry[0] == '\0') {
                uint16_t l = 0; const uint8_t *p = tlv_find(rx, rl, 0x5F24, &l);
                if (p && l >= 2) { snprintf(in->emv_expiry, sizeof in->emv_expiry, "%02X/%02X", p[1], p[0]); }
            }
            if (in->emv_name[0] == '\0') {
                uint16_t l = 0; const uint8_t *p = tlv_find(rx, rl, 0x5F20, &l);
                if (p && l) {
                    uint16_t c = (l < sizeof in->emv_name - 1) ? l : (uint16_t)(sizeof in->emv_name - 1);
                    memcpy(in->emv_name, p, c); in->emv_name[c] = '\0';
                }
            }
            if (in->emv_pan[0] == '\0' || in->emv_expiry[0] == '\0') {   /* track2 fallback (57) */
                uint16_t l = 0; const uint8_t *p = tlv_find(rx, rl, 0x57, &l);
                if (p && l) { emv_parse_track2(p, l, in); }
            }
        }
    }

    in->emv_present = (in->emv_pan[0] != '\0');
    ESP_LOGW(TAG, "EMV: %d record(s) read; PAN=%s exp=%s name=%s label=%s present=%d",
             reads, in->emv_pan[0] ? in->emv_pan : "-", in->emv_expiry[0] ? in->emv_expiry : "-",
             in->emv_name[0] ? in->emv_name : "-", in->emv_label[0] ? in->emv_label : "-", in->emv_present);
    return in->emv_present;
}

/* Gather everything about the activated device into the back result slot + dump buffer,
 * publish the snapshot, and log a clone-grade report over serial. */
static void identify_and_read(const rfalNfcDevice *dev)
{
    nocsif_nfc_info_t *in = info_begin();
    in->present = true;
    uint8_t ul = dev->nfcidLen; if (ul > NOCSIF_NFC_UID_MAX) { ul = NOCSIF_NFC_UID_MAX; }
    if (dev->nfcid != NULL) { memcpy(in->uid, dev->nfcid, ul); }
    in->uid_len = ul;
    s_dump_len  = 0;

    /* RFAL tags the discovered/activated device with the RFAL_NFC_LISTEN_TYPE_* enumerators
     * (NFC-A=0, -B=1, -F=2, -V=3, ST25TB=4) — rfalNfcPollCollResolution sets devList[].type to
     * those and rfalNfcPollActivation switches on them; it never promotes .type to a POLL_TYPE_*.
     * (Matching POLL_TYPE_* here is the long-standing #305 bug: it never matched, so every tag fell
     * to default -> "Unknown HF tag", 0 bytes, even though the UID read fine.) */
    switch (dev->type) {
    case RFAL_NFC_LISTEN_TYPE_NFCA: {
        in->family = NOCSIF_NFC_FAM_A;
        snprintf(in->tech, sizeof in->tech, "NFC-A");
        in->has_atqa_sak = true;
        in->atqa[0] = dev->dev.nfca.sensRes.anticollisionInfo;
        in->atqa[1] = dev->dev.nfca.sensRes.platformInfo;
        in->sak     = dev->dev.nfca.selRes.sak;
        uint8_t sak = in->sak;
        if (sak & 0x20U) {                               /* ISO-DEP / Type 4 */
            snprintf(in->standard, sizeof in->standard, "ISO14443-4A (ISO-DEP / Type 4)");
            /* Try the EMV app flow first (SELECT PPSE ...). A payment card answers with its card data
             * (PAN/expiry/name); anything else fails PPSE cleanly and we fall back to the DESFire/4A probe. */
            if (emv_read(in)) {
                snprintf(in->model, sizeof in->model, "%s", in->emv_scheme[0] ? in->emv_scheme : "EMV card");
            } else {
                uint8_t cmd = 0x60; uint8_t v[16]; uint16_t vl = 0;
                if (nfc_xchg(&cmd, 1, v, sizeof v, &vl, RFAL_FWT_NONE) == ERR_NONE && vl >= 5) {
                    uint8_t n = (vl > NOCSIF_NFC_VER_MAX) ? NOCSIF_NFC_VER_MAX : (uint8_t)vl;
                    memcpy(in->version, v, n); in->version_len = n;
                    if (v[0] == 0x04 && v[1] == 0x01) { snprintf(in->model, sizeof in->model, "MIFARE DESFire"); }
                    else                               { snprintf(in->model, sizeof in->model, "ISO14443-4 card"); }
                } else {
                    snprintf(in->model, sizeof in->model, "ISO14443-4 card");
                }
            }
        } else if ((sak & 0x60U) == 0x00U) {             /* Type 2 (NTAG / Ultralight) */
            snprintf(in->standard, sizeof in->standard, "ISO14443-3A / Type 2 (T2T)");
            in->block_size  = 4;
            in->block_count = 16;                        /* plain-Ultralight default until GET_VERSION says more */
            snprintf(in->model, sizeof in->model, "MIFARE Ultralight / NTAG");

            /* Read the mandatory first pages BEFORE the optional GET_VERSION. A plain MIFARE Ultralight
             * (MF0ICU1) does not support GET_VERSION (0x60) and HALTS on the unexpected command, which
             * kills the READ that follows — that is exactly why the first working read returned 0 blocks.
             * 16 pages = 64 B covers a whole Ultralight and the UID/CC/first user pages of any NTAG. */
            t2t_dump_range(in, 0, 16);

            /* Best-effort GET_VERSION for the exact model + full geometry. Tags that support it
             * (NTAG21x / Ultralight-EV1) answer and are NOT halted, so it is safe to extend the dump to
             * their real size afterwards; on a plain Ultralight it just fails and we keep the 16 pages. */
            uint8_t cmd = 0x60; uint8_t v[16]; uint16_t vl = 0;
            if (nfc_xchg(&cmd, 1, v, sizeof v, &vl, RFAL_FWT_NONE) == ERR_NONE && vl >= 7) {
                memcpy(in->version, v, NOCSIF_NFC_VER_MAX);
                in->version_len = NOCSIF_NFC_VER_MAX;
                classify_t2t_version(v, in);             /* sets model + block_count/mem_bytes */
                if (in->block_count > 16) { t2t_dump_range(in, 16, in->block_count); }
            }
        } else {                                         /* MIFARE Classic family */
            snprintf(in->standard, sizeof in->standard, "ISO14443-3A / MIFARE Classic");
            classify_classic_sak(sak, in);
            /* Full dictionary dump lands in the next increment. */
        }
        break;
    }
    case RFAL_NFC_LISTEN_TYPE_NFCB:
        in->family = NOCSIF_NFC_FAM_B;
        snprintf(in->tech, sizeof in->tech, "NFC-B");
        snprintf(in->standard, sizeof in->standard, "ISO14443-B");
        /* NFC-B payment cards are ISO-DEP too — try the EMV flow before the generic label. */
        if (emv_read(in)) { snprintf(in->model, sizeof in->model, "%s", in->emv_scheme[0] ? in->emv_scheme : "EMV card"); }
        else              { snprintf(in->model, sizeof in->model, "ISO14443-B card"); }
        break;
    case RFAL_NFC_LISTEN_TYPE_NFCF:
        in->family = NOCSIF_NFC_FAM_F;
        snprintf(in->tech, sizeof in->tech, "NFC-F");
        snprintf(in->standard, sizeof in->standard, "FeliCa / ISO18092");
        snprintf(in->model, sizeof in->model, "FeliCa (IDm)");
        break;
    case RFAL_NFC_LISTEN_TYPE_NFCV:
        in->family = NOCSIF_NFC_FAM_V;
        snprintf(in->tech, sizeof in->tech, "NFC-V");
        snprintf(in->standard, sizeof in->standard, "ISO15693 / Type 5");
        in->has_dsfid = true;
        in->dsfid     = dev->dev.nfcv.InvRes.DSFID;
        snprintf(in->model, sizeof in->model, "ISO15693 tag");
        nfcv_dump(dev, in);
        break;
    case RFAL_NFC_LISTEN_TYPE_ST25TB:
        in->family = NOCSIF_NFC_FAM_ST25TB;
        snprintf(in->tech, sizeof in->tech, "ST25TB");
        snprintf(in->standard, sizeof in->standard, "ISO14443-2B' / ST25TB");
        snprintf(in->model, sizeof in->model, "ST25TB / SRIxx");
        /* Block dump (Read Block 0x08) lands in the next increment. */
        break;
    default:
        in->family = NOCSIF_NFC_FAM_NONE;
        snprintf(in->tech, sizeof in->tech, "HF");
        snprintf(in->standard, sizeof in->standard, "HF tag (type %d)", (int)dev->type);
        snprintf(in->model, sizeof in->model, "Unknown HF tag");
        break;
    }

    /* Short status tag + a multi-line readout summary + a clone-grade serial report. */
    char compact[2 * NOCSIF_NFC_UID_MAX + 1]; size_t cp = 0;
    char spaced [3 * NOCSIF_NFC_UID_MAX + 1]; size_t sp = 0;
    for (uint8_t i = 0; i < in->uid_len; i++) {
        cp += snprintf(compact + cp, sizeof compact - cp, "%02X", in->uid[i]);
        sp += snprintf(spaced  + sp, sizeof spaced  - sp, (i ? " %02X" : "%02X"), in->uid[i]);
    }
    char shorttag[16];
    snprintf(shorttag, sizeof shorttag, "%.8s%s", compact, (in->uid_len > 4) ? ".." : "");
    publish_status(shorttag);

    char sum[192]; int k = 0;
    k += snprintf(sum + k, sizeof sum - k, "%s\n%s\nUID %s", in->model, in->standard, spaced);
    if (in->mem_bytes)        { k += snprintf(sum + k, sizeof sum - k, "\n%u B", (unsigned)in->mem_bytes); }
    if (in->writable == 1)    { k += snprintf(sum + k, sizeof sum - k, " \xC2\xB7 writable"); }
    else if (in->writable == 0) { k += snprintf(sum + k, sizeof sum - k, " \xC2\xB7 locked"); }
    publish_readout(sum);

    info_commit();

    ESP_LOGW(TAG, "==== TAG READ: %s (%s) ====", in->model, in->tech);
    ESP_LOGW(TAG, "  UID: %s (%u bytes)  standard=%s", spaced, in->uid_len, in->standard);
    if (in->has_atqa_sak) { ESP_LOGW(TAG, "  ATQA: %02X %02X   SAK: %02X", in->atqa[0], in->atqa[1], in->sak); }
    if (in->version_len)  { ESP_LOGW(TAG, "  GET_VERSION: %02X %02X %02X %02X %02X %02X %02X %02X",
                                     in->version[0], in->version[1], in->version[2], in->version[3],
                                     in->version[4], in->version[5], in->version[6], in->version[7]); }
    if (in->has_dsfid)    { ESP_LOGW(TAG, "  DSFID: %02X", in->dsfid); }
    ESP_LOGW(TAG, "  memory: %u bytes, %u x %u, read %u blocks (%u bytes), writable=%d",
             (unsigned)in->mem_bytes, in->block_count, in->block_size,
             in->read_blocks, (unsigned)s_dump_len, in->writable);
    for (size_t off = 0; off < s_dump_len && off < 256; off += 16) {
        char ln[64]; size_t lp = 0;
        for (size_t i = 0; i < 16 && (off + i) < s_dump_len; i++) {
            lp += snprintf(ln + lp, sizeof ln - lp, "%02X ", s_dump[off + i]);
        }
        ESP_LOGW(TAG, "  %04X: %s", (unsigned)off, ln);
    }
    ESP_LOGW(TAG, "=====================================================");
}

/* One universal discovery cycle (all HF techs). If a tag activates, gather it fully.
 * Returns true if a tag was activated. Assumes bring-up done + the SD lock held. */
static bool read_one(uint16_t durMs, bool mark_reading)
{
    rfalNfcDiscoverParam disc;
    memset(&disc, 0, sizeof disc);
    disc.compMode      = RFAL_COMPLIANCE_MODE_NFC;
    disc.techs2Find    = RFAL_NFC_POLL_TECH_A | RFAL_NFC_POLL_TECH_B |
                         RFAL_NFC_POLL_TECH_F | RFAL_NFC_POLL_TECH_V |
                         RFAL_NFC_POLL_TECH_ST25TB;
    disc.totalDuration = durMs;
    disc.devLimit      = 1;
    disc.nfcfBR        = RFAL_BR_212;    /* required whenever TECH_F is requested */
    disc.notifyCb      = NULL;

    if (s_nfc->rfalNfcDiscover(&disc) != ERR_NONE) { return false; }

    bool found = false;
    int  maxState  = (int)RFAL_NFC_STATE_IDLE;  /* highest state seen this window                  */
    int  maxPoll   = (int)RFAL_NFC_STATE_IDLE;  /* highest POLL-phase state (10..13) seen           */
    bool tech_seen = false;                     /* a technology was detected (reached colavoidance) */
    uint32_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS((uint32_t)durMs + 200U);
    while ((int32_t)(xTaskGetTickCount() - deadline) < 0) {
        s_nfc->rfalNfcWorker();
        int st = (int)s_nfc->rfalNfcGetState();
        if (st > maxState) maxState = st;
        /* POLL_COLAVOIDANCE(11)/SELECT(12)/ACTIVATION(13) are entered ONLY after the poller
         * detected at least one technology, so reaching them proves a tag answered REQA/anticoll. */
        if (st >= (int)RFAL_NFC_STATE_POLL_TECHDETECT && st <= (int)RFAL_NFC_STATE_POLL_ACTIVATION) {
            if (st > maxPoll) maxPoll = st;
            if (st >= (int)RFAL_NFC_STATE_POLL_COLAVOIDANCE) tech_seen = true;
        }
        if (st == (int)RFAL_NFC_STATE_ACTIVATED) {
            rfalNfcDevice *dev = NULL;
            if (s_nfc->rfalNfcGetActiveDevice(&dev) == ERR_NONE && dev != NULL) {
                if (mark_reading) {
                    set_scan_state(NOCSIF_NFC_SCAN_READING);
                    publish_status("read");
                    publish_readout("Reading\xE2\x80\xA6");
                }
                identify_and_read(dev);
                found = true;
            }
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }

    /* NocSif diag (throttled ~1/s). The two failure modes have DIFFERENT state signatures — do not
     * confuse them (the old log called maxState==20 a "Type-4/EMV RATS failure", which is backwards):
     *   - tech_seen  => a tag answered REQA/anticollision (reached POLL_COLAVOIDANCE..ACTIVATION,
     *                   11..13) but we could not finish activation — the real ISO-DEP RATS / auth /
     *                   anticollision failure (EMV/passport/keyed-tag signature).
     *   - !tech_seen => the loop only ran POLL_TECHDETECT(10) then rolled to LISTEN_TECHDETECT(20):
     *                   techsFound==NONE, i.e. NOTHING answered the field (no tag, weak coupling, or
     *                   the card never powered up enough to reply — the EMV-on-this-antenna case). */
    if (!found) {
        static uint32_t s_last_diag;
        uint32_t now = xTaskGetTickCount();
        if (s_last_diag == 0 || (int32_t)(now - s_last_diag) > (int32_t)pdMS_TO_TICKS(1000)) {
            s_last_diag = now;
            if (tech_seen) {
                ESP_LOGW(TAG, "scan: tag answered but activation FAILED (maxPoll=%d) "
                              "— anticollision/ISO-DEP RATS/auth", maxPoll);
            } else {
                ESP_LOGW(TAG, "scan: no tag detected (maxState=%d) "
                              "— nothing answered the field / weak coupling / card unpowered", maxState);
            }
        }
    }

    /* Stop the field + return to idle; pump the worker so deactivation completes. */
    s_nfc->rfalNfcDeactivate(false);
    for (int i = 0; i < 8; i++) { s_nfc->rfalNfcWorker(); vTaskDelay(pdMS_TO_TICKS(1)); }
    return found;
}

static void do_read(void)
{
    bool selftest = s_selftest_req;
    s_selftest_req = false;

    publish_status("scan");
    publish_readout(selftest ? "NFC self-test\xE2\x80\xA6" : "Scanning\xE2\x80\xA6");

    if (!nocsif_sdcard_lock(1500)) {
        ESP_LOGW(TAG, "read: could not take the shared-SPI lock");
        publish_status("busy");
        publish_readout("SPI bus busy — try again.");
        return;
    }

    if (!s_brought_up) {
        bring_up();
    }
    if (s_brought_up) {
        /* Self-test path (nocsif_nfc_request_selftest / boot -DNOCSIF_NFC_BOOT_SELFTEST=1):
         * run the RF front-end / antenna health check, then still attempt a real read so a
         * healthy unit also reports a tag if one is present. Normal reads skip the test. */
        if (selftest) {
            hw_selftest();
        }
        bool found = read_one(NFC_DISCOVER_MS, false);
        /* Energizing and reading a tag is unanswerable proof the RF front-end radiates — upgrade a
         * self-test verdict to OK regardless of the register-level handshake result above. */
        if (selftest && found) {
            s_test_result = NOCSIF_NFC_TEST_OK;
        }
        if (!found && !selftest) {
            publish_status("none");
            publish_readout("No tag in field. Hold a tag to the back and tap again.");
        }
    }

    nocsif_sdcard_unlock();
}

/* Continuous auto-detect loop for the read-flow screen: the field stays on, polling short
 * cycles until a tag activates (WAITING -> READING -> DONE) or the UI cancels. The SD lock
 * is taken per cycle and released between cycles so SD isn't starved while we wait. */
static void scan_loop(void)
{
    if (!nocsif_sdcard_lock(1500)) {
        set_scan_state(NOCSIF_NFC_SCAN_ERROR);
        publish_readout("SPI bus busy — close and retry.");
        s_scan_on = false;
        return;
    }
    if (!s_brought_up) { bring_up(); }
    nocsif_sdcard_unlock();
    if (!s_brought_up) {
        set_scan_state(NOCSIF_NFC_SCAN_ERROR);
        s_scan_on = false;
        return;
    }

    set_scan_state(NOCSIF_NFC_SCAN_WAITING);
    publish_status("tap");
    publish_readout("Place a tag against the back of the watch\xE2\x80\xA6");

    while (s_scan_on) {
        bool found = false;
        if (nocsif_sdcard_lock(1000)) {
            found = read_one(NFC_SCAN_CYCLE_MS, true);
            nocsif_sdcard_unlock();
        }
        if (found) {
            set_scan_state(NOCSIF_NFC_SCAN_DONE);
            s_scan_on = false;
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(60));   /* breathe between cycles; lets SD / others run */
    }
    set_scan_state(NOCSIF_NFC_SCAN_IDLE);
}

static void nfc_task(void *arg)
{
    (void)arg;
    volatile uint8_t probe;                          /* used only to check where this stack lives */
    ESP_LOGI(TAG, "worker up: stack in %s", esp_ptr_external_ram((void *)&probe) ? "PSRAM" : "INTERNAL");
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);   /* wait for a read / scan request */
        if (s_scan_on) { scan_loop(); }            /* read-flow auto-detect (nocsif_nfc_scan_start) */
        else           { do_read(); }              /* one-shot read / self-test (request_read/selftest) */
    }
}

esp_err_t nocsif_nfc_init(void)
{
    if (s_task != NULL) {
        return ESP_OK;   /* already running */
    }
    /* Set the initial published strings before anything can read them. */
    if (nocsif_reliability_safe_mode()) {
        publish_status("off");
        publish_readout("NFC disabled (safe mode).");
        ESP_LOGW(TAG, "safe mode — NFC bring-up skipped");
        return ESP_OK;   /* no worker task created; nocsif_nfc_available() stays false */
    }
    publish_status("tap");
    publish_readout("Tap Read Tag to scan for a tag.");

    /* Low priority: the RFAL poll loops busy-yield, so keep them below the UI/system tasks.
     * Not Task-WDT-subscribed, so a bounded discovery can't panic; a pathological spin is
     * still caught by the idle-task WDT (reliability A1) -> reboot + recorded. */
    /* Stack in INTERNAL RAM (plain xTaskCreate, NOT xTaskCreateWithCaps+SPIRAM): RFAL's register ops hand
     * small *stack* buffers and an on-stack spi_transaction_t to the DMA-enabled SPI3 driver, and a
     * PSRAM-resident stack panics that path in spi_master setup_priv_desc (observed: panic task=nfc, see
     * gotcha-nfc-worker-spi-psram-stack-panic). A plain internal stack is regular internal RAM — NOT the
     * scarce contiguous int-DMA pool — so the co-resident budget still holds. The old PSRAM stack dated
     * from when this unit's NFC TX was assumed dead (PLAN §4.12) and the worker was dead weight; the
     * replacement unit's NFC works and is actively used, so it now does real, repeated SPI DMA. Never deleted. */
    if (xTaskCreate(nfc_task, "nfc", 6144, NULL, 3, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "failed to create nfc worker task");
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "nfc worker ready (lazy bring-up on first read)");
    return ESP_OK;
}

void nocsif_nfc_request_read(void)
{
    if (s_task != NULL) {
        xTaskNotifyGive(s_task);   /* wakes the worker; harmless if a scan is already running */
    }
}

void nocsif_nfc_request_selftest(void)
{
    s_test_result = NOCSIF_NFC_TEST_PENDING;   /* cleared; hw_selftest sets the verdict when it runs */
    s_selftest_req = true;         /* picked up by the worker's next scan */
    if (s_task != NULL) {
        xTaskNotifyGive(s_task);
    }
}

/* ---- read-flow (auto-detect) API ---------------------------------------------------
 * The read screen calls scan_start() on entry and scan_stop() on exit; it polls
 * nocsif_nfc_scan_state() to swap its panel (place-tag -> reading -> result). */
void nocsif_nfc_scan_start(void)
{
    if (s_task == NULL) { return; }      /* safe mode: no worker, stays IDLE */
    s_scan_on = true;
    set_scan_state(NOCSIF_NFC_SCAN_WAITING);
    xTaskNotifyGive(s_task);
}

void nocsif_nfc_scan_stop(void)
{
    s_scan_on = false;   /* the loop notices within a cycle, turns the field off, goes IDLE */
}
