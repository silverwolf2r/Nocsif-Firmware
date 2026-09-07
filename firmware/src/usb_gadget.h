/*
 * NocSif — USB mode server public API (UI-shell P4.5). Implementation in usb_gadget.c.
 *
 * The ESP32-S3 has ONE internal USB PHY, shared between the ROM USB-Serial/JTAG
 * peripheral (the boot/dev console on COM7) and the USB-OTG controller TinyUSB drives.
 * tinyusb_driver_install() hands the PHY to USB-OTG, so COM7 goes dark the instant it
 * runs. To keep the everyday flash->read-COM7 loop intact, the install is DEFERRED: a
 * worker task idles until the user picks a USB mode, then it installs TinyUSB ONCE.
 *
 * P4.5 presents ONE USB class at a time (DETACHED / CDC serial / HID keyboard; MSC in
 * P4.5.2), chosen at runtime. The driver is installed exactly once and NEVER uninstalled;
 * modes switch by rewriting a mutable descriptor in place + tud_disconnect()/tud_connect()
 * (see usb_gadget.c for the two on-device constraints that force this). The ESP-IDF console
 * is NOT redirected onto CDC (that redirect crashes on re-init), so logs during a gadget
 * mode stay on the — dark — USB-Serial/JTAG and return on reboot.
 *
 * History: P2 = bare CDC, P3 = CDC+MSC, P4 = a CDC+MSC+HID composite (single always-on
 * descriptor). P4.5 replaced the always-composite with runtime single-class selection.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NOCSIF_USB_GADGET_OFF = 0,   /* host sees no device (DETACHED). Before the first mode pick
                                  * TinyUSB is not installed and COM7 is live; after the first pick
                                  * the PHY is kept and COM7 does not return until reboot. */
    NOCSIF_USB_GADGET_STARTING,  /* a mode switch is in progress */
    NOCSIF_USB_GADGET_ON,        /* a gadget class (CDC or HID) is enumerated and ready */
    NOCSIF_USB_GADGET_FAILED,    /* the switch failed (see the last USB-Serial/JTAG log) */
} nocsif_usb_gadget_state_t;

/* USB mode (UI-shell P4.5). The device presents ONE class at a time; at boot DETACHED installs
 * nothing (USB-Serial/JTAG stays live). Switching modes re-enumerates the host by rewriting the
 * active descriptor in place + tud_disconnect()/tud_connect() — the driver is installed once and
 * never uninstalled (uninstall+reinstall recreates the OTG PHY, which crashes; see usb_gadget.c).
 * MSC (File Share) lands in P4.5.2; DETACHED/CDC/HID are live in the P4.5.1 spike. */
typedef enum {
    NOCSIF_USB_MODE_DETACHED = 0,   /* nothing installed; host sees no device; COM7 available */
    NOCSIF_USB_MODE_CDC,            /* Console: USB CDC serial only */
    NOCSIF_USB_MODE_HID,            /* HID keyboard only */
    NOCSIF_USB_MODE_MSC,            /* File Share: USB mass-storage only (P4.5.2) */
} nocsif_usb_mode_t;

/* RAM Phase A3 — claim the File-Share entry's internal-DMA block (NOCSIF_RADIO_MIN_DMA_USB, coex.h) from
 * the pristine boot pool. Call from app_main right after the BLE + audio reserves (before nocsif_ui_init's
 * esp_wifi_init) so it lands next to the other permanent claims instead of splitting the post-WiFi
 * tail. Released just before the one-time tinyusb_driver_install. Idempotent; never fails loudly. */
void nocsif_usb_gadget_boot_reserve(void);

/* Create the idle worker task that installs TinyUSB on request. Idempotent; call once
 * at boot. Does NOT touch USB by itself — the PHY stays on USB-Serial/JTAG until a
 * start request arrives. */
esp_err_t nocsif_usb_gadget_init(void);

/* Request a switch to `mode`. Non-blocking and safe from an LVGL callback — it only records the
 * target and signals the worker task, which does the (blocking) disconnect + descriptor rewrite +
 * reconnect off the UI thread (no driver teardown/reinstall). A request equal to the current mode
 * is a no-op. Switching between two gadget modes re-enumerates (host sees a brief unplug/replug). */
void nocsif_usb_gadget_request_mode(nocsif_usb_mode_t mode);

