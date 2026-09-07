/*
 * NocSif — TinyUSB USB mode server (UI-shell P4.5). See usb_gadget.h.
 *
 * The device presents ONE USB class at a time, chosen by the user:
 *   DETACHED  the host sees no NocSif device. At BOOT this means TinyUSB is not installed at all,
 *             so the ESP32-S3 USB-Serial/JTAG console (COM7) stays live for flashing/logs.
 *   CDC       a USB CDC serial port.
 *   HID       a USB HID keyboard (Run Macro / Keymap).
 *   MSC       (P4.5.2) mass-storage — /sd exposed to the host as a drive.
 *
 * Two ESP32-S3 realities shape the design (both found on-device in the P4.5.1 spike):
 *   1. DELETING and RE-CREATING the OTG PHY (usb_del_phy -> usb_new_phy) crashes; the first
 *      creation is fine. So TinyUSB is installed EXACTLY ONCE (lazily, on the first mode pick, so
 *      COM7 stays live until then) and NEVER uninstalled.
 *   2. Cycling the CDC helper (tinyusb_cdcacm_init/_deinit + tinyusb_console_init/_deinit on every
 *      CDC enter/leave) crashes on the re-init. So the CDC-ACM helper is initialised ONCE and never
 *      deinitialised, and the ESP-IDF console is NOT redirected onto CDC at all (logs during a
 *      gadget mode go to the — dark — USB-Serial/JTAG and are simply not observed; the CDC port
 *      still enumerates and works as a serial device).
 *
 * esp_tinyusb STORES (does not copy) the descriptor pointers it is handed and returns them from
 * its descriptor callbacks. So a mode switch is just: rewrite the MUTABLE active descriptor in
 * place + tud_disconnect()/tud_connect(), and the host re-reads it and re-enumerates:
 *   DETACHED (boot, not installed) -> gadget   install ONCE, cdcacm_init ONCE, connect.
 *   installed -> gadget                         tud_disconnect() + rewrite descriptor + tud_connect().
 *   installed -> DETACHED                       tud_disconnect() only (PHY kept).
 * Each mode uses a DISTINCT product id so the host re-reads instead of serving a cached descriptor.
 * Trade-off: once a mode has been picked, DETACHED shows the host nothing but does NOT restore
 * USB-Serial/JTAG until a reboot. At boot, DETACHED is truly uninstalled (COM7 live).
 *
 * THREADING: all install/switch runs on this worker task. UI callbacks only call
 * nocsif_usb_gadget_request_mode() (record target + notify).
 */
#include "usb_gadget.h"
#include "freertos/idf_additions.h" /* xTaskCreateWithCaps — PSRAM worker stack (RAM Phase A2) */
#include "esp_heap_caps.h"          /* MALLOC_CAP_SPIRAM */
#include "esp_memory_utils.h"       /* esp_ptr_external_ram — PSRAM-stack placement probe */
#include "coex.h"                   /* NOCSIF_DMA_CAPS + NOCSIF_RADIO_MIN_DMA_USB — the entry reserve/gate (A3) */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "tinyusb.h"
#include "tinyusb_default_config.h"   /* TINYUSB_DEFAULT_CONFIG() macro */
#include "tinyusb_cdc_acm.h"
#include "tinyusb_msc.h"              /* File Share (MSC) storage over the microSD */
#include "nocsif_usb_desc.h"          /* per-mode + runtime-active descriptors */
#include "sdcard.h"                   /* nocsif_sdcard_card() — the raw card MSC wraps */
#include "esp_log.h"
#include "esp_timer.h"                /* esp_timer_get_time() — File Share host-mount settle window */

#define SD_MOUNT_POINT  "/sd"

static const char *TAG = "usb_gadget";

