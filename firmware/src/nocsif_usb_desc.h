/*
 * NocSif — declares the hand-written USB descriptors for the CDC + MSC + HID composite
 * gadget, since esp_tinyusb's built-in default descriptor has no HID interface.
 *
 * The composite uses 4 interfaces (CDC, CDC-DATA, MSC, HID) and exactly 5 IN endpoints
 * (counting EP0), which is the ESP32-S3 full-speed core's hard limit — do not add another
 * IN endpoint, or enumeration silently fails. usb_gadget.c hands these descriptors to
 * esp_tinyusb at install time; it serves them from its own descriptor callbacks.
 */
#pragma once

#include "tinyusb.h"   /* brings in the descriptor struct types this header uses */

#ifdef __cplusplus
extern "C" {
#endif

/* --- Compile-time device identity. Ships as a generic keyboard VID/PID; build with
 * -DNOCSIF_USB_DEV_IDENTITY=1 to advertise the Espressif dev VID/PID instead, which makes
 * the CDC console easier to find during bring-up. The host identifies the device by
 * this VID/PID pair regardless of which USB class is actually doing the talking. */
#ifndef NOCSIF_USB_DEV_IDENTITY
#define NOCSIF_USB_DEV_IDENTITY 0   /* 0 = shipping generic-keyboard identity */
#endif
#if NOCSIF_USB_DEV_IDENTITY
#define NOCSIF_USB_VID 0x303A   /* Espressif — bring-up only */
#define NOCSIF_USB_PID 0x4004
#else
#define NOCSIF_USB_VID 0x1A2C   /* generic USB-keyboard vendor id */
#define NOCSIF_USB_PID 0x2124
#endif

/* Older always-on composite descriptors (full CDC+MSC+HID, and a CDC+HID fallback). No
 * longer used on the live path — kept for reference. Only the shared string table below is
 * still consumed by the mode-switching code. */
const tusb_desc_device_t *nocsif_usb_desc_device(void);          /* unused: CDC+MSC+HID     */
const uint8_t            *nocsif_usb_desc_fs_config(void);        /* unused: CDC+MSC+HID     */
const uint8_t            *nocsif_usb_desc_fs_config_cdchid(void); /* unused: CDC+HID, no MSC */
const char              **nocsif_usb_desc_strings(void);          /* still used: string table */
int                       nocsif_usb_desc_string_count(void);     /* still used: string count */

/* --- Per-mode descriptors. Each USB mode advertises a single class with its own product ID,
 * so the host cleanly re-enumerates when the mode server switches between them (rewriting the
 * active descriptor and toggling the bus rather than reinstalling the driver). Endpoint
 * addresses can be reused across modes since only one config is ever live at a time. */
const tusb_desc_device_t *nocsif_usb_desc_device_cdc(void);  /* CDC serial console identity */
const tusb_desc_device_t *nocsif_usb_desc_device_hid(void);  /* HID keyboard identity */
const tusb_desc_device_t *nocsif_usb_desc_device_msc(void);  /* mass-storage (File Share) identity */
const uint8_t            *nocsif_usb_desc_config_cdc(void);   /* CDC-only config descriptor */
const uint8_t            *nocsif_usb_desc_config_hid(void);   /* HID-only config descriptor */
const uint8_t            *nocsif_usb_desc_config_msc(void);   /* MSC-only config descriptor */

/* --- The single active descriptor esp_tinyusb was installed against. Since esp_tinyusb keeps
 * a pointer to (not a copy of) the descriptors it's given, the mode server installs once
 * pointing at these buffers, then switches modes by copying a per-mode descriptor in here and
 * cycling the USB connection so the host re-reads it. */
void nocsif_usb_desc_activate(const tusb_desc_device_t *dev, const uint8_t *cfg);
const tusb_desc_device_t *nocsif_usb_desc_active_device(void);  /* currently-active device descriptor */
const uint8_t            *nocsif_usb_desc_active_config(void);   /* currently-active config descriptor */

/* The keyboard HID report descriptor (boot-keyboard layout, no report ID). Optionally
 * returns its byte length via len_out. */
const uint8_t *nocsif_usb_hid_report_desc(uint16_t *len_out);

#ifdef __cplusplus
}
#endif
