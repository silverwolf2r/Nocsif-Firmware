/*
 * NocSif — hand-written USB descriptor set for the M4-P4 CDC + MSC + HID composite.
 *
 * P2/P3 used esp_tinyusb's built-in default descriptor (CDC, then CDC+MSC). That default
 * has NO HID interface, so P4 must supply its own composite configuration descriptor:
 * a keyboard HID interface has no default and esp_tinyusb returns ESP_ERR_INVALID_ARG at
 * install if HID is compiled in without an app-provided config descriptor.
 *
 * All values here were verified against the real esp_tinyusb 2.2.1 + TinyUSB headers on
 * disk (device/usbd.h macro arg orders + *_DESC_LEN, class/hid/hid.h, dwc2_esp32.h):
 *   - The four interfaces are CDC(0) + CDC-DATA(1) + MSC(2) + HID(3): bNumInterfaces = 4.
 *   - wTotalLength = 9 + 66 + 23 + 25 = 123 (CONFIG + CDC + MSC + single-IN HID).
 *   - IN endpoints: EP0-IN + 0x81(CDC notif) + 0x82(CDC in) + 0x83(MSC in) + 0x84(HID in)
 *     = exactly 5, the ESP32-S3 full-speed core ceiling (dwc2 ep_in_count = 5, counting
 *     EP0). There is ZERO headroom: a further IN endpoint trips a runtime assert in
 *     dcd_edpt_open() and enumeration fails SILENTLY (no build error). Do not add one.
 *   - IAD composite device class triple: 0xEF / 0x02 / 0x01 (MISC / COMMON / IAD).
 *
 * These descriptors are handed to esp_tinyusb via tinyusb_config_t.descriptor.* (device,
 * full_speed_config, string, string_count) in usb_gadget.c. esp_tinyusb owns the
 * tud_descriptor_*_cb glue and reads them from there; the app only additionally supplies
 * the three HID-class callbacks (see hid_kbd.c).
 */
#pragma once

#include "tinyusb.h"   /* pulls tusb.h: tusb_desc_device_t + config/descriptor types */

#ifdef __cplusplus
extern "C" {
#endif

/* --- Device identity (compile-time). Default: a generic USB HID keyboard VID/PID so the
 * composite is identified as an ordinary keyboard to the host. Set NOCSIF_USB_DEV_IDENTITY
 * to 1 for the Espressif development VID/PID during bring-up. HID binds to the host by
 * interface class 0x03 regardless of VID/PID, so this only affects how the host identifies
 * and driver-caches the device. NOTE: the P4 PID MUST differ from the P3 CDC+MSC PID or the
 * host may reuse a stale cached driver (a config quirk, not a code bug). The exact generic
 * pair below is illustrative — confirm on the target host. */
#ifndef NOCSIF_USB_DEV_IDENTITY
#define NOCSIF_USB_DEV_IDENTITY 0   /* 0 = generic-keyboard identity (shipping default);
                                     * build with -DNOCSIF_USB_DEV_IDENTITY=1 for the Espressif
                                     * 0x303A dev identity (makes the CDC console findable by the
                                     * scratchpad read_cdc.py during bring-up). */
#endif
#if NOCSIF_USB_DEV_IDENTITY
#define NOCSIF_USB_VID 0x303A   /* Espressif Systems — dev/bring-up only */
#define NOCSIF_USB_PID 0x4004   /* CDC+MSC+HID (dev); differs from the P3 default PID */
#else
#define NOCSIF_USB_VID 0x1A2C   /* generic USB-keyboard vendor id (default) */
#define NOCSIF_USB_PID 0x2124   /* generic keyboard product id; != 0x303A / P3 PID */
#endif

/* Pre-P4.5 composite descriptor accessors (the always-on CDC+MSC+HID / CDC+HID set). SUPERSEDED
 * by the per-mode single-class descriptors below and no longer on the live path — retained for
 * reference pending removal with the P4.5.2 MSC work. Of this group only nocsif_usb_desc_strings()
 * / _string_count() are still consumed by the mode server (the string table is shared across
 * modes). The returned pointers are static and live for the device lifetime (esp_tinyusb stores,
 * does not copy, the device/config pointers). */
const tusb_desc_device_t *nocsif_usb_desc_device(void);          /* CDC + MSC + HID (unused)     */
const uint8_t            *nocsif_usb_desc_fs_config(void);        /* CDC + MSC + HID (unused)     */
const uint8_t            *nocsif_usb_desc_fs_config_cdchid(void); /* CDC + HID, no microSD (unused) */
const char              **nocsif_usb_desc_strings(void);          /* shared string table (live)   */
int                       nocsif_usb_desc_string_count(void);     /* shared string count (live)   */

/* --- Per-mode descriptors (UI-shell P4.5 — USB mode selection). Each USB mode enumerates a
 * SINGLE class so the host cleanly re-detects on a switch (the mode server rewrites the mutable
 * active descriptor + tud_disconnect()/tud_connect() — it installs the driver ONCE and never
 * uninstalls; see nocsif_usb_desc_activate below), and each mode carries a DISTINCT product id
 * (NOCSIF_USB_PID + a per-mode offset) so a host does not serve a cached descriptor from the
 * previous mode across the re-enumeration (the PID-cache quirk noted above). Endpoint addresses
 * are reused across modes safely because only ONE config is ever active at a time. The composite
 * getters above are the superseded pre-P4.5 set, retained for reference; the mode server uses
 * these per-mode getters. */
const tusb_desc_device_t *nocsif_usb_desc_device_cdc(void);  /* Console: CDC identity (IAD class) */
const tusb_desc_device_t *nocsif_usb_desc_device_hid(void);  /* HID keyboard identity (per-interface class) */
const tusb_desc_device_t *nocsif_usb_desc_device_msc(void);  /* File Share: mass-storage identity */
const uint8_t            *nocsif_usb_desc_config_cdc(void);   /* CDC serial only  */
const uint8_t            *nocsif_usb_desc_config_hid(void);   /* HID keyboard only */
const uint8_t            *nocsif_usb_desc_config_msc(void);   /* mass-storage (MSC) only */

/* --- Runtime-active descriptor (P4.5). esp_tinyusb STORES (does not copy) the device +
 * full_speed_config pointers it is handed at install and returns them from its descriptor
 * callbacks. So the mode server installs ONCE pointing esp_tinyusb at these MUTABLE buffers,
 * then switches gadget modes by copying a per-mode descriptor into them
 * (nocsif_usb_desc_activate) and disconnecting/reconnecting the device — the host re-reads the
 * new descriptor with NO PHY re-cycle (the uninstall+reinstall path crashed on-device). */
void nocsif_usb_desc_activate(const tusb_desc_device_t *dev, const uint8_t *cfg);
const tusb_desc_device_t *nocsif_usb_desc_active_device(void);  /* the mutable device descriptor */
const uint8_t            *nocsif_usb_desc_active_config(void);   /* the mutable config descriptor */

/* The keyboard HID report descriptor (8-byte boot-keyboard report, no report ID).
 * hid_kbd.c's tud_hid_descriptor_report_cb returns this. If len_out != NULL it receives
 * the byte length. */
const uint8_t *nocsif_usb_hid_report_desc(uint16_t *len_out);

#ifdef __cplusplus
}
#endif