static TaskHandle_t                        s_task;
static volatile nocsif_usb_gadget_state_t  s_state    = NOCSIF_USB_GADGET_OFF;
static volatile nocsif_usb_mode_t          s_cur_mode = NOCSIF_USB_MODE_DETACHED;
static volatile nocsif_usb_mode_t          s_req_mode = NOCSIF_USB_MODE_DETACHED;
static bool                                s_installed;    /* TinyUSB installed; never torn down */
static bool                                s_cdc_inited;   /* tinyusb_cdcacm_init done once        */
static tinyusb_msc_storage_handle_t        s_msc;          /* File Share storage; non-NULL only in MSC */
/* RAM Phase A3 — the entry's memory, reserved at BOOT. The one-time tinyusb_driver_install needs ~5-7 KB of
 * INTERNAL memory at runtime (esp_tinyusb's device task has an internal 4 KB stack with no caps option, plus
 * its context, the CDC-ACM rings and the MSC FAT handoff). Before Phase A the steady-state largest
 * contiguous block was ~2 KB and the install REFUSED; A1/A2 lifted it to ~30 KB, but Signal Hunt's WiFi
 * monitor still takes it to ~21 KB and nothing stops a future feature from eating the rest. So: claim
 * NOCSIF_RADIO_MIN_DMA_USB from the pristine boot pool in nocsif_usb_gadget_init (before the runtime
 * fragments it) and free it immediately before the install, so the install's allocations land in that
 * hole regardless of what the rest of the system did since boot. A stop-gap that instead STOPPED WiFi on
 * File-Share entry (msc_shed_wifi, never merged) was measured to recover zero contiguity — runtime
 * teardown never defragments (docs/DMA-COEXISTENCE-PLAN.md). */
static void       *s_boot_reserve;                          /* NULL once released (or never claimed) */
static const char *s_fail_reason = "";                      /* honest cause of the last FAILED state */

/* File Share host-mount settle (P4.5.4). No device-visible FS-mount signal exists, so once a host
 * has enumerated the drive (tud_mounted) we hold the "preparing…" indicator for this window to cover
 * the host OS's mount latency (measured ~5 s on Windows). s_msc_host_seen_us latches the enumeration
 * instant; it is written only from nocsif_usb_gadget_msc_host() on the LVGL task (a monotonic latch). */
#define MSC_HOST_SETTLE_US   (5 * 1000 * 1000)
static int64_t                             s_msc_host_seen_us;   /* 0 = no host enumerated yet */

static const char *mode_name(nocsif_usb_mode_t m)
{
    switch (m) {
    case NOCSIF_USB_MODE_CDC: return "CDC";
    case NOCSIF_USB_MODE_HID: return "HID";
    case NOCSIF_USB_MODE_MSC: return "MSC";
    case NOCSIF_USB_MODE_DETACHED:
    default:                  return "DETACHED";
    }
}

/* Resolve a mode to its per-mode source device + config descriptors. Returns false for an unknown
 * value — the caller then rejects the request WITHOUT disturbing whatever is currently enumerated.
 * Pure (no side effects); the MSC storage lifecycle is handled separately around the switch. */
static bool desc_for_mode(nocsif_usb_mode_t m, const tusb_desc_device_t **dev, const uint8_t **cfg)
{
    switch (m) {
    case NOCSIF_USB_MODE_CDC:
        *dev = nocsif_usb_desc_device_cdc(); *cfg = nocsif_usb_desc_config_cdc(); return true;
    case NOCSIF_USB_MODE_HID:
        *dev = nocsif_usb_desc_device_hid(); *cfg = nocsif_usb_desc_config_hid(); return true;
    case NOCSIF_USB_MODE_MSC:
        *dev = nocsif_usb_desc_device_msc(); *cfg = nocsif_usb_desc_config_msc(); return true;
    default:
        return false;   /* an unknown mode */
    }
}

/* Bring the CDC-ACM helper up ONCE (never deinitialised — re-init crashes on-device). Safe to
 * call while the active descriptor has no CDC interface; the helper simply stays idle until a
 * CDC descriptor is enumerated. */
static void cdc_helper_ensure(void)
{
    if (s_cdc_inited) {
        return;
    }
    const tinyusb_config_cdcacm_t acm = { .cdc_port = TINYUSB_CDC_ACM_0 };
    esp_err_t err = tinyusb_cdcacm_init(&acm);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_cdcacm_init -> %s", esp_err_to_name(err));
        return;
    }
    s_cdc_inited = true;
}

/* File Share (MSC) storage — created ONCE at boot over the raw microSD and never deleted; File Share
 * just toggles who owns it. mount_point=APP means the esp_tinyusb MSC helper FAT-mounts /sd for the
 * firmware (Files + Run Macro read it offline, no host needed); mount_point=USB hands the raw card to
 * a connected host. auto-mount is DISABLED so ownership changes ONLY on our explicit File Share
 * on/off — the default (auto) would flip the card to the host on any raw USB connect, wrong for the
 * single-class CDC/HID modes that must keep /sd for the app. Creating the storage installs the MSC
 * glue driver (NOT the TinyUSB stack/PHY), so it is fine before tinyusb_driver_install (the proven M4
 * order). s_msc stays NULL when there is no card (File Share unavailable, /sd unmounted). */
