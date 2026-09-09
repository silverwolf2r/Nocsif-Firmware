/*
 * USB mode server public API (UI-shell P4.5). Implementation lives in usb_gadget.c.
 *
 * The ESP32-S3 only has one internal USB PHY, shared between the ROM
 * USB-Serial/JTAG peripheral (the boot/dev console on COM7) and the USB-OTG
 * controller that TinyUSB drives. Calling tinyusb_driver_install() hands the PHY
 * over to USB-OTG, which means COM7 goes dark the moment it runs. To keep the usual
 * flash-then-read-COM7 workflow working, the install is deferred: a worker task
 * idles until the user actually picks a USB mode, and only then installs TinyUSB,
 * exactly once.
 *
 * As of P4.5, the device presents exactly one USB class at a time — detached, CDC
 * serial, or HID keyboard (MSC arrives in P4.5.2) — chosen at runtime. The driver
 * is installed once and never uninstalled again; switching between modes works by
 * rewriting a mutable descriptor in place and calling tud_disconnect()/tud_connect()
 * (see usb_gadget.c for the two on-device constraints that force this approach). The
 * ESP-IDF console is deliberately not redirected onto CDC, since that redirect
 * crashes on re-init, so log output during a gadget mode stays on the — now dark —
 * USB-Serial/JTAG port and only comes back after a reboot.
 *
 * History: P2 shipped bare CDC, P3 added CDC+MSC, P4 combined CDC+MSC+HID into one
 * always-on composite descriptor. P4.5 replaced that always-composite approach with
 * runtime single-class selection.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NOCSIF_USB_GADGET_OFF = 0,   /* the host sees no device at all. Before the first mode is ever picked, TinyUSB
                                  * isn't installed and COM7 stays live; after the first pick, the PHY is
                                  * kept installed and COM7 doesn't come back until the next reboot. */
    NOCSIF_USB_GADGET_STARTING,  /* a mode switch is currently in progress */
    NOCSIF_USB_GADGET_ON,        /* a gadget class (CDC or HID) is enumerated and ready for use */
    NOCSIF_USB_GADGET_FAILED,    /* the switch failed; see the last USB-Serial/JTAG log line for why */
} nocsif_usb_gadget_state_t;

/* USB mode (UI-shell P4.5). The device only ever presents one class at a time; at
 * boot, DETACHED means nothing is installed and USB-Serial/JTAG stays live.
 * Switching modes re-enumerates the host by rewriting the active descriptor in
 * place and calling tud_disconnect()/tud_connect() — the driver is installed once
 * and never torn down again, since uninstalling and reinstalling recreates the OTG
 * PHY, which crashes (see usb_gadget.c). MSC (File Share) arrives in P4.5.2;
 * DETACHED/CDC/HID are already live as of the P4.5.1 spike. */
typedef enum {
    NOCSIF_USB_MODE_DETACHED = 0,   /* nothing installed; the host sees no device; COM7 stays available */
    NOCSIF_USB_MODE_CDC,            /* Console mode: USB CDC serial only */
    NOCSIF_USB_MODE_HID,            /* HID keyboard only */
    NOCSIF_USB_MODE_MSC,            /* File Share: USB mass-storage only (P4.5.2) */
} nocsif_usb_mode_t;

/* (RAM Phase A3) Claims the File-Share entry point's internal-DMA block
 * (NOCSIF_RADIO_MIN_DMA_USB, coex.h) from the pristine boot memory pool. Call this
 * from app_main right after the BLE and audio reserves, and before
 * nocsif_ui_init's esp_wifi_init call, so it lands alongside the other permanent
 * claims instead of fragmenting the memory left after WiFi comes up. It's released
 * again just before the one-time tinyusb_driver_install call. Safe to call more
 * than once; never fails loudly. */
void nocsif_usb_gadget_boot_reserve(void);

/* Creates the idle worker task that installs TinyUSB the first time it's asked
 * to. Safe to call more than once; call it once at boot. Doesn't touch USB by
 * itself — the PHY stays assigned to USB-Serial/JTAG until an actual start
 * request comes in. */
esp_err_t nocsif_usb_gadget_init(void);

/* Requests a switch to `mode`. Non-blocking and safe to call from an LVGL
 * callback — it only records the target mode and wakes the worker task, which
 * does the actual (blocking) disconnect, descriptor rewrite, and reconnect off
 * the UI thread (no driver teardown or reinstall involved). Requesting the mode
 * that's already active is a no-op. Switching between two gadget modes causes a
 * brief re-enumeration, so the host sees an unplug/replug. */
void nocsif_usb_gadget_request_mode(nocsif_usb_mode_t mode);

/* The currently enumerated USB mode. Safe to call from the LVGL task, since it's a plain read. */
nocsif_usb_mode_t nocsif_usb_gadget_mode(void);

/* The mode a switch is currently heading toward — only meaningful while
 * nocsif_usb_gadget_state() returns STARTING; lets the UI show a "switching..."
 * hint on the specific target row. Safe to call from the LVGL task. */
