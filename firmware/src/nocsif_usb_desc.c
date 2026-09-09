/*
 * NocSif — the actual USB descriptor byte arrays declared in nocsif_usb_desc.h.
 *
 * Built from TinyUSB's descriptor-builder macros (TUD_CONFIG_DESCRIPTOR, TUD_CDC_DESCRIPTOR,
 * TUD_MSC_DESCRIPTOR, TUD_HID_DESCRIPTOR), which each pack one interface's descriptor bytes
 * given its interface number, string index, endpoints, and sizes.
 */
#include "nocsif_usb_desc.h"
#include "tusb.h"

#include <string.h>   /* memcpy for the active-descriptor buffer */

/* --- Boot-keyboard HID report descriptor, no report ID. Must appear before the config
 * array below, which takes sizeof() of it. */
static const uint8_t s_hid_report[] = {
    TUD_HID_REPORT_DESC_KEYBOARD()
};

/* --- Interface numbers for the full composite: CDC takes two (control + data). */
enum {
    ITF_NUM_CDC = 0,
    ITF_NUM_CDC_DATA,
    ITF_NUM_MSC,
    ITF_NUM_HID,
    ITF_NUM_TOTAL,
};

/* --- Endpoint addresses for the full composite (bit 7 set = IN direction). Exactly 5 IN
 * endpoints including EP0, which is the hard ceiling on this chip — do not add more. */
#define EPNUM_CDC_NOTIF 0x81
#define EPNUM_CDC_OUT   0x02
#define EPNUM_CDC_IN    0x82
#define EPNUM_MSC_OUT   0x03
#define EPNUM_MSC_IN    0x83
#define EPNUM_HID_IN    0x84

/* --- String descriptor table indices, referenced from the device fields below and from
 * each interface's string-index slot. */
enum {
    STRID_LANGID = 0,
    STRID_MANUFACTURER,
    STRID_PRODUCT,
    STRID_SERIAL,
    STRID_CDC_INTERFACE,
    STRID_MSC_INTERFACE,
    STRID_HID_INTERFACE,
};

/* --- Full composite device descriptor. The MISC/COMMON/IAD class triple tells the host
 * to group interfaces via Interface Association Descriptors (needed because of the CDC pair). */
static const tusb_desc_device_t s_desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = NOCSIF_USB_VID,
    .idProduct          = NOCSIF_USB_PID,
    .bcdDevice          = 0x0100,
    .iManufacturer      = STRID_MANUFACTURER,
    .iProduct           = STRID_PRODUCT,
    .iSerialNumber      = STRID_SERIAL,
    .bNumConfigurations = 0x01,
};

/* --- Full composite configuration descriptor. Total length is computed at compile time
 * from each sub-descriptor's known length. */
#define NOCSIF_CFG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + \
                              TUD_MSC_DESC_LEN + TUD_HID_DESC_LEN)

static const uint8_t s_desc_fs_config[] = {
    /* Config header covering all 4 interfaces. */
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, NOCSIF_CFG_TOTAL_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    /* CDC control + data interfaces. */
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, STRID_CDC_INTERFACE, EPNUM_CDC_NOTIF, 8,
                       EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),
    /* Mass-storage interface. */
    TUD_MSC_DESCRIPTOR(ITF_NUM_MSC, STRID_MSC_INTERFACE, EPNUM_MSC_OUT, EPNUM_MSC_IN, 64),
    /* HID keyboard interface. */
    TUD_HID_DESCRIPTOR(ITF_NUM_HID, STRID_HID_INTERFACE, HID_ITF_PROTOCOL_KEYBOARD,
                       sizeof(s_hid_report), EPNUM_HID_IN, CFG_TUD_HID_EP_BUFSIZE, 5),
};

/* --- Fallback config with CDC + HID but no MSC interface, used when the microSD failed to
 * initialise. Advertising an MSC interface with nothing behind it makes the host's first
 * SCSI access crash the watch, so this config drops MSC entirely instead. */