static void msc_storage_init_once(void)
{
    if (s_msc != NULL) {
        return;
    }
    sdmmc_card_t *card = nocsif_sdcard_card();
    if (card == NULL) {
        ESP_LOGW(TAG, "no microSD — File Share unavailable, /sd not mounted for the app");
        return;
    }
    /* Install the MSC glue driver with auto-mount OFF so WE own the APP<->USB handoff explicitly. */
    const tinyusb_msc_driver_config_t drv = {
        .user_flags = { .auto_mount_off = 1 },
        .callback = NULL,
        .callback_arg = NULL,
    };
    esp_err_t err = tinyusb_msc_install_driver(&drv);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "tinyusb_msc_install_driver -> %s", esp_err_to_name(err));
        return;
    }
    const tinyusb_msc_storage_config_t cfg = {
        .medium.card = card,
        .fat_fs = {
            .base_path = SD_MOUNT_POINT,
            .config = { .format_if_mount_failed = false, .max_files = 5,
                        .allocation_unit_size = 16 * 1024 },
            .do_not_format = true,     /* never format the user's card */
            .format_flags = 0,
        },
        .mount_point = TINYUSB_MSC_STORAGE_MOUNT_APP,  /* app owns /sd at boot (offline reads) */
    };
    err = tinyusb_msc_new_storage_sdmmc(&cfg, &s_msc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_msc_new_storage_sdmmc -> %s", esp_err_to_name(err));
        s_msc = NULL;
        return;
    }
    ESP_LOGI(TAG, "MSC storage ready; /sd mounted for the app (host gets it only in File Share)");
}

/* Set who owns the microSD: APP (firmware FAT-mounts /sd) or USB (raw card handed to the host). The
 * helper does the FAT unmount/remount synchronously. No-op when there is no storage. */
static void msc_set_owner(tinyusb_msc_mount_point_t owner)
{
    if (s_msc != NULL) {
        tinyusb_msc_set_storage_mount_point(s_msc, owner);
    }
}

/* -> DETACHED: show the host nothing. Boot state (not installed) => nothing to do (COM7 live).
 * Installed => tud_disconnect() only; the PHY is never uninstalled (recreate crashes). */
static void enter_detached(void)
{
    if (s_installed) {
        tud_disconnect();
        if (s_cur_mode == NOCSIF_USB_MODE_MSC) {
            vTaskDelay(pdMS_TO_TICKS(150));   /* host registers the unplug + flushes before we remount */
            msc_set_owner(TINYUSB_MSC_STORAGE_MOUNT_APP);   /* firmware regains /sd */
        }
    }
    s_cur_mode = NOCSIF_USB_MODE_DETACHED;
}

/* -> a gadget mode with its descriptor already resolved (dev/cfg). First pick installs the stack
 * (the only usb_new_phy); afterwards every entry is the same proven path: tud_disconnect() +
 * rewrite the active descriptor + tud_connect(). Returns false only on an install failure. */
