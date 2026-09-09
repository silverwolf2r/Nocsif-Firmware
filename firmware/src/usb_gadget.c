/*
 * TinyUSB USB mode server implementation (UI-shell P4.5). See usb_gadget.h.
 *
 * The device presents one USB class at a time, picked by the user:
 *   DETACHED  the host sees no NocSif device at all. At boot this means TinyUSB
 *             isn't installed, so the ESP32-S3's USB-Serial/JTAG console (COM7)
 *             stays live for flashing and logs.
 *   CDC       a USB CDC serial port.
 *   HID       a USB HID keyboard (used by Run Macro / Keymap).
 *   MSC       (P4.5.2) mass storage — exposes /sd to the host as a drive.
 *
 * Two ESP32-S3 quirks shape this design, both discovered on-device during the
 * P4.5.1 spike:
 *   1. Deleting and recreating the OTG PHY (usb_del_phy then usb_new_phy) crashes,
 *      though the very first creation works fine. So TinyUSB gets installed
 *      exactly once — lazily, on the first mode pick, so COM7 stays available
 *      until then — and is never uninstalled after that.
 *   2. Cycling the CDC helper (calling tinyusb_cdcacm_init/_deinit plus
 *      tinyusb_console_init/_deinit on every CDC entry/exit) crashes on the
 *      re-init. So the CDC-ACM helper is initialized exactly once and never
 *      deinitialized, and the ESP-IDF console is never redirected onto CDC at
 *      all — log output during a gadget mode just goes to the now-dark
 *      USB-Serial/JTAG port and is effectively unobserved, while the CDC port
 *      itself still enumerates and works fine as a plain serial device.
 *
 * esp_tinyusb stores the descriptor pointers it's handed (it doesn't copy them)
 * and returns them from its own descriptor callbacks. So switching modes is just:
 * rewrite the mutable active descriptor in place, then call
 * tud_disconnect()/tud_connect() so the host re-reads it and re-enumerates:
 *   DETACHED (boot, not installed) -> gadget:  install once, cdcacm_init once, connect.
 *   installed -> gadget:                       tud_disconnect(), rewrite the descriptor, tud_connect().
 *   installed -> DETACHED:                     tud_disconnect() only (the PHY stays installed).
 * Every mode uses a distinct product id, so the host re-reads the descriptor
 * instead of serving up a cached one. The trade-off: once any mode has been
 * picked, going DETACHED shows the host nothing but does not restore
 * USB-Serial/JTAG until the next reboot. At boot, though, DETACHED genuinely has
 * nothing installed, so COM7 stays live.
 *
 * THREADING: all install and switch work runs on this module's own worker task.
 * UI callbacks only ever call nocsif_usb_gadget_request_mode(), which just
 * records the target mode and notifies the worker.
 */
#include "usb_gadget.h"
#include "freertos/idf_additions.h" /* for xTaskCreateWithCaps, used to put the worker's stack in PSRAM (RAM Phase A2) */
#include "esp_heap_caps.h"          /* for MALLOC_CAP_SPIRAM */
#include "esp_memory_utils.h"       /* for esp_ptr_external_ram, used to confirm the stack actually landed in PSRAM */
#include "coex.h"                   /* for NOCSIF_DMA_CAPS and NOCSIF_RADIO_MIN_DMA_USB, the entry-point memory reserve/gate (Phase A3) */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "tinyusb.h"
#include "tinyusb_default_config.h"   /* for the TINYUSB_DEFAULT_CONFIG() macro */
#include "tinyusb_cdc_acm.h"
#include "tinyusb_msc.h"              /* for File Share (MSC) storage over the microSD */
#include "ff.h"                       /* (section 4.15 sd format) for f_mkfs / MKFS_PARM / FF_MAX_SS */
#include "diskio_impl.h"              /* for ff_diskio_get_drive / ff_diskio_unregister */
#include "diskio_sdmmc.h"             /* for ff_diskio_register_sdmmc */
#include "nocsif_usb_desc.h"          /* for the per-mode and runtime-active USB descriptors */
#include "sdcard.h"                   /* for nocsif_sdcard_card(), the raw card object MSC wraps */
#include "esp_log.h"
#include "esp_timer.h"                /* for esp_timer_get_time(), used by the File Share host-mount settle window */

