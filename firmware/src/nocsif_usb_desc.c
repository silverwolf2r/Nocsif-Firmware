/*
 * NocSif — hand-written CDC + MSC + HID composite descriptor set (M4-P4). See nocsif_usb_desc.h.
 *
 * Every macro arg order + length constant below was read from the on-disk TinyUSB headers
 * (device/usbd.h) and the stock hid_composite / cdc_msc examples, then re-verified by a
 * header-reading workflow:
 *   TUD_CONFIG_DESCRIPTOR(config_num, itf_count, str_idx, total_len, attribute, power_ma)
 *   TUD_CDC_DESCRIPTOR   (itfnum, str_idx, ep_notif, ep_notif_size, ep_out, ep_in, ep_size)
 *   TUD_MSC_DESCRIPTOR   (itfnum, str_idx, ep_out, ep_in, ep_size)
 *   TUD_HID_DESCRIPTOR   (itfnum, str_idx, boot_protocol, report_desc_len, ep_in, ep_size, interval)
 * TUD_CONFIG_DESCRIPTOR halves power_ma to bMaxPower and auto-sets the bus-powered bit; the
 * endpoint macros already carry bit7 for IN, so the EP address #defines are passed as-is.
 */
#include "nocsif_usb_desc.h"
#include "tusb.h"

#include <string.h>   /* memcpy for the runtime-active descriptor (P4.5) */

/* --- HID report descriptor: boot keyboard, NO report ID (empty macro args). Every
 * tud_hid_keyboard_report()/tud_hid_ready() call therefore uses report_id = 0 — a non-zero
 * id would prepend a byte and corrupt each keystroke. Must be defined before the config
 * array (which takes sizeof of it). */
static const uint8_t s_hid_report[] = {
    TUD_HID_REPORT_DESC_KEYBOARD()
};

/* --- Interface numbers. CDC occupies TWO (control + data = itfnum+1), so it must start at
 * 0; MSC and HID follow. bNumInterfaces = ITF_NUM_TOTAL = 4. */
enum {
    ITF_NUM_CDC = 0,
    ITF_NUM_CDC_DATA,   /* = 1, referenced implicitly by TUD_CDC_DESCRIPTOR */
    ITF_NUM_MSC,        /* = 2 */
    ITF_NUM_HID,        /* = 3 */
    ITF_NUM_TOTAL,      /* = 4 */
};

/* --- Endpoint addresses (bit7 = IN). Unique per direction. HID IN is 0x84 — NOT the
 * hid_composite example's 0x81, which would collide with the CDC notification endpoint.
 * IN set = EP0 + 0x81 + 0x82 + 0x83 + 0x84 = 5 (the S3 ceiling). OUT set = EP0 + 0x02 + 0x03. */
#define EPNUM_CDC_NOTIF 0x81
#define EPNUM_CDC_OUT   0x02
#define EPNUM_CDC_IN    0x82
#define EPNUM_MSC_OUT   0x03
#define EPNUM_MSC_IN    0x83
#define EPNUM_HID_IN    0x84

/* --- String descriptor indices. Referenced by iManufacturer/iProduct/iSerialNumber and by
 * each interface's string index. */
enum {
    STRID_LANGID = 0,
    STRID_MANUFACTURER,
    STRID_PRODUCT,
    STRID_SERIAL,
    STRID_CDC_INTERFACE,
    STRID_MSC_INTERFACE,
    STRID_HID_INTERFACE,
};

/* --- Device descriptor. IAD composite => device class MUST be MISC/COMMON/IAD (0xEF/0x02/
 * 0x01) so the host applies the interface-association grouping. */
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

/* --- Configuration descriptor. Total length is expressed symbolically so the compiler
 * computes it from the verified per-descriptor lengths (9 + 66 + 23 + 25 = 123). */
#define NOCSIF_CFG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + \
                              TUD_MSC_DESC_LEN + TUD_HID_DESC_LEN)