static bool enter_gadget(nocsif_usb_mode_t mode, const tusb_desc_device_t *dev, const uint8_t *cfg)
{
    if (!s_installed) {
        /* First mode pick: install once (the only usb_new_phy of the session). */
        nocsif_usb_desc_activate(dev, cfg);
        tinyusb_config_t tcfg = TINYUSB_DEFAULT_CONFIG();
        tcfg.descriptor.device            = nocsif_usb_desc_active_device();
        tcfg.descriptor.full_speed_config = nocsif_usb_desc_active_config();
        tcfg.descriptor.string            = nocsif_usb_desc_strings();
        tcfg.descriptor.string_count      = nocsif_usb_desc_string_count();

        /* A3: hand the boot reserve back right before the install so its internal allocations land in
         * that hole. With the reserve released the gate below is satisfied by construction; it can only
         * refuse when the reserve was never claimed (safe mode / boot alloc failure) — say so. */
        if (s_boot_reserve != NULL) {
            heap_caps_free(s_boot_reserve);
            s_boot_reserve = NULL;
            nocsif_log_dma_free("usb: boot reserve released for the install");
        }
        if (nocsif_int_dma_largest() < NOCSIF_RADIO_MIN_DMA_USB) {
            s_fail_reason = "needs memory";
            ESP_LOGE(TAG, "USB install refused: only %u B contiguous internal DMA (need %u) — restart the watch",
                     (unsigned)nocsif_int_dma_largest(), (unsigned)NOCSIF_RADIO_MIN_DMA_USB);
            return false;
        }
        esp_err_t err = tinyusb_driver_install(&tcfg);
        if (err != ESP_OK) {
            s_fail_reason = "install failed";
            ESP_LOGE(TAG, "tinyusb_driver_install(%s) -> %s", mode_name(mode), esp_err_to_name(err));
            return false;
        }
        s_fail_reason = "";
        nocsif_log_dma_free("usb: TinyUSB installed");
        s_installed = true;   /* stays true for the rest of the session */
        cdc_helper_ensure();  /* CDC-ACM ready for whenever CDC is the active descriptor */
        if (mode == NOCSIF_USB_MODE_MSC) {
            /* First pick is File Share: the stack auto-connected with the MSC descriptor but /sd is
             * still APP-owned (from boot), so the host would see "no media". Drop the bus, hand the
             * card to the host, reconnect for a clean enumeration with the drive present. */
            if (s_msc == NULL) {
                return false;                    /* no card -> File Share unavailable */
            }
            tud_disconnect();
            vTaskDelay(pdMS_TO_TICKS(150));
            msc_set_owner(TINYUSB_MSC_STORAGE_MOUNT_USB);
            tud_connect();
        }
        s_cur_mode = mode;
        return true;
    }

    /* Already installed: uniform re-enumeration — disconnect, rewrite the descriptor, reconnect.
     * (Same operations that switch cleanly between two gadget modes; no PHY or helper cycling.) The
     * microSD ownership is flipped while the bus is disconnected: back to the app when LEAVING File
     * Share, to the host when ENTERING it, so the host never sees the MSC interface without media. */
    tud_disconnect();
    vTaskDelay(pdMS_TO_TICKS(150));              /* let the host register the disconnect */
    if (s_cur_mode == NOCSIF_USB_MODE_MSC) {
        msc_set_owner(TINYUSB_MSC_STORAGE_MOUNT_APP);   /* leaving File Share: firmware regains /sd */
    }
    if (mode == NOCSIF_USB_MODE_MSC) {
        if (s_msc == NULL) {
            return false;                        /* no card -> can't enter File Share; caller detaches */
        }
        msc_set_owner(TINYUSB_MSC_STORAGE_MOUNT_USB);   /* entering File Share: host owns the card */
    }
    nocsif_usb_desc_activate(dev, cfg);          /* the descriptor esp_tinyusb returns is now `mode` */
    cdc_helper_ensure();                         /* no-op after the first time */
    tud_connect();                               /* host re-reads the new descriptor + enumerates */
    s_cur_mode = mode;
    return true;
}

/* Apply the latest requested mode (runs on the worker task). */
static void apply_mode(nocsif_usb_mode_t target)
{
    if (target == s_cur_mode) {
        return;
    }
    s_state = NOCSIF_USB_GADGET_STARTING;
    ESP_LOGW(TAG, "USB mode: %s -> %s", mode_name(s_cur_mode), mode_name(target));

    if (target == NOCSIF_USB_MODE_DETACHED) {
        enter_detached();
        s_state = NOCSIF_USB_GADGET_OFF;
        ESP_LOGI(TAG, "USB detached — host sees no device%s.",
                 s_installed ? " (PHY kept; USB-Serial/JTAG returns on reboot)" : "");
        return;
    }

    /* Resolve the target's descriptor BEFORE any teardown. An unknown mode value is rejected here,
     * leaving whatever is currently enumerated untouched — a rejected request must never tear down a
     * working device. */
    const tusb_desc_device_t *dev;
    const uint8_t *cfg;
    if (!desc_for_mode(target, &dev, &cfg)) {
        ESP_LOGW(TAG, "USB mode %s not available — staying on %s",
                 mode_name(target), mode_name(s_cur_mode));
        s_state = (s_cur_mode == NOCSIF_USB_MODE_DETACHED) ? NOCSIF_USB_GADGET_OFF
                                                           : NOCSIF_USB_GADGET_ON;
        return;
    }

    if (enter_gadget(target, dev, cfg)) {
        s_state = NOCSIF_USB_GADGET_ON;
        ESP_LOGI(TAG, "USB mode ON: %s", mode_name(target));
    } else {
        /* enter_gadget only fails on a genuine install error (first pick; nothing enumerated). */
        s_state = NOCSIF_USB_GADGET_FAILED;
        enter_detached();                        /* no-op when not installed; never uninstalls */
    }
}