#define SD_MOUNT_POINT  "/sd"

static const char *TAG = "usb_gadget";

static TaskHandle_t                        s_task;
static volatile nocsif_usb_gadget_state_t  s_state    = NOCSIF_USB_GADGET_OFF;
static volatile nocsif_usb_mode_t          s_cur_mode = NOCSIF_USB_MODE_DETACHED;
static volatile nocsif_usb_mode_t          s_req_mode = NOCSIF_USB_MODE_DETACHED;
static bool                                s_installed;    /* whether TinyUSB has been installed; once true it's never torn down again */
static bool                                s_cdc_inited;   /* whether tinyusb_cdcacm_init has run yet */
static tinyusb_msc_storage_handle_t        s_msc;          /* the File Share storage handle; only non-NULL while in MSC mode */
/* (RAM Phase A3) The USB entry point's reserved memory, claimed at boot. The
 * one-time tinyusb_driver_install call needs roughly 5-7 KB of internal memory at
 * runtime (esp_tinyusb's device task has a fixed 4 KB internal stack with no way
 * to redirect it, plus its own context, the CDC-ACM rings, and the MSC FAT
 * handoff). Before Phase A, the largest contiguous block available in steady
 * state was only about 2 KB and the install simply refused to run; A1/A2 raised
 * that to roughly 30 KB, but Signal Hunt's WiFi monitor can still bring it down
 * to about 21 KB, and nothing prevents some future feature from eating the rest.
 * So: claim NOCSIF_RADIO_MIN_DMA_USB from the still-pristine boot pool inside
 * nocsif_usb_gadget_init, before runtime activity has a chance to fragment it,
 * and free it again right before the install so the install's allocations land
 * in that hole regardless of what else has happened since boot. An earlier
 * stop-gap approach that instead stopped WiFi on File-Share entry
 * (msc_shed_wifi, never merged) was measured to recover zero contiguity, since
 * runtime teardown never actually defragments memory
 * (see docs/DMA-COEXISTENCE-PLAN.md). */
static void       *s_boot_reserve;                          /* becomes NULL once released, or if it was never successfully claimed */
static const char *s_fail_reason = "";                      /* an honest description of why the last switch failed */

/* File Share host-mount settle window (P4.5.4). Since there's no device-visible
 * signal for "the filesystem is actually mounted", once a host has enumerated the
 * drive (tud_mounted) this window is used to hold the "preparing..." indicator up
 * for long enough to cover the host OS's own mount latency (measured at roughly
 * 5s on Windows). s_msc_host_seen_us latches the moment enumeration was first
 * seen; it's only ever written from nocsif_usb_gadget_msc_host() on the LVGL
 * task, so it's a simple monotonic latch with no locking needed. */
#define MSC_HOST_SETTLE_US   (5 * 1000 * 1000)
static int64_t                             s_msc_host_seen_us;   /* 0 means no host has enumerated yet */

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

/* Resolves a mode to its per-mode source device and config descriptors. Returns
 * false for an unrecognized value, so the caller can reject the request without
 * disturbing whatever is currently enumerated. This function is pure (no side
 * effects); the MSC storage lifecycle is handled separately, around the switch
 * itself. */
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
        return false;   /* an unrecognized mode value */
    }
}

/* Brings the CDC-ACM helper up exactly once — it's never deinitialized again,
 * since re-init crashes on-device. Safe to call even while the active descriptor
 * has no CDC interface at all; the helper just stays idle until a CDC
 * descriptor actually gets enumerated. */
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

