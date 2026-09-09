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
static char           s_readout[2][96];
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

/* ---- module state ----------------------------------------------------------------- */
static SPIClass               s_spi;              /* Arduino-style SPI shim RFAL talks to */
static spi_device_handle_t    s_spi_dev;
static RfalRfST25R3916Class  *s_reader;
static RfalNfcClass          *s_nfc;
static TaskHandle_t           s_task;
static bool                   s_brought_up;       /* chip init has succeeded */
static volatile bool          s_available;        /* public mirror of s_brought_up */
static volatile bool          s_selftest_req;     /* run the HW self-test on the next scan */

bool nocsif_nfc_available(void) { return s_available; }
const char *nocsif_nfc_status_str(void)  { return s_status[s_status_i]; }
const char *nocsif_nfc_readout_str(void) { return s_readout[s_readout_i]; }

/* Bring up SPI3 if it isn't already running, and register the ST25R3916 on it as a
 * second device with manually-driven chip select. Returns false on a hard SPI error. */
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

    /* 8) Combine the results above into a single pass/fail verdict. */
    int  amp_delta  = (int)amp_c - (int)amp_off;
    bool tx_ever    = tx_seen_c || tx_on_c || tx_seen_d;
    bool over_curr  = ilim_c || ilim_d;
    ESP_LOGW(TAG, "---- VERDICT (amp on-off delta=%d, tx_ever=%d, over_current=%d) ----",
             amp_delta, tx_ever, over_curr);
    if (!idok) {
        ESP_LOGW(TAG, "  DIGITAL FAULT: chip not answering on SPI -> wiring/power; retest first.");
    } else if (over_curr) {
        ESP_LOGW(TAG, "  Over-current tripped while driving the antenna ->");
        ESP_LOGW(TAG, "  SHORTED antenna / matching network: HARDWARE fault.");
    } else if (tx_on_c && amp_delta >= 8) {
        ESP_LOGW(TAG, "  TX on + antenna amplitude ROSE -> RF front-end + antenna RADIATE OK.");
        ESP_LOGW(TAG, "  Hardware is fine; a no-read is discovery-logic / positioning / tag.");
    } else if (tx_ever && amp_delta < 8) {
        ESP_LOGW(TAG, "  TX asserted but antenna amplitude stayed FLAT -> RF not coupling into the coil:");
        ESP_LOGW(TAG, "  OPEN antenna / broken match: HARDWARE fault.");
    } else {
        ESP_LOGW(TAG, "  TX never engaged (tx_on stays 0), no over-current, all rails healthy ->");
        ESP_LOGW(TAG, "  driver output stage not starting: chip TX fault or analog-config gap.");
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
    vTaskDelay(pdMS_TO_TICKS(10));   /* give the rail a moment to settle before talking to the chip */

    if (!ensure_spi()) {
        publish_status("err");
        publish_readout("SPI bus init failed.");
        return;
    }

    if (s_reader == NULL) {
        s_reader = new RfalRfST25R3916Class(&s_spi, NFC_PIN_CS, NFC_PIN_IRQ, NFC_SPI_HZ);
        s_nfc    = new RfalNfcClass(s_reader);
    }

    ReturnCode err = s_nfc->rfalNfcInitialize();
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

    publish_readout("Chip ready. Tap Read Tag to scan.");
}

/* Publish a discovered tag's UID: a short form for the status row, a full spaced-out
 * hex form for the readout line. */