static void gadget_task(void *arg)
{
    (void)arg;
    volatile uint8_t probe;                          /* Phase A2: is this stack in PSRAM? */
    ESP_LOGI(TAG, "worker up: stack in %s", esp_ptr_external_ram((void *)&probe) ? "PSRAM" : "INTERNAL");
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        apply_mode(s_req_mode);      /* coalesces: always applies the latest requested mode */
    }
}

void nocsif_usb_gadget_boot_reserve(void)
{
    if (s_boot_reserve != NULL) {
        return;                                   /* idempotent */
    }
    /* Claim the entry's memory while the pool is still whole (see s_boot_reserve). Cheap, held until the
     * first mode pick; unconditional (safe mode too — it costs nothing at runtime). */
    s_boot_reserve = heap_caps_malloc(NOCSIF_RADIO_MIN_DMA_USB, NOCSIF_DMA_CAPS);
    if (s_boot_reserve != NULL) {
        nocsif_log_dma_free("usb reserve: entry block claimed (held until the first USB mode pick)");
    } else {
        nocsif_log_dma_free("usb reserve: FAILED to claim the entry block (install will gate live)");
    }
}

esp_err_t nocsif_usb_gadget_init(void)
{
    if (s_task != NULL) {
        return ESP_OK;
    }
    /* A3: the entry reserve is claimed by nocsif_usb_gadget_boot_reserve() from app_main, right after the
     * BLE + I2S reserves (pristine region). Claim it here only if that call was skipped (safe mode). */
    if (s_boot_reserve == NULL) {
        nocsif_usb_gadget_boot_reserve();
    }
    /* Create the MSC storage now (card is up from nocsif_sdcard_init) so /sd is FAT-mounted for the
     * app from boot — Files + Run Macro read it offline; the host only gets it in File Share mode. */
    msc_storage_init_once();
    /* Priority 4 (below the prio-5 TinyUSB task it spawns). 6 KB stack covers descriptor prep
     * + esp_log formatting. Stack in PSRAM (xTaskCreateWithCaps + SPIRAM, RAM Phase A2): this worker only
     * flips USB modes (tinyusb_driver_install / tud_connect / the MSC APP<->USB FAT remount — SD over SPI,
     * never internal flash) and never runs with the flash cache disabled, so its 6 KB no longer sits in
     * the contended internal-DMA pool. Audited in docs/DMA-COEXISTENCE-VERIFICATION.md §3. Never deleted. */
    if (xTaskCreateWithCaps(gadget_task, "usb_gadget", 6144, NULL, 4, &s_task, MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "failed to create usb_gadget task");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "usb_gadget worker ready (idle, DETACHED) — pick a USB mode to attach");
    return ESP_OK;
}

void nocsif_usb_gadget_request_mode(nocsif_usb_mode_t mode)
{
    s_req_mode = mode;
    if (s_task != NULL) {
        xTaskNotifyGive(s_task);     /* non-blocking; safe from an LVGL callback */
    }
}

nocsif_usb_mode_t nocsif_usb_gadget_mode(void)
{
    return s_cur_mode;
}

nocsif_usb_mode_t nocsif_usb_gadget_target_mode(void)
{
    return s_req_mode;   /* the mode being switched TO; meaningful while state == STARTING */
}

nocsif_usb_gadget_state_t nocsif_usb_gadget_state(void)
{
    return s_state;
}

const char *nocsif_usb_gadget_fail_reason(void)
{
    return s_fail_reason;
}

bool nocsif_usb_gadget_hid_ready(void)
{
    /* Gate on a STABLE ON state so a caller mid-switch never touches a torn-down stack. */
    return s_state == NOCSIF_USB_GADGET_ON && s_cur_mode == NOCSIF_USB_MODE_HID &&
           tud_mounted() && tud_hid_ready();
}