/* File Share (MSC) storage: created once at boot over the raw microSD and
 * never deleted afterward; File Share just toggles who currently owns it.
 * mount_point=APP means the esp_tinyusb MSC helper FAT-mounts /sd for the
 * firmware itself, so Files and Run Macro can read it offline with no host
 * needed; mount_point=USB instead hands the raw card over to a connected host.
 * Auto-mount is deliberately disabled, so ownership only ever changes on our own
 * explicit File Share on/off action — the default auto-mount behavior would flip
 * the card over to the host on any raw USB connect, which would be wrong for the
 * single-class CDC/HID modes that need to keep /sd available to the app.
 * Creating the storage only installs the MSC glue driver, not the TinyUSB
 * stack/PHY itself, so it's safe to do before tinyusb_driver_install (the same
 * order proven out in M4). s_msc stays NULL when there's no card present, in
 * which case File Share is simply unavailable and /sd stays unmounted. */
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
    /* Installs the MSC glue driver with auto-mount off, so this module controls the app<->USB handoff explicitly. */
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
            .do_not_format = true,     /* never format the user's existing card */
            .format_flags = 0,
        },
        .mount_point = TINYUSB_MSC_STORAGE_MOUNT_APP,  /* the app owns /sd at boot, for offline reads */
    };
    err = tinyusb_msc_new_storage_sdmmc(&cfg, &s_msc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_msc_new_storage_sdmmc -> %s", esp_err_to_name(err));
        s_msc = NULL;
        return;
    }
    ESP_LOGI(TAG, "MSC storage ready; /sd mounted for the app (host gets it only in File Share)");
}

/* Sets who currently owns the microSD: APP means the firmware FAT-mounts /sd,
 * USB means the raw card is handed to the host. The helper does the FAT
 * unmount/remount synchronously. No-op if there's no storage at all. */
static void msc_set_owner(tinyusb_msc_mount_point_t owner)
{
    if (s_msc != NULL) {
        tinyusb_msc_set_storage_mount_point(s_msc, owner);
    }
}

/* Switches to DETACHED: show the host nothing. If TinyUSB was never installed
 * (boot state), there's nothing to do and COM7 stays live. If it was already
 * installed, this just calls tud_disconnect() — the PHY itself is never
 * uninstalled, since recreating it crashes. */
static void enter_detached(void)
{
    if (s_installed) {
        tud_disconnect();
        if (s_cur_mode == NOCSIF_USB_MODE_MSC) {
            vTaskDelay(pdMS_TO_TICKS(150));   /* give the host time to register the unplug and flush before we remount */
            msc_set_owner(TINYUSB_MSC_STORAGE_MOUNT_APP);   /* the firmware regains ownership of /sd */
        }
    }
    s_cur_mode = NOCSIF_USB_MODE_DETACHED;
}

/* Switches to a gadget mode whose descriptor has already been resolved
 * (dev/cfg). The very first pick installs the whole stack (the only
 * usb_new_phy call of the session); every entry after that follows the same
 * proven path: tud_disconnect(), rewrite the active descriptor, tud_connect().
 * Only returns false on a genuine install failure. */