static void report_uid(const rfalNfcDevice *dev)
{
    uint8_t n = dev->nfcidLen;
    if (n > NFC_MAX_UID) {
        n = NFC_MAX_UID;
    }
    char compact[2 * NFC_MAX_UID + 1];
    char spaced[3 * NFC_MAX_UID + 1];
    size_t ci = 0, si = 0;
    for (uint8_t i = 0; i < n; i++) {
        ci += snprintf(compact + ci, sizeof compact - ci, "%02X", dev->nfcid[i]);
        si += snprintf(spaced + si, sizeof spaced - si, (i ? " %02X" : "%02X"), dev->nfcid[i]);
    }
    /* Truncate to the first 4 bytes for the status row; the full UID goes in the readout. */
    char tag[16];
    snprintf(tag, sizeof tag, "%.8s%s", compact, (dev->nfcidLen > 4) ? ".." : "");
    publish_status(tag);

    char line[96];
    snprintf(line, sizeof line, "NFC-A tag  UID %s  (%u bytes)", spaced, (unsigned)dev->nfcidLen);
    publish_readout(line);
    ESP_LOGI(TAG, "read %s", line);
}

/* Run one NFC-A discovery cycle and report a UID if a tag activates. Caller must have
 * already brought the chip up and be holding the SD lock. */
static void discover_once(void)
{
    rfalNfcDiscoverParam disc;
    memset(&disc, 0, sizeof disc);
    disc.compMode      = RFAL_COMPLIANCE_MODE_NFC;
    disc.techs2Find    = RFAL_NFC_POLL_TECH_A;
    disc.totalDuration = NFC_DISCOVER_MS;
    disc.devLimit      = 1;
    disc.notifyCb      = NULL;

    ReturnCode err = s_nfc->rfalNfcDiscover(&disc);
    ESP_LOGI(TAG, "rfalNfcDiscover(NFC-A, dur=%ums) -> %d", (unsigned)NFC_DISCOVER_MS, (int)err);
    if (err != ERR_NONE) {
        publish_status("err");
        publish_readout("Discover failed.");
        return;
    }

    /* Track the highest state the discovery loop reaches, purely for diagnostics on a miss. */
    bool found = false;
    int high_state = 0, iters = 0;
    uint32_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(NFC_SCAN_TMO_MS);
    while ((int32_t)(xTaskGetTickCount() - deadline) < 0) {
        s_nfc->rfalNfcWorker();
        rfalNfcState st = s_nfc->rfalNfcGetState();
        if ((int)st > high_state) { high_state = (int)st; }
        iters++;
        if (st == RFAL_NFC_STATE_ACTIVATED) {
            rfalNfcDevice *dev = NULL;
            if (s_nfc->rfalNfcGetActiveDevice(&dev) == ERR_NONE && dev != NULL && dev->nfcidLen > 0) {
                report_uid(dev);
                found = true;
            }
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(2));   /* yield briefly between polls */
    }
    ESP_LOGI(TAG, "discover end: found=%d high_state=%d iters=%d", found, high_state, iters);

    /* Turn the RF field off and pump the worker until deactivation finishes. */
    s_nfc->rfalNfcDeactivate(false);
    for (int i = 0; i < 8; i++) {
        s_nfc->rfalNfcWorker();
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    if (!found) {
        publish_status("none");
        publish_readout("No tag in field. Hold a tag to the back and tap again.");
    }
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
        /* Self-test requests run the hardware check first, then still fall through to a
         * normal discovery so a healthy unit reports a tag if one happens to be present. */
        if (selftest) {
            hw_selftest();
        }
        discover_once();
    }

    nocsif_sdcard_unlock();
}

static void nfc_task(void *arg)
{
    (void)arg;
    volatile uint8_t probe;                          /* used only to check where this stack lives */
    ESP_LOGI(TAG, "worker up: stack in %s", esp_ptr_external_ram((void *)&probe) ? "PSRAM" : "INTERNAL");
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);   /* wait for a read request */
        do_read();
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

    /* Low priority, since the RFAL poll loop busy-yields and shouldn't compete with the UI.
     * Runs a PSRAM-backed stack — the driver only needs its own DMA buffers, no flash/NVS
     * access from this task. The task is never deleted once created. */
    if (xTaskCreateWithCaps(nfc_task, "nfc", 6144, NULL, 3, &s_task, MALLOC_CAP_SPIRAM) != pdPASS) {
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
    s_selftest_req = true;         /* picked up by the worker's next scan */
    if (s_task != NULL) {
        xTaskNotifyGive(s_task);
    }
}