nocsif_usb_msc_host_t nocsif_usb_gadget_msc_host(void)
{
    /* Reset the latch whenever we are not actively sharing to an enumerated host (leaving File
     * Share, mid-switch, or the host unplugged) so the next share restarts the settle window. */
    if (s_cur_mode != NOCSIF_USB_MODE_MSC || s_state != NOCSIF_USB_GADGET_ON || !tud_mounted()) {
        s_msc_host_seen_us = 0;
        return NOCSIF_USB_MSC_HOST_NONE;
    }
    int64_t now = esp_timer_get_time();
    if (s_msc_host_seen_us == 0) {
        s_msc_host_seen_us = now;   /* first frame the host is enumerated: start the settle window */
    }
    return (now - s_msc_host_seen_us >= MSC_HOST_SETTLE_US) ? NOCSIF_USB_MSC_HOST_READY
                                                            : NOCSIF_USB_MSC_HOST_SETTLING;
}

/* App-side microSD access for Run Macro. In every non-File-Share mode /sd is already FAT-mounted for
 * the app (the MSC storage is APP-owned), so this just confirms APP ownership and returns OK; in File
 * Share the host owns the card, so a claim is refused. Mode-aware so ducky.c is unchanged. Returns:
 *   ESP_OK                claimed (/sd readable by the app)
 *   ESP_ERR_INVALID_STATE no card, or the host owns it (File Share mode)
 *   ESP_ERR_TIMEOUT       ownership did not confirm in time */
esp_err_t nocsif_usb_gadget_claim_sd(uint32_t timeout_ms)
{
    if (s_msc == NULL) {
        return ESP_ERR_INVALID_STATE;            /* no card / storage */
    }
    if (s_cur_mode == NOCSIF_USB_MODE_MSC) {
        return ESP_ERR_INVALID_STATE;            /* host owns the drive in File Share */
    }
    msc_set_owner(TINYUSB_MSC_STORAGE_MOUNT_APP);
    uint32_t waited = 0;
    for (;;) {
        tinyusb_msc_mount_point_t mp;
        if (tinyusb_msc_get_storage_mount_point(s_msc, &mp) == ESP_OK &&
            mp == TINYUSB_MSC_STORAGE_MOUNT_APP) {
            return ESP_OK;
        }
        if (waited >= timeout_ms) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
        waited += 10;
    }
}

void nocsif_usb_gadget_release_sd(void)
{
    /* Firmware keeps /sd (APP-owned) in every non-File-Share mode; nothing to release. */
}

/* ---- Live capture over CDC (M5-P5+): raw byte pipe to the host serial port -------------- *
 * The CDC-ACM helper is up (cdc_helper_ensure) whenever a gadget mode has been picked, but its TX
 * endpoint only carries bytes to a host while CDC is the enumerated class AND a host app has the
 * port open (DTR asserted). The ESP-IDF console is deliberately NOT redirected onto CDC (see the
 * file header), so the TX FIFO is otherwise idle — a caller may stream arbitrary bytes (e.g. a live
 * PCAP feed) without fighting log text. All three run on the CALLER's task; the esp_tinyusb CDC
 * helper's write ring is safe to feed from a task other than the USB worker. */
bool nocsif_usb_gadget_cdc_ready(void)
{
    return s_cdc_inited && s_state == NOCSIF_USB_GADGET_ON &&
           s_cur_mode == NOCSIF_USB_MODE_CDC && tud_mounted() && tud_cdc_connected();
}

/* Queue up to `len` bytes into the CDC TX ring; returns the count ACCEPTED (0..len — a short
 * return means the ring is full, i.e. the host is not draining fast enough). Never blocks. Bytes
 * accepted are committed to the wire, so the caller must send whole records to stay framed. */
size_t nocsif_usb_gadget_cdc_write(const uint8_t *buf, size_t len)
{
    if (!nocsif_usb_gadget_cdc_ready() || buf == NULL || len == 0) {
        return 0;
    }
    return tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, buf, len);
}

/* Push queued CDC TX bytes toward the host, waiting up to timeout_ms for endpoint space. */
void nocsif_usb_gadget_cdc_flush(uint32_t timeout_ms)
{
    if (!s_cdc_inited) {
        return;
    }
    tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, pdMS_TO_TICKS(timeout_ms));
}