static bool enter_gadget(nocsif_usb_mode_t mode, const tusb_desc_device_t *dev, const uint8_t *cfg)
{
    if (!s_installed) {
        /* the first mode pick: install once — the only usb_new_phy call this session will ever make */
        nocsif_usb_desc_activate(dev, cfg);
        tinyusb_config_t tcfg = TINYUSB_DEFAULT_CONFIG();
        tcfg.descriptor.device            = nocsif_usb_desc_active_device();
        tcfg.descriptor.full_speed_config = nocsif_usb_desc_active_config();
        tcfg.descriptor.string            = nocsif_usb_desc_strings();
        tcfg.descriptor.string_count      = nocsif_usb_desc_string_count();

        /* (Phase A3) Hand the boot reserve back right before the install, so its
         * internal allocations land in that freed hole. With the reserve released, the
         * gate check below is satisfied by construction; it can only refuse if the
         * reserve was never successfully claimed in the first place (safe mode, or a
         * boot allocation failure) — and if so, say why. */
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
        cdc_helper_ensure();  /* get CDC-ACM ready for whenever CDC ends up being the active descriptor */
        if (mode == NOCSIF_USB_MODE_MSC) {
            /* The very first pick is File Share: the stack already auto-connected using
             * the MSC descriptor, but /sd is still owned by the app (from boot), so the
             * host would just see "no media". Drop the bus, hand the card over to the
             * host, then reconnect so the drive enumerates cleanly with media present. */
            if (s_msc == NULL) {
                return false;                    /* no card present, so File Share isn't available */
            }
            tud_disconnect();
            vTaskDelay(pdMS_TO_TICKS(150));
            msc_set_owner(TINYUSB_MSC_STORAGE_MOUNT_USB);
            tud_connect();
        }
        s_cur_mode = mode;
        return true;
    }

    /* Already installed: a uniform re-enumeration sequence — disconnect, rewrite
     * the descriptor, reconnect. (The same operations used to switch cleanly between
     * any two gadget modes; no PHY or helper cycling involved.) The microSD's
     * ownership is flipped while the bus is disconnected: back to the app when
     * leaving File Share, over to the host when entering it, so the host never sees
     * the MSC interface without any media behind it. */
    tud_disconnect();
    vTaskDelay(pdMS_TO_TICKS(150));              /* give the host time to register the disconnect */
    if (s_cur_mode == NOCSIF_USB_MODE_MSC) {
        msc_set_owner(TINYUSB_MSC_STORAGE_MOUNT_APP);   /* leaving File Share: the firmware regains /sd */
    }
    if (mode == NOCSIF_USB_MODE_MSC) {
        if (s_msc == NULL) {
            return false;                        /* no card present, so File Share can't be entered; the caller falls back to detached */
        }
        msc_set_owner(TINYUSB_MSC_STORAGE_MOUNT_USB);   /* entering File Share: the host takes ownership of the card */
    }
    nocsif_usb_desc_activate(dev, cfg);          /* whatever descriptor esp_tinyusb returns from now on reflects `mode` */
    cdc_helper_ensure();                         /* a no-op after the very first call */
    tud_connect();                               /* the host re-reads the new descriptor and re-enumerates */
    s_cur_mode = mode;
    return true;
}

/* Applies the most recently requested mode; runs on the worker task. */
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

    /* Resolve the target mode's descriptor before doing any teardown. An
     * unrecognized mode is rejected right here, leaving whatever is currently
     * enumerated completely untouched — a rejected request must never tear down a
     * device that's actually working. */
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
        /* enter_gadget only ever fails on a genuine install error during the first pick, before anything is enumerated */
        s_state = NOCSIF_USB_GADGET_FAILED;
        enter_detached();                        /* a no-op if nothing is installed yet; never uninstalls anything that is */
    }
}

static void gadget_task(void *arg)
{
    (void)arg;
    volatile uint8_t probe;                          /* (Phase A2) checks whether this worker's stack actually ended up in PSRAM */
    ESP_LOGI(TAG, "worker up: stack in %s", esp_ptr_external_ram((void *)&probe) ? "PSRAM" : "INTERNAL");
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        apply_mode(s_req_mode);      /* naturally coalesces multiple requests: always applies whatever the latest requested mode is */
    }
}

void nocsif_usb_gadget_boot_reserve(void)
{
    if (s_boot_reserve != NULL) {
        return;                                   /* safe to call more than once */
    }
    /* Claims the entry point's memory while the pool is still whole (see the
     * s_boot_reserve comment above). Cheap to do, and held onto until the first mode
     * pick; unconditional — even in safe mode, since it costs nothing at runtime. */
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
    /* (Phase A3) The entry-point reserve is normally claimed by
     * nocsif_usb_gadget_boot_reserve() from app_main, right after the BLE and I2S
     * reserves, while memory is still pristine. Only claim it here if that earlier
     * call was skipped, e.g. in safe mode. */
    if (s_boot_reserve == NULL) {
        nocsif_usb_gadget_boot_reserve();
    }
    /* Create the MSC storage now that the card is up (from nocsif_sdcard_init), so
     * /sd is FAT-mounted for the app right from boot — Files and Run Macro can read
     * it offline; the host only gets access to it while in File Share mode. */
    msc_storage_init_once();
    /* Priority 4, below the priority-5 TinyUSB task this worker itself spawns. A
     * 6 KB stack covers descriptor prep plus esp_log formatting. The stack lives in
     * PSRAM (via xTaskCreateWithCaps + SPIRAM, RAM Phase A2): this worker only ever
     * flips USB modes — tinyusb_driver_install, tud_connect, the MSC app<->USB FAT
     * remount (which goes over SPI to the SD card, never touching internal flash) —
     * and never runs with the flash cache disabled, so its 6 KB no longer competes
     * for the contended internal-DMA memory pool. Audited in
     * docs/DMA-COEXISTENCE-VERIFICATION.md section 3. Never deleted. */
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
        xTaskNotifyGive(s_task);     /* non-blocking; safe to call from an LVGL callback */
    }
}