static const uint8_t s_desc_fs_config[] = {
    /* config #1, 4 interfaces, no config string, total len, remote-wakeup, 100 mA */
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, NOCSIF_CFG_TOTAL_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    /* CDC: control itf 0 (+ data itf 1), notif EP 0x81 (8 B), data OUT/IN 0x02/0x82 (64 B) */
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, STRID_CDC_INTERFACE, EPNUM_CDC_NOTIF, 8,
                       EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),
    /* MSC: itf 2, bulk OUT/IN 0x03/0x83 (64 B) */
    TUD_MSC_DESCRIPTOR(ITF_NUM_MSC, STRID_MSC_INTERFACE, EPNUM_MSC_OUT, EPNUM_MSC_IN, 64),
    /* HID: itf 3, boot keyboard protocol, IN EP 0x84, 5 ms poll interval */
    TUD_HID_DESCRIPTOR(ITF_NUM_HID, STRID_HID_INTERFACE, HID_ITF_PROTOCOL_KEYBOARD,
                       sizeof(s_hid_report), EPNUM_HID_IN, CFG_TUD_HID_EP_BUFSIZE, 5),
};

/* --- Fallback CDC+HID configuration (NO MSC) --- used when the microSD did not initialise,
 * so the composite never advertises an MSC interface with no storage behind it. Declaring
 * MSC without a backing store makes the host's first SCSI access hit the MSC class driver
 * with no storage and reboots the watch — so if there's no card we simply drop the MSC
 * interface. Interfaces: CDC(0)+CDC-DATA(1)+HID(2), bNumInterfaces=3; IN endpoints
 * EP0+0x81+0x82+0x84 (0x83 unused). wTotalLength = 9 + 66 + 25 = 100. */
#define NOCSIF_CFG_CDCHID_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_HID_DESC_LEN)
static const uint8_t s_desc_fs_config_cdchid[] = {
    TUD_CONFIG_DESCRIPTOR(1, 3, 0, NOCSIF_CFG_CDCHID_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, STRID_CDC_INTERFACE, EPNUM_CDC_NOTIF, 8,
                       EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),
    /* HID takes interface 2 here (MSC's slot) since there is no MSC interface. */
    TUD_HID_DESCRIPTOR(ITF_NUM_MSC, STRID_HID_INTERFACE, HID_ITF_PROTOCOL_KEYBOARD,
                       sizeof(s_hid_report), EPNUM_HID_IN, CFG_TUD_HID_EP_BUFSIZE, 5),
};

/* ======================================================================================== *
 * P4.5 — per-mode descriptors: ONE class per mode, a DISTINCT product id per mode.           *
 * Only one config is ever ACTIVE at a time — the mode server installs the driver once and     *
 * rewrites the single mutable active descriptor (nocsif_usb_desc_activate) on each switch,     *
 * never uninstalling — so endpoint addresses may be reused across modes without collision. A   *
 * per-mode PID keeps a host from serving a cached descriptor from the previous mode.           *
 * ======================================================================================== */
#define NOCSIF_PID_CDC ((uint16_t)(NOCSIF_USB_PID + 0))
#define NOCSIF_PID_HID ((uint16_t)(NOCSIF_USB_PID + 1))
#define NOCSIF_PID_MSC ((uint16_t)(NOCSIF_USB_PID + 2))   /* File Share (mass-storage) — P4.5.2 */

#define EPNUM_HID_SOLO_IN  0x81  /* HID-only mode: single IN endpoint (no CDC to collide with) */
#define EPNUM_MSC_SOLO_OUT 0x01  /* MSC-only mode: bulk OUT/IN (no other class to collide with) */
#define EPNUM_MSC_SOLO_IN  0x81

/* Console (CDC serial only). CDC carries its own Interface Association Descriptor, so the
 * device class is the IAD/MISC triple (as the composite uses). */
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

/* HID keyboard only. A single-interface HID device declares its class at the interface, so
 * the device-level class triple is 0/0/0 (a plain keyboard, not an IAD composite). */
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

/* CDC-only config: 2 interfaces (control itf 0 + data itf 1). EPs: notif 0x81, data OUT 0x02
 * / IN 0x82. wTotalLength = 9 + 66 = 75. */
#define NOCSIF_CFG_CDC_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN)
static const uint8_t s_cfg_cdc[] = {
    TUD_CONFIG_DESCRIPTOR(1, 2, 0, NOCSIF_CFG_CDC_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, STRID_CDC_INTERFACE, EPNUM_CDC_NOTIF, 8,
                       EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),
};