#define NOCSIF_CFG_CDCHID_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_HID_DESC_LEN)
static const uint8_t s_desc_fs_config_cdchid[] = {
    TUD_CONFIG_DESCRIPTOR(1, 3, 0, NOCSIF_CFG_CDCHID_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, STRID_CDC_INTERFACE, EPNUM_CDC_NOTIF, 8,
                       EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),
    /* HID takes MSC's would-be interface slot since there's no MSC interface here. */
    TUD_HID_DESCRIPTOR(ITF_NUM_MSC, STRID_HID_INTERFACE, HID_ITF_PROTOCOL_KEYBOARD,
                       sizeof(s_hid_report), EPNUM_HID_IN, CFG_TUD_HID_EP_BUFSIZE, 5),
};

/* ======================================================================================== *
 * Per-mode descriptors: each USB mode enumerates a single class with its own product ID,      *
 * so the host doesn't serve a stale cached descriptor when the gadget switches modes.          *
 * Endpoint numbers can be reused between modes since only one config is ever active.           *
 * ======================================================================================== */
#define NOCSIF_PID_CDC ((uint16_t)(NOCSIF_USB_PID + 0))
#define NOCSIF_PID_HID ((uint16_t)(NOCSIF_USB_PID + 1))
#define NOCSIF_PID_MSC ((uint16_t)(NOCSIF_USB_PID + 2))

#define EPNUM_HID_SOLO_IN  0x81
#define EPNUM_MSC_SOLO_OUT 0x01
#define EPNUM_MSC_SOLO_IN  0x81

/* Console mode: CDC serial only. Still uses the IAD/MISC class triple since CDC needs it. */
static const tusb_desc_device_t s_dev_cdc = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = NOCSIF_USB_VID,
    .idProduct          = NOCSIF_PID_CDC,
    .bcdDevice          = 0x0100,
    .iManufacturer      = STRID_MANUFACTURER,
    .iProduct           = STRID_PRODUCT,
    .iSerialNumber      = STRID_SERIAL,
    .bNumConfigurations = 0x01,
};

/* HID mode: keyboard only. A single-interface HID device declares its class at the
 * interface level, so the device-level class triple stays 0/0/0. */
static const tusb_desc_device_t s_dev_hid = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = 0x00,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = NOCSIF_USB_VID,
    .idProduct          = NOCSIF_PID_HID,
    .bcdDevice          = 0x0100,
    .iManufacturer      = STRID_MANUFACTURER,
    .iProduct           = STRID_PRODUCT,
    .iSerialNumber      = STRID_SERIAL,
    .bNumConfigurations = 0x01,
};

/* Console mode config descriptor: the CDC control + data interfaces only. */
#define NOCSIF_CFG_CDC_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN)
static const uint8_t s_cfg_cdc[] = {
    TUD_CONFIG_DESCRIPTOR(1, 2, 0, NOCSIF_CFG_CDC_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, STRID_CDC_INTERFACE, EPNUM_CDC_NOTIF, 8,
                       EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),
};

/* HID mode config descriptor: a single boot-keyboard interface. */
#define NOCSIF_CFG_HID_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN)
static const uint8_t s_cfg_hid[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, NOCSIF_CFG_HID_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_HID_DESCRIPTOR(0, STRID_HID_INTERFACE, HID_ITF_PROTOCOL_KEYBOARD,
                       sizeof(s_hid_report), EPNUM_HID_SOLO_IN, CFG_TUD_HID_EP_BUFSIZE, 5),
};

/* File Share mode: mass storage only. Like HID, MSC declares its class at the interface
 * (via TUD_MSC_DESCRIPTOR), so the device-level triple here is 0/0/0. */
static const tusb_desc_device_t s_dev_msc = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = 0x00,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = NOCSIF_USB_VID,
    .idProduct          = NOCSIF_PID_MSC,
    .bcdDevice          = 0x0100,
    .iManufacturer      = STRID_MANUFACTURER,
    .iProduct           = STRID_PRODUCT,
    .iSerialNumber      = STRID_SERIAL,
    .bNumConfigurations = 0x01,
};