nocsif_usb_mode_t nocsif_usb_gadget_mode(void)
{
    return s_cur_mode;
}

nocsif_usb_mode_t nocsif_usb_gadget_target_mode(void)
{
    return s_req_mode;   /* the mode currently being switched to; only meaningful while state == STARTING */
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
    /* Gate on a stable ON state, so a caller mid-switch never touches a torn-down stack. */
    return s_state == NOCSIF_USB_GADGET_ON && s_cur_mode == NOCSIF_USB_MODE_HID &&
           tud_mounted() && tud_hid_ready();
}

nocsif_usb_msc_host_t nocsif_usb_gadget_msc_host(void)
{
    /* Resets the latch whenever we're not actively sharing to an enumerated host
     * — leaving File Share, mid-switch, or the host has unplugged — so the next
     * share session restarts the settle window from scratch. */
    if (s_cur_mode != NOCSIF_USB_MODE_MSC || s_state != NOCSIF_USB_GADGET_ON || !tud_mounted()) {
        s_msc_host_seen_us = 0;
        return NOCSIF_USB_MSC_HOST_NONE;
    }
    int64_t now = esp_timer_get_time();
    if (s_msc_host_seen_us == 0) {
        s_msc_host_seen_us = now;   /* the first frame in which the host is seen enumerated: start the settle window */
    }
    return (now - s_msc_host_seen_us >= MSC_HOST_SETTLE_US) ? NOCSIF_USB_MSC_HOST_READY
                                                            : NOCSIF_USB_MSC_HOST_SETTLING;
}

/* App-side microSD access, used by Run Macro. In every mode other than File
 * Share, /sd is already FAT-mounted for the app (the MSC storage is app-owned),
 * so this just confirms that ownership and returns OK; in File Share the host
 * owns the card, so a claim is refused. This is mode-aware specifically so
 * ducky.c doesn't need to change. Returns:
 *   ESP_OK                  claimed; /sd is readable by the app
 *   ESP_ERR_INVALID_STATE   no card present, or the host owns it (File Share mode)
 *   ESP_ERR_TIMEOUT         ownership didn't confirm in time */