/* HID-only config: 1 interface (itf 0), boot keyboard, IN endpoint 0x81 (no CDC to collide
 * with). wTotalLength = 9 + 25 = 34. */
#define NOCSIF_CFG_HID_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN)
static const uint8_t s_cfg_hid[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, NOCSIF_CFG_HID_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_HID_DESCRIPTOR(0, STRID_HID_INTERFACE, HID_ITF_PROTOCOL_KEYBOARD,
                       sizeof(s_hid_report), EPNUM_HID_SOLO_IN, CFG_TUD_HID_EP_BUFSIZE, 5),
};

/* File Share (mass-storage only). MSC is a single-interface class, so the device-level class
 * triple is 0/0/0 (the class lives on the interface — 0x08/0x06/0x50, set by TUD_MSC_DESCRIPTOR). */
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

/* MSC-only config: 1 interface (itf 0), bulk OUT 0x01 / IN 0x81, 64 B. wTotalLength = 9 + 23 = 32. */
#define NOCSIF_CFG_MSC_LEN (TUD_CONFIG_DESC_LEN + TUD_MSC_DESC_LEN)
static const uint8_t s_cfg_msc[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, NOCSIF_CFG_MSC_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_MSC_DESCRIPTOR(0, STRID_MSC_INTERFACE, EPNUM_MSC_SOLO_OUT, EPNUM_MSC_SOLO_IN, 64),
};

/* --- String descriptors. Index 0 is the LANGID pair (en-US 0x0409), NOT text. The serial
 * string MUST be non-NULL or the control transfer stalls (esp_tinyusb has no board-serial
 * fallback in this path). string_count is a literal element count (cap is 8). */
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
    return (int)(sizeof(s_strings) / sizeof(s_strings[0]));   /* = 7 */
}

const uint8_t *nocsif_usb_hid_report_desc(uint16_t *len_out)
{
    if (len_out != NULL) {
        *len_out = (uint16_t)sizeof(s_hid_report);
    }
    return s_hid_report;
}

/* --- P4.5 per-mode descriptor accessors --- */
const tusb_desc_device_t *nocsif_usb_desc_device_cdc(void) { return &s_dev_cdc; }
const tusb_desc_device_t *nocsif_usb_desc_device_hid(void) { return &s_dev_hid; }
const tusb_desc_device_t *nocsif_usb_desc_device_msc(void) { return &s_dev_msc; }
const uint8_t            *nocsif_usb_desc_config_cdc(void) { return s_cfg_cdc; }
const uint8_t            *nocsif_usb_desc_config_hid(void) { return s_cfg_hid; }
const uint8_t            *nocsif_usb_desc_config_msc(void) { return s_cfg_msc; }

/* --- Runtime-active descriptor (mutated in place on a mode switch; see the header). --- */
static tusb_desc_device_t s_active_dev;
static uint8_t            s_active_cfg[128];   /* >= the largest activatable single-mode config */

/* Every config that can be activated must fit the active buffer, else the host would read a config
 * whose advertised wTotalLength runs past it. Bound that at COMPILE time — a new mode whose config
 * exceeds the buffer fails the build here (add its *_LEN when it lands). */
_Static_assert(NOCSIF_CFG_CDC_LEN <= sizeof(s_active_cfg), "s_active_cfg too small for CDC config");
_Static_assert(NOCSIF_CFG_HID_LEN <= sizeof(s_active_cfg), "s_active_cfg too small for HID config");
_Static_assert(NOCSIF_CFG_MSC_LEN <= sizeof(s_active_cfg), "s_active_cfg too small for MSC config");

void nocsif_usb_desc_activate(const tusb_desc_device_t *dev, const uint8_t *cfg)
{
    s_active_dev = *dev;
    /* wTotalLength is bytes 2..3 of the config descriptor (little-endian) — copy exactly that many
     * bytes so the host reads a well-formed descriptor. The asserts above guarantee every
     * activatable config fits; the clamp is belt-and-suspenders, and rewriting the copied
     * wTotalLength keeps the advertised length consistent with what the buffer holds so a clamp
     * could never advertise bytes past the buffer. */
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