/* The active USB mode (what is currently enumerated). LVGL-safe (a plain read). */
nocsif_usb_mode_t nocsif_usb_gadget_mode(void);

/* The mode a switch is heading TO — meaningful while nocsif_usb_gadget_state()==STARTING (lets the
 * UI show a "switching…" hint on the specific target row). LVGL-safe (a plain read). */
nocsif_usb_mode_t nocsif_usb_gadget_target_mode(void);

/* Current state, for the UI to poll. */
nocsif_usb_gadget_state_t nocsif_usb_gadget_state(void);

/* Why the last switch FAILED, as a short user-facing phrase ("needs memory", "install failed"); "" when
 * no failure is recorded. RAM Phase A3: the entry is reserved at boot, so "needs memory" only appears
 * when that reserve could not be claimed (safe mode / boot alloc failure). LVGL-safe (a plain read). */
const char *nocsif_usb_gadget_fail_reason(void);

/* True once the composite has enumerated AND the HID keyboard endpoint is ready to send —
 * the precondition for playing a macro. */
bool nocsif_usb_gadget_hid_ready(void);

/* File Share (MSC) host state, for the P4.5.4 switching indicator. MSC exposes no device-visible
 * "the host mounted the filesystem" signal — the esp_tinyusb MSC helper owns the SCSI layer, so a
 * host READ/mount is invisible to app code. This is therefore gated on real host ENUMERATION
 * (tud_mounted) plus a short settle window that covers the host OS's mount latency (~5 s on Windows,
 * a heuristic — not a true mount callback). Meaningful only while mode()==MSC && state()==ON:
 *   NONE      not sharing, or no host has enumerated the drive yet  -> "waiting for host";
 *   SETTLING  host enumerated, still inside the mount-settle window  -> "preparing…";
 *   READY     settle window elapsed; assume the drive is mounted     -> "/sd on host".
 * LVGL-safe: a plain state read + a monotonic latch. Poll it from the UI. */
typedef enum {
    NOCSIF_USB_MSC_HOST_NONE = 0,
    NOCSIF_USB_MSC_HOST_SETTLING,
    NOCSIF_USB_MSC_HOST_READY,
} nocsif_usb_msc_host_t;
nocsif_usb_msc_host_t nocsif_usb_gadget_msc_host(void);

/* Deterministically claim the microSD for app-side FAT access (USB-MSC ownership ->
 * MOUNT_APP), so firmware can read a file from /sd. Does NOT rely on a host eject (which is
 * unreliable — see the P3 handoff note). Blocks up to timeout_ms for the switch to confirm
 * via the mount-point getter. Returns:
 *   ESP_OK               claimed (FAT mounted for the app)
 *   ESP_ERR_INVALID_STATE MSC storage not up (CDC-only or gadget off)
 *   ESP_ERR_TIMEOUT      switch did not confirm in time
 * NOTE: while claimed, a connected host sees the drive as "no media" — expected. */
esp_err_t nocsif_usb_gadget_claim_sd(uint32_t timeout_ms);

/* Release the microSD back to the host (USB-MSC ownership -> MOUNT_USB). Best-effort. */
void nocsif_usb_gadget_release_sd(void);

/* ---- Live capture over CDC (M5-P5+) ---------------------------------------------------- *
 * A raw byte pipe to the host over the CDC serial port, for streaming a live PCAP feed (frames
 * captured in monitor mode) so a host tool (Wireshark via the bundled extcap) reads them in real
 * time instead of retrieving a .pcap off the card. The ESP-IDF console is NOT on CDC, so the TX
 * endpoint is free for arbitrary bytes. All three are safe to call from any task. */

/* True only when CDC is the enumerated class, the switch is settled (state ON), and a host app has
 * the port open (DTR). The precondition for streaming; poll it before/while feeding bytes. */
bool nocsif_usb_gadget_cdc_ready(void);

/* Queue up to `len` bytes into the CDC TX ring; returns bytes ACCEPTED (0..len). A short return
 * means the host is not draining fast enough. Never blocks. Bytes accepted are on the wire, so
 * send whole records to stay framed. Returns 0 when !cdc_ready(). */
size_t nocsif_usb_gadget_cdc_write(const uint8_t *buf, size_t len);

/* Push queued CDC TX bytes toward the host, waiting up to timeout_ms for endpoint space. */
void nocsif_usb_gadget_cdc_flush(uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