esp_err_t nocsif_usb_gadget_claim_sd(uint32_t timeout_ms)
{
    if (s_msc == NULL) {
        return ESP_ERR_INVALID_STATE;            /* no card or storage present */
    }
    if (s_cur_mode == NOCSIF_USB_MODE_MSC) {
        return ESP_ERR_INVALID_STATE;            /* the host owns the drive while in File Share mode */
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
    /* The firmware already keeps /sd in every non-File-Share mode, so there's nothing to actually release. */
}

/* (section 4.15, see the header for more) The helper mounts via
 * ff_diskio_register_sdmmc plus f_mount on its own FATFS object rather than
 * esp_vfs_fat_sdmmc_mount, so IDF's esp_vfs_fat_sdcard_format can't find it.
 * Instead this uses the helper's own mount-point switch to cleanly unmount,
 * formats through a temporary diskio slot, and switches back so the helper
 * remounts the freshly formatted volume. */
const char *nocsif_usb_gadget_sd_format(void)
{
    if (s_msc == NULL) return "no microSD storage";
    if (s_cur_mode == NOCSIF_USB_MODE_MSC) return "File Share has the card";
    sdmmc_card_t *card = nocsif_sdcard_card();
    if (card == NULL) return "no microSD card";

    /* 1. Drop the app's FAT mount (the helper unmounts via f_mount(0) and
     *    unregisters its diskio slot). No host is enumerated on MSC in this mode, so
     *    "USB owns it" here really just means nobody is touching the card. */
    msc_set_owner(TINYUSB_MSC_STORAGE_MOUNT_USB);
    for (int i = 0; i < 200; i++) {
        tinyusb_msc_mount_point_t mp;
        if (tinyusb_msc_get_storage_mount_point(s_msc, &mp) == ESP_OK && mp == TINYUSB_MSC_STORAGE_MOUNT_USB) break;
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    /* 2. Format through a throwaway diskio registration. The work buffer must be
     *    at least one sector (FF_MAX_SS); allocating it DMA-capable keeps the SPI
     *    host from having to bounce-buffer it. */
    const char *err = NULL;
    BYTE pdrv = 0xff;
    void *work = heap_caps_malloc(FF_MAX_SS, MALLOC_CAP_DMA);
    if (work == NULL) work = heap_caps_malloc(FF_MAX_SS, MALLOC_CAP_SPIRAM);
    if (work == NULL) {
        err = "out of memory";
    } else if (ff_diskio_get_drive(&pdrv) != ESP_OK) {
        err = "no free disk slot";
    } else {
        ff_diskio_register_sdmmc(pdrv, card);
        char drv[3] = { (char)('0' + pdrv), ':', 0 };
        const MKFS_PARM opt = { .fmt = FM_ANY, .n_fat = 1, .align = 0, .n_root = 0, .au_size = 16 * 1024 };
        FRESULT fr = f_mkfs(drv, &opt, work, FF_MAX_SS);
        ff_diskio_unregister(pdrv);
        if (fr != FR_OK) {
            ESP_LOGE(TAG, "sd format: f_mkfs -> %d", (int)fr);
            err = "format failed";
        }
    }
    if (work) heap_caps_free(work);

    /* 3. Hand it back: the helper remounts /sd for the app on the newly formatted volume. */
    msc_set_owner(TINYUSB_MSC_STORAGE_MOUNT_APP);
    bool back = false;
    for (int i = 0; i < 300; i++) {
        tinyusb_msc_mount_point_t mp;
        if (tinyusb_msc_get_storage_mount_point(s_msc, &mp) == ESP_OK && mp == TINYUSB_MSC_STORAGE_MOUNT_APP) { back = true; break; }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!err && !back) err = "formatted, but /sd did not remount — restart the watch";
    ESP_LOGW(TAG, "sd format: %s", err ? err : "ok, /sd remounted");
    return err;
}

/* ---- live capture over CDC (M5-P5+): a raw byte pipe to the host serial port ---- *
 * The CDC-ACM helper comes up (via cdc_helper_ensure) whenever any gadget mode
 * has been picked, but its TX endpoint only actually carries bytes to a host
 * while CDC is the enumerated class and a host app has the port open (DTR
 * asserted). The ESP-IDF console is deliberately never redirected onto CDC (see
 * the file header), so its TX FIFO is otherwise idle — a caller can stream
 * arbitrary bytes (e.g. a live PCAP feed) without competing with log text. All
 * three functions below run on the calling task; the esp_tinyusb CDC helper's
 * write ring is safe to feed from any task other than the USB worker itself. */
bool nocsif_usb_gadget_cdc_ready(void)
{
    return s_cdc_inited && s_state == NOCSIF_USB_GADGET_ON &&
           s_cur_mode == NOCSIF_USB_MODE_CDC && tud_mounted() && tud_cdc_connected();
}

/* Queues up to `len` bytes into the CDC TX ring, returning the count actually
 * accepted (0..len — a short return means the ring is full, i.e. the host isn't
 * draining fast enough). Never blocks. Accepted bytes are already committed to
 * the wire, so the caller needs to send whole records at a time to stay framed. */
size_t nocsif_usb_gadget_cdc_write(const uint8_t *buf, size_t len)
{
    if (!nocsif_usb_gadget_cdc_ready() || buf == NULL || len == 0) {
        return 0;
    }
    return tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, buf, len);
}

/* Pushes any queued CDC TX bytes toward the host, waiting up to timeout_ms for endpoint space to free up. */
void nocsif_usb_gadget_cdc_flush(uint32_t timeout_ms)
{
    if (!s_cdc_inited) {
        return;
    }
    tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, pdMS_TO_TICKS(timeout_ms));
}