nocsif_usb_mode_t nocsif_usb_gadget_target_mode(void);

/* The current state, for the UI to poll. */
nocsif_usb_gadget_state_t nocsif_usb_gadget_state(void);

/* A short, user-facing phrase describing why the last switch failed (e.g. "needs
 * memory", "install failed"), or "" if no failure is recorded. Under RAM Phase
 * A3, the entry point's memory is reserved at boot, so "needs memory" should only
 * ever show up if that reservation itself couldn't be claimed (safe mode, or a
 * boot allocation failure). Safe to call from the LVGL task. */
const char *nocsif_usb_gadget_fail_reason(void);

/* True once the composite has enumerated and the HID keyboard endpoint is ready
 * to accept data — this is the precondition for playing back a macro. */
bool nocsif_usb_gadget_hid_ready(void);

/* File Share (MSC) host state, used by the P4.5.4 switching indicator. MSC gives
 * no device-visible signal for "the host has actually mounted the filesystem" —
 * the esp_tinyusb MSC helper owns the SCSI layer, so a host read or mount is
 * invisible to application code. So instead this is gated on real host
 * enumeration (tud_mounted) plus a short settle window meant to cover the host
 * OS's own mount latency (roughly 5s on Windows — a heuristic, not a genuine
 * mount callback). Only meaningful while mode() == MSC and state() == ON:
 *   NONE      not sharing, or no host has enumerated the drive yet -> "waiting for host"
 *   SETTLING  host enumerated, still inside the settle window -> "preparing..."
 *   READY     settle window has elapsed; assume the drive is mounted -> "/sd on host"
 * Safe to call from the LVGL task: just a state read plus a monotonic latch. Poll
 * it from the UI. */
typedef enum {
    NOCSIF_USB_MSC_HOST_NONE = 0,
    NOCSIF_USB_MSC_HOST_SETTLING,
    NOCSIF_USB_MSC_HOST_READY,
} nocsif_usb_msc_host_t;
nocsif_usb_msc_host_t nocsif_usb_gadget_msc_host(void);

/* Deterministically claims the microSD for app-side FAT access (moves USB-MSC
 * ownership to MOUNT_APP), so firmware can read a file from /sd. Doesn't rely on
 * the host ejecting the drive, which is unreliable (see the P3 handoff note).
 * Blocks for up to timeout_ms waiting for the switch to confirm via the
 * mount-point getter. Returns:
 *   ESP_OK                 claimed; FAT is mounted for the app
 *   ESP_ERR_INVALID_STATE  MSC storage isn't up (CDC-only mode, or the gadget is off)
 *   ESP_ERR_TIMEOUT        the switch didn't confirm in time
 * While claimed, a connected host will see the drive report "no media" —
 * expected behavior. */
esp_err_t nocsif_usb_gadget_claim_sd(uint32_t timeout_ms);

/* Hands the microSD back to the host (moves USB-MSC ownership to MOUNT_USB). Best-effort. */
void nocsif_usb_gadget_release_sd(void);

/* (section 4.15) Reformats the microSD with a fresh FAT filesystem. The MSC
 * helper owns the FAT mount, so this lives here: it hands the card over to the
 * idle, non-enumerated USB side to drop the app's mount, runs f_mkfs against a
 * throwaway disk registration, then hands it back so the helper remounts the
 * fresh volume. The caller must already hold nocsif_sdcard_lock and have claimed
 * the card via nocsif_usb_gadget_claim_sd (i.e. not currently in File Share
 * mode). Returns NULL on success, or a short reason string on failure.
 * Destroys everything already on the card. */
const char *nocsif_usb_gadget_sd_format(void);

/* ---- live capture over CDC (M5-P5+) ---- *
 * A raw byte pipe to the host over the CDC serial port, used to stream a live
 * PCAP feed (frames captured in monitor mode) so a host tool (Wireshark, via the
 * bundled extcap) can read them in real time instead of pulling a .pcap file off
 * the card afterward. The ESP-IDF console is never routed onto CDC, so its TX
 * endpoint is free for arbitrary bytes. All three functions below are safe to
 * call from any task. */

/* True only when CDC is the enumerated class, the mode switch has settled
 * (state ON), and a host app actually has the port open (DTR asserted) — the
 * precondition for streaming. Poll it before, and while, feeding bytes. */
bool nocsif_usb_gadget_cdc_ready(void);

/* Queues up to `len` bytes into the CDC TX ring, returning how many bytes were
 * actually accepted (0..len). A short return means the host isn't draining fast
 * enough. Never blocks. Accepted bytes are already committed to the wire, so
 * callers should send whole records at a time to stay framed. Returns 0 if
 * cdc_ready() is false. */
size_t nocsif_usb_gadget_cdc_write(const uint8_t *buf, size_t len);

/* Pushes any queued CDC TX bytes toward the host, waiting up to timeout_ms for endpoint space to free up. */
void nocsif_usb_gadget_cdc_flush(uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