/* File Share mode config descriptor: a single mass-storage interface. */
#define NOCSIF_CFG_MSC_LEN (TUD_CONFIG_DESC_LEN + TUD_MSC_DESC_LEN)
static const uint8_t s_cfg_msc[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, NOCSIF_CFG_MSC_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_MSC_DESCRIPTOR(0, STRID_MSC_INTERFACE, EPNUM_MSC_SOLO_OUT, EPNUM_MSC_SOLO_IN, 64),
};

/* --- USB string table. Index 0 is the LANGID pair, not text. The serial string must be
 * non-empty or the control transfer stalls. */
static const char *s_strings[] = {
    (const char[]){0x09, 0x04},   /* 0: LANGID en-US */
    "NocSif",                     /* 1: manufacturer */
    "NocSif USB Gadget",          /* 2: product */
    "0001",                       /* 3: serial (non-NULL, required) */
    "NocSif CDC",                 /* 4: CDC interface */
    "NocSif MSC",                 /* 5: MSC interface */
    "NocSif HID Keyboard",        /* 6: HID interface */
};

const tusb_desc_device_t *nocsif_usb_desc_device(void)
{
    return &s_desc_device;
}

const uint8_t *nocsif_usb_desc_fs_config(void)
{
    return s_desc_fs_config;
}

const uint8_t *nocsif_usb_desc_fs_config_cdchid(void)
{
    return s_desc_fs_config_cdchid;
}

const char **nocsif_usb_desc_strings(void)
{
    return s_strings;
}

int nocsif_usb_desc_string_count(void)
{
    return (int)(sizeof(s_strings) / sizeof(s_strings[0]));
}

const uint8_t *nocsif_usb_hid_report_desc(uint16_t *len_out)
{
    if (len_out != NULL) {
        *len_out = (uint16_t)sizeof(s_hid_report);
    }
    return s_hid_report;
}

/* --- Per-mode descriptor accessors --- */
const tusb_desc_device_t *nocsif_usb_desc_device_cdc(void) { return &s_dev_cdc; }
const tusb_desc_device_t *nocsif_usb_desc_device_hid(void) { return &s_dev_hid; }
const tusb_desc_device_t *nocsif_usb_desc_device_msc(void) { return &s_dev_msc; }
const uint8_t            *nocsif_usb_desc_config_cdc(void) { return s_cfg_cdc; }
const uint8_t            *nocsif_usb_desc_config_hid(void) { return s_cfg_hid; }
const uint8_t            *nocsif_usb_desc_config_msc(void) { return s_cfg_msc; }

/* --- The currently-active descriptor buffers, overwritten in place on each mode switch. --- */
static tusb_desc_device_t s_active_dev;
static uint8_t            s_active_cfg[128];   /* sized for the largest single-mode config */

/* Compile-time guarantee that every per-mode config actually fits the buffer above; a new
 * mode whose config is too large will fail the build here rather than corrupt memory. */
_Static_assert(NOCSIF_CFG_CDC_LEN <= sizeof(s_active_cfg), "s_active_cfg too small for CDC config");
_Static_assert(NOCSIF_CFG_HID_LEN <= sizeof(s_active_cfg), "s_active_cfg too small for HID config");
_Static_assert(NOCSIF_CFG_MSC_LEN <= sizeof(s_active_cfg), "s_active_cfg too small for MSC config");

void nocsif_usb_desc_activate(const tusb_desc_device_t *dev, const uint8_t *cfg)
{
    s_active_dev = *dev;
    /* The config's total length is stored little-endian at bytes 2-3; read it so only the
     * real descriptor bytes get copied. Clamped defensively even though the static asserts
     * above should already guarantee every mode's config fits. */
    size_t len = (size_t)cfg[2] | ((size_t)cfg[3] << 8);
    if (len > sizeof(s_active_cfg)) {
        len = sizeof(s_active_cfg);
    }
    memcpy(s_active_cfg, cfg, len);
    s_active_cfg[2] = (uint8_t)(len & 0xFF);
    s_active_cfg[3] = (uint8_t)((len >> 8) & 0xFF);
}

const tusb_desc_device_t *nocsif_usb_desc_active_device(void) { return &s_active_dev; }
const uint8_t            *nocsif_usb_desc_active_config(void) { return s_active_cfg; }
