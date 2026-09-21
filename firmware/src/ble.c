/*
 * NocSif — Bluetooth LE (on-SoC 2.4 GHz, NimBLE host) worker (M7-P1). See ble.h.
 *
 * Architecture (mirrors wifi.c / nfc.cpp):
 *   - A dedicated worker task owns every stack action. LVGL callbacks only post a command onto the
 *     worker's queue instead of touching the stack themselves, so LVGL stays single-threaded and
 *     can never stall on the radio.
 *   - NimBLE runs its own host task (nimble_port_freertos_init). Its GAP discovery callback fires
 *     there whenever an advertisement report arrives; it does O(1) work — parse the ~31-byte adv
 *     payload, then upsert one row into a BDA-keyed table under a short spinlock. The UI reads a
 *     lock-free snapshot. Nothing here calls LVGL, and nothing here blocks.
 *   - Bring-up (nimble_port_init + the host task) is LAZY: it happens on the first scan request and
 *     is gated on nocsif_reliability_safe_mode(), so boot stays fast and the coexistence risk (BLE
 *     controller RAM competing with the display-flush DMA path) only shows up on first use.
 *
 * Coexistence: WiFi + BLE share the single 2.4 GHz radio via esp_coex software coexistence
 * (CONFIG_ESP_COEX_SW_COEXIST_ENABLE, sdkconfig.defaults). Enabling BT claws back internal DMA RAM,
 * so bring-up logs the internal-DMA free/largest-block numbers for the on-device headroom check.
 *
 * Authorized testing only: this receives the advertisements every nearby device already
 * broadcasts — the same discovery a phone performs. Scope every use to devices you're authorized
 * to test.
 */
#include "ble.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>       /* strncasecmp — case-insensitive BLE-serial-module name match       */
#include <ctype.h>         /* isxdigit — parsing the persisted alert-omit address list          */
#include <stdlib.h>        /* atof — parse AMS volume ("0.0".."1.0")                            */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_random.h"    /* esp_random / esp_fill_random — randomized Continuity fields (resilience test) */

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nimble/ble.h"            /* BLE_ERR_REM_USER_CONN_TERM — GATT disconnect reason code   */
#include "nimble/hci_common.h"      /* BLE_HCI_ADV_RPT_EVTYPE_* — connectable PDU classification */
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"       /* BLE_HS_ADV_F_* / _TX_PWR_LVL_AUTO — advertise field flags   */
#include "host/ble_gap.h"          /* ble_gap_connect / _terminate / _adv_start                   */
#include "host/ble_gatt.h"         /* ble_gattc_* (client) + ble_gatts_* / ble_gatt_svc_def (server) */
#include "host/ble_att.h"          /* BLE_ATT_ERR_* / BLE_ATT_F_READ — GATT server access, HID    */
#include "host/ble_uuid.h"         /* ble_uuid_to_str / _u16 — UUID formatting                    */
#include "host/ble_hs_mbuf.h"      /* ble_hs_mbuf_to_flat / _from_flat — read values / notify     */
#include "host/ble_sm.h"           /* BLE_SM_PAIR_KEY_DIST_* — bonding key distribution (ANCS)     */
#include "host/ble_store.h"        /* ble_store_clear / _util_delete_peer — unpair / re-pair       */
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"    /* ble_svc_gap_init / device_name_set — HID GATT server  */
#include "services/gatt/ble_svc_gatt.h"  /* ble_svc_gatt_init — Service Changed, for HID GATT     */
#include "os/os_mbuf.h"            /* os_mbuf_append / OS_MBUF_PKTLEN — HID access callbacks      */
#include "store/config/ble_store_config.h"  /* ble_store_config_init — NVS-backed bond store (ANCS) */

#include "coex.h"           /* NOCSIF_DMA_CAPS + nocsif_int_dma_largest/_free + per-radio gates   */
#include "reliability.h"    /* nocsif_reliability_safe_mode */
#include "settings.h"       /* nocsif_settings_* — persists the advertise config (M7-P3)          */
#include "wifi.h"           /* WiFi state getters (the controller is resident; BLE never yields WiFi) */
#include "sdcard.h"         /* nocsif_sdcard_lock/unlock — /sd access for the advert PCAP (P4·3)   */
#include "usb_gadget.h"     /* nocsif_usb_gadget_claim_sd — keeps /sd away from USB-MSC while writing */
#include "power.h"          /* nocsif_power_batt_pct — read by the HID Battery Service             */

#include <sys/stat.h>       /* mkdir + stat — PCAP output dir / next-free filename (P4·3)          */

static const char *TAG = "ble";

/* Declared by the NimBLE port's config store (libbt.a) but not exposed in its public header. */
void ble_store_config_init(void);

/* ---- tunables --------------------------------------------------------------------- */
#define BLE_DEV_MAX     48      /* discovered-device table size (first-seen order; stalest evicted) */
#define BLE_CMD_QLEN    6       /* worker command queue depth                                  */
#define BLE_NAME_MAX    32      /* local-name field, including NUL                              */

/* Middot "·" (U+00B7) and ellipsis "…" (U+2026) as UTF-8 literals — ble.c has no UI headers to pull them from. */
#define BLE_DOT "\xC2\xB7"
#define BLE_ELL "\xE2\x80\xA6"

/* ---- worker commands (UI task -> worker) ------------------------------------------ */
typedef enum {
    CMD_BLE_SCAN_ON = 0,
    CMD_BLE_SCAN_OFF,
    CMD_BLE_RELEASE,       /* tear NimBLE down + restore WiFi (on leaving a BLE screen)      */
    CMD_GATT_CONNECT,      /* connect to a scan-table device + explore its GATT server (P2) */
    CMD_GATT_DISCONNECT,   /* terminate the GATT link, back to device-pick                  */
    CMD_GATT_READ,         /* read one characteristic value                                 */
    CMD_ADV_START,         /* start broadcasting our advertisement / beacon (P3)            */
    CMD_ADV_STOP,          /* stop advertising                                              */
    CMD_RESTEST_START,     /* start the Advertisement Resilience Test (authorized bench)    */
    CMD_RESTEST_STOP,      /* stop the resilience test + re-arm the phone advert            */
    CMD_BLE_PCAP_ON,       /* record received adverts to a PCAP file on /sd (P4·3)          */
    CMD_BLE_PCAP_OFF,      /* stop recording + close the file                               */
    CMD_ANCS_START,        /* enter Phone-Notifications mode: advertise + bond + mirror     */
    CMD_ANCS_FORGET,       /* erase the phone bond (unpair)                                */
    CMD_AMS_CMD,           /* send an AMS Remote Command (arg = command id) — media remote  */
    CMD_AMS_VOL_SET,       /* step the phone volume toward a target percent (arg = 0..100)  */
    CMD_HID_START,         /* enter BLE keyboard mode: advertise as a HID keyboard (M7)     */
    CMD_BLE_RESERVE,       /* boot: claim the controller block UNCONDITIONALLY (resident even if BT off) */
    CMD_PHONE_CONNECT,     /* Connect Phone: ensure master on + arm + advertise (open re-arm) */
    CMD_PHONE_DISCONNECT,  /* Connect Phone: drop the current link (master re-advertises)     */
    CMD_PHONE_FORGET,      /* Connect Phone: delete one saved phone's bond + drop its link    */
    CMD_PHONE_META,        /* record a (re)connection into the saved-phone metadata (NVS)     */
    CMD_BT_ENABLE,         /* Bluetooth master on/off (arg = 0/1): bring up / release          */
    CMD_NOTIF_SET,         /* phone-notifications gate on/off (arg = 0/1): live CCCD write     */
    CMD_HIDBOND_META,      /* record a HID host (keyboard) bond into its metadata list (NVS)   */
    CMD_HIDBOND_FORGET,    /* delete one saved HID host's bond + drop its link if it's live    */
} ble_cmd_type_t;

typedef struct {
    ble_cmd_type_t type;
    int            arg;    /* CONNECT: scan-table device index. READ: flattened item index.   */
} ble_cmd_t;

static void post(ble_cmd_type_t type);              /* fwd: host-task callbacks post worker commands */
static void post_arg(ble_cmd_type_t type, int arg);

/* ---- discovered-device table (BDA-keyed; spinlock-guarded) ------------------------- *
 * Written by the NimBLE host task's GAP callback, read (as a copied-out snapshot) by the LVGL
 * task. The short spinlock only guards the table itself, so a reader is never blocked for long.
 * First-seen ordering keeps rows stable; once the table fills, the stalest entry (oldest last_us)
 * is evicted to make room. */
typedef struct {
    bool     used;
    uint8_t  addr[6];
    uint8_t  addr_type;
    char     name[BLE_NAME_MAX];
    int8_t   rssi;
    int8_t   tx_pwr;
    bool     tx_pwr_ok;
    uint16_t appearance;
    bool     appearance_ok;
    uint16_t company;
    bool     company_ok;
    uint16_t uuid16;
    uint8_t  n_uuid;
    bool     connectable;
    uint8_t  tracker;      /* nocsif_ble_tracker_t if recognized (sticky; 0 = none) */
    uint16_t frames;
    int64_t  last_us;
    int64_t  first_us;     /* when this entry was created — anti-stalk "present this long" heuristic */
    bool     follow_alerted; /* a follow alert already fired for this address (one per device)      */
} ble_dev_t;

/* Raw AD payload store, kept in PSRAM (parallel to s_dev, same index) instead of inline in the internal
 * table — the raw bytes are ~64 B/device and the device-detail decoder is the only reader, so keeping
 * them off the scarce internal-DMA pool preserves the headroom the radios + IMU FIFO drain need (an
 * over-tight internal pool starves the IMU worker → task-wdt). Written/read under s_dev_mux (PSRAM is
 * cache-backed; accessing it with interrupts disabled is fine — only flash ops disable the cache). */
typedef struct {
    uint8_t adv_data[NOCSIF_BLE_ADV_MAX];
    uint8_t adv_len;
    uint8_t rsp_data[NOCSIF_BLE_ADV_MAX];
    uint8_t rsp_len;
} ble_dev_raw_t;
static ble_dev_raw_t *s_dev_raw;   /* [BLE_DEV_MAX] in PSRAM; NULL until nocsif_ble_init allocates it */

static ble_dev_t          s_dev[BLE_DEV_MAX];
static volatile int       s_dev_cnt;
static volatile uint32_t  s_dev_gen;
static portMUX_TYPE       s_dev_mux = portMUX_INITIALIZER_UNLOCKED;

/* ---- omit list (persisted; addresses dropped at ingestion) ------------------------- *
 * Loaded ONCE from NVS in nocsif_ble_init (before any scan, so the host-task reader can't race the
 * load). Read lock-free-ish by omit_contains on the host task under s_omit_mux; mutated + persisted
 * only from the LVGL task (the sole writer), so omit_save reads the array without the lock. */
static nocsif_ble_omit_t  s_omit[NOCSIF_BLE_OMIT_MAX];
static volatile int       s_omit_cnt;
static bool               s_omit_loaded;
static portMUX_TYPE       s_omit_mux = portMUX_INITIALIZER_UNLOCKED;
static void               omit_load(void);   /* fwd: called once in nocsif_ble_init */

/* ---- module state ----------------------------------------------------------------- */
static TaskHandle_t   s_task;
static QueueHandle_t  s_q;
static bool           s_host_up;      /* nimble_port_init has run and the host task is spawned    */
static volatile bool  s_synced;       /* host<->controller sync completed (sync_cb fired)         */
static volatile bool  s_available;    /* mirrors s_synced for the public C getter                 */
static volatile bool  s_scan_active;  /* discovery is running                                     */
static volatile bool  s_starting;     /* bring-up in progress (releasing WiFi + NimBLE init)      */
static bool           s_want_scan;    /* intent: start scanning once the stack is ready           */

/* ---- GATT explore tables (M7-P2; guarded by s_gatt_mux) ---------------------------- *
 * Once connected to one device, the host task's GATT callbacks append its services and
 * characteristics here (append-only during discovery, zeroed on disconnect), plus a small read
 * cache per characteristic. The UI reads a flattened snapshot lock-free (nocsif_ble_gatt_item_get). */
#define BLE_SVC_MAX  14        /* services per device                          */
#define BLE_CHR_MAX  48        /* characteristics total (across all services)  */
#define BLE_VAL_MAX  22        /* cached read bytes per characteristic          */
#define BLE_TGT_MAX  40        /* connected-peer label (name / address)         */

typedef struct {
    uint16_t       start, end;
    ble_uuid_any_t uuid;
} g_svc_t;

typedef struct {
    uint8_t        svc;          /* owning service index (into s_svc)   */
    uint16_t       val_handle;
    uint8_t        props;        /* BLE_GATT_CHR_PROP_* bitfield         */
    ble_uuid_any_t uuid;
    bool           has_val;
    uint8_t        vlen;
    uint8_t        val[BLE_VAL_MAX];
} g_chr_t;

static g_svc_t            s_svc[BLE_SVC_MAX];
static g_chr_t            s_chr[BLE_CHR_MAX];
static volatile int       s_svc_cnt;
static volatile int       s_chr_cnt;
static volatile uint32_t  s_gatt_gen;
static portMUX_TYPE       s_gatt_mux = portMUX_INITIALIZER_UNLOCKED;

static volatile nocsif_ble_gatt_state_t s_gatt_state;    /* IDLE / CONNECTING / … / READY   */
static uint16_t           s_conn_handle = BLE_HS_CONN_HANDLE_NONE;  /* valid while linked     */
static int                s_disc_svc_i;    /* the service whose chars are currently being enumerated */
static volatile bool      s_gatt_fail;     /* the last connect attempt failed (header hint)         */
static char               s_target[BLE_TGT_MAX];   /* connected-peer label                    */

/* ---- advertise / beacon (M7-P3, broadcaster) --------------------------------------- *
 * A non-connectable advertisement we transmit: either a plain named advert or an Apple iBeacon.
 * Config is persisted to NVS and applied on the next start. The name buffer is written from the
 * LVGL task before a start is requested (sequential user actions) and read by the worker/host at
 * start time — a benign, non-racy handoff. */
static nocsif_ble_adv_mode_t s_adv_mode;                 /* CUSTOM / IBEACON                 */
static char               s_adv_name[BLE_NAME_MAX] = "NocSif";
static volatile bool      s_adv_active;                  /* advertising is running            */
static volatile bool      s_adv_starting;                /* bring-up window (releasing WiFi)  */
static bool               s_want_adv;                    /* intent: advertise once synced    */
static bool               s_adv_loaded;                  /* NVS config has been pulled in once */

/* Advertisement Resilience Test (authorized bench, broadcaster): emit device pop-up adverts to test an owned/authorized target; cadence floored, auto-stops at a bounded duration — not for bystander devices. */
static esp_timer_handle_t s_rt_timer;
static volatile bool      s_rt_active;
static bool               s_rt_connect;                  /* "Connect to watch": pop-ups become connectable */
static nocsif_ble_rt_intensity_t s_rt_intensity;
static int                s_rt_dur_s = 60;               /* auto-stop duration (30 / 60 / 120)   */
static volatile uint32_t  s_rt_emitted;                  /* advert reconfigures this run          */
static int64_t            s_rt_deadline_us;              /* auto-stop time                        */
static int                s_rt_variant;                  /* rolling variant index                 */
static const char *volatile s_rt_variant_lbl = "";       /* current variant label (UI)            */
static bool               s_rt_loaded;                   /* NVS config pulled in once             */
static bool               s_rt_resume_phone;             /* re-arm the phone advert on stop        */

/* ---- Signal Hunt target (M7-P4·2; guarded by s_dev_mux) ---------------------------- *
 * One pinned device whose RSSI is tracked live (EMA-smoothed) for the proximity gradient.
 * Updated in dev_upsert on the host task whenever the target's advert arrives; read as a snapshot
 * by the UI. */
static uint8_t            s_hunt_addr[6];
static uint8_t            s_hunt_addr_type;
static volatile bool      s_hunt_active;
static int16_t            s_hunt_raw;         /* last raw RSSI dBm                             */
static int32_t            s_hunt_ema_x100;    /* EMA of RSSI * 100 (the smoothed gradient value) */
static int16_t            s_hunt_peak;        /* strongest (closest) RSSI seen since pinned    */
static int64_t            s_hunt_last_us;     /* last time the target was heard                */
static uint16_t           s_hunt_frames;      /* adverts heard from the target since pinned    */
static char               s_hunt_name[BLE_NAME_MAX];

/* ---- advert PCAP export (M7-P4·3) -------------------------------------------------- *
 * Records every received advertisement to /sd as a Wireshark-readable PCAP. Since the single
 * 2.4 GHz radio leaves little internal RAM to spare during BLE, this reuses the WiFi PCAP writer's
 * *pattern* but not a fresh task: the GAP callback (host task, the sole producer) copies each raw
 * advert into a PSRAM SPSC ring in O(1), and the existing worker task drains the ring to the file
 * on a 100 ms wake (its stack is enlarged to fit FATFS — see nocsif_ble_init). Each record is
 * synthesized into a BLE link-layer advertising PDU with a pseudo-header
 * (LINKTYPE_BLUETOOTH_LE_LL_WITH_PHDR = 256) so the RSSI, address, and AD payload decode natively
 * in Wireshark. Fidelity note: the observer hands us the assembled adv report rather than the raw
 * over-the-air bits, so the channel is recorded as nominal and the CRC is left unchecked. */
#define BLE_PCAP_SNAP   40       /* AD bytes captured per advert (legacy adv payload <= 31)     */
#define BLE_PCAP_SLOTS  256      /* ring depth (power of two); ~20 KB PSRAM                     */
#define BLE_PCAP_PHDR   10       /* LE-LL pseudo-header length                                  */
#define BLE_PCAP_DRAIN  64       /* max records written per worker service pass (bounded)       */

typedef struct {
    int64_t  ts_us;              /* capture time (esp_timer)                    */
    uint8_t  addr[6];            /* advertiser address (as received, LE)        */
    uint8_t  addr_type;          /* NimBLE address type (odd = random)          */
    int8_t   rssi;               /* report RSSI (dBm) -> pseudo-header signal    */
    uint8_t  evtype;             /* BLE_HCI_ADV_RPT_EVTYPE_* -> LL PDU type      */
    uint8_t  dlen;               /* AD bytes copied (<= BLE_PCAP_SNAP)          */
    uint8_t  data[BLE_PCAP_SNAP];
} ble_pcap_slot_t;

static ble_pcap_slot_t   *s_pcap_ring;             /* PSRAM, BLE_PCAP_SLOTS entries (allocated lazily) */
static volatile uint32_t  s_pcap_head;             /* consumer index (worker task)            */
static volatile uint32_t  s_pcap_tail;             /* producer index (host task)              */
static volatile uint32_t  s_pcap_drop;             /* adverts dropped (ring full)             */
static volatile bool      s_pcap_want;             /* recording armed (worker owns the file)  */
static volatile bool      s_pcap_active;           /* file open -> host cb fills the ring      */
static volatile uint32_t  s_pcap_frames;           /* adverts written this session            */
static volatile uint32_t  s_pcap_bytes;            /* bytes written (incl. headers)           */
static char               s_pcap_path[64];         /* current/last file (worker + LVGL read)  */
static FILE              *s_pcap_f;                 /* open capture file (worker task only)    */
typedef enum { BPCAP_OFF = 0, BPCAP_REC, BPCAP_NOSD, BPCAP_FILESHARE, BPCAP_ERR } ble_pcap_state_t;
static volatile int       s_pcap_state = BPCAP_OFF;

/* ---- Drone Detection (M7-P4·4): OpenDroneID / ASTM F3411 Remote ID ------------------ *
 * Receive-side airspace awareness: a compliant drone is required to broadcast a public Remote
 * ID — identity, its own position, and the operator (pilot) position. Over Bluetooth it rides a
 * Service Data AD (UUID 0xFFFA, app code 0x0D); each BT4-legacy advert carries ONE 25-byte ODID
 * message, cycling Basic-ID / Location / System / Operator-ID across successive adverts, which we
 * accumulate per drone (address-keyed). Parsed on the host task inside the GAP callback; the UI
 * reads a lock-free snapshot. No transmit, no interaction — the same identification any Remote-ID
 * receiver shows. */
#define BLE_DRONE_MAX  10       /* tracked drones (first-seen; stalest evicted)   */
#define ODID_MSG_LEN   25       /* one ASTM F3411 message                         */

typedef struct {
    bool     used;
    uint8_t  addr[6];
    uint8_t  addr_type;
    int8_t   rssi;
    int64_t  last_us;
    uint16_t msgs;
    bool     has_basic;
    uint8_t  id_type;           /* 1=serial 2=CAA reg 3=UTM(UUID) 4=session        */
    uint8_t  ua_type;           /* UA category (see nocsif_ble_ua_type_str)         */
    char     uas_id[21];        /* UAS ID (serial / registration), ASCII            */
    bool     has_loc;
    int32_t  lat_e7, lon_e7;    /* drone position (deg * 1e7)                       */
    int16_t  alt_m;             /* geodetic altitude, m                             */
    uint16_t speed_x10;         /* ground speed, m/s * 10                           */
    uint16_t track_deg;         /* course over ground, 0-359                        */
    bool     has_op_loc;
    int32_t  op_lat_e7, op_lon_e7;   /* operator / pilot position                   */
    bool     has_op_id;
    char     operator_id[21];
} ble_drone_t;

static ble_drone_t        s_drone[BLE_DRONE_MAX];
static volatile int       s_drone_cnt;
static volatile uint32_t  s_drone_gen;
static portMUX_TYPE       s_drone_mux = portMUX_INITIALIZER_UNLOCKED;

/* ---- Phone Notifications (M7, ANCS — PERIPHERAL + CENTRAL-of-GATT) ------------------ *
 * The watch advertises connectably to solicit ANCS; the phone connects and bonds, and the watch
 * then becomes the GATT client of the phone's ANCS service. The host task drives the
 * discover->subscribe->fetch chain and parses notifications into a table; the UI reads a
 * lock-free snapshot. The three ANCS characteristic UUIDs are 128-bit
 * (BLE_UUID128_INIT wants little-endian byte order, i.e. the printed UUID reversed byte-by-byte). */
#define ANCS_MAX_NOTIF   16      /* recent notifications kept for the UI            */
#define ANCS_DS_BUF      512     /* Data Source reassembly buffer                   */

/* 7905F431-B5CE-4E99-A40F-4B1E122D00D0 — ANCS service */
static const ble_uuid128_t ANCS_SVC_UUID = BLE_UUID128_INIT(
    0xD0, 0x00, 0x2D, 0x12, 0x1E, 0x4B, 0x0F, 0xA4,
    0x99, 0x4E, 0xCE, 0xB5, 0x31, 0xF4, 0x05, 0x79);
/* 9FBF120D-6301-42D9-8C58-25E699A21DBD — Notification Source (notify) */
static const ble_uuid128_t ANCS_NS_UUID = BLE_UUID128_INIT(
    0xBD, 0x1D, 0xA2, 0x99, 0xE6, 0x25, 0x58, 0x8C,
    0xD9, 0x42, 0x01, 0x63, 0x0D, 0x12, 0xBF, 0x9F);
/* 69D1D8F3-45E1-49A8-9821-9BBDFDAAD9D9 — Control Point (write) */
static const ble_uuid128_t ANCS_CP_UUID = BLE_UUID128_INIT(
    0xD9, 0xD9, 0xAA, 0xFD, 0xBD, 0x9B, 0x21, 0x98,
    0xA8, 0x49, 0xE1, 0x45, 0xF3, 0xD8, 0xD1, 0x69);
/* 22EAC6E9-24D6-4BB5-BE44-B36ACE7C7BFB — Data Source (notify) */
static const ble_uuid128_t ANCS_DS_UUID = BLE_UUID128_INIT(
    0xFB, 0x7B, 0x7C, 0xCE, 0x6A, 0xB3, 0x44, 0xBE,
    0xB5, 0x4B, 0xD6, 0x24, 0xE9, 0xC6, 0xEA, 0x22);

/* Apple Media Service (AMS) — the media-remote sibling of ANCS, discovered on the SAME bonded
 * phone link. The watch (client) reads now-playing over Entity Update and drives playback over
 * Remote Command. UUIDs stored little-endian, same convention as ANCS above.
 * 89D3502B-0F36-433A-8EF4-C502AD55F8DC — service */
static const ble_uuid128_t AMS_SVC_UUID = BLE_UUID128_INIT(
    0xDC, 0xF8, 0x55, 0xAD, 0x02, 0xC5, 0xF4, 0x8E,
    0x3A, 0x43, 0x36, 0x0F, 0x2B, 0x50, 0xD3, 0x89);
/* 9B3C81D8-57B1-4A8A-B8DF-0E56F7CA51C2 — Remote Command (write: send a command id) */
static const ble_uuid128_t AMS_RC_UUID = BLE_UUID128_INIT(
    0xC2, 0x51, 0xCA, 0xF7, 0x56, 0x0E, 0xDF, 0xB8,
    0x8A, 0x4A, 0xB1, 0x57, 0xD8, 0x81, 0x3C, 0x9B);
/* 2F7CABCE-808D-411F-9A0C-BB92BA96C102 — Entity Update (write: register interest; notify: now-playing) */
static const ble_uuid128_t AMS_EU_UUID = BLE_UUID128_INIT(
    0x02, 0xC1, 0x96, 0xBA, 0x92, 0xBB, 0x0C, 0x9A,
    0x1F, 0x41, 0x8D, 0x80, 0xCE, 0xAB, 0x7C, 0x2F);

typedef struct {
    bool     used;
    uint32_t uid;
    uint8_t  category;
    int64_t  rx_us;
    bool     have_attrs;
    char     app[28];
    char     title[40];
    char     message[100];
} anc_notif_t;

static anc_notif_t        s_anc[ANCS_MAX_NOTIF];      /* newest at index 0 (shifted down on insert) */
static volatile int       s_anc_cnt;
static volatile uint32_t  s_anc_gen;
static portMUX_TYPE       s_anc_mux = portMUX_INITIALIZER_UNLOCKED;

static volatile int       s_anc_state;               /* nocsif_ble_ancs_state_t              */
static bool               s_want_ancs;               /* intent: advertise for ANCS once synced */
static uint16_t           s_anc_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t           s_anc_ns_val, s_anc_ns_cccd;   /* Notification Source value + CCCD  */
static uint16_t           s_anc_cp_val;                  /* Control Point value handle        */
static uint16_t           s_anc_ds_val, s_anc_ds_cccd;   /* Data Source value + CCCD          */
static uint16_t           s_anc_svc_start, s_anc_svc_end;
static uint8_t            s_anc_ds_buf[ANCS_DS_BUF];     /* Data Source reassembly            */
static int                s_anc_ds_len;
static uint32_t           s_anc_ds_uid;                  /* the uid the current DS response belongs to */

/* ---- AMS (media remote) — rides the same bonded phone link as ANCS, discovered right after it ---- */
static uint16_t           s_ams_svc_start, s_ams_svc_end;
static uint16_t           s_ams_rc_val;                  /* Remote Command value handle (write)   */
static uint16_t           s_ams_eu_val, s_ams_eu_cccd;   /* Entity Update value + CCCD (notify)   */
static bool               s_ams_started;                 /* AMS discovery has been kicked off on this link */
static bool               s_ams_ready;                   /* EU subscribed + attrs registered      */
static portMUX_TYPE       s_ams_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t  s_ams_gen;                     /* bumps on any now-playing change        */
static bool               s_ams_playing;                 /* PlaybackInfo state == Playing          */
static int                s_ams_vol_pct = -1;            /* -1 = unknown                           */
static char               s_ams_title[64];
static char               s_ams_artist[48];
static void ams_start_discovery(void);   /* fwd: kicked off by the ANCS chain once its ATT work is done */

/* ---- Phone companion hub (M7, Connect Phone screen): persisted lifecycle + saved-phone metadata --- *
 * The NimBLE bond store is authoritative for pairing keys but doesn't carry a friendly name or
 * recency, so a small parallel table lives in our own settings NVS. It's slot-based (fixed keys
 * "ph_s0".."ph_sN") so it can be enumerated WITHOUT bringing the stack up — main.c gates the boot
 * auto-connect on the count. */
static volatile bool      s_bt_master     = true;   /* Bluetooth master switch (persisted): governs auto-connect */
static volatile bool      s_boot_reserve_ran;       /* nocsif_ble_boot_reserve claimed the block before WiFi */
static volatile bool      s_notif_enabled = true;   /* mirrors phone notifications (persisted)              */
static bool               s_phone_cfg_loaded;        /* NVS has been pulled in once                        */
static uint32_t           s_phone_seq;               /* monotonic recency counter (persisted)              */
static nocsif_ble_phone_t s_phone[NOCSIF_BLE_PHONE_MAX];   /* RAM cache of the saved-phone metadata        */
static int                s_phone_cnt;
static portMUX_TYPE       s_phone_mux = portMUX_INITIALIZER_UNLOCKED;
/* Identity address of the live link, cached on ENC_CHANGE and cleared on DISCONNECT, so the
 * "connected" flag and the recency-capture path never need a host call from the LVGL task. */
static volatile bool      s_phone_conn_valid;
static uint8_t            s_phone_conn_addr[6];
static uint8_t            s_phone_conn_atype;
/* Pending metadata record: the host task fills it on a bond, the worker consumes it and writes NVS,
 * keeping the actual flash write off the host task. */
static uint8_t            s_meta_addr[6];
static uint8_t            s_meta_atype;
static char               s_meta_name[BLE_NAME_MAX];   /* peer GAP device name (0x2A00), best-effort  */
static volatile bool      s_meta_pending;
/* Forget target: set by the LVGL-task request function, consumed by the worker (a serialized user action). */
static ble_addr_t         s_forget_target;
static volatile bool      s_forget_pending;
static void phone_cfg_load(void);        /* fwd: preloaded in nocsif_ble_init, also guarded at each call site */

/* saved HID hosts (keyboards/computers bonded to the watch's HID keyboard) */
static nocsif_ble_phone_t s_hidbond[NOCSIF_BLE_HIDBOND_MAX];   /* reuse the phone struct (same fields) */
static int                s_hidbond_cnt;
static uint32_t           s_hidbond_seq;
static bool               s_hidbond_loaded;
static uint8_t            s_hidmeta_addr[6];        /* host fills on HID bond, worker persists off-task */
static uint8_t            s_hidmeta_atype;
static char               s_hidmeta_name[BLE_NAME_MAX];  /* host GAP device name (0x2A00), best-effort  */
static volatile bool      s_hidmeta_pending;
static ble_addr_t         s_hidforget_target;       /* LVGL-task request → worker deletes the bond      */
static volatile bool      s_hidforget_pending;
static void hidbond_cfg_load(void);      /* fwd */

/* ---- BLE HID keyboard (M7, PERIPHERAL + GATT server) ------------------------------- *
 * The watch hosts a HID-over-GATT keyboard service and advertises with the keyboard appearance; a
 * host pairs and subscribes, and the watch types by notifying 8-byte boot-keyboard reports (built
 * by the shared hid_kbd keymap, driven by the DuckyScript engine over its BLE sink). Mutually
 * exclusive with the ANCS/AMS phone link at the connection level (MAX_CONNECTIONS=1) —
 * do_hid_start drops the phone link and swaps to the keyboard advert. The HID service is
 * registered PERMANENTLY at boot (gatt_server_register), so s_hid_mode no longer gates whether the
 * table exists — it just means "currently advertising/linked as a keyboard" (RAM Phase 2 #10). */
static uint8_t            s_hid_mode;                        /* currently advertising/linked as a keyboard */
static volatile int       s_hid_state;                      /* nocsif_ble_hid_state_t                   */
static uint16_t           s_hid_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t           s_hid_input_val;                  /* keyboard input report handle (notify, ID1)*/
static uint16_t           s_hid_mouse_val;                  /* mouse input report handle (notify, ID2)  */
static uint16_t           s_hid_consumer_val;               /* consumer/media input report (notify, ID3)*/
static uint16_t           s_hid_batt_val;                   /* battery level char value handle          */
static volatile bool      s_hid_subscribed;                 /* host subscribed to input-report notifies */
static volatile bool      s_hid_encrypted;                  /* link encrypted (bonded)                  */
static volatile bool      s_hid_conn_valid;                 /* identity of the live HID host is cached   */
static uint8_t            s_hid_conn_addr[6];               /* live HID host identity (for "connected")  */
static uint8_t            s_hid_led;                         /* last LED output report (caps/num/scroll) */
static uint8_t            s_hid_proto = 1;                   /* HID protocol mode (1 = report protocol)  */
static void hid_teardown(void);           /* fwd: leave keyboard role (stop advert/link, clear s_hid_mode) */
static void gatt_server_register(void);   /* fwd: bring_up registers the permanent GATT server at boot   */

/* ---- device-table upsert (NimBLE host task) --------------------------------------- *
 * Merges one advertisement report into the table. A field defaults to "absent" — a scan response
 * for a device already in the table only *adds* what it carries (typically the name), never
 * clears data collected earlier. */
static void dev_upsert(const uint8_t addr[6], uint8_t addr_type, int8_t rssi, bool connectable,
                       const char *name, int8_t tx_pwr, bool tx_pwr_ok,
                       uint16_t appearance, bool appearance_ok,
                       uint16_t company, bool company_ok,
                       uint16_t uuid16, uint8_t n_uuid, uint8_t tracker,
                       const uint8_t *raw, uint8_t raw_len, bool is_scan_rsp)
{
    int64_t now = esp_timer_get_time();

    /* Omit list: an omitted address never enters the table (checked before the table lock; its own
     * short lock is not nested inside s_dev_mux). Dropped silently — it vanishes from every BLE screen. */
    if (nocsif_ble_omit_contains(addr, addr_type)) {
        return;
    }

    portENTER_CRITICAL(&s_dev_mux);
    int idx = -1;
    for (int i = 0; i < s_dev_cnt; i++) {
        if (s_dev[i].used && s_dev[i].addr_type == addr_type &&
            memcmp(s_dev[i].addr, addr, 6) == 0) { idx = i; break; }
    }
    bool structural = false;
    if (idx < 0) {
        if (s_dev_cnt < BLE_DEV_MAX) {
            idx = s_dev_cnt++;
        } else {                                    /* table full — evict the stalest entry */
            idx = 0;
            int64_t oldest = s_dev[0].last_us;
            for (int i = 1; i < BLE_DEV_MAX; i++) {
                if (s_dev[i].last_us < oldest) { oldest = s_dev[i].last_us; idx = i; }
            }
        }
        ble_dev_t *n = &s_dev[idx];
        memset(n, 0, sizeof *n);
        n->used = true;
        memcpy(n->addr, addr, 6);
        n->addr_type = addr_type;
        n->first_us = now;                                                  /* anti-stalk presence clock */
        if (s_dev_raw) memset(&s_dev_raw[idx], 0, sizeof s_dev_raw[idx]);   /* clear the slot's raw AD */
        structural = true;
    }
    ble_dev_t *d = &s_dev[idx];
    d->rssi = rssi;
    d->last_us = now;
    if (connectable)  d->connectable = true;        /* sticky: a scan-response PDU must not clear this */
    if (name && name[0]) {                          /* reveal / refresh the name, keeping a known one */
        if (d->name[0] == '\0') structural = true;  /* a name newly revealed by a scan-rsp changes the
                                                     * row's headline text, so force a UI refresh rather
                                                     * than let it sit unseen until the row next churns */
        size_t k = 0;
        while (name[k] && k < sizeof(d->name) - 1) { d->name[k] = name[k]; k++; }
        d->name[k] = '\0';
    }
    if (tx_pwr_ok)     { d->tx_pwr = tx_pwr; d->tx_pwr_ok = true; }
    if (appearance_ok) { d->appearance = appearance; d->appearance_ok = true; }
    if (company_ok)    { d->company = company; d->company_ok = true; }
    if (uuid16)        d->uuid16 = uuid16;
    if (n_uuid > d->n_uuid) d->n_uuid = n_uuid;
    if (tracker)       d->tracker = tracker;        /* sticky: a later bare PDU must not clear this  */
    if (d->frames < 0xFFFF) d->frames++;
    /* Keep the raw AD payload (PSRAM, parallel to s_dev) for the device-detail decoder. ADV and SCAN_RSP
     * land in separate reports (active scan), so store each in its own buffer — a scan-rsp must not
     * overwrite the adv payload. */
    if (s_dev_raw && raw && raw_len) {
        ble_dev_raw_t *rw = &s_dev_raw[idx];
        uint8_t n = raw_len > NOCSIF_BLE_ADV_MAX ? NOCSIF_BLE_ADV_MAX : raw_len;
        if (is_scan_rsp) { memcpy(rw->rsp_data, raw, n); rw->rsp_len = n; }
        else             { memcpy(rw->adv_data, raw, n); rw->adv_len = n; }
    }
    if (structural) s_dev_gen++;

    /* Signal Hunt (M7-P4·2): if this advert came from the pinned target, update its live gradient. */
    if (s_hunt_active && addr_type == s_hunt_addr_type && memcmp(addr, s_hunt_addr, 6) == 0) {
        s_hunt_raw = rssi;
        s_hunt_ema_x100 = (s_hunt_ema_x100 == 0)
                              ? (int32_t)rssi * 100
                              : (s_hunt_ema_x100 * 7 + (int32_t)rssi * 100 * 3) / 10;  /* EMA, alpha = 0.3 */
        if (rssi > s_hunt_peak) s_hunt_peak = rssi;   /* closer = higher (less negative) */
        s_hunt_last_us = now;
        if (s_hunt_frames < 0xFFFF) s_hunt_frames++;
    }
    portEXIT_CRITICAL(&s_dev_mux);
}

/* Recognizes a common item tracker from its advertisement (M7-P4·1). A receive-side classification
 * of signatures the tracker is already broadcasting: Apple Find My / AirTag = the offline-finding
 * beacon (company 0x004C, manufacturer message type 0x12); Tile = service UUID 0xFEED/0xFEEC;
 * Samsung SmartTag = service UUID 0xFD5A. `mfg` points at the raw manufacturer AD (mfg[0..1] holds
 * the company id). */
static uint8_t classify_tracker(uint16_t company, bool company_ok,
                                const uint8_t *mfg, uint8_t mfg_len, uint16_t uuid16)
{
    if (company_ok && company == 0x004C && mfg && mfg_len >= 3 && mfg[2] == 0x12) {
        return NOCSIF_BLE_TRACKER_FINDMY;
    }
    if (uuid16 == 0xFEED || uuid16 == 0xFEEC) return NOCSIF_BLE_TRACKER_TILE;
    if (uuid16 == 0xFD5A)                     return NOCSIF_BLE_TRACKER_SMARTTAG;
    return NOCSIF_BLE_TRACKER_NONE;
}

/* card-skimmer detection (passive heuristic; see ble.h) */

/* Case-insensitive: does the advertised name begin with a known BLE-serial-module default? */
static bool skim_name_match(const char *n)
{
    if (n == NULL || n[0] == '\0') return false;
    static const char *const k[] = {
        "HC-05", "HC-06", "HC-08", "HC05", "HC06", "BT05", "BT-05", "MLT-BT05", "HMSoft",
        "AT-09", "AT-19", "CC41", "JDY-", "SPP-CA", "FSC-BT", "BOLUTEK", "SPP",
    };
    for (size_t j = 0; j < sizeof k / sizeof k[0]; j++) {
        if (strncasecmp(n, k[j], strlen(k[j])) == 0) return true;
    }
    return false;
}

/* Walk an AD payload for a 16-bit service UUID == want (AD types 0x02 incomplete / 0x03 complete). */
static bool ad_has_uuid16(const uint8_t *b, int len, uint16_t want)
{
    int i = 0;
    while (i + 1 < len) {
        int l = b[i];
        if (l == 0 || i + 1 + l > len) break;
        uint8_t t = b[i + 1];
        if (t == 0x02 || t == 0x03) {
            for (int p = i + 2; p + 1 < i + 1 + l; p += 2) {
                if ((b[p] | (b[p + 1] << 8)) == want) return true;
            }
        }
        i += 1 + l;
    }
    return false;
}

/* Walk an AD payload for the Nordic UART Service 128-bit UUID (AD types 0x06/0x07) */
static bool ad_has_nus(const uint8_t *b, int len)
{
    static const uint8_t nus_le[12] = { 0x9E,0xCA,0xDC,0x24,0x0E,0xE5,0xA9,0xE0,0x93,0xF3,0xA3,0xB5 };
    int i = 0;
    while (i + 1 < len) {
        int l = b[i];
        if (l == 0 || i + 1 + l > len) break;
        uint8_t t = b[i + 1];
        if ((t == 0x06 || t == 0x07) && l >= 17) {
            for (int p = i + 2; p + 16 <= i + 1 + l; p += 16) {
                if (memcmp(&b[p], nus_le, 12) == 0) return true;
            }
        }
        i += 1 + l;
    }
    return false;
}

/* Internal classifier over the table entry + its raw AD (both under s_dev_mux) */
static const char *skim_reason(const ble_dev_t *d, const ble_dev_raw_t *raw)
{
    if (skim_name_match(d->name)) return "serial module name";
    if (d->uuid16 == 0xFFE0)      return "serial svc 0xFFE0";
    if (raw) {
        if (ad_has_uuid16(raw->adv_data, raw->adv_len, 0xFFE0) ||
            ad_has_uuid16(raw->rsp_data, raw->rsp_len, 0xFFE0)) return "serial svc 0xFFE0";
        if (ad_has_nus(raw->adv_data, raw->adv_len) ||
            ad_has_nus(raw->rsp_data, raw->rsp_len))            return "Nordic UART";
    }
    return NULL;
}

/* Copy one raw advertisement into the PCAP ring. */
static inline void pcap_push(const struct ble_gap_disc_desc *d)
{
    if (!s_pcap_active || !s_pcap_ring) {
        return;
    }
    uint32_t tail = s_pcap_tail;                                   /* sole producer */
    uint32_t head = __atomic_load_n(&s_pcap_head, __ATOMIC_ACQUIRE);
    if ((tail - head) >= BLE_PCAP_SLOTS) {
        s_pcap_drop++;                                             /* ring full — drop this advert */
        return;
    }
    ble_pcap_slot_t *s = &s_pcap_ring[tail & (BLE_PCAP_SLOTS - 1)];
    s->ts_us     = esp_timer_get_time();
    memcpy(s->addr, d->addr.val, 6);
    s->addr_type = d->addr.type;
    s->rssi      = d->rssi;
    s->evtype    = d->event_type;
    uint8_t n    = d->length_data > BLE_PCAP_SNAP ? BLE_PCAP_SNAP : d->length_data;
    s->dlen      = n;
    if (n) {
        memcpy(s->data, d->data, n);
    }
    __atomic_store_n(&s_pcap_tail, tail + 1, __ATOMIC_RELEASE);
}

/* Reads a little-endian int32 (ODID latitude/longitude are stored as deg * 1e7). */
static int32_t odid_le32(const uint8_t *p)
{
    return (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                     ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
}

/* Copies an ASCII ODID text field (fixed width, may be unterminated); maps non-printables to '?'. */
static void odid_copy_text(char *dst, size_t dstsz, const uint8_t *src, int width)
{
    int k = 0;
    for (; k < width && (size_t)k < dstsz - 1; k++) {
        char c = (char)src[k];
        if (c == '\0') break;
        dst[k] = (c >= 0x20 && c < 0x7F) ? c : '?';
    }
    dst[k] = '\0';
}

/* Merges one 25-byte ODID message into a drone record (host task; caller holds s_drone_mux). Only
 * decodes the fields a receiver would actually act on: identity, drone position, and operator
 * position. */
static void odid_apply_msg(ble_drone_t *d, const uint8_t *m, int mlen)
{
    if (mlen < ODID_MSG_LEN) {
        return;
    }
    uint8_t type = (m[0] >> 4) & 0x0F;          /* high nibble = message type; low nibble = protocol version */
    switch (type) {
    case 0x0:   /* Basic ID: identity + aircraft category */
        d->id_type = (m[1] >> 4) & 0x0F;
        d->ua_type = m[1] & 0x0F;
        odid_copy_text(d->uas_id, sizeof d->uas_id, m + 2, 20);
        d->has_basic = true;
        break;
    case 0x1: {  /* Location / Vector: the drone's own position + motion */
        uint8_t flags = m[1];
        bool ew  = flags & 0x02;                /* east/west direction segment (adds 180 to the track) */
        bool mult = flags & 0x01;               /* speed multiplier: 0.25 m/s (clear) vs 0.75 m/s      */
        d->track_deg = (uint16_t)(((int)m[2] + (ew ? 180 : 0)) % 360);
        d->speed_x10 = mult ? (uint16_t)((uint32_t)m[3] * 75 / 10 + 638)   /* m/s*10 at the 0.75 multiplier */
                            : (uint16_t)((uint32_t)m[3] * 25 / 10);        /* m/s*10 at the 0.25 multiplier */
        d->lat_e7 = odid_le32(m + 5);
        d->lon_e7 = odid_le32(m + 9);
        uint16_t alt_raw = (uint16_t)(m[15] | (m[16] << 8));   /* geodetic altitude */
        d->alt_m = (int16_t)((int)alt_raw / 2 - 1000);         /* encoding: raw * 0.5 - 1000        */
        d->has_loc = true;
        break;
    }
    case 0x4:   /* System: operator (pilot) position + area info */
        d->op_lat_e7 = odid_le32(m + 2);
        d->op_lon_e7 = odid_le32(m + 6);
        d->has_op_loc = true;
        break;
    case 0x5:   /* Operator ID (registration/authority string) */
        odid_copy_text(d->operator_id, sizeof d->operator_id, m + 2, 20);
        d->has_op_id = true;
        break;
    case 0xF: { /* Message Pack: N single messages back-to-back (BT5 ext-adv; one level, no nesting) */
        int sz = m[1], cnt = m[2];
        if (sz != ODID_MSG_LEN) break;
        for (int i = 0; i < cnt; i++) {
            int off = 3 + i * ODID_MSG_LEN;
            if (off + ODID_MSG_LEN > mlen) break;
            if (((m[off] >> 4) & 0x0F) == 0xF) continue;   /* never recurse into a nested pack */
            odid_apply_msg(d, m + off, ODID_MSG_LEN);
        }
        break;
    }
    default:
        break;                                  /* Auth (2) / Self-ID (3) not decoded yet */
    }
}

/* Merges an ODID advertisement into the drone table (host task; uses its own lock, must NOT nest
 * inside s_dev_mux). `msg` points at the first ODID message, after the UUID, app code, and counter. */
static void odid_upsert(const uint8_t addr[6], uint8_t addr_type, int8_t rssi,
                        const uint8_t *msg, int msg_len)
{
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_drone_mux);
    int idx = -1;
    for (int i = 0; i < s_drone_cnt; i++) {
        if (s_drone[i].used && s_drone[i].addr_type == addr_type &&
            memcmp(s_drone[i].addr, addr, 6) == 0) { idx = i; break; }
    }
    bool structural = false;
    if (idx < 0) {
        if (s_drone_cnt < BLE_DRONE_MAX) {
            idx = s_drone_cnt++;
        } else {                                /* table full — evict the stalest entry */
            idx = 0;
            int64_t oldest = s_drone[0].last_us;
            for (int i = 1; i < BLE_DRONE_MAX; i++) {
                if (s_drone[i].last_us < oldest) { oldest = s_drone[i].last_us; idx = i; }
            }
        }
        memset(&s_drone[idx], 0, sizeof s_drone[idx]);
        s_drone[idx].used = true;
        memcpy(s_drone[idx].addr, addr, 6);
        s_drone[idx].addr_type = addr_type;
        structural = true;
    }
    ble_drone_t *d = &s_drone[idx];
    d->rssi = rssi;
    d->last_us = now;
    if (d->msgs < 0xFFFF) d->msgs++;
    odid_apply_msg(d, msg, msg_len);
    if (structural) s_drone_gen++;
    portEXIT_CRITICAL(&s_drone_mux);
}

/* ---- NimBLE GAP discovery callback (host task) ------------------------------------- */
static int gap_disc_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        const struct ble_gap_disc_desc *d = &event->disc;

        pcap_push(d);           /* M7-P4·3: records the raw advert (no-op unless capture is armed) */

        bool is_rsp = (d->event_type == BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP);

        struct ble_hs_adv_fields f;
        if (ble_hs_adv_parse_fields(&f, d->data, d->length_data) != 0) {
            /* Malformed adv payload — still record the address + RSSI (name/fields blank) + raw bytes. */
            bool conn = (d->event_type == BLE_HCI_ADV_RPT_EVTYPE_ADV_IND ||
                         d->event_type == BLE_HCI_ADV_RPT_EVTYPE_DIR_IND);
            dev_upsert(d->addr.val, d->addr.type, d->rssi, conn,
                       NULL, 0, false, 0, false, 0, false, 0, 0, 0,
                       d->data, d->length_data, is_rsp);
            return 0;
        }

        char name[BLE_NAME_MAX] = {0};
        if (f.name && f.name_len) {
            size_t k = f.name_len < sizeof(name) - 1 ? f.name_len : sizeof(name) - 1;
            memcpy(name, f.name, k);
            name[k] = '\0';
        }
        uint16_t company = 0; bool company_ok = false;
        if (f.mfg_data && f.mfg_data_len >= 2) {          /* first 2 bytes = company id, little-endian */
            company = (uint16_t)f.mfg_data[0] | ((uint16_t)f.mfg_data[1] << 8);
            company_ok = true;
        }
        uint16_t uuid16 = 0;
        if (f.num_uuids16 && f.uuids16) {
            uuid16 = ble_uuid_u16(&f.uuids16[0].u);
        }
        uint8_t n_uuid = (uint8_t)(f.num_uuids16 + f.num_uuids32 + f.num_uuids128);
        bool conn = (d->event_type == BLE_HCI_ADV_RPT_EVTYPE_ADV_IND ||
                     d->event_type == BLE_HCI_ADV_RPT_EVTYPE_DIR_IND);
        uint8_t tracker = classify_tracker(company, company_ok, f.mfg_data, f.mfg_data_len, uuid16);

        dev_upsert(d->addr.val, d->addr.type, d->rssi, conn,
                   name, f.tx_pwr_lvl, f.tx_pwr_lvl_is_present,
                   f.appearance, f.appearance_is_present,
                   company, company_ok, uuid16, n_uuid, tracker,
                   d->data, d->length_data, is_rsp);

        /* OpenDroneID / ASTM F3411 Remote ID (M7-P4·4): a Service Data AD carrying UUID 0xFFFA
         * (bytes FA FF, little-endian) plus app code 0x0D, then a 1-byte counter, then the ODID
         * message(s). Uses its own lock, separate from dev_upsert (portMUX is not recursive). */
        if (f.svc_data_uuid16 && f.svc_data_uuid16_len >= 4 + ODID_MSG_LEN &&
            f.svc_data_uuid16[0] == 0xFA && f.svc_data_uuid16[1] == 0xFF &&
            f.svc_data_uuid16[2] == 0x0D) {
            odid_upsert(d->addr.val, d->addr.type, d->rssi,
                        f.svc_data_uuid16 + 4, (int)f.svc_data_uuid16_len - 4);
        }
        return 0;
    }
    case BLE_GAP_EVENT_DISC_COMPLETE:
        /* Only reached if a time-limited discovery ended; since we scan BLE_HS_FOREVER, this means
         * a cancel or an internal stop instead. Reflect it so the UI shows "stopped". */
        s_scan_active = false;
        ESP_LOGI(TAG, "discovery complete (reason %d)", event->disc_complete.reason);
        return 0;
    default:
        return 0;
    }
}

/* ---- NimBLE host lifecycle -------------------------------------------------------- */
static void scan_start(void)   /* host- or worker-task; NimBLE host API is internally locked */
{
    if (!s_synced) {
        s_want_scan = true;     /* on_sync will start it */
        return;
    }
    uint8_t own_addr_type = 0;
    int rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        ESP_LOGW(TAG, "id_infer_auto rc=%d", rc);
        return;
    }
    /* With CONFIG_BT_CTRL_BLE_MAX_ACT=3 (bumped from 2) the controller has a spare slot for the
     * observer scan ALONGSIDE the persistent ANCS phone advert, so scanning no longer needs to stop
     * advertising first — they coexist, and the phone can still reconnect while a scan runs. (At
     * MAX_ACT=2, adv(1)+scan(1) left no free slot and ble_gap_disc was rejected with 0x207
     * BLE_ERR_MEM_CAPACITY — confirmed via SCANDBG.) */
    struct ble_gap_disc_params dp = {0};
    dp.passive = 0;             /* active scan: solicits scan responses so device names come back */
    dp.filter_duplicates = 0;   /* keeps repeats: refreshes RSSI / last-seen per report            */
    dp.itvl = 0;                /* use the controller's defaults                                   */
    dp.window = 0;
    dp.filter_policy = 0;
    dp.limited = 0;
    rc = ble_gap_disc(own_addr_type, BLE_HS_FOREVER, &dp, gap_disc_event_cb, NULL);
    if (rc == 0) {
        s_scan_active = true;
        s_want_scan = false;
        s_starting = false;
        ESP_LOGI(TAG, "discovery started (active scan)");
    } else if (rc == BLE_HS_EALREADY) {
        s_scan_active = true;   /* already scanning — nothing to do */
        s_want_scan = false;
        s_starting = false;
    } else {
        s_starting = false;
        ESP_LOGW(TAG, "ble_gap_disc rc=%d", rc);
    }
}

static void scan_stop(void)
{
    s_want_scan = false;
    if (s_scan_active) {
        int rc = ble_gap_disc_cancel();
        if (rc != 0 && rc != BLE_HS_EALREADY) {
            ESP_LOGW(TAG, "ble_gap_disc_cancel rc=%d", rc);
        }
        s_scan_active = false;
    }
}

/* ---- advertise / beacon (M7-P3) --------------------------------------------------- *
 * The fixed NocSif iBeacon proximity UUID (its leading bytes spell "NOCSIF" so it's recognizable
 * in a beacon scanner). iBeacon major/minor are fixed at 1/1 for this phase. */
static const uint8_t NOCSIF_BEACON_UUID[16] = {
    0x4E, 0x4F, 0x43, 0x53, 0x49, 0x46,   /* "NOCSIF" */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
};

/* Builds the raw 30-byte iBeacon advertising payload (flags AD + Apple manufacturer-data AD). */
static int build_ibeacon(uint8_t *o)
{
    int n = 0;
    o[n++] = 0x02; o[n++] = 0x01; o[n++] = 0x06;          /* AD: flags = LE General + BR/EDR unsupported */
    o[n++] = 0x1A; o[n++] = 0xFF;                         /* AD: len 26, manufacturer-specific       */
    o[n++] = 0x4C; o[n++] = 0x00;                         /* company = Apple (0x004C, little-endian)  */
    o[n++] = 0x02; o[n++] = 0x15;                         /* iBeacon type + payload length            */
    memcpy(&o[n], NOCSIF_BEACON_UUID, 16); n += 16;       /* proximity UUID                           */
    o[n++] = 0x00; o[n++] = 0x01;                         /* major = 1 (big-endian)                   */
    o[n++] = 0x00; o[n++] = 0x01;                         /* minor = 1                                */
    o[n++] = 0xC5;                                        /* measured power: -59 dBm at 1 m           */
    return n;                                             /* 30 bytes                                 */
}

/* Non-connectable advertising generates no GAP events to react to (no connection, we advertise
 * FOREVER), but a stub is passed instead of NULL so the host never dereferences a null callback. */
static int gap_adv_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)event; (void)arg;
    return 0;
}

/* Fwd: the unified companion/HID GAP handler */
static int ancs_gap_event(struct ble_gap_event *event, void *arg);

/* Start advertising with the current config */
static void adv_start_now(void)
{
    if (!s_synced) {
        s_want_adv = true;      /* on_sync will start it */
        return;
    }
    uint8_t own_addr_type = 0;
    if (ble_hs_id_infer_auto(0, &own_addr_type) != 0) {
        own_addr_type = 0;
    }

    int rc;
    if (s_adv_mode == NOCSIF_BLE_ADV_IBEACON) {
        uint8_t payload[31];
        int len = build_ibeacon(payload);
        rc = ble_gap_adv_set_data(payload, len);
    } else {
        /* Caps the name so flags(3) + tx-power(3) + name-header(2) + name fits the 31-byte adv
         * payload; a longer name is advertised truncated and marked incomplete rather than
         * failing the set outright. */
        size_t nlen = strlen(s_adv_name);
        const size_t NAME_MAX_ADV = 23;
        struct ble_hs_adv_fields fields = {0};
        fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
        fields.name = (const uint8_t *)s_adv_name;
        fields.name_len = (uint8_t)(nlen > NAME_MAX_ADV ? NAME_MAX_ADV : nlen);
        fields.name_is_complete = (nlen <= NAME_MAX_ADV) ? 1 : 0;
        fields.tx_pwr_lvl_is_present = 1;
        fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;   /* the stack fills in the real level */
        rc = ble_gap_adv_set_fields(&fields);
    }
    if (rc != 0) {
        ESP_LOGW(TAG, "adv set-data rc=%d", rc);
        s_adv_starting = false;
        return;
    }

    struct ble_gap_adv_params p = {0};
    p.conn_mode = BLE_GAP_CONN_MODE_NON;                  /* non-connectable beacon */
    p.disc_mode = (s_adv_mode == NOCSIF_BLE_ADV_IBEACON) ? BLE_GAP_DISC_MODE_NON
                                                         : BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &p, gap_adv_event_cb, NULL);
    if (rc == 0 || rc == BLE_HS_EALREADY) {
        s_adv_active = true;
        s_want_adv = false;
        s_adv_starting = false;
        ESP_LOGI(TAG, "advertising started (%s)",
                 s_adv_mode == NOCSIF_BLE_ADV_IBEACON ? "iBeacon" : "named");
    } else {
        s_adv_starting = false;
        ESP_LOGW(TAG, "ble_gap_adv_start rc=%d", rc);
    }
}

static void adv_stop(void)
{
    s_want_adv = false;
    if (s_adv_active) {
        int rc = ble_gap_adv_stop();
        if (rc != 0 && rc != BLE_HS_EALREADY) {
            ESP_LOGW(TAG, "ble_gap_adv_stop rc=%d", rc);
        }
        s_adv_active = false;
    }
}

/* Pulls the persisted advertise config (mode + name) from NVS once, so the screen shows the saved
 * values. Idempotent; safe to call from the LVGL task (settings are RAM-cached after the first read). */
static void adv_config_load(void)
{
    if (s_adv_loaded) {
        return;
    }
    s_adv_loaded = true;
    int32_t m = nocsif_settings_get_i32("adv_mode", NOCSIF_BLE_ADV_CUSTOM);
    s_adv_mode = (m == NOCSIF_BLE_ADV_IBEACON) ? NOCSIF_BLE_ADV_IBEACON : NOCSIF_BLE_ADV_CUSTOM;
    char name[BLE_NAME_MAX];
    if (nocsif_settings_get_str("adv_name", name, sizeof name, "NocSif") == ESP_OK && name[0]) {
        strncpy(s_adv_name, name, sizeof s_adv_name - 1);
        s_adv_name[sizeof s_adv_name - 1] = '\0';
    }
}

/* Advertisement Resilience Test engine: a periodic timer reconfigures the one adv set per variant and auto-stops at the deadline. */

/* Pull the persisted test config once. LVGL-task-safe (settings are RAM-cached). */
static void rt_config_load(void)
{
    if (s_rt_loaded) {
        return;
    }
    s_rt_loaded = true;
    s_rt_connect = nocsif_settings_get_i32("rt_connect", 0) ? true : false;
    int32_t in = nocsif_settings_get_i32("rt_int", NOCSIF_BLE_RT_MED);
    s_rt_intensity = (in >= 0 && in <= NOCSIF_BLE_RT_HIGH) ? (nocsif_ble_rt_intensity_t)in : NOCSIF_BLE_RT_MED;
    int32_t d  = nocsif_settings_get_i32("rt_dur", 60);
    if (d < NOCSIF_BLE_RT_DUR_MIN_S) d = NOCSIF_BLE_RT_DUR_MIN_S;
    if (d > NOCSIF_BLE_RT_DUR_MAX_S) d = NOCSIF_BLE_RT_DUR_MAX_S;
    s_rt_dur_s     = (int)d;
}

/* Milliseconds between advert reconfigures, floored so this stays a bench tool (not saturation). */
static uint32_t rt_cadence_ms(void)
{
    switch (s_rt_intensity) {
    case NOCSIF_BLE_RT_HIGH: return 120;
    case NOCSIF_BLE_RT_MED:  return 250;
    case NOCSIF_BLE_RT_LOW:
    default:                 return 600;
    }
}

/* Legacy adv interval (0.625 ms units); 0xA0 = 100 ms is the non-connectable spec floor. */
static void rt_interval(struct ble_gap_adv_params *p)
{
    uint16_t itvl = (s_rt_intensity == NOCSIF_BLE_RT_HIGH) ? 0x00A0 :   /* 100 ms */
                    (s_rt_intensity == NOCSIF_BLE_RT_MED)  ? 0x0100 :   /* 160 ms */
                                                             0x0200;    /* 320 ms */
    p->itvl_min = itvl;
    p->itvl_max = itvl;
}


/* Apple Continuity Nearby-Action action types — each renders a different iOS pop-up card. */
static const struct { uint8_t action; const char *lbl; } k_apple_actions[] = {
    { 0x13, "popup 0x13: AppleTV AutoFill"   },
    { 0x24, "popup 0x24: Vision Pro"         },
    { 0x05, "popup 0x05: Apple Watch"        },
    { 0x27, "popup 0x27: AppleTV connect"    },
    { 0x20, "popup 0x20: join AppleTV"       },
    { 0x19, "popup 0x19: AppleTV audio sync" },
    { 0x1E, "popup 0x1E: AppleTV color"      },
    { 0x09, "popup 0x09: setup iPhone"       },
    { 0x2F, "popup 0x2F: sign in nearby"     },
    { 0x02, "popup 0x02: transfer number"    },
    { 0x0B, "popup 0x0B: HomePod setup"      },
    { 0x01, "popup 0x01: setup AppleTV"      },
    { 0x06, "popup 0x06: pair AppleTV"       },
    { 0x0D, "popup 0x0D: AppleTV HomeKit"    },
    { 0x2B, "popup 0x2B: AppleID AppleTV"    },
};
#define RT_APPLE_ACT_N ((int)(sizeof k_apple_actions / sizeof k_apple_actions[0]))

/* Apple Proximity-Pairing device models (subset) — AirPods-style pairing cards. */
static const struct { uint16_t model; const char *lbl; } k_apple_models[] = {
    { 0x0E20, "popup: AirPods Pro"    },
    { 0x0A20, "popup: AirPods Max"    },
    { 0x0220, "popup: AirPods"        },
    { 0x1420, "popup: AirPods Pro 2"  },
};
#define RT_APPLE_MODEL_N ((int)(sizeof k_apple_models / sizeof k_apple_models[0]))

/* Google Fast Pair models (24-bit) — Android pairing pop-up. */
static const struct { uint32_t model; const char *lbl; } k_fastpair[] = {
    { 0xCD8256, "popup: Bose NC 700 (FP)" },
    { 0x92BBBD, "popup: Pixel Buds (FP)"  },
    { 0x0E30C3, "popup: Galaxy Buds (FP)" },
};
#define RT_FASTPAIR_N ((int)(sizeof k_fastpair / sizeof k_fastpair[0]))

/* Samsung EasySetup Buds models (24-bit) — Galaxy "connect Buds" pop-up (Samsung phones). */
static const struct { uint32_t model; const char *lbl; } k_sam_buds[] = {
    { 0xB8B905, "popup: Pure White Buds"  },
    { 0x850116, "popup: Black Buds Live"  },
    { 0xEAAA17, "popup: Pure White Buds2" },
};
#define RT_SAM_BUDS_N ((int)(sizeof k_sam_buds / sizeof k_sam_buds[0]))

/* Samsung EasySetup Watch models (8-bit) — Galaxy "Watch" pop-up (Samsung phones). */
static const struct { uint8_t model; const char *lbl; } k_sam_watch[] = {
    { 0x01, "popup: Watch4 Classic" },
    { 0x11, "popup: Watch5 44mm"    },
    { 0x1E, "popup: Watch6 Classic" },
};
#define RT_SAM_WATCH_N ((int)(sizeof k_sam_watch / sizeof k_sam_watch[0]))

/* Device pop-up advert payloads (Apple/Google/Samsung/MS) with hardware-random auth tag, battery and MAC fields. */
static int rt_build_popup(uint8_t *o, int v)
{
    const int total = RT_APPLE_ACT_N + RT_APPLE_MODEL_N + RT_FASTPAIR_N + RT_SAM_BUDS_N + RT_SAM_WATCH_N + 1;
    int idx = (((v % total) + total) % total);
    int i = 0;
    int base = 0;

    if (idx < (base += RT_APPLE_ACT_N)) {             /* Apple Continuity — Nearby Action (11 bytes) */
        int k = idx;
        const uint8_t size = 11;
        o[i++] = size - 1;                            /* 0x0A */
        o[i++] = 0xFF; o[i++] = 0x4C; o[i++] = 0x00;  /* manufacturer: Apple */
        o[i++] = 0x0F;                                /* Continuity type: Nearby Action */
        o[i++] = (uint8_t)(size - i - 1);             /* continuity size = 5 */
        o[i++] = 0xC0;                                /* action flags */
        o[i++] = k_apple_actions[k].action;
        esp_fill_random(&o[i], 3); i += 3;            /* auth tag (randomized) */
        s_rt_variant_lbl = k_apple_actions[k].lbl;
    } else if (idx < (base += RT_APPLE_MODEL_N)) {    /* Apple Proximity Pairing (31 bytes) */
        int k = idx - (base - RT_APPLE_MODEL_N);
        uint16_t model = k_apple_models[k].model;
        const uint8_t size = 31;
        o[i++] = size - 1;                            /* 0x1E */
        o[i++] = 0xFF; o[i++] = 0x4C; o[i++] = 0x00;
        o[i++] = 0x07;                                /* Continuity type: Proximity Pairing */
        o[i++] = (uint8_t)(size - i - 1);             /* continuity size = 0x19 */
        o[i++] = 0x07;                                /* prefix: new device */
        o[i++] = (model >> 8) & 0xFF;
        o[i++] = (model >> 0) & 0xFF;
        o[i++] = 0x55;                                /* status */
        o[i++] = (uint8_t)(((esp_random() % 10) << 4) + (esp_random() % 10));   /* bud battery */
        o[i++] = (uint8_t)(((esp_random() % 8)  << 4) + (esp_random() % 10));   /* charge/case battery */
        o[i++] = (uint8_t)(esp_random() % 256);                                 /* lid-open counter */
        o[i++] = 0x00;                                /* device color (white) */
        o[i++] = 0x00;
        esp_fill_random(&o[i], 16); i += 16;          /* encrypted payload (randomized) */
        s_rt_variant_lbl = k_apple_models[k].lbl;
    } else if (idx < (base += RT_FASTPAIR_N)) {       /* Google Fast Pair (Android) */
        int k = idx - (base - RT_FASTPAIR_N);
        uint32_t model = k_fastpair[k].model;
        o[i++] = 0x03; o[i++] = 0x03; o[i++] = 0x2C; o[i++] = 0xFE;   /* svc UUID list: 0xFE2C */
        o[i++] = 0x06; o[i++] = 0x16; o[i++] = 0x2C; o[i++] = 0xFE;   /* svc data, 0xFE2C      */
        o[i++] = (model >> 16) & 0xFF; o[i++] = (model >> 8) & 0xFF; o[i++] = (model >> 0) & 0xFF;
        o[i++] = 0x02; o[i++] = 0x0A;                                 /* Tx power level AD     */
        o[i++] = (uint8_t)((esp_random() % 120) - 100);              /* -100..+19 dBm         */
        s_rt_variant_lbl = k_fastpair[k].lbl;
    } else if (idx < (base += RT_SAM_BUDS_N)) {       /* Samsung EasySetup — Buds (28 bytes) */
        int k = idx - (base - RT_SAM_BUDS_N);
        uint32_t model = k_sam_buds[k].model;
        static const uint8_t head[] = { 0x1B, 0xFF, 0x75, 0x00, 0x42, 0x09, 0x81, 0x02,
                                        0x14, 0x15, 0x03, 0x21, 0x01, 0x09 };
        memcpy(o, head, sizeof head); i = sizeof head;
        o[i++] = (model >> 16) & 0xFF; o[i++] = (model >> 8) & 0xFF; o[i++] = 0x01;
        o[i++] = (model >> 0) & 0xFF;
        static const uint8_t tail[] = { 0x06, 0x3C, 0x94, 0x8E, 0x00, 0x00, 0x00, 0x00, 0xC7, 0x00 };
        memcpy(&o[i], tail, sizeof tail); i += sizeof tail;
        s_rt_variant_lbl = k_sam_buds[k].lbl;
    } else if (idx < (base += RT_SAM_WATCH_N)) {      /* Samsung EasySetup — Watch (15 bytes) */
        int k = idx - (base - RT_SAM_WATCH_N);
        static const uint8_t head[] = { 0x0E, 0xFF, 0x75, 0x00, 0x01, 0x00, 0x02, 0x00,
                                        0x01, 0x01, 0xFF, 0x00, 0x00, 0x43 };
        memcpy(o, head, sizeof head); i = sizeof head;
        o[i++] = k_sam_watch[k].model;
        s_rt_variant_lbl = k_sam_watch[k].lbl;
    } else {                                          /* Microsoft Swift Pair (Windows) */
        static const uint8_t head[] = { 0x0B, 0xFF, 0x06, 0x00, 0x03, 0x00, 0x80,
                                        'N', 'B', '-', 'S', 'P' };
        memcpy(o, head, sizeof head); i = sizeof head;
        s_rt_variant_lbl = "popup: Swift Pair (Win)";
    }
    return i;
}

static void ancs_adv_start(void);       /* fwd (defined below) */

/* Reconfigure the adv set once: stop, (optionally) fresh random address, set raw data, start */
static void rt_apply(const uint8_t *data, int len, bool random_addr, bool connectable)
{
    ble_gap_adv_stop();                                  /* EALREADY is fine */
    uint8_t own = 0;                                     /* public identity by default */
    if (random_addr) {
        ble_addr_t a;
        if (ble_hs_id_gen_rnd(1, &a) == 0 && ble_hs_id_set_rnd(a.val) == 0) {
            own = BLE_OWN_ADDR_RANDOM;                   /* each cycle looks like a new advertiser */
        }
    }
    if (ble_gap_adv_set_data(data, len) != 0) {
        return;                                          /* a rejected payload just skips this tick */
    }
    struct ble_gap_adv_params p = {0};
    p.conn_mode = connectable ? BLE_GAP_CONN_MODE_UND : BLE_GAP_CONN_MODE_NON;
    p.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rt_interval(&p);
    /* Connect-to-watch: route a CONNECT through the unified companion/HID handler so the device becomes a tracked, bonded link. */
    ble_gap_adv_start(own, NULL, BLE_HS_FOREVER, &p,
                      connectable ? ancs_gap_event : gap_adv_event_cb, NULL);
}

static void rt_stop_engine(void);   /* fwd */

/* Periodic tick (esp_timer task): auto-stop at the deadline, else emit the next variant */
static void rt_tick(void *arg)
{
    (void)arg;
    if (!s_rt_active) {
        return;
    }
    /* Connect-to-watch: a device tapped Connect and linked to the watch (ancs_gap_event set s_anc_conn) */
    if (s_rt_connect && s_anc_conn != BLE_HS_CONN_HANDLE_NONE) {
        rt_stop_engine();
        return;
    }
    if (esp_timer_get_time() >= s_rt_deadline_us) {
        rt_stop_engine();
        return;
    }
    uint8_t payload[31];
    int len = rt_build_popup(payload, s_rt_variant);
    /* Connect-to-watch ON: advertise the same device pop-ups as CONNECTABLE at a stable address, so tapping Connect links that device. */
    rt_apply(payload, len, !s_rt_connect /*fresh random MAC only when non-connectable*/,
             s_rt_connect /*connectable*/);
    s_rt_variant++;
    s_rt_emitted++;
}

static void rt_start_engine(void)
{
    if (s_rt_active) {
        return;
    }
    if (s_rt_timer == NULL) {
        const esp_timer_create_args_t ta = { .callback = rt_tick, .name = "ble_rt" };
        if (esp_timer_create(&ta, &s_rt_timer) != ESP_OK) {
            ESP_LOGE(TAG, "restest: timer create failed");
            return;
        }
    }
    s_rt_emitted     = 0;
    s_rt_variant     = 0;
    s_rt_variant_lbl = "starting";
    s_rt_deadline_us = esp_timer_get_time() + (int64_t)s_rt_dur_s * 1000000;
    s_rt_active      = true;
    rt_tick(NULL);                                       /* emit the first variant immediately */
    esp_timer_start_periodic(s_rt_timer, (uint64_t)rt_cadence_ms() * 1000);
    ESP_LOGW(TAG, "restest START connect=%d intensity=%d dur=%ds cadence=%ums", (int)s_rt_connect,
             (int)s_rt_intensity, s_rt_dur_s, (unsigned)rt_cadence_ms());
}

static void rt_stop_engine(void)
{
    if (s_rt_timer) {
        esp_timer_stop(s_rt_timer);
    }
    s_rt_active      = false;
    s_rt_variant_lbl = "";
    adv_stop();                                          /* drop the test advert */
    ESP_LOGW(TAG, "restest STOP (emitted=%u)", (unsigned)s_rt_emitted);
    if (s_rt_resume_phone) {
        s_rt_resume_phone = false;
        post(CMD_PHONE_CONNECT);                         /* re-arm the phone advert (master on) */
    }
}

/* ==== Phone Notifications: ANCS notification client (host task) ===================== *
 * Once the phone connects and bonds, the watch discovers ANCS, subscribes to the Notification
 * Source (the 8-byte "a notification changed" stream) and Data Source, and for each new
 * notification asks the Control Point for the app id, title, and message. Everything below runs
 * on the NimBLE host task. */
static void ancs_adv_start(void);      /* fwd: called again after a disconnect */

static const uint8_t ANCS_SUB[2]   = { 0x01, 0x00 }; /* CCCD value: notifications enabled (LE)  */
static const uint8_t ANCS_UNSUB[2] = { 0x00, 0x00 }; /* CCCD value: notifications disabled       */

/* Copies an ANCS text attribute into dst, mapping non-ASCII / control bytes so our fonts stay safe. */
static void anc_copy_txt(char *dst, size_t dstsz, const char *src, int srclen)
{
    size_t k = 0;
    for (int i = 0; i < srclen && k < dstsz - 1; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c >= 0x20 && c < 0x7F)                 dst[k++] = (char)c;
        else if (c == '\n' || c == '\r' || c == '\t') dst[k++] = ' ';
        else                                       dst[k++] = '?';
    }
    dst[k] = '\0';
}

/* Inserts (newest-first) or refreshes a notification by UID. Runs on the host task. */
static void anc_notif_upsert(uint32_t uid, uint8_t category)
{
    portENTER_CRITICAL(&s_anc_mux);
    int idx = -1;
    for (int i = 0; i < s_anc_cnt; i++) {
        if (s_anc[i].used && s_anc[i].uid == uid) { idx = i; break; }
    }
    if (idx < 0) {
        int last = (s_anc_cnt < ANCS_MAX_NOTIF) ? s_anc_cnt : ANCS_MAX_NOTIF - 1;
        for (int i = last; i > 0; i--) s_anc[i] = s_anc[i - 1];   /* shift down; drop the oldest */
        memset(&s_anc[0], 0, sizeof s_anc[0]);
        s_anc[0].used = true;
        s_anc[0].uid  = uid;
        if (s_anc_cnt < ANCS_MAX_NOTIF) s_anc_cnt++;
        idx = 0;
    }
    s_anc[idx].category = category;
    s_anc[idx].rx_us    = esp_timer_get_time();
    portEXIT_CRITICAL(&s_anc_mux);
    s_anc_gen++;
}

static void anc_notif_remove(uint32_t uid)
{
    portENTER_CRITICAL(&s_anc_mux);
    for (int i = 0; i < s_anc_cnt; i++) {
        if (s_anc[i].used && s_anc[i].uid == uid) {
            for (int j = i; j < s_anc_cnt - 1; j++) s_anc[j] = s_anc[j + 1];
            memset(&s_anc[s_anc_cnt - 1], 0, sizeof s_anc[0]);
            s_anc_cnt--;
            break;
        }
    }
    portEXIT_CRITICAL(&s_anc_mux);
    s_anc_gen++;
}

static void anc_notif_set_attrs(uint32_t uid, const char *app, const char *title, const char *msg)
{
    portENTER_CRITICAL(&s_anc_mux);
    for (int i = 0; i < s_anc_cnt; i++) {
        if (s_anc[i].used && s_anc[i].uid == uid) {
            strncpy(s_anc[i].app,     app,   sizeof s_anc[i].app - 1);
            strncpy(s_anc[i].title,   title, sizeof s_anc[i].title - 1);
            strncpy(s_anc[i].message, msg,   sizeof s_anc[i].message - 1);
            s_anc[i].have_attrs = true;
            break;
        }
    }
    portEXIT_CRITICAL(&s_anc_mux);
    s_anc_gen++;
}

/* Asks the Control Point for a notification's App Identifier, Title, and Message. */
static void anc_request_attrs(uint32_t uid)
{
    if (s_anc_cp_val == 0 || s_anc_conn == BLE_HS_CONN_HANDLE_NONE) {
        return;
    }
    uint8_t cmd[12];
    int n = 0;
    cmd[n++] = 0x00;                                          /* CommandID: Get Notification Attributes */
    cmd[n++] = (uint8_t)uid;         cmd[n++] = (uint8_t)(uid >> 8);
    cmd[n++] = (uint8_t)(uid >> 16); cmd[n++] = (uint8_t)(uid >> 24);
    cmd[n++] = 0x00;                                          /* AttrID App Identifier (no length)     */
    cmd[n++] = 0x01; cmd[n++] = 0x20; cmd[n++] = 0x00;        /* AttrID Title,   max len 32 (LE)       */
    cmd[n++] = 0x03; cmd[n++] = 0x40; cmd[n++] = 0x00;        /* AttrID Message, max len 64 (LE)       */
    s_anc_ds_len = 0;                                         /* start a fresh reassembly for this response */
    s_anc_ds_uid = uid;
    int rc = ble_gattc_write_flat(s_anc_conn, s_anc_cp_val, cmd, n, NULL, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "ancs: control-point write rc=%d", rc);
    }
}

/* Tries to parse the accumulated Data Source bytes: [CmdID:1][UID:4]{ attrID:1, len:2 LE, value }... */
static bool anc_ds_try_parse(void)
{
    const uint8_t *d = s_anc_ds_buf;
    int len = s_anc_ds_len;
    if (len < 5 || d[0] != 0x00) {
        return false;
    }
    uint32_t uid = (uint32_t)d[1] | ((uint32_t)d[2] << 8) | ((uint32_t)d[3] << 16) | ((uint32_t)d[4] << 24);
    /* Kept static (not on the stack) — this runs on the single NimBLE host task; see the note in
     * NOTIFY_RX below on why 168 bytes of locals here previously overflowed that task's stack. */
    static char app[28], title[40], msg[100];
    app[0] = title[0] = msg[0] = '\0';
    int p = 5, got = 0;
    while (p + 3 <= len) {
        uint8_t  aid  = d[p];
        uint16_t alen = (uint16_t)(d[p + 1] | (d[p + 2] << 8));
        p += 3;
        if (p + alen > len) {
            return false;                                    /* value not fully arrived yet — wait for more */
        }
        const char *v = (const char *)&d[p];
        if      (aid == 0x00) anc_copy_txt(app,   sizeof app,   v, alen);
        else if (aid == 0x01) anc_copy_txt(title, sizeof title, v, alen);
        else if (aid == 0x03) anc_copy_txt(msg,   sizeof msg,   v, alen);
        p += alen;
        if (++got >= 3) break;                               /* App + Title + Message all present */
    }
    if (got >= 3) {
        anc_notif_set_attrs(uid, app, title, msg);
        return true;
    }
    return false;
}

/* Notification Source event (8 bytes): EventID, EventFlags, CategoryID, CategoryCount, UID (4 LE). */
static void anc_ns_event(const uint8_t *d, int len)
{
    if (len < 8) {
        return;
    }
    uint8_t  event    = d[0];                                /* 0 added · 1 modified · 2 removed */
    uint8_t  category = d[2];
    uint32_t uid = (uint32_t)d[4] | ((uint32_t)d[5] << 8) | ((uint32_t)d[6] << 16) | ((uint32_t)d[7] << 24);
    if (event == 2) {
        anc_notif_remove(uid);
        return;
    }
    anc_notif_upsert(uid, category);
    anc_request_attrs(uid);
}

/* ---- ANCS discovery chain (service → characteristics → CCCDs → subscribe) ---------- */
static int anc_sub_ds_cb(uint16_t conn, const struct ble_gatt_error *err,
                         struct ble_gatt_attr *attr, void *arg)
{
    (void)conn; (void)attr; (void)arg;
    if (err && err->status != 0) ESP_LOGW(TAG, "ancs: DS subscribe status=%d", err->status);
    ESP_LOGI(TAG, "ancs: subscribed — mirroring notifications");
    s_anc_state = NOCSIF_ANCS_READY;
    ams_start_discovery();          /* ANCS's ATT work is done — now discover AMS on the same link */
    return 0;
}

static int anc_sub_ns_cb(uint16_t conn, const struct ble_gatt_error *err,
                         struct ble_gatt_attr *attr, void *arg)
{
    (void)attr; (void)arg;
    if (err && err->status != 0) ESP_LOGW(TAG, "ancs: NS subscribe status=%d", err->status);
    if (s_anc_ds_cccd) {
        ble_gattc_write_flat(conn, s_anc_ds_cccd, ANCS_SUB, sizeof ANCS_SUB, anc_sub_ds_cb, NULL);
    } else {
        s_anc_state = NOCSIF_ANCS_READY;
        ams_start_discovery();
    }
    return 0;
}

static int anc_disc_dsc_cb(uint16_t conn, const struct ble_gatt_error *error,
                           uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc, void *arg)
{
    (void)chr_val_handle; (void)arg;
    if (dsc && ble_uuid_u16(&dsc->uuid.u) == 0x2902) {       /* CCCD -> assign to the nearest preceding characteristic */
        uint16_t cccd = dsc->handle, best = 0;
        int which = 0;
        if (s_anc_ns_val && s_anc_ns_val < cccd && s_anc_ns_val > best) { best = s_anc_ns_val; which = 1; }
        if (s_anc_ds_val && s_anc_ds_val < cccd && s_anc_ds_val > best) { best = s_anc_ds_val; which = 2; }
        if      (which == 1) s_anc_ns_cccd = cccd;
        else if (which == 2) s_anc_ds_cccd = cccd;
    }
    if (error && error->status != 0) {                       /* discovery has finished */
        if (!s_notif_enabled) {                              /* notifications toggled off: link + media only */
            ESP_LOGI(TAG, "ancs: notifications disabled — skipping NS subscribe (media still discovers)");
            s_anc_state = NOCSIF_ANCS_READY;
            ams_start_discovery();
        } else if (s_anc_ns_cccd) {
            ble_gattc_write_flat(conn, s_anc_ns_cccd, ANCS_SUB, sizeof ANCS_SUB, anc_sub_ns_cb, NULL);
        } else {
            ESP_LOGW(TAG, "ancs: no Notification-Source CCCD");
            s_anc_state = NOCSIF_ANCS_FAILED;
        }
    }
    return 0;
}

static int anc_disc_chr_cb(uint16_t conn, const struct ble_gatt_error *error,
                           const struct ble_gatt_chr *chr, void *arg)
{
    (void)arg;
    if (chr) {
        if      (ble_uuid_cmp(&chr->uuid.u, &ANCS_NS_UUID.u) == 0) s_anc_ns_val = chr->val_handle;
        else if (ble_uuid_cmp(&chr->uuid.u, &ANCS_CP_UUID.u) == 0) s_anc_cp_val = chr->val_handle;
        else if (ble_uuid_cmp(&chr->uuid.u, &ANCS_DS_UUID.u) == 0) s_anc_ds_val = chr->val_handle;
    }
    if (error && error->status != 0) {
        if (s_anc_ns_val && s_anc_cp_val && s_anc_ds_val) {
            ESP_LOGI(TAG, "ancs: chars NS=%u CP=%u DS=%u — discovering CCCDs",
                     s_anc_ns_val, s_anc_cp_val, s_anc_ds_val);
            ble_gattc_disc_all_dscs(conn, s_anc_svc_start, s_anc_svc_end, anc_disc_dsc_cb, NULL);
        } else {
            ESP_LOGW(TAG, "ancs: missing chars (NS=%u CP=%u DS=%u)",
                     s_anc_ns_val, s_anc_cp_val, s_anc_ds_val);
            s_anc_state = NOCSIF_ANCS_FAILED;
        }
    }
    return 0;
}

static int anc_disc_svc_cb(uint16_t conn, const struct ble_gatt_error *error,
                           const struct ble_gatt_svc *svc, void *arg)
{
    (void)arg;
    if (svc) {
        s_anc_svc_start = svc->start_handle;
        s_anc_svc_end   = svc->end_handle;
    }
    if (error && error->status != 0) {
        if (s_anc_svc_start) {
            ESP_LOGI(TAG, "ancs: service %u-%u — discovering chars", s_anc_svc_start, s_anc_svc_end);
            ble_gattc_disc_all_chrs(conn, s_anc_svc_start, s_anc_svc_end, anc_disc_chr_cb, NULL);
        } else {
            ESP_LOGW(TAG, "ancs: ANCS service not found (phone not sharing notifications?)");
            s_anc_state = NOCSIF_ANCS_FAILED;
            ams_start_discovery();      /* still try AMS — the phone may share media but not notifications */
        }
    }
    return 0;
}

static void anc_start_discovery(void)
{
    s_anc_ns_val = s_anc_cp_val = s_anc_ds_val = 0;
    s_anc_ns_cccd = s_anc_ds_cccd = 0;
    s_anc_svc_start = s_anc_svc_end = 0;
    int rc = ble_gattc_disc_svc_by_uuid(s_anc_conn, &ANCS_SVC_UUID.u, anc_disc_svc_cb, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "ancs: disc service rc=%d", rc);
        s_anc_state = NOCSIF_ANCS_FAILED;
    }
}

/* ---- AMS (media remote) ------------------------------------------------------------- *
 * Discovered on the SAME connection as ANCS, kicked off right alongside it once encryption
 * completes. NimBLE serializes ATT traffic, so the two discovery chains coexist fine on one link.
 * Entity Update streams now-playing data; Remote Command drives playback. */

/* AMS Entity Update notification: [EntityID][AttributeID][Flags][Value UTF-8...].
 * Entities: 0=Player, 2=Track. Track attrs: 0=Artist, 2=Title. Player attr 1=PlaybackInfo
 * ("state,rate,elapsed" — state 1=Playing), 2=Volume ("0.0".."1.0"). Runs on the host task only. */
static void ams_entity_update(const uint8_t *d, int len)
{
    if (len < 3) return;
    uint8_t entity = d[0], attr = d[1];          /* d[2] = flags (bit0 truncated — ignored) */
    const char *v = (const char *)(d + 3);
    int vlen = len - 3;
    if (vlen < 0) vlen = 0;

    if (entity == 2 && attr == 0) {              /* Track Artist */
        portENTER_CRITICAL(&s_ams_mux);
        anc_copy_txt(s_ams_artist, sizeof s_ams_artist, v, vlen);
        portEXIT_CRITICAL(&s_ams_mux);
    } else if (entity == 2 && attr == 2) {       /* Track Title */
        portENTER_CRITICAL(&s_ams_mux);
        anc_copy_txt(s_ams_title, sizeof s_ams_title, v, vlen);
        portEXIT_CRITICAL(&s_ams_mux);
    } else if (entity == 0 && attr == 1) {       /* Player PlaybackInfo -> play state */
        bool playing = (vlen > 0 && v[0] == '1');
        portENTER_CRITICAL(&s_ams_mux);
        s_ams_playing = playing;
        portEXIT_CRITICAL(&s_ams_mux);
    } else if (entity == 0 && attr == 2) {       /* Player Volume ("0.0".."1.0") — parsed off the mux */
        char t[12];
        int n = vlen < (int)sizeof t - 1 ? vlen : (int)sizeof t - 1;
        for (int i = 0; i < n; i++) t[i] = v[i];
        t[n] = '\0';
        float f = atof(t);
        int pct = (f <= 0.0f) ? 0 : (f >= 1.0f) ? 100 : (int)(f * 100.0f + 0.5f);
        portENTER_CRITICAL(&s_ams_mux);
        s_ams_vol_pct = pct;
        portEXIT_CRITICAL(&s_ams_mux);
    } else {
        return;                                  /* not one we track — skip the gen bump */
    }
    s_ams_gen++;
}

/* ---- AMS discovery chain (service → chars → Entity-Update CCCD → subscribe → register) ------- */
static const uint8_t AMS_SUB[2] = { 0x01, 0x00 };   /* CCCD: enable notifications */

static int ams_reg_player_cb(uint16_t conn, const struct ble_gatt_error *err,
                             struct ble_gatt_attr *attr, void *arg)
{
    (void)conn; (void)attr; (void)arg;
    if (err && err->status != 0) ESP_LOGW(TAG, "ams: player register status=%d", err->status);
    s_ams_ready = true;
    ESP_LOGI(TAG, "ams: media remote ready (rc=%u eu=%u)", s_ams_rc_val, s_ams_eu_val);
    return 0;
}

static int ams_reg_track_cb(uint16_t conn, const struct ble_gatt_error *err,
                            struct ble_gatt_attr *attr, void *arg)
{
    (void)attr; (void)arg;
    if (err && err->status != 0) ESP_LOGW(TAG, "ams: track register status=%d", err->status);
    static const uint8_t player_cfg[] = { 0x00, 0x01, 0x02 };   /* Player: PlaybackInfo + Volume */
    ble_gattc_write_flat(conn, s_ams_eu_val, player_cfg, sizeof player_cfg, ams_reg_player_cb, NULL);
    return 0;
}

static int ams_sub_eu_cb(uint16_t conn, const struct ble_gatt_error *err,
                         struct ble_gatt_attr *attr, void *arg)
{
    (void)attr; (void)arg;
    if (err && err->status != 0) ESP_LOGW(TAG, "ams: EU subscribe status=%d", err->status);
    static const uint8_t track_cfg[] = { 0x02, 0x00, 0x02 };    /* Track: Artist + Title */
    ble_gattc_write_flat(conn, s_ams_eu_val, track_cfg, sizeof track_cfg, ams_reg_track_cb, NULL);
    return 0;
}

static int ams_disc_dsc_cb(uint16_t conn, const struct ble_gatt_error *error,
                           uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc, void *arg)
{
    (void)chr_val_handle; (void)arg;
    if (dsc && ble_uuid_u16(&dsc->uuid.u) == 0x2902) {   /* CCCD -> assign to Entity Update */
        uint16_t cccd = dsc->handle, best = 0;
        int which = 0;
        if (s_ams_rc_val && s_ams_rc_val < cccd && s_ams_rc_val > best) { best = s_ams_rc_val; which = 1; }
        if (s_ams_eu_val && s_ams_eu_val < cccd && s_ams_eu_val > best) { best = s_ams_eu_val; which = 2; }
        if (which == 2) s_ams_eu_cccd = cccd;            /* RC's own CCCD is ignored — only RC itself is written */
    }
    if (error && error->status != 0) {                   /* discovery has finished */
        if (s_ams_eu_cccd) {
            ble_gattc_write_flat(conn, s_ams_eu_cccd, AMS_SUB, sizeof AMS_SUB, ams_sub_eu_cb, NULL);
        } else {
            ESP_LOGW(TAG, "ams: no Entity-Update CCCD");
        }
    }
    return 0;
}

static int ams_disc_chr_cb(uint16_t conn, const struct ble_gatt_error *error,
                           const struct ble_gatt_chr *chr, void *arg)
{
    (void)arg;
    if (chr) {
        if      (ble_uuid_cmp(&chr->uuid.u, &AMS_RC_UUID.u) == 0) s_ams_rc_val = chr->val_handle;
        else if (ble_uuid_cmp(&chr->uuid.u, &AMS_EU_UUID.u) == 0) s_ams_eu_val = chr->val_handle;
    }
    if (error && error->status != 0) {
        if (s_ams_rc_val && s_ams_eu_val) {
            ble_gattc_disc_all_dscs(conn, s_ams_svc_start, s_ams_svc_end, ams_disc_dsc_cb, NULL);
        } else {
            ESP_LOGW(TAG, "ams: missing chars (RC=%u EU=%u)", s_ams_rc_val, s_ams_eu_val);
        }
    }
    return 0;
}

static int ams_disc_svc_cb(uint16_t conn, const struct ble_gatt_error *error,
                           const struct ble_gatt_svc *svc, void *arg)
{
    (void)arg;
    if (svc) {
        s_ams_svc_start = svc->start_handle;
        s_ams_svc_end   = svc->end_handle;
    }
    if (error && error->status != 0) {
        if (s_ams_svc_start) {
            ble_gattc_disc_all_chrs(conn, s_ams_svc_start, s_ams_svc_end, ams_disc_chr_cb, NULL);
        } else {
            ESP_LOGI(TAG, "ams: no AMS service (phone not sharing media)");
        }
    }
    return 0;
}

static void ams_start_discovery(void)
{
    if (s_ams_started || s_anc_conn == BLE_HS_CONN_HANDLE_NONE) return;
    s_ams_started = true;
    s_ams_rc_val = s_ams_eu_val = s_ams_eu_cccd = 0;
    s_ams_svc_start = s_ams_svc_end = 0;
    s_ams_ready = false;
    int rc = ble_gattc_disc_svc_by_uuid(s_anc_conn, &AMS_SVC_UUID.u, ams_disc_svc_cb, NULL);
    if (rc != 0) ESP_LOGW(TAG, "ams: disc service rc=%d", rc);
}

/* Resets AMS state when the phone link drops, so the next connection discovers it cleanly. */
static void ams_reset(void)
{
    s_ams_started = s_ams_ready = false;
    s_ams_rc_val = s_ams_eu_val = s_ams_eu_cccd = 0;
    s_ams_svc_start = s_ams_svc_end = 0;
    portENTER_CRITICAL(&s_ams_mux);
    s_ams_playing = false;
    s_ams_vol_pct = -1;
    s_ams_title[0] = s_ams_artist[0] = '\0';
    portEXIT_CRITICAL(&s_ams_mux);
    s_ams_gen++;
}

/* Sends an AMS Remote Command (worker task; NimBLE host APIs are lock-guarded, same as do_gatt_read).
 * Command ids: 0 Play · 1 Pause · 2 TogglePlayPause · 3 NextTrack · 4 PreviousTrack · 5 VolumeUp
 * · 6 VolumeDown. */
static void do_ams_cmd(int cmd)
{
    if (!s_ams_ready || s_ams_rc_val == 0 || s_anc_conn == BLE_HS_CONN_HANDLE_NONE) {
        ESP_LOGW(TAG, "ams: cmd %d ignored (ready=%d rc=%u conn=%u)",
                 cmd, s_ams_ready, s_ams_rc_val, s_anc_conn);
        return;
    }
    /* AMS Remote Command is a write-WITH-response characteristic per Apple's spec — a
     * write-without-response is silently dropped by iOS, so ble_gattc_write_flat (with response) is
     * used deliberately. */
    uint8_t c = (uint8_t)cmd;
    int rc = ble_gattc_write_flat(s_anc_conn, s_ams_rc_val, &c, 1, NULL, NULL);
    ESP_LOGI(TAG, "ams: cmd %u -> rc=%d (handle=%u)", c, rc, s_ams_rc_val);
}

/* Steps the phone volume toward a target (0..100%). AMS only offers relative steps
 * (VolumeUp/VolumeDown), and iOS media volume has ~16 discrete levels, so the requested gap is
 * translated into that many paced up/down commands. Paced with short delays so a big jump doesn't
 * burst NimBLE's GATT-procedure pool all at once. Worker task. */
static void do_ams_vol_set(int target_pct)
{
    if (!s_ams_ready || s_ams_rc_val == 0 || s_anc_conn == BLE_HS_CONN_HANDLE_NONE) {
        return;
    }
    if (target_pct < 0)   target_pct = 0;
    if (target_pct > 100) target_pct = 100;
    int cur   = (s_ams_vol_pct < 0) ? 50 : s_ams_vol_pct;
    int steps = (target_pct - cur) * 16 / 100;       /* iOS has roughly 16 discrete volume steps */
    int n     = steps < 0 ? -steps : steps;
    if (n > 16) n = 16;
    uint8_t cmd = (steps > 0) ? NOCSIF_AMS_CMD_VOL_UP : NOCSIF_AMS_CMD_VOL_DN;
    for (int i = 0; i < n; i++) {
        ble_gattc_write_flat(s_anc_conn, s_ams_rc_val, &cmd, 1, NULL, NULL);
        vTaskDelay(pdMS_TO_TICKS(25));               /* pace: roughly one write per 25 ms */
    }
    ESP_LOGI(TAG, "ams: vol %d%% -> %d%% (%d %s)", cur, target_pct, n, steps > 0 ? "up" : "down");
}

/* Peer device-name read (0x2A00) — a real name for Saved devices */
static const ble_uuid16_t GAP_DEVNAME_UUID = BLE_UUID16_INIT(0x2A00);

static int peer_name_read_cb(uint16_t conn, const struct ble_gatt_error *error,
                             struct ble_gatt_attr *attr, void *arg)
{
    (void)conn; (void)error;
    bool is_hid = (arg != NULL);
    if (attr && attr->om) {                          /* a value: copy the printable-ASCII name */
        char *dst = is_hid ? s_hidmeta_name : s_meta_name;
        char tmp[BLE_NAME_MAX] = {0};
        uint16_t copied = 0;
        ble_hs_mbuf_to_flat(attr->om, tmp, sizeof tmp - 1, &copied);
        int k = 0;
        for (int i = 0; i < (int)copied && tmp[i] && k < BLE_NAME_MAX - 1; i++)
            if ((uint8_t)tmp[i] >= 0x20 && (uint8_t)tmp[i] < 0x7F) dst[k++] = tmp[i];
        dst[k] = '\0';
        return 0;
    }
    /* terminator (attr == NULL): commit the metadata with whatever name we captured, then continue */
    if (is_hid) { s_hidmeta_pending = true; post(CMD_HIDBOND_META); }
    else        { s_meta_pending   = true; post(CMD_PHONE_META); anc_start_discovery(); }
    return 0;
}

/* Kick a GAP Device-Name read on `conn` */
static int peer_name_read(uint16_t conn, bool is_hid)
{
    if (conn == BLE_HS_CONN_HANDLE_NONE) return -1;
    (is_hid ? s_hidmeta_name : s_meta_name)[0] = '\0';
    return ble_gattc_read_by_uuid(conn, 1, 0xffff, &GAP_DEVNAME_UUID.u,
                                  peer_name_read_cb, is_hid ? (void *)1 : NULL);
}

/* iOS on-screen-keyboard coexistence */
static esp_timer_handle_t s_ios_kbd_timer;
static bool               s_hid_ready_edge;   /* not-ready -> ready latch: arm the nudge once per (re)connect */
static int                s_ios_kbd_tries;    /* remaining ANCS-ready polls before we give up (computer host) */

static void ios_kbd_show_cb(void *arg)
{
    (void)arg;
    if (!nocsif_ble_hid_ready()) return;                        /* link dropped before the timer fired */
    /* Only an Apple host runs ANCS, and it only reaches READY once ANCS discovery finishes */
    if (s_anc_state == NOCSIF_ANCS_READY) {
        ESP_LOGI(TAG, "hid: nudging iOS to keep its on-screen keyboard (AL Keyboard Layout)");
        nocsif_ble_hid_consumer(NOCSIF_HID_CC_KBD_LAYOUT);
        return;
    }
    /* Not confirmed Apple yet — ANCS may still be discovering */
    if (s_ios_kbd_tries-- > 0 && s_ios_kbd_timer) esp_timer_start_once(s_ios_kbd_timer, 1000000);
}

/* Call after any transition that can complete "ready" (bonded + subscribed) */
static void hid_ready_edge_check(void)
{
    bool ready = (s_hid_conn != BLE_HS_CONN_HANDLE_NONE && s_hid_subscribed && s_hid_encrypted);
    if (ready && !s_hid_ready_edge) {
        s_hid_ready_edge = true;
        if (s_ios_kbd_timer == NULL) {
            const esp_timer_create_args_t a = { .callback = ios_kbd_show_cb, .name = "ioskbd" };
            esp_timer_create(&a, &s_ios_kbd_timer);
        }
        if (s_ios_kbd_timer) {
            s_ios_kbd_tries = 5;                              /* ~1.2 s + 5 × 1 s ≈ 6 s for ANCS to go READY */
            esp_timer_stop(s_ios_kbd_timer);                 /* harmless if not running */
            esp_timer_start_once(s_ios_kbd_timer, 1200000);
        }
    } else if (!ready) {
        s_hid_ready_edge = false;
    }
}

/* ---- ANCS GAP events (our connectable advertising) --------------------------------- */
static int ancs_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            /* Unified bond: one link is both the phone companion (ANCS/AMS, GATT client) and the HID keyboard (controllers, GATT server). */
            s_anc_conn  = event->connect.conn_handle;
            s_hid_conn  = event->connect.conn_handle;
            s_anc_state = NOCSIF_ANCS_CONNECTED;
            s_hid_state = NOCSIF_HID_CONNECTED;
            s_want_ancs = true;                              /* the Resilience-Test "Connect to watch" path
                                                              * relies on this so its bond persists */
            ESP_LOGI(TAG, "ble: host connected (conn=%u) — requesting pairing", s_anc_conn);
            ble_gap_security_initiate(s_anc_conn);           /* prompt the host to pair/bond */
        } else {
            ESP_LOGW(TAG, "ble: connect failed status=%d", event->connect.status);
            /* Phantom connect-fail: never clobber a live link; only re-advertise if truly idle. */
            if (s_anc_conn == BLE_HS_CONN_HANDLE_NONE && s_want_ancs) ancs_adv_start();
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "ble: disconnected (reason=%d)", event->disconnect.reason);
        s_anc_conn = BLE_HS_CONN_HANDLE_NONE;
        s_hid_conn = BLE_HS_CONN_HANDLE_NONE;
        s_hid_subscribed = false;
        s_hid_encrypted  = false;
        s_hid_conn_valid = false;
        s_hid_ready_edge = false;    /* re-arm the iOS keyboard nudge on the next (re)connect */
        s_hid_state = s_hid_mode ? NOCSIF_HID_ADVERTISING : NOCSIF_HID_IDLE;
        s_anc_ns_val = s_anc_cp_val = s_anc_ds_val = 0;
        s_phone_conn_valid = false;                      /* no live peer — clears the Connect Phone "connected" tag */
        ams_reset();                                     /* drop media-remote state along with the link */
        if (s_want_ancs) { s_anc_state = NOCSIF_ANCS_ADVERTISING; ancs_adv_start(); }
        else             { s_anc_state = NOCSIF_ANCS_IDLE; }
        return 0;
    case BLE_GAP_EVENT_ENC_CHANGE:
        ESP_LOGI(TAG, "ble: encryption status=%d", event->enc_change.status);
        s_hid_encrypted = (event->enc_change.status == 0);
        if (s_anc_conn == BLE_HS_CONN_HANDLE_NONE) s_anc_conn = event->enc_change.conn_handle;  /* recover */
        if (s_hid_conn == BLE_HS_CONN_HANDLE_NONE) s_hid_conn = event->enc_change.conn_handle;
        if (s_hid_encrypted && s_hid_subscribed) s_hid_state = NOCSIF_HID_READY;
        hid_ready_edge_check();     /* arm the iOS on-screen-keyboard nudge if this completed "ready" */
        if (event->enc_change.status == 0) {
            /* Bond established: cache the peer's identity for the saved-phone list */
            struct ble_gap_conn_desc desc;
            if (ble_gap_conn_find(s_anc_conn, &desc) == 0) {
                memcpy(s_phone_conn_addr, desc.peer_id_addr.val, 6);
                s_phone_conn_atype = desc.peer_id_addr.type;
                s_phone_conn_valid = true;
                memcpy(s_meta_addr, desc.peer_id_addr.val, 6);
                s_meta_atype = desc.peer_id_addr.type;
            }
            /* Read the phone's GAP name first (friendly label), then start ANCS discovery from the read's completion callback. */
            if (peer_name_read(s_anc_conn, false) != 0) {
                s_meta_pending = true;
                post(CMD_PHONE_META);
                anc_start_discovery();
            }
        } else {
            s_anc_state = NOCSIF_ANCS_FAILED;
        }
        return 0;
    case BLE_GAP_EVENT_SUBSCRIBE:
        /* The host subscribed to our HID input report → controllers can send */
        if (event->subscribe.attr_handle == s_hid_input_val) {
            s_hid_subscribed = event->subscribe.cur_notify;
            if (s_hid_subscribed && s_hid_conn == BLE_HS_CONN_HANDLE_NONE)
                s_hid_conn = event->subscribe.conn_handle;
            ESP_LOGI(TAG, "hid: input-report subscribe=%d", (int)s_hid_subscribed);
            if (s_hid_subscribed && s_hid_encrypted) s_hid_state = NOCSIF_HID_READY;
            hid_ready_edge_check();   /* arm the iOS on-screen-keyboard nudge if this completed "ready" */
        }
        return 0;
    case BLE_GAP_EVENT_NOTIFY_RX: {
        uint16_t h = event->notify_rx.attr_handle;
        /* Kept static (not on the stack): this runs on the single NimBLE host task, and a 256-byte
         * stack frame here — plus the deep ble_gattc_write_flat call chain anc_request_attrs
         * triggers below — once overflowed the 4096-byte host task stack and corrupted NimBLE's own
         * ble_hs_timer callout (a crash inside npl_freertos_callout_is_active). Moving this off the
         * stack, plus a larger host stack in sdkconfig, fixed it. */
        static uint8_t buf[256];
        uint16_t copied = 0;
        ble_hs_mbuf_to_flat(event->notify_rx.om, buf, sizeof buf, &copied);
        if (h == s_anc_ns_val) {
            anc_ns_event(buf, copied);
        } else if (h == s_anc_ds_val) {
            if (s_anc_ds_len + (int)copied > ANCS_DS_BUF) s_anc_ds_len = 0;   /* overflow guard */
            memcpy(s_anc_ds_buf + s_anc_ds_len, buf, copied);
            s_anc_ds_len += copied;
            if (anc_ds_try_parse()) s_anc_ds_len = 0;
        } else if (h == s_ams_eu_val) {
            ams_entity_update(buf, copied);      /* AMS now-playing (media remote) */
        }
        return 0;
    }
    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        struct ble_gap_conn_desc desc;                       /* iOS is re-pairing — drop the stale bond */
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }
    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "ancs: MTU=%d", event->mtu.value);
        return 0;
    default:
        return 0;
    }
}

/* Advertises connectable, soliciting ANCS. The name goes in the scan response (the 128-bit
 * solicitation fills most of the 31-byte adv payload). Host- or worker-task safe (NimBLE API is
 * internally locked). */
static void ancs_adv_start(void)
{
    if (!s_synced) {
        s_want_ancs = true;                                  /* on_sync will start it */
        return;
    }
    if (s_anc_conn != BLE_HS_CONN_HANDLE_NONE) {
        return;                                              /* already linked */
    }
    uint8_t own_addr_type = 0;
    ble_hs_id_infer_auto(0, &own_addr_type);

    struct ble_gap_adv_params ap = {0};
    ap.conn_mode = BLE_GAP_CONN_MODE_UND;                    /* connectable undirected */
    ap.disc_mode = BLE_GAP_DISC_MODE_GEN;

    /* The UNIFIED KEYBOARD advert */
    static const ble_uuid16_t hid_svc_uuid = BLE_UUID16_INIT(0x1812);
    const char *nm = "NocSif";
    size_t nlen = strlen(nm);
    struct ble_hs_adv_fields adv = {0};
    adv.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    adv.appearance = 0x03C1;                                  /* GAP appearance: Keyboard */
    adv.appearance_is_present = 1;
    adv.uuids16 = (ble_uuid16_t *)&hid_svc_uuid;
    adv.num_uuids16 = 1;
    adv.uuids16_is_complete = 1;
    if (ble_gap_adv_set_fields(&adv) != 0) {
        ESP_LOGW(TAG, "ble: adv_set_fields failed (payload too big?) — advert will be empty");
    }
    struct ble_hs_adv_fields rsp = {0};
    rsp.name = (const uint8_t *)nm;
    rsp.name_len = (uint8_t)(nlen > 29 ? 29 : nlen);
    rsp.name_is_complete = (nlen <= 29);
    ble_gap_adv_rsp_set_fields(&rsp);

    int rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &ap, ancs_gap_event, NULL);
    if (rc == 0 || rc == BLE_HS_EALREADY) {
        s_anc_state = NOCSIF_ANCS_ADVERTISING;
        s_hid_state = NOCSIF_HID_ADVERTISING;
        ESP_LOGI(TAG, "ble: advertising as keyboard \"%s\" (unified bond); int-dma free=%u largest=%u",
                 nm, (unsigned)nocsif_int_dma_free(), (unsigned)nocsif_int_dma_largest());
    } else {
        ESP_LOGW(TAG, "ble: adv_start rc=%d", rc);
    }
}

/* Drop the unified link + advertising + clear the mirror */
static void ancs_teardown(void)
{
    s_want_ancs = false;
    if (s_anc_conn != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_anc_conn, BLE_ERR_REM_USER_CONN_TERM);
        s_anc_conn = BLE_HS_CONN_HANDLE_NONE;
    }
    s_hid_conn = BLE_HS_CONN_HANDLE_NONE;                 /* same link — clear the HID side */
    s_hid_subscribed = false;
    s_hid_encrypted  = false;
    s_hid_conn_valid = false;
    s_hid_state = NOCSIF_HID_IDLE;
    ble_gap_adv_stop();
    s_anc_state = NOCSIF_ANCS_IDLE;
    s_anc_ns_val = s_anc_cp_val = s_anc_ds_val = 0;
    s_anc_ds_len = 0;
    portENTER_CRITICAL(&s_anc_mux);
    s_anc_cnt = 0;
    portEXIT_CRITICAL(&s_anc_mux);
    s_anc_gen++;
}

static void on_sync(void)      /* host task: host + controller are ready */
{
    s_synced = true;
    s_available = true;
    int rc = ble_hs_util_ensure_addr(0);   /* generates an identity address if none exists yet */
    if (rc != 0) {
        ESP_LOGW(TAG, "ensure_addr rc=%d", rc);
    }
    ESP_LOGI(TAG, "NimBLE synced; int-dma free=%u largest=%u",
             (unsigned)nocsif_int_dma_free(),
             (unsigned)nocsif_int_dma_largest());
    if (s_want_scan) {
        scan_start();
    }
    if (s_want_adv) {
        adv_start_now();
    }
    if (s_want_ancs) {
        ancs_adv_start();          /* the unified keyboard/companion advert */
    }
}

static void on_reset(int reason)
{
    s_synced = false;
    s_available = false;
    s_scan_active = false;
    s_adv_active = false;
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    s_gatt_state = NOCSIF_BLE_GATT_IDLE;
    s_anc_conn = BLE_HS_CONN_HANDLE_NONE;
    s_anc_state = NOCSIF_ANCS_IDLE;
    s_hid_conn = BLE_HS_CONN_HANDLE_NONE;
    s_hid_subscribed = false;
    s_hid_encrypted = false;
    s_hid_state = NOCSIF_HID_IDLE;
    ESP_LOGW(TAG, "NimBLE reset; reason=%d", reason);
}

static void ble_host_task(void *param)
{
    (void)param;
    nimble_port_run();                 /* blocks until nimble_port_stop() is called (never, in practice) */
    nimble_port_freertos_deinit();
}

/* One-time controller bring-up on the worker. Runs ONLY at boot now (from nocsif_ble_boot_reserve,
 * before WiFi); once up, the controller is held RESIDENT for the whole session — WiFi and BLE
 * coexist on the single 2.4 GHz radio via esp_coex software coexistence
 * (CONFIG_ESP_COEX_SW_COEXIST_ENABLE). The ~31.7 KB CONTIGUOUS internal-DMA block is claimed from
 * the pristine boot pool and NEVER freed at runtime, since a released block can't be re-claimed
 * once WiFi is up (measured: WiFi-released largest 21,504 < 31,744) — which is why a runtime
 * "make room for BLE" was structurally impossible and has been removed (see
 * nocsif_ble_boot_reserve + coex.h). The gate below is the measured floor NOCSIF_RADIO_MIN_DMA_BLE:
 *    largest = 27648  -> "BLE_INIT: Malloc failed"   (fails, and the controller's int-WDT then panics)
 *    largest = 31736  -> controller up, host synced  (works)
 * so bring-up is only attempted when it will very likely succeed. From the pristine ~90 KB boot
 * pool it clears easily; if it ever refuses, the boot init order has regressed (WiFi came up first). */
static bool bring_up(void)
{
    if (s_host_up) {
        return true;
    }
    /* Gates on CONTIGUOUS internal DMA before touching the controller — a failed
     * esp_bt_controller_init trips the internal watchdog, so refuse cleanly here instead of
     * crashing. There is no runtime "make room" step: once WiFi is up the hole can't be widened, so
     * the old lean-WiFi poll and "turn WiFi off" hint are both gone. */
    size_t largest = nocsif_int_dma_largest();
    if (largest < NOCSIF_RADIO_MIN_DMA_BLE) {
        ESP_LOGE(TAG, "BLE controller refused: only %u B contiguous int-DMA (need %u) — boot reserve ran "
                      "too late? (WiFi must init AFTER nocsif_ble_boot_reserve)", (unsigned)largest,
                      (unsigned)NOCSIF_RADIO_MIN_DMA_BLE);
        return false;                       /* clean failure — no crash, no restart hint */
    }
    ESP_LOGI(TAG, "NimBLE bring-up (WiFi coexists); int-dma free (pre)=%u largest=%u",
             (unsigned)nocsif_int_dma_free(),
             (unsigned)largest);

    esp_err_t e = nimble_port_init();
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init: %s", esp_err_to_name(e));
        return false;
    }
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb  = on_sync;
    /* Bonding config (M7 ANCS): Just Works pairing (no display/keyboard), Secure Connections, and
     * the encryption + identity keys are exchanged so the bond survives and iOS can reconnect.
     * Harmless for the scan/central/broadcaster modes, since they never pair. The NVS-backed store
     * persists the bond across reboots. */
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_our_key_dist   = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    /* Bond-store overflow handler: round-robin evicts the oldest bond when the NVS store fills, so a new pairing never fails for room. */
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_store_config_init();               /* register the NVS bond store (idempotent) */
    /* GATT server: registered permanently here, in the one window between host init and host start. */
    gatt_server_register();
    nimble_port_freertos_init(ble_host_task);
    s_host_up = true;
    /* Note (M7 ANCS): the PERIPHERAL role used to leave the largest free internal-DMA block small
     * enough to starve the display flush here — fixed for good by the persistent LVGL transport
     * buffer (buf3, ui.c disp_cfg.trans_size), so the display no longer needs any per-flush
     * internal-DMA allocation. */
    ESP_LOGI(TAG, "NimBLE host task started; int-dma free (post-init)=%u largest=%u",
             (unsigned)nocsif_int_dma_free(),
             (unsigned)nocsif_int_dma_largest());
    return true;
}

/* ---- advert PCAP writer (M7-P4·3; worker task) ------------------------------------- *
 * Everything below the ring producer runs on the worker task: it owns the FILE* and the /sd lock,
 * so no FATFS I/O ever touches the NimBLE host task. */

/* Maps a legacy adv-report event type to its LL advertising-PDU type (header bits 3:0). */
static uint8_t pcap_ll_pdu_type(uint8_t evtype)
{
    switch (evtype) {
        case BLE_HCI_ADV_RPT_EVTYPE_ADV_IND:     return 0x0;   /* ADV_IND         */
        case BLE_HCI_ADV_RPT_EVTYPE_DIR_IND:     return 0x1;   /* ADV_DIRECT_IND  */
        case BLE_HCI_ADV_RPT_EVTYPE_NONCONN_IND: return 0x2;   /* ADV_NONCONN_IND */
        case BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP:    return 0x4;   /* SCAN_RSP        */
        case BLE_HCI_ADV_RPT_EVTYPE_SCAN_IND:    return 0x6;   /* ADV_SCAN_IND    */
        default:                                 return 0x0;
    }
}

/* Synthesizes one LINKTYPE_BLUETOOTH_LE_LL_WITH_PHDR (256) packet from a ring slot into o[]: a
 * 10-byte pseudo-header (RF channel, signal, reference access address, flags) followed by the
 * reconstructed LL advertising PDU (access address + header + AdvA + AdvData + CRC). Returns the
 * packet length. */
static int pcap_build_packet(const ble_pcap_slot_t *s, uint8_t *o)
{
    int n = 0;
    /* Pseudo-header (DLT 256). Channel is nominal — the report doesn't record which of 37/38/39 it hit. */
    o[n++] = 37;                                       /* RF channel (primary advertising)     */
    o[n++] = (uint8_t)s->rssi;                         /* signal power (dBm, int8)             */
    o[n++] = 0;                                        /* noise power (marked invalid)          */
    o[n++] = 0;                                        /* access-address offenses               */
    o[n++] = 0xD6; o[n++] = 0xBE; o[n++] = 0x89; o[n++] = 0x8E;   /* reference access addr 0x8E89BED6 */
    uint16_t flags = 0x0013;                           /* dewhitened | signal-valid | ref-AA-valid */
    o[n++] = (uint8_t)(flags & 0xFF); o[n++] = (uint8_t)(flags >> 8);
    /* LL packet: advertising access address (transmitted LSB-first) + header + payload + CRC. */
    o[n++] = 0xD6; o[n++] = 0xBE; o[n++] = 0x89; o[n++] = 0x8E;
    uint8_t txadd = (uint8_t)(s->addr_type & 0x01);    /* NimBLE odd address types are random    */
    o[n++] = (uint8_t)(pcap_ll_pdu_type(s->evtype) | (txadd << 6));   /* LL header byte 0        */
    o[n++] = (uint8_t)(6 + s->dlen);                   /* LL header byte 1: payload length      */
    memcpy(&o[n], s->addr, 6); n += 6;                 /* AdvA (as received, LE)                */
    if (s->dlen) { memcpy(&o[n], s->data, s->dlen); n += s->dlen; }  /* AdvData / ScanRspData    */
    o[n++] = 0; o[n++] = 0; o[n++] = 0;                /* CRC (not computed — flags mark it unchecked) */
    return n;                                          /* BLE_PCAP_PHDR + 4 + 2 + 6 + dlen + 3   */
}

/* Picks the next free capture file name. Assumes the /sd lock is already held. */
static void pcap_pick_path(char *out, size_t outlen)
{
    for (int i = 0; i < 1000; i++) {
        snprintf(out, outlen, "/sd/nocsif/ble/adv-%03d.pcap", i);
        struct stat st;
        if (stat(out, &st) != 0) {
            return;                                    /* first name that doesn't already exist */
        }
    }
    snprintf(out, outlen, "/sd/nocsif/ble/adv-999.pcap");   /* fallback: reuse the last slot */
}

/* Claims the card, creates the output directory, and writes the PCAP global header. Returns the
 * FILE* (with state set) or NULL on failure (s_pcap_state left holding the reason). Worker task only. */
static FILE *pcap_open_file(void)
{
    esp_err_t ce = nocsif_usb_gadget_claim_sd(2000);
    if (ce != ESP_OK) {
        s_pcap_state = (ce == ESP_ERR_INVALID_STATE) ? BPCAP_FILESHARE : BPCAP_NOSD;
        ESP_LOGW(TAG, "pcap: claim_sd -> %s", esp_err_to_name(ce));
        return NULL;
    }
    if (!nocsif_sdcard_lock(3000)) {
        s_pcap_state = BPCAP_ERR;
        ESP_LOGE(TAG, "pcap: /sd lock timeout");
        return NULL;
    }
    mkdir("/sd/nocsif", 0777);                          /* EEXIST is fine — ignored */
    mkdir("/sd/nocsif/ble", 0777);
    pcap_pick_path(s_pcap_path, sizeof s_pcap_path);
    FILE *f = fopen(s_pcap_path, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "pcap: fopen(%s) failed", s_pcap_path);
        nocsif_sdcard_unlock();
        s_pcap_state = BPCAP_ERR;
        s_pcap_path[0] = '\0';
        return NULL;
    }
    /* PCAP global header (little-endian): magic a1b2c3d4, v2.4, zone 0, sigfigs 0, snaplen, LINKTYPE=256. */
    uint8_t gh[24];
    uint32_t magic = 0xa1b2c3d4u;
    uint32_t snaplen = BLE_PCAP_PHDR + 4 + 2 + 6 + BLE_PCAP_SNAP + 3;
    uint32_t net = 256;                                 /* DLT_BLUETOOTH_LE_LL_WITH_PHDR */
    memcpy(gh + 0, &magic, 4);
    gh[4] = 2; gh[5] = 0; gh[6] = 4; gh[7] = 0;         /* version_major 2, version_minor 4 */
    memset(gh + 8, 0, 8);                               /* thiszone + sigfigs */
    memcpy(gh + 16, &snaplen, 4);
    memcpy(gh + 20, &net, 4);
    fwrite(gh, 1, sizeof gh, f);
    fflush(f);
    nocsif_sdcard_unlock();

    s_pcap_bytes  = sizeof gh;
    s_pcap_frames = 0;
    s_pcap_state  = BPCAP_REC;
    ESP_LOGI(TAG, "pcap: recording -> %s", s_pcap_path);
    return f;
}

/* Appends one advert record (16-byte record header + the synthesized LE-LL packet). Caller holds the lock. */
static void pcap_write_one(FILE *f, const ble_pcap_slot_t *s)
{
    uint8_t pkt[BLE_PCAP_PHDR + 4 + 2 + 6 + BLE_PCAP_SNAP + 3];
    int plen = pcap_build_packet(s, pkt);

    uint32_t ts_sec  = (uint32_t)(s->ts_us / 1000000);
    uint32_t ts_usec = (uint32_t)(s->ts_us % 1000000);
    uint32_t incl = (uint32_t)plen, orig = (uint32_t)plen;
    uint8_t rh[16];
    memcpy(rh + 0,  &ts_sec,  4);
    memcpy(rh + 4,  &ts_usec, 4);
    memcpy(rh + 8,  &incl,    4);
    memcpy(rh + 12, &orig,    4);
    fwrite(rh, 1, sizeof rh, f);
    fwrite(pkt, 1, plen, f);
    s_pcap_bytes += sizeof rh + plen;
    s_pcap_frames++;
}

/* Opens the file lazily, then drains a bounded chunk of the ring under one /sd lock. Worker task;
 * called once per loop pass while recording is armed. Skips the lock entirely when nothing is
 * pending, so an idle capture costs nothing. */
static void pcap_service(void)
{
    if (!s_pcap_want) {
        return;
    }
    if (s_pcap_f == NULL) {
        s_pcap_f = pcap_open_file();
        if (s_pcap_f == NULL) {                         /* open failed — disarm and surface the state */
            s_pcap_want = false;
            s_pcap_active = false;
            return;
        }
        s_pcap_head = s_pcap_tail = s_pcap_drop = 0;     /* fresh ring for this session */
        s_pcap_active = true;                            /* NOW the host callback starts filling the ring */
        return;
    }
    uint32_t head = s_pcap_head;                         /* sole consumer */
    uint32_t tail = __atomic_load_n(&s_pcap_tail, __ATOMIC_ACQUIRE);
    if (head == tail) {
        return;                                          /* nothing pending — leave the lock alone */
    }
    if (!nocsif_sdcard_lock(1000)) {
        return;                                          /* card busy — try again next pass */
    }
    int budget = BLE_PCAP_DRAIN;
    while (head != tail && budget-- > 0) {
        pcap_write_one(s_pcap_f, &s_pcap_ring[head & (BLE_PCAP_SLOTS - 1)]);
        head++;
    }
    fflush(s_pcap_f);
    nocsif_sdcard_unlock();
    __atomic_store_n(&s_pcap_head, head, __ATOMIC_RELEASE);
}

/* Final-drains and closes the capture file (worker task). Safe to call when nothing is open. */
static void pcap_close(void)
{
    s_pcap_active = false;                               /* the host callback stops filling immediately */
    if (s_pcap_f) {
        if (nocsif_sdcard_lock(2000)) {
            uint32_t head = s_pcap_head;
            uint32_t tail = __atomic_load_n(&s_pcap_tail, __ATOMIC_ACQUIRE);
            while (head != tail) {
                pcap_write_one(s_pcap_f, &s_pcap_ring[head & (BLE_PCAP_SLOTS - 1)]);
                head++;
            }
            __atomic_store_n(&s_pcap_head, head, __ATOMIC_RELEASE);
            fclose(s_pcap_f);
            nocsif_sdcard_unlock();
        } else {
            fclose(s_pcap_f);                            /* lock timeout — close it best-effort anyway */
        }
        s_pcap_f = NULL;
        ESP_LOGI(TAG, "pcap: closed (%u adverts, %u bytes, %u dropped)",
                 (unsigned)s_pcap_frames, (unsigned)s_pcap_bytes, (unsigned)s_pcap_drop);
    }
    if (s_pcap_state == BPCAP_REC) {
        s_pcap_state = BPCAP_OFF;
    }
}

/* nimble_teardown() was REMOVED in RAM Phase 2 (#10/C13). It used to be the last runtime NimBLE
 * host+controller teardown (nimble_port_stop/deinit), used only to swap the GATT attribute table
 * when entering/leaving HID keyboard mode. That freed the reserved ~31.7 KB controller block, which
 * can NEVER be re-claimed once WiFi is up (measured: WiFi-released largest 21,504 < the 31,744
 * gate), so it could strand Bluetooth until a reboot. The GATT server (including HID) is now
 * registered PERMANENTLY at boot (gatt_server_register) and keyboard mode is a pure advert swap
 * (do_hid_start / hid_teardown), so the controller stays resident for the whole session — the
 * governor invariant now holds for HID too. */

/* Drops the discovered devices, drones, and GATT view so the next scan starts clean. Shared by the
 * session-exit quiesce path and the full release. Does NOT touch a live GATT connection handle —
 * the caller decides whether to terminate it. */
static void ble_clear_recon_tables(void)
{
    portENTER_CRITICAL(&s_dev_mux);
    s_dev_cnt = 0;
    s_hunt_active = false;
    portEXIT_CRITICAL(&s_dev_mux);
    s_dev_gen++;
    portENTER_CRITICAL(&s_drone_mux);
    s_drone_cnt = 0;
    portEXIT_CRITICAL(&s_drone_mux);
    s_drone_gen++;
    portENTER_CRITICAL(&s_gatt_mux);
    s_svc_cnt = 0;
    s_chr_cnt = 0;
    portEXIT_CRITICAL(&s_gatt_mux);
    s_gatt_state = NOCSIF_BLE_GATT_IDLE;
    s_gatt_fail = false;
    s_gatt_gen++;
}

/* Session-exit QUIESCE (BLE<->WiFi coexistence). Leaving a BLE screen must NOT free the
 * controller: its ~30 KB CONTIGUOUS internal-DMA block was reserved at boot, before WiFi, and
 * once WiFi is up that hole can never be re-formed (fragmentation, not shortage — see
 * nocsif_ble_boot_reserve). Freeing it would strand BLE until the next reboot: WiFi fragments the
 * hole and the NOCSIF_RADIO_MIN_DMA_BLE gate then refuses every re-bring-up. So instead of tearing
 * down, this stops the screen's transient recon activity and keeps the controller RESIDENT, with
 * WiFi left on its lean profile. The persistent phone link is preserved or re-armed.
 * do_ble_release now routes here in ALL cases (the governor invariant — the controller is never
 * torn down at runtime); even a Bluetooth-master-OFF toggle keeps the block reserved
 * (bt_master_off). Runs on the worker. */
static void ble_quiesce(void)
{
    if (!s_host_up) {
        s_want_scan = false;
        s_want_adv  = false;
        return;                         /* controller isn't up (the gate refused earlier) — nothing to keep */
    }
    /* Stops transient recon activity, but leaves the controller and phone link running. */
    if (s_pcap_want || s_pcap_f) {      /* closes any advert recording first */
        s_pcap_want = false;
        pcap_close();
    }
    scan_stop();                        /* cancels discovery + clears s_want_scan */
    if (s_want_adv) {
        adv_stop();                     /* stops a recon beacon (Advertise / Beacon screen) */
    }
    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {   /* drops a transient GATT-explore link (recon) */
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        vTaskDelay(pdMS_TO_TICKS(30));
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    }
    /* Leaving keyboard mode is now just dropping the keyboard advert/link (RAM Phase 2 #10): the
     * GATT table is permanent and the controller stays resident, so there's NO NimBLE teardown and
     * re-claim here — the old "could not re-claim the controller after HID — restart to restore
     * BLE" failure path is gone. The phone advert is re-armed by the tail below. */
    if (s_hid_mode) {
        hid_teardown();
    }
    ble_clear_recon_tables();
    /* Re-asserts the persistent phone advert so a bonded phone reconnects (the master governs
     * this). Skips if a phone link is already live, or if the controller isn't synced yet
     * (ancs_adv_start then arms s_want_ancs and on_sync starts it). */
    if (s_bt_master && !nocsif_reliability_safe_mode() && s_host_up &&
        s_anc_conn == BLE_HS_CONN_HANDLE_NONE) {
        s_want_ancs = true;
        ancs_adv_start();
    }
    /* Deliberately NO nimble_teardown and NO nocsif_wifi_set_lean(false): the reserved block stays
     * claimed and WiFi stays lean, so re-entering any BLE screen is instant and reliable. */
    ESP_LOGI(TAG, "BLE quiesced (controller resident); int-dma largest=%u",
             (unsigned)nocsif_int_dma_largest());
}

/* Logical Bluetooth OFF (BT master toggled off). GOVERNOR INVARIANT: never a runtime teardown —
 * the ~31.7 KB controller block stays reserved so turning Bluetooth back ON is instant, never a
 * reboot (operator constraint #1). So OFF stops all activity and drops every link but KEEPS the
 * controller: stops recon + advert + the phone link, clears the tables, disarms re-advertise —
 * and deliberately does NOT call nimble_teardown. WiFi stays lean while the block is held (the
 * honest, restart-free trade-off). Runs on the worker. In safe mode, or if the boot reserve was
 * skipped, the controller isn't up: just clear intent. */
static void bt_master_off(void)
{
    if (!s_host_up) {
        s_want_ancs = s_want_scan = s_want_adv = false;
        return;
    }
    if (s_pcap_want || s_pcap_f) {      /* stops + flushes any advert recording */
        s_pcap_want = false;
        pcap_close();
    }
    scan_stop();
    adv_stop();
    ancs_teardown();                    /* drops the phone ANCS link + advertising, clears the mirror */
    ams_reset();                        /* clears media-remote state along with the link */
    hid_teardown();                     /* drops the keyboard link + advertising, if any */
    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {   /* drops a transient GATT-explore link */
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        vTaskDelay(pdMS_TO_TICKS(30));
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    }
    ble_clear_recon_tables();
    s_want_ancs = s_want_scan = s_want_adv = false;
    /* NO nimble_teardown: the reserved controller block stays claimed so ON is instant, no reboot. */
    nocsif_log_dma_free("Bluetooth OFF (logical) — controller resident");
}

/* Leaving a BLE screen. ALWAYS quiesces — the controller is never torn down at runtime (governor
 * invariant), whether the Bluetooth master is on or off. Quiesce stops the screen's transient
 * recon and, while the master is on, re-arms the persistent phone advert; it never frees the
 * reserved block. A freed block can't be re-claimed once WiFi is up (measured), which is what used
 * to strand Bluetooth until a reboot. Runs on the worker. */
static void do_ble_release(void)
{
    phone_cfg_load();
    ble_quiesce();
}

/* ---- worker command handlers ------------------------------------------------------- */
static void do_scan_on(void)
{
    if (nocsif_reliability_safe_mode()) {
        ESP_LOGW(TAG, "safe mode — scan request ignored");
        return;
    }
    if (s_hid_mode) {
        hid_teardown();         /* leaving keyboard mode — stops the keyboard advert (the GATT table stays) */
    }
    s_starting = true;          /* the UI shows "starting…" through the WiFi-release + init window */
    if (!bring_up()) {
        s_starting = false;
        return;
    }
    s_want_scan = true;
    scan_start();               /* if not yet synced this just arms s_want_scan; on_sync clears s_starting */
}

static void do_scan_off(void)
{
    scan_stop();
}

/* Arms advert recording (M7-P4·3). Makes sure the observer is scanning so there's something to
 * capture, allocates the PSRAM ring on first use, then flags the worker loop to open the file and drain. */
static void do_pcap_on(void)
{
    if (nocsif_reliability_safe_mode()) {
        ESP_LOGW(TAG, "safe mode — capture request ignored");
        return;
    }
    if (s_hid_mode) {
        hid_teardown();                         /* leaving keyboard mode — stops the keyboard advert */
    }
    if (s_pcap_want) {
        return;                                 /* already recording */
    }
    if (s_pcap_ring == NULL) {
        s_pcap_ring = heap_caps_malloc(sizeof(ble_pcap_slot_t) * BLE_PCAP_SLOTS, MALLOC_CAP_SPIRAM);
        if (s_pcap_ring == NULL) {
            ESP_LOGE(TAG, "pcap: ring alloc failed (%u B PSRAM)",
                     (unsigned)(sizeof(ble_pcap_slot_t) * BLE_PCAP_SLOTS));
            s_pcap_state = BPCAP_ERR;
            return;
        }
    }
    if (!s_host_up && !bring_up()) {            /* the radio has to be up to hear any adverts */
        s_pcap_state = BPCAP_ERR;
        return;
    }
    if (!s_scan_active) {                       /* capture is meaningless without a running scan */
        s_want_scan = true;
        scan_start();
    }
    s_pcap_frames = s_pcap_bytes = s_pcap_drop = 0;
    s_pcap_want = true;                          /* the worker loop opens the file on its next pass */
}

static void do_pcap_off(void)
{
    s_pcap_want = false;
    pcap_close();
}

/* ---- worker command handlers (advertise / beacon, M7-P3) --------------------------- */
static void do_adv_start(void)
{
    if (nocsif_reliability_safe_mode()) {
        ESP_LOGW(TAG, "safe mode — advertise request ignored");
        return;
    }
    if (s_hid_mode) {
        hid_teardown();         /* leaving keyboard mode — stops the keyboard advert (the GATT table stays) */
    }
    s_adv_starting = true;      /* the UI shows "starting…" through the WiFi-release + init window */
    if (!bring_up()) {
        s_adv_starting = false;
        return;
    }
    s_want_adv = true;
    adv_start_now();            /* if not yet synced this just arms s_want_adv; on_sync clears s_adv_starting */
}

static void do_adv_stop(void)
{
    adv_stop();
}

/* ---- worker handlers (Advertisement Resilience Test) ------------------------------- */
static void do_restest_start(void)
{
    if (nocsif_reliability_safe_mode()) {
        ESP_LOGW(TAG, "safe mode — resilience test ignored");
        return;
    }
    rt_config_load();
    if (s_hid_mode) {
        hid_teardown();                 /* the single legacy adv set is ours for the run */
    }
    if (!bring_up()) {
        return;
    }
    s_rt_resume_phone = s_bt_master;    /* re-arm the phone advert when the run ends (master on) */
    adv_stop();                         /* free the single legacy adv set (beacon, if any) */
    rt_start_engine();
}

static void do_restest_stop(void)
{
    rt_stop_engine();
}

/* ---- Phone Notifications worker handlers (M7 ANCS) --------------------------------- */
static void do_ancs_start(void)
{
    if (nocsif_reliability_safe_mode()) {
        ESP_LOGW(TAG, "safe mode — ANCS request ignored");
        return;
    }
    if (s_hid_mode) {
        hid_teardown();             /* leaving keyboard mode — stops the keyboard advert (the GATT table stays) */
    }
    if (s_want_ancs) {              /* a phone session is already active — it now PERSISTS across screens
                                     * (see the notif/media screens). Re-opening a screen must NOT
                                     * re-advertise or reset a live connection — just keep it as-is. */
        return;
    }
    if (!bring_up()) {
        s_anc_state = NOCSIF_ANCS_FAILED;
        return;
    }
    ams_reset();                    /* starts fresh media-remote state for this session (re-discovers AMS) */
    s_want_ancs = true;
    s_anc_state = NOCSIF_ANCS_ADVERTISING;
    ancs_adv_start();               /* if not yet synced, arms s_want_ancs; on_sync starts it */
}

static void do_ancs_forget(void)
{
    if (!s_host_up) {
        return;
    }
    ble_store_clear();              /* wipes every bond so the next pairing starts fresh */
    ESP_LOGI(TAG, "ancs: bonds cleared");
    if (s_anc_conn != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_anc_conn, BLE_ERR_REM_USER_CONN_TERM);
        s_anc_conn = BLE_HS_CONN_HANDLE_NONE;
    }
    portENTER_CRITICAL(&s_anc_mux);
    s_anc_cnt = 0;
    portEXIT_CRITICAL(&s_anc_mux);
    s_anc_gen++;
    if (s_want_ancs) {
        s_anc_state = NOCSIF_ANCS_ADVERTISING;
        ancs_adv_start();           /* re-advertises so the phone can pair again */
    }
}


/* ---- Phone companion hub: persisted config + saved-phone metadata (M7) -------------- *
 * A slot-based NVS table (keys "ph_s0".."ph_sN") that can be enumerated without the stack up.
 * Each slot serializes to "<seq>|<12hex identity addr>|<atype>|<name>". Addresses are stored in
 * NimBLE byte order (val[0] = LSB); the round-trip is symmetric, so the ordering itself doesn't
 * matter. */
static void phone_addr_hex(const uint8_t a[6], char out[13])
{
    static const char H[] = "0123456789abcdef";
    for (int i = 0; i < 6; i++) { out[i*2] = H[a[i] >> 4]; out[i*2+1] = H[a[i] & 0xF]; }
    out[12] = '\0';
}

static void phone_slot_fmt(const nocsif_ble_phone_t *p, char *out, size_t n)
{
    char hex[13];
    phone_addr_hex(p->addr, hex);
    snprintf(out, n, "%u|%s|%u|%s", (unsigned)p->seq, hex, (unsigned)p->addr_type, p->name);
}

static bool phone_slot_parse(const char *s, nocsif_ble_phone_t *p)
{
    memset(p, 0, sizeof *p);
    unsigned seq = 0, atype = 0;
    char hex[13] = {0};
    if (sscanf(s, "%u|%12[0-9a-fA-F]|%u|", &seq, hex, &atype) < 3 || strlen(hex) != 12) {
        return false;
    }
    p->seq = seq;
    p->addr_type = (uint8_t)atype;
    for (int i = 0; i < 6; i++) {
        char b[3] = { hex[i*2], hex[i*2+1], 0 };
        p->addr[i] = (uint8_t)strtol(b, NULL, 16);
    }
    /* the name is whatever follows the 3rd '|' (it never itself contains one) */
    const char *bar = s;
    for (int k = 0; k < 3 && bar; k++) { bar = strchr(bar, '|'); if (bar) bar++; }
    if (bar && *bar) { strncpy(p->name, bar, sizeof p->name - 1); p->name[sizeof p->name - 1] = '\0'; }
    return true;
}

static void phone_cfg_load(void)
{
    if (s_phone_cfg_loaded) {
        return;
    }
    s_phone_cfg_loaded = true;
    s_bt_master     = nocsif_settings_get_i32("bt_master", 1) != 0;
    s_notif_enabled = nocsif_settings_get_i32("ph_notif", 1) != 0;
    s_phone_seq     = (uint32_t)nocsif_settings_get_i32("ph_seq", 0);
    int n = nocsif_settings_get_i32("ph_n", 0);
    if (n < 0) n = 0;
    if (n > NOCSIF_BLE_PHONE_MAX) n = NOCSIF_BLE_PHONE_MAX;
    int cnt = 0;
    for (int k = 0; k < n; k++) {
        char key[8], val[96];
        snprintf(key, sizeof key, "ph_s%d", k);
        if (nocsif_settings_get_str(key, val, sizeof val, "") == ESP_OK && val[0]) {
            nocsif_ble_phone_t p;
            if (phone_slot_parse(val, &p)) s_phone[cnt++] = p;
        }
    }
    s_phone_cnt = cnt;
}

static void phone_slots_save(void)
{
    nocsif_settings_set_i32("ph_n", s_phone_cnt);
    nocsif_settings_set_i32("ph_seq", (int32_t)s_phone_seq);
    for (int k = 0; k < NOCSIF_BLE_PHONE_MAX; k++) {
        char key[8];
        snprintf(key, sizeof key, "ph_s%d", k);
        if (k < s_phone_cnt) {
            char val[96];
            phone_slot_fmt(&s_phone[k], val, sizeof val);
            nocsif_settings_set_str(key, val);
        } else {
            nocsif_settings_set_str(key, "");
        }
    }
}

/* Records a (re)connection to `addr`: bumps recency, sets the name (a fallback if none given),
 * adds a new slot or evicts one as needed. */
static void phone_meta_touch(const uint8_t addr[6], uint8_t atype, const char *name)
{
    phone_cfg_load();
    portENTER_CRITICAL(&s_phone_mux);
    uint32_t seq = ++s_phone_seq;
    int idx = -1;
    for (int k = 0; k < s_phone_cnt; k++) {
        if (memcmp(s_phone[k].addr, addr, 6) == 0) { idx = k; break; }
    }
    if (idx < 0) {
        if (s_phone_cnt < NOCSIF_BLE_PHONE_MAX) {
            idx = s_phone_cnt++;
        } else {                                     /* evicts the least-recently-used slot */
            idx = 0;
            for (int k = 1; k < s_phone_cnt; k++) if (s_phone[k].seq < s_phone[idx].seq) idx = k;
        }
        memset(&s_phone[idx], 0, sizeof s_phone[idx]);
        memcpy(s_phone[idx].addr, addr, 6);
        s_phone[idx].addr_type = atype;
    }
    s_phone[idx].seq = seq;
    if (name && name[0]) {
        strncpy(s_phone[idx].name, name, sizeof s_phone[idx].name - 1);
        s_phone[idx].name[sizeof s_phone[idx].name - 1] = '\0';
    } else if (s_phone[idx].name[0] == '\0') {
        snprintf(s_phone[idx].name, sizeof s_phone[idx].name, "Phone %02X:%02X", addr[1], addr[0]);
    }
    portEXIT_CRITICAL(&s_phone_mux);
    phone_slots_save();
}

static void phone_meta_remove(const uint8_t addr[6])
{
    phone_cfg_load();
    int idx = -1;
    portENTER_CRITICAL(&s_phone_mux);
    for (int k = 0; k < s_phone_cnt; k++) {
        if (memcmp(s_phone[k].addr, addr, 6) == 0) { idx = k; break; }
    }
    if (idx >= 0) {
        for (int k = idx; k < s_phone_cnt - 1; k++) s_phone[k] = s_phone[k + 1];
        s_phone_cnt--;
        memset(&s_phone[s_phone_cnt], 0, sizeof s_phone[s_phone_cnt]);
    }
    portEXIT_CRITICAL(&s_phone_mux);
    if (idx >= 0) phone_slots_save();
}

/* ---- saved HID hosts metadata (mirrors the saved-phone list; NVS keys "hb_*") -------------- */
static void hidbond_cfg_load(void)
{
    if (s_hidbond_loaded) {
        return;
    }
    s_hidbond_loaded = true;
    s_hidbond_seq = (uint32_t)nocsif_settings_get_i32("hb_seq", 0);
    int n = nocsif_settings_get_i32("hb_n", 0);
    if (n < 0) n = 0;
    if (n > NOCSIF_BLE_HIDBOND_MAX) n = NOCSIF_BLE_HIDBOND_MAX;
    int cnt = 0;
    for (int k = 0; k < n; k++) {
        char key[8], val[96];
        snprintf(key, sizeof key, "hb_s%d", k);
        if (nocsif_settings_get_str(key, val, sizeof val, "") == ESP_OK && val[0]) {
            nocsif_ble_phone_t p;
            if (phone_slot_parse(val, &p)) s_hidbond[cnt++] = p;
        }
    }
    s_hidbond_cnt = cnt;
}

static void hidbond_slots_save(void)
{
    nocsif_settings_set_i32("hb_n", s_hidbond_cnt);
    nocsif_settings_set_i32("hb_seq", (int32_t)s_hidbond_seq);
    for (int k = 0; k < NOCSIF_BLE_HIDBOND_MAX; k++) {
        char key[8];
        snprintf(key, sizeof key, "hb_s%d", k);
        if (k < s_hidbond_cnt) {
            char val[96];
            phone_slot_fmt(&s_hidbond[k], val, sizeof val);
            nocsif_settings_set_str(key, val);
        } else {
            nocsif_settings_set_str(key, "");
        }
    }
}

/* Record a HID host bond: bump recency, add/evict as needed */
static void hidbond_meta_touch(const uint8_t addr[6], uint8_t atype, const char *name)
{
    hidbond_cfg_load();
    portENTER_CRITICAL(&s_phone_mux);
    uint32_t seq = ++s_hidbond_seq;
    int idx = -1;
    for (int k = 0; k < s_hidbond_cnt; k++) {
        if (memcmp(s_hidbond[k].addr, addr, 6) == 0) { idx = k; break; }
    }
    if (idx < 0) {
        if (s_hidbond_cnt < NOCSIF_BLE_HIDBOND_MAX) {
            idx = s_hidbond_cnt++;
        } else {                                     /* evict the least-recent */
            idx = 0;
            for (int k = 1; k < s_hidbond_cnt; k++) if (s_hidbond[k].seq < s_hidbond[idx].seq) idx = k;
        }
        memset(&s_hidbond[idx], 0, sizeof s_hidbond[idx]);
        memcpy(s_hidbond[idx].addr, addr, 6);
        s_hidbond[idx].addr_type = atype;
    }
    if (name && name[0]) {
        strncpy(s_hidbond[idx].name, name, sizeof s_hidbond[idx].name - 1);
        s_hidbond[idx].name[sizeof s_hidbond[idx].name - 1] = '\0';
    } else if (s_hidbond[idx].name[0] == '\0') {
        snprintf(s_hidbond[idx].name, sizeof s_hidbond[idx].name, "Keyboard host %02X:%02X", addr[1], addr[0]);
    }
    s_hidbond[idx].seq = seq;
    portEXIT_CRITICAL(&s_phone_mux);
    hidbond_slots_save();
}

static void hidbond_meta_remove(const uint8_t addr[6])
{
    hidbond_cfg_load();
    int idx = -1;
    portENTER_CRITICAL(&s_phone_mux);
    for (int k = 0; k < s_hidbond_cnt; k++) {
        if (memcmp(s_hidbond[k].addr, addr, 6) == 0) { idx = k; break; }
    }
    if (idx >= 0) {
        for (int k = idx; k < s_hidbond_cnt - 1; k++) s_hidbond[k] = s_hidbond[k + 1];
        s_hidbond_cnt--;
        memset(&s_hidbond[s_hidbond_cnt], 0, sizeof s_hidbond[s_hidbond_cnt]);
    }
    portEXIT_CRITICAL(&s_phone_mux);
    if (idx >= 0) hidbond_slots_save();
}

/* ---- Phone companion hub worker handlers (M7) -------------------------------------- */

/* Boot-time controller reservation — the core of the governor design. Claims the controller's
 * ~31.7 KB block from the pristine boot pool and holds it RESIDENT for the whole session —
 * UNCONDITIONALLY, regardless of the persisted Bluetooth-master toggle. This is what makes
 * "Bluetooth on" an instant logical re-enable rather than a reboot (operator constraint #1): once
 * WiFi is up the block can never be re-claimed (measured 21,504 < 31,744), so it has to be taken
 * now or never. If the master is ON, this also arms the phone advert / auto-connect; if OFF, the
 * controller stays resident but idle (a logical off — no advert). Safe mode skips this whole
 * int-DMA-heavy bring-up. Worker task (bring_up blocks). */
static void do_ble_reserve(void)
{
    phone_cfg_load();
    if (nocsif_reliability_safe_mode()) {
        return;
    }
    if (!bring_up()) {
        return;                     /* the gate refused — should never happen from the pristine boot pool */
    }
    if (s_bt_master) {
        do_ancs_start();            /* master on: advertises (open re-arm) + auto-connects the last phone */
    }
    /* master off: controller stays resident and idle. Turning Bluetooth on later
     * (do_bt_enable/do_ancs_start) finds s_host_up already true, so bring_up returns instantly —
     * no gate, no reboot. */
}

static void do_phone_connect(void)
{
    phone_cfg_load();
    if (!s_bt_master) {
        s_bt_master = true;
        nocsif_settings_set_i32("bt_master", 1);
    }
    do_ancs_start();                /* arms + advertises (open re-arm); brings NimBLE up if needed */
}

static void do_phone_disconnect(void)
{
    if (s_anc_conn != BLE_HS_CONN_HANDLE_NONE) {
        /* A momentary drop: keeps s_want_ancs so the disconnect handler re-advertises and
         * reconnects once back in range. To stop for good, toggle the Bluetooth master off instead. */
        ble_gap_terminate(s_anc_conn, BLE_ERR_REM_USER_CONN_TERM);
    }
}

static void do_bt_enable(int on)
{
    if (on) {
        /* The controller was reserved at boot and is held RESIDENT for the whole session
         * (do_ble_release / bt_master_off never free it), so turning Bluetooth back ON is a
         * LOGICAL re-enable: just re-arms the phone advert on the already-up controller. Instant,
         * no gate, no bring-up race, no restart hint — operator constraint #1 (Bluetooth is always
         * available at runtime). If the controller somehow isn't up (safe mode / boot reserve
         * skipped), do_ancs_start brings it up from whatever the pool allows, and the gate refuses
         * cleanly rather than crashing. */
        do_ancs_start();
    } else {
        bt_master_off();            /* stops all activity + drops the link; KEEPS the controller resident */
    }
}

static void do_notif_set(int on)
{
    /* Applies live if already connected and the Notification-Source CCCD is already known;
     * otherwise the gate in anc_disc_dsc_cb takes effect on the next connect. */
    if (s_anc_conn != BLE_HS_CONN_HANDLE_NONE && s_anc_ns_cccd) {
        const uint8_t *v = on ? ANCS_SUB : ANCS_UNSUB;
        ble_gattc_write_flat(s_anc_conn, s_anc_ns_cccd, v, 2, NULL, NULL);
    }
}

static void do_phone_meta(void)
{
    if (!s_meta_pending) {
        return;
    }
    s_meta_pending = false;
    uint8_t addr[6], atype;
    memcpy(addr, s_meta_addr, 6);
    atype = s_meta_atype;
    phone_meta_touch(addr, atype, s_meta_name[0] ? s_meta_name : NULL);   /* real GAP name if read; else fallback */
}

static void do_phone_forget(void)
{
    if (!s_forget_pending) {
        return;
    }
    s_forget_pending = false;
    ble_addr_t tgt = s_forget_target;
    /* The metadata was already dropped optimistically by the request function; this deletes the
     * pairing keys and drops the live link if this is the connected peer. Needs the host to be up. */
    if (s_host_up) {
        ble_store_util_delete_peer(&tgt);
        if (s_anc_conn != BLE_HS_CONN_HANDLE_NONE && s_phone_conn_valid &&
            memcmp(s_phone_conn_addr, tgt.val, 6) == 0) {
            ble_gap_terminate(s_anc_conn, BLE_ERR_REM_USER_CONN_TERM);
        }
    }
    ESP_LOGI(TAG, "phone: forgot a saved phone (bond + metadata)");
}

static void do_hidbond_meta(void)
{
    if (!s_hidmeta_pending) {
        return;
    }
    s_hidmeta_pending = false;
    uint8_t addr[6], atype;
    memcpy(addr, s_hidmeta_addr, 6);
    atype = s_hidmeta_atype;
    hidbond_meta_touch(addr, atype, s_hidmeta_name[0] ? s_hidmeta_name : NULL);   /* real GAP name if read */
}

static void do_hidbond_forget(void)
{
    if (!s_hidforget_pending) {
        return;
    }
    s_hidforget_pending = false;
    ble_addr_t tgt = s_hidforget_target;
    /* Delete the pairing keys + drop the live keyboard link if this host is the one currently linked. */
    if (s_host_up) {
        ble_store_util_delete_peer(&tgt);
        if (s_hid_conn != BLE_HS_CONN_HANDLE_NONE && s_hid_conn_valid &&
            memcmp(s_hid_conn_addr, tgt.val, 6) == 0) {
            ble_gap_terminate(s_hid_conn, BLE_ERR_REM_USER_CONN_TERM);
        }
    }
    ESP_LOGI(TAG, "hid: forgot a saved keyboard host (bond + metadata)");
}

/* ============================== BLE HID keyboard (M7) ================================ *
 * The watch hosts a standard HID-over-GATT keyboard (report protocol), plus Device Information and
 * Battery services, and advertises with the keyboard appearance. A host pairs (Just Works, bonded)
 * and subscribes to the input report; the DuckyScript engine then types by notifying 8-byte
 * boot-keyboard reports on the input-report value handle. The GATT *server* is registered
 * PERMANENTLY at boot (gatt_server_register) alongside the resident controller; the input-report
 * notify path is gated on an actual subscribed and encrypted keyboard host, so a
 * registered-but-not-advertised HID service emits nothing on its own. Type only on hosts you own. */

/* Composite HID report map — three collections behind Report IDs so one HID service backs the keyboard, mouse and media control. */
static const uint8_t HID_REPORT_MAP[] = {
    /* ---- Keyboard (Report ID 1) ---- */
    0x05, 0x01,        /* Usage Page (Generic Desktop) */
    0x09, 0x06,        /* Usage (Keyboard)             */
    0xA1, 0x01,        /* Collection (Application)     */
    0x85, 0x01,        /*   Report ID (1)              */
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02, /* modifiers */
    0x95, 0x01, 0x75, 0x08, 0x81, 0x01,                                                             /* reserved  */
    0x95, 0x05, 0x75, 0x01, 0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02,                         /* LED out   */
    0x95, 0x01, 0x75, 0x03, 0x91, 0x01,                                                             /* LED pad   */
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x26, 0xE7, 0x00, 0x05, 0x07, 0x19, 0x00, 0x2A, 0xE7, 0x00, 0x81, 0x00, /* 6 keys 0..0xE7 (match iOS-proven) */
    0xC0,
    /* ---- Mouse (Report ID 2) ---- */
    0x05, 0x01,        /* Usage Page (Generic Desktop) */
    0x09, 0x02,        /* Usage (Mouse)                */
    0xA1, 0x01,        /* Collection (Application)     */
    0x85, 0x02,        /*   Report ID (2)              */
    0x09, 0x01, 0xA1, 0x00,                                                                         /* Pointer, Physical */
    0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x03, 0x81, 0x02, /* 3 buttons */
    0x75, 0x05, 0x95, 0x01, 0x81, 0x01,                                                             /* 5-bit pad */
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x03, 0x81, 0x06, /* X,Y,Wheel rel */
    0xC0,
    0xC0,
    /* ---- Consumer Control / media (Report ID 3) ---- */
    0x05, 0x0C,        /* Usage Page (Consumer)        */
    0x09, 0x01,        /* Usage (Consumer Control)     */
    0xA1, 0x01,        /* Collection (Application)     */
    0x85, 0x03,        /*   Report ID (3)              */
    0x15, 0x00, 0x26, 0xFF, 0x03,                                                                   /* logical 0..0x3FF */
    0x19, 0x00, 0x2A, 0xFF, 0x03,                                                                   /* usage 0..0x3FF   */
    0x75, 0x10, 0x95, 0x01, 0x81, 0x00,                                                             /* 16-bit array     */
    0xC0,
};

/* HID Information: bcdHID 0x0111, country 0, flags 0x01 (remote-wake capable). */
static const uint8_t HID_INFO[] = { 0x11, 0x01, 0x00, 0x01 };
/* PnP ID: vendor source 0x02 (USB IF), VID 0x303A (Espressif), PID 0x0001, version 0x0100. */
static const uint8_t HID_PNP_ID[] = { 0x02, 0x3A, 0x30, 0x01, 0x00, 0x00, 0x01 };
/* Report Reference descriptors: {report id, type} — 0x01 Input, 0x02 Output. One per report id. */
static const uint8_t HID_RPT_REF_IN[]       = { 0x01, 0x01 };   /* keyboard input  */
static const uint8_t HID_RPT_REF_OUT[]      = { 0x01, 0x02 };   /* keyboard LED out */
static const uint8_t HID_RPT_REF_MOUSE[]    = { 0x02, 0x01 };   /* mouse input     */
static const uint8_t HID_RPT_REF_CONSUMER[] = { 0x03, 0x01 };   /* consumer input  */

/* Access-callback dispatch tags (chr/dsc .arg). */
enum {
    HID_A_INFO = 1, HID_A_REPORT_MAP, HID_A_CTRL, HID_A_PROTO,
    HID_A_INPUT, HID_A_OUTPUT, HID_A_PNP, HID_A_MANUF, HID_A_BATT,
    HID_A_INPUT_MOUSE, HID_A_INPUT_CONSUMER,
};

/* Appends `len` bytes to a read response; maps a full mbuf to the ATT resource-exhausted error. */
static int hid_chr_read(struct os_mbuf *om, const void *data, uint16_t len)
{
    return os_mbuf_append(om, data, len) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

/* One access callback shared by every HID/DIS/Battery characteristic; dispatches on the per-characteristic tag in arg. */
static int hid_access_cb(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn; (void)attr;
    switch ((int)(intptr_t)arg) {
    case HID_A_REPORT_MAP: return hid_chr_read(ctxt->om, HID_REPORT_MAP, sizeof HID_REPORT_MAP);
    case HID_A_INFO:       return hid_chr_read(ctxt->om, HID_INFO, sizeof HID_INFO);
    case HID_A_PNP:        return hid_chr_read(ctxt->om, HID_PNP_ID, sizeof HID_PNP_ID);
    case HID_A_MANUF:      return hid_chr_read(ctxt->om, "NocSif", 6);
    case HID_A_CTRL:       return 0;   /* HID Control Point write (suspend/exit) — accepted and ignored */
    case HID_A_PROTO:
        if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
            return hid_chr_read(ctxt->om, &s_hid_proto, 1);
        }
        ble_hs_mbuf_to_flat(ctxt->om, &s_hid_proto, 1, NULL);
        return 0;
    case HID_A_INPUT: {                 /* GET_REPORT(input): reports keys-up; keystrokes arrive via notify */
        uint8_t zero[8] = {0};
        return hid_chr_read(ctxt->om, zero, sizeof zero);
    }
    case HID_A_INPUT_MOUSE: {           /* GET_REPORT(mouse): idle 4-byte report */
        uint8_t zero[4] = {0};
        return hid_chr_read(ctxt->om, zero, sizeof zero);
    }
    case HID_A_INPUT_CONSUMER: {        /* GET_REPORT(consumer): idle 2-byte report */
        uint8_t zero[2] = {0};
        return hid_chr_read(ctxt->om, zero, sizeof zero);
    }
    case HID_A_OUTPUT:                  /* LED output report (Caps/Num/Scroll) */
        if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
            uint8_t v = 0;
            if (ble_hs_mbuf_to_flat(ctxt->om, &v, 1, NULL) == 0) {
                s_hid_led = v;
            }
            return 0;
        }
        return hid_chr_read(ctxt->om, &s_hid_led, 1);
    case HID_A_BATT: {
        int p = nocsif_power_batt_pct();
        uint8_t pct = (p < 0) ? 100 : (uint8_t)p;
        return hid_chr_read(ctxt->om, &pct, 1);
    }
    default:
        return BLE_ATT_ERR_UNLIKELY;
    }
}

/* Report Reference descriptor read — arg points at the 2-byte {report id, type} constant. */
static int hid_dsc_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn; (void)attr;
    return hid_chr_read(ctxt->om, arg, 2);
}

/* HID keyboard + Device Information + Battery GATT server. */
static const struct ble_gatt_svc_def hid_gatt_svcs[] = {
    { /* Human Interface Device (0x1812) */
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x1812),
        .characteristics = (struct ble_gatt_chr_def[]){
            { .uuid = BLE_UUID16_DECLARE(0x2A4A), .access_cb = hid_access_cb,     /* HID Information   */
              .arg = (void *)(intptr_t)HID_A_INFO,       .flags = BLE_GATT_CHR_F_READ },
            { .uuid = BLE_UUID16_DECLARE(0x2A4B), .access_cb = hid_access_cb,     /* Report Map        */
              .arg = (void *)(intptr_t)HID_A_REPORT_MAP, .flags = BLE_GATT_CHR_F_READ },
            { .uuid = BLE_UUID16_DECLARE(0x2A4C), .access_cb = hid_access_cb,     /* HID Control Point */
              .arg = (void *)(intptr_t)HID_A_CTRL,       .flags = BLE_GATT_CHR_F_WRITE_NO_RSP },
            { .uuid = BLE_UUID16_DECLARE(0x2A4E), .access_cb = hid_access_cb,     /* Protocol Mode     */
              .arg = (void *)(intptr_t)HID_A_PROTO,      .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE_NO_RSP },
            { .uuid = BLE_UUID16_DECLARE(0x2A4D), .access_cb = hid_access_cb,     /* Report (Kbd Input)*/
              .arg = (void *)(intptr_t)HID_A_INPUT,
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_READ_ENC,
              .val_handle = &s_hid_input_val,
              .descriptors = (struct ble_gatt_dsc_def[]){
                  { .uuid = BLE_UUID16_DECLARE(0x2908), .att_flags = BLE_ATT_F_READ,
                    .access_cb = hid_dsc_access, .arg = (void *)HID_RPT_REF_IN },
                  { 0 },
              } },
            { .uuid = BLE_UUID16_DECLARE(0x2A4D), .access_cb = hid_access_cb,     /* Report (Mouse In) */
              .arg = (void *)(intptr_t)HID_A_INPUT_MOUSE,
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_READ_ENC,
              .val_handle = &s_hid_mouse_val,
              .descriptors = (struct ble_gatt_dsc_def[]){
                  { .uuid = BLE_UUID16_DECLARE(0x2908), .att_flags = BLE_ATT_F_READ,
                    .access_cb = hid_dsc_access, .arg = (void *)HID_RPT_REF_MOUSE },
                  { 0 },
              } },
            { .uuid = BLE_UUID16_DECLARE(0x2A4D), .access_cb = hid_access_cb,     /* Report (Media In) */
              .arg = (void *)(intptr_t)HID_A_INPUT_CONSUMER,
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_READ_ENC,
              .val_handle = &s_hid_consumer_val,
              .descriptors = (struct ble_gatt_dsc_def[]){
                  { .uuid = BLE_UUID16_DECLARE(0x2908), .att_flags = BLE_ATT_F_READ,
                    .access_cb = hid_dsc_access, .arg = (void *)HID_RPT_REF_CONSUMER },
                  { 0 },
              } },
            { .uuid = BLE_UUID16_DECLARE(0x2A4D), .access_cb = hid_access_cb,     /* Report (Output)   */
              .arg = (void *)(intptr_t)HID_A_OUTPUT,
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
              .descriptors = (struct ble_gatt_dsc_def[]){
                  { .uuid = BLE_UUID16_DECLARE(0x2908), .att_flags = BLE_ATT_F_READ,
                    .access_cb = hid_dsc_access, .arg = (void *)HID_RPT_REF_OUT },
                  { 0 },
              } },
            { 0 },
        },
    },
    { /* Device Information (0x180A) */
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x180A),
        .characteristics = (struct ble_gatt_chr_def[]){
            { .uuid = BLE_UUID16_DECLARE(0x2A50), .access_cb = hid_access_cb,     /* PnP ID            */
              .arg = (void *)(intptr_t)HID_A_PNP,   .flags = BLE_GATT_CHR_F_READ },
            { .uuid = BLE_UUID16_DECLARE(0x2A29), .access_cb = hid_access_cb,     /* Manufacturer Name */
              .arg = (void *)(intptr_t)HID_A_MANUF, .flags = BLE_GATT_CHR_F_READ },
            { 0 },
        },
    },
    { /* Battery (0x180F) */
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x180F),
        .characteristics = (struct ble_gatt_chr_def[]){
            { .uuid = BLE_UUID16_DECLARE(0x2A19), .access_cb = hid_access_cb,     /* Battery Level     */
              .arg = (void *)(intptr_t)HID_A_BATT, .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
              .val_handle = &s_hid_batt_val },
            { 0 },
        },
    },
    { 0 },
};

/* Registers the permanent GATT server (GAP + GATT + HID/DIS/Battery). Called ONCE from bring_up at
 * boot, in the window between host init and host start; since the controller stays resident, the
 * table is never torn down afterward. The GAP device name is kept neutral ("NocSif") — the
 * keyboard identity lives only in the keyboard advertisement (hid_adv_start sets
 * appearance=Keyboard plus the 0x1812 UUID and a "NocSif Kbd" scan response), so a phone
 * connecting for ANCS sees a neutral peripheral even though the HID service is present. */
static void gatt_server_register(void)
{
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set("NocSif");
    int rc = ble_gatts_count_cfg(hid_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "hid: gatts_count_cfg rc=%d", rc);
        return;
    }
    rc = ble_gatts_add_svcs(hid_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "hid: gatts_add_svcs rc=%d", rc);
        return;
    }
    ESP_LOGI(TAG, "gatt: permanent server registered (GAP+GATT+HID/DIS/Battery, host tables in PSRAM)");
}

/* NOTE: the keyboard no longer has its own advert or GAP handler */

/* Leaves the keyboard role — the single "stop being a keyboard" primitive (RAM Phase 2 #10). Drops
 * the keyboard link, its connectable advert, and the subscription, and clears s_hid_mode. Does NOT
 * touch the resident controller or the permanent GATT table: the HID service stays registered, it
 * is simply no longer advertised. Every mode switch and the session-exit quiesce call THIS instead
 * of a NimBLE teardown. Host-up-safe (a down host means the boot reserve was skipped — just clear
 * intent). */
static void hid_teardown(void)
{
    /* Unified bond: leaving controller mode is a pure state change — never drop the link (it is also the phone companion). */
    s_hid_mode = 0;
}

/* Enter controller mode */
static void do_hid_start(void)
{
    if (nocsif_reliability_safe_mode()) {
        ESP_LOGW(TAG, "safe mode — controllers request ignored");
        s_hid_state = NOCSIF_HID_FAILED;
        return;
    }
    if (!s_host_up) {               /* controller resident since boot; down only if the reserve was skipped */
        ESP_LOGW(TAG, "hid: controller not up (boot reserve skipped?) — cannot enter controllers");
        s_hid_state = NOCSIF_HID_FAILED;
        return;
    }
    s_hid_mode = 1;                 /* controllers active → alerts muted (ui gates the phone ingest on this) */
    phone_cfg_load();
    /* The one advert is the keyboard advert; keep it up when Bluetooth is on and nothing is linked, so an unbonded host can pair. */
    if (s_bt_master && s_anc_conn == BLE_HS_CONN_HANDLE_NONE && s_anc_state != NOCSIF_ANCS_ADVERTISING) {
        s_want_ancs = true;
        ancs_adv_start();
    }
}

/* ================================ GATT explore (M7-P2) =============================== *
 * Connects to one device as a central and walks its attribute database. The worker initiates the
 * connection and reads; NimBLE delivers results on its host task through the callbacks below,
 * which do O(1) work and publish under s_gatt_mux. Read-only exploration of a device you're
 * authorized to test — the same service/characteristic walk a phone performs when it connects. */

static void gatt_disc_chrs(int svc_i);   /* fwd: characteristic discovery is chained per service */

/* Service discovery: one callback per service, then a terminating call (status != 0, typically
 * BLE_HS_EDONE) once all services are enumerated — at which point characteristic discovery starts. */
static int gatt_disc_svc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                            const struct ble_gatt_svc *svc, void *arg)
{
    (void)conn_handle; (void)arg;
    if (error->status == 0 && svc) {
        portENTER_CRITICAL(&s_gatt_mux);
        if (s_svc_cnt < BLE_SVC_MAX) {
            g_svc_t *s = &s_svc[s_svc_cnt++];
            s->start = svc->start_handle;
            s->end   = svc->end_handle;
            s->uuid  = svc->uuid;
            s_gatt_gen++;
        }
        portEXIT_CRITICAL(&s_gatt_mux);
        return 0;
    }
    /* Services are all enumerated — walk the characteristics service-by-service. */
    if (s_svc_cnt > 0) {
        s_disc_svc_i = 0;
        gatt_disc_chrs(0);
    } else {
        s_gatt_state = NOCSIF_BLE_GATT_READY;   /* no services (unusual) — nothing left to browse */
        s_gatt_gen++;
    }
    return 0;
}

/* Characteristic discovery for one service; on its terminating status, advances to the next
 * service (or finishes). arg carries the service index this batch belongs to. */
static int gatt_disc_chr_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                            const struct ble_gatt_chr *chr, void *arg)
{
    (void)conn_handle;
    int svc_i = (int)(intptr_t)arg;
    if (error->status == 0 && chr) {
        portENTER_CRITICAL(&s_gatt_mux);
        if (s_chr_cnt < BLE_CHR_MAX) {
            g_chr_t *c = &s_chr[s_chr_cnt++];
            memset(c, 0, sizeof *c);
            c->svc        = (uint8_t)svc_i;
            c->val_handle = chr->val_handle;
            c->props      = chr->properties;
            c->uuid       = chr->uuid;
            s_gatt_gen++;
        }
        portEXIT_CRITICAL(&s_gatt_mux);
        return 0;
    }
    s_disc_svc_i++;
    if (s_disc_svc_i < s_svc_cnt) {
        gatt_disc_chrs(s_disc_svc_i);
    } else {
        s_gatt_state = NOCSIF_BLE_GATT_READY;
        s_gatt_gen++;
        ESP_LOGI(TAG, "GATT discovery complete: %d svc, %d chr", s_svc_cnt, s_chr_cnt);
    }
    return 0;
}

static void gatt_disc_chrs(int svc_i)
{
    int rc = ble_gattc_disc_all_chrs(s_conn_handle, s_svc[svc_i].start, s_svc[svc_i].end,
                                     gatt_disc_chr_cb, (void *)(intptr_t)svc_i);
    while (rc != 0) {                            /* a start failure — skip this service so discovery still ends */
        ESP_LOGW(TAG, "disc_all_chrs svc %d rc=%d", s_disc_svc_i, rc);
        s_disc_svc_i++;
        if (s_disc_svc_i >= s_svc_cnt) {
            s_gatt_state = NOCSIF_BLE_GATT_READY;
            s_gatt_gen++;
            return;
        }
        rc = ble_gattc_disc_all_chrs(s_conn_handle, s_svc[s_disc_svc_i].start,
                                     s_svc[s_disc_svc_i].end, gatt_disc_chr_cb,
                                     (void *)(intptr_t)s_disc_svc_i);
    }
}

/* Characteristic-value read result. arg = the s_chr[] index the read was issued for. */
static int gatt_read_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                        struct ble_gatt_attr *attr, void *arg)
{
    (void)conn_handle;
    int ci = (int)(intptr_t)arg;
    if (error->status == 0 && attr && attr->om) {
        uint8_t  buf[BLE_VAL_MAX];
        uint16_t out_len = 0;
        if (ble_hs_mbuf_to_flat(attr->om, buf, sizeof buf, &out_len) == 0) {
            portENTER_CRITICAL(&s_gatt_mux);
            if (ci >= 0 && ci < s_chr_cnt) {
                g_chr_t *c = &s_chr[ci];
                c->vlen = (uint8_t)(out_len > BLE_VAL_MAX ? BLE_VAL_MAX : out_len);
                memcpy(c->val, buf, c->vlen);
                c->has_val = true;
                s_gatt_gen++;
            }
            portEXIT_CRITICAL(&s_gatt_mux);
        }
    } else if (error->status != 0) {
        ESP_LOGW(TAG, "read chr %d status=%d", ci, error->status);
    }
    return 0;
}

/* All connection events (connect / disconnect) route here — the discovery scan keeps its own
 * separate callback (gap_disc_event_cb). */
static int gap_conn_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            portENTER_CRITICAL(&s_gatt_mux);
            s_svc_cnt = 0;
            s_chr_cnt = 0;
            portEXIT_CRITICAL(&s_gatt_mux);
            s_gatt_state = NOCSIF_BLE_GATT_DISCOVERING;
            s_gatt_gen++;
            ESP_LOGI(TAG, "connected (handle %u) — discovering services", (unsigned)s_conn_handle);
            int rc = ble_gattc_disc_all_svcs(s_conn_handle, gatt_disc_svc_cb, NULL);
            if (rc != 0) {
                ESP_LOGW(TAG, "disc_all_svcs rc=%d", rc);
                s_gatt_state = NOCSIF_BLE_GATT_READY;
                s_gatt_gen++;
            }
        } else {
            ESP_LOGW(TAG, "connect failed status=%d", event->connect.status);
            s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            s_gatt_fail = true;
            s_gatt_state = NOCSIF_BLE_GATT_IDLE;
            s_gatt_gen++;
            scan_start();   /* resumes device discovery so the pick list repopulates */
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnected (reason %d)", event->disconnect.reason);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        portENTER_CRITICAL(&s_gatt_mux);
        s_svc_cnt = 0;
        s_chr_cnt = 0;
        portEXIT_CRITICAL(&s_gatt_mux);
        s_gatt_state = NOCSIF_BLE_GATT_IDLE;
        s_gatt_gen++;
        scan_start();       /* back to the device-pick list */
        return 0;
    default:
        return 0;
    }
}

/* Resolves a flattened item index into (service index, characteristic index or -1 for the service
 * header). Caller must hold s_gatt_mux. Returns false once idx is past the end. */
static bool gatt_flat_resolve(int idx, int *out_s, int *out_c)
{
    int pos = 0;
    for (int s = 0; s < s_svc_cnt; s++) {
        if (pos == idx) { *out_s = s; *out_c = -1; return true; }
        pos++;
        for (int c = 0; c < s_chr_cnt; c++) {
            if (s_chr[c].svc != s) continue;
            if (pos == idx) { *out_s = s; *out_c = c; return true; }
            pos++;
        }
    }
    return false;
}

/* ---- worker command handlers (GATT) ------------------------------------------------ */
static void do_gatt_connect(int dev_idx)
{
    if (!s_host_up || !s_synced) {
        return;
    }
    uint8_t addr[6], atype = 0;
    char    label[BLE_TGT_MAX];
    bool    ok = false;
    portENTER_CRITICAL(&s_dev_mux);
    if (dev_idx >= 0 && dev_idx < s_dev_cnt && s_dev[dev_idx].used) {
        memcpy(addr, s_dev[dev_idx].addr, 6);
        atype = s_dev[dev_idx].addr_type;
        if (s_dev[dev_idx].name[0]) {
            strncpy(label, s_dev[dev_idx].name, sizeof label - 1);
            label[sizeof label - 1] = '\0';
        } else {
            snprintf(label, sizeof label, "%02x:%02x:%02x:%02x:%02x:%02x",
                     addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
        }
        ok = true;
    }
    portEXIT_CRITICAL(&s_dev_mux);
    if (!ok) {
        return;
    }

    scan_stop();                    /* discovery must be off to initiate a connection */
    s_gatt_fail = false;
    strncpy(s_target, label, sizeof s_target - 1);
    s_target[sizeof s_target - 1] = '\0';
    s_gatt_state = NOCSIF_BLE_GATT_CONNECTING;
    s_gatt_gen++;

    ble_addr_t peer = { .type = atype };
    memcpy(peer.val, addr, 6);
    uint8_t own_addr_type = 0;
    ble_hs_id_infer_auto(0, &own_addr_type);

    /* A just-cancelled discovery can leave the controller briefly busy — retry a few times. */
    int rc = BLE_HS_EBUSY;
    for (int i = 0; i < 6 && rc == BLE_HS_EBUSY; i++) {
        rc = ble_gap_connect(own_addr_type, &peer, 20000, NULL, gap_conn_event_cb, NULL);
        if (rc == BLE_HS_EBUSY) {
            vTaskDelay(pdMS_TO_TICKS(40));
        }
    }
    if (rc != 0 && rc != BLE_HS_EDONE) {
        ESP_LOGW(TAG, "ble_gap_connect rc=%d", rc);
        s_gatt_fail = true;
        s_gatt_state = NOCSIF_BLE_GATT_IDLE;
        s_gatt_gen++;
        scan_start();
    }
}

static void do_gatt_disconnect(void)
{
    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        s_gatt_state = NOCSIF_BLE_GATT_DISCONNECTING;
        s_gatt_gen++;
        int rc = ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        if (rc != 0 && rc != BLE_HS_ENOTCONN) {
            ESP_LOGW(TAG, "ble_gap_terminate rc=%d", rc);
        }
        /* the DISCONNECT event finishes the transition and resumes the scan */
    } else if (s_gatt_state == NOCSIF_BLE_GATT_CONNECTING) {
        ble_gap_conn_cancel();      /* aborts the in-flight connection attempt */
        s_gatt_state = NOCSIF_BLE_GATT_IDLE;
        s_gatt_gen++;
        scan_start();               /* back to the device-pick list */
    }
}

static void do_gatt_read(int item_idx)
{
    int      ci = -1;
    uint16_t vh = 0;
    uint8_t  props = 0;
    portENTER_CRITICAL(&s_gatt_mux);
    int si, cc;
    if (gatt_flat_resolve(item_idx, &si, &cc) && cc >= 0) {
        ci    = cc;
        vh    = s_chr[cc].val_handle;
        props = s_chr[cc].props;
    }
    portEXIT_CRITICAL(&s_gatt_mux);
    if (ci < 0 || !(props & BLE_GATT_CHR_PROP_READ)) {
        return;
    }
    int rc = ble_gattc_read(s_conn_handle, vh, gatt_read_cb, (void *)(intptr_t)ci);
    if (rc != 0) {
        ESP_LOGW(TAG, "ble_gattc_read rc=%d", rc);
    }
}

static void ble_task(void *arg)
{
    (void)arg;
    ble_cmd_t c;
    for (;;) {
        /* Blocks until a command arrives — but while recording, wakes every 100 ms to drain the
         * PCAP ring to /sd (the SD FATFS I/O runs here, never on the NimBLE host task). */
        TickType_t wait = s_pcap_want ? pdMS_TO_TICKS(100) : portMAX_DELAY;
        if (xQueueReceive(s_q, &c, wait) == pdTRUE) {
            switch (c.type) {
            case CMD_BLE_SCAN_ON:     do_scan_on();          break;
            case CMD_BLE_SCAN_OFF:    do_scan_off();         break;
            case CMD_BLE_RELEASE:     do_ble_release();      break;
            case CMD_GATT_CONNECT:    do_gatt_connect(c.arg); break;
            case CMD_GATT_DISCONNECT: do_gatt_disconnect();  break;
            case CMD_GATT_READ:       do_gatt_read(c.arg);   break;
            case CMD_ADV_START:       do_adv_start();        break;
            case CMD_ADV_STOP:        do_adv_stop();         break;
            case CMD_RESTEST_START:   do_restest_start();    break;
            case CMD_RESTEST_STOP:    do_restest_stop();     break;
            case CMD_BLE_PCAP_ON:     do_pcap_on();          break;
            case CMD_BLE_PCAP_OFF:    do_pcap_off();         break;
            case CMD_ANCS_START:      do_ancs_start();       break;
            case CMD_ANCS_FORGET:     do_ancs_forget();      break;
            case CMD_AMS_CMD:         do_ams_cmd(c.arg);     break;
            case CMD_AMS_VOL_SET:     do_ams_vol_set(c.arg); break;
            case CMD_HID_START:       do_hid_start();        break;
            case CMD_BLE_RESERVE:     do_ble_reserve();      break;
            case CMD_PHONE_CONNECT:   do_phone_connect();    break;
            case CMD_PHONE_DISCONNECT:do_phone_disconnect(); break;
            case CMD_PHONE_FORGET:    do_phone_forget();     break;
            case CMD_PHONE_META:      do_phone_meta();       break;
            case CMD_HIDBOND_META:    do_hidbond_meta();     break;
            case CMD_HIDBOND_FORGET:  do_hidbond_forget();   break;
            case CMD_BT_ENABLE:       do_bt_enable(c.arg);   break;
            case CMD_NOTIF_SET:       do_notif_set(c.arg);   break;
            }
        }
        if (s_pcap_want) {
            pcap_service();
        }
    }
}

/* ---- public API -------------------------------------------------------------------- */
static void post_arg(ble_cmd_type_t type, int arg)
{
    if (s_q == NULL) {
        return;
    }
    ble_cmd_t c = { .type = type, .arg = arg };
    xQueueSend(s_q, &c, 0);     /* non-blocking; a full queue drops the request (the UI retries) */
}
static void post(ble_cmd_type_t type) { post_arg(type, 0); }

esp_err_t nocsif_ble_init(void)
{
    adv_config_load();          /* pulls the saved advertise config in (harmless in safe mode) */
    phone_cfg_load();           /* + the phone-hub config + saved-phone metadata (LVGL task, once) */
    omit_load();                /* + the persisted omit list, before any scan can feed the table */
    if (s_dev_raw == NULL) {    /* raw-AD store in PSRAM (off the scarce internal-DMA pool) */
        s_dev_raw = heap_caps_calloc(BLE_DEV_MAX, sizeof(ble_dev_raw_t), MALLOC_CAP_SPIRAM);
        if (s_dev_raw == NULL) ESP_LOGW(TAG, "raw-AD store alloc failed — device detail shows no raw AD");
    }
    if (s_task != NULL) {
        return ESP_OK;          /* already initialized */
    }
    if (nocsif_reliability_safe_mode()) {
        ESP_LOGW(TAG, "safe mode — BLE bring-up skipped");
        return ESP_OK;          /* no task; nocsif_ble_available() stays false */
    }
    s_q = xQueueCreate(BLE_CMD_QLEN, sizeof(ble_cmd_t));
    if (s_q == NULL) {
        ESP_LOGE(TAG, "failed to create command queue");
        return ESP_ERR_NO_MEM;
    }
    /* Priority below the UI/system tasks; the host task does the reactive work, so the worker
     * mostly blocks on the queue. Not Task-WDT-subscribed (no unbounded spin). The 6144-byte stack
     * (up from 4096) gives FATFS room for the P4·3 advert PCAP, which drains to /sd on this task;
     * allocated at boot while internal RAM is plentiful, so it never competes with the tight
     * BLE-active window. */
    if (xTaskCreate(ble_task, "ble", 6144, NULL, 4, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "failed to create ble worker task");
        vQueueDelete(s_q);
        s_q = NULL;
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "ble worker ready (lazy NimBLE bring-up on first scan)");
    return ESP_OK;
}

void nocsif_ble_request_scan(bool on) { post(on ? CMD_BLE_SCAN_ON : CMD_BLE_SCAN_OFF); }
void nocsif_ble_request_release(void) { post(CMD_BLE_RELEASE); }

/* ---- advert PCAP export (M7-P4·3; RAM/volatile getters, LVGL-task-safe) ------------- */
void nocsif_ble_request_pcap(bool on) { post(on ? CMD_BLE_PCAP_ON : CMD_BLE_PCAP_OFF); }
bool     nocsif_ble_pcap_active(void)  { return s_pcap_active; }
uint32_t nocsif_ble_pcap_frames(void)  { return s_pcap_frames; }
uint32_t nocsif_ble_pcap_bytes(void)   { return s_pcap_bytes; }
uint32_t nocsif_ble_pcap_dropped(void) { return s_pcap_drop; }
const char *nocsif_ble_pcap_path(void) { return s_pcap_path; }

const char *nocsif_ble_pcap_status_str(void)
{
    switch (s_pcap_state) {
        case BPCAP_REC:       return "rec";
        case BPCAP_NOSD:      return "no card";
        case BPCAP_FILESHARE: return "file share";
        case BPCAP_ERR:       return "err";
        default:              return "off";
    }
}

/* Menu-row tag: "rec" while recording, else the last state ("off"/"no card"/…). */
const char *nocsif_ble_pcap_tag_str(void)
{
    return s_pcap_active ? "rec" : nocsif_ble_pcap_status_str();
}

/* ---- Drone Detection (M7-P4·4; snapshot getters, LVGL-task-safe) -------------------- */
static void fill_drone_out(nocsif_ble_drone_t *o, const ble_drone_t *d, int64_t now)
{
    memcpy(o->addr, d->addr, 6);
    o->rssi   = d->rssi;
    o->age_ms = (uint32_t)((now - d->last_us) / 1000);
    o->msgs   = d->msgs;
    o->has_basic = d->has_basic;
    o->id_type   = d->id_type;
    o->ua_type   = d->ua_type;
    memcpy(o->uas_id, d->uas_id, sizeof o->uas_id);
    o->has_loc = d->has_loc;
    o->lat_e7  = d->lat_e7;
    o->lon_e7  = d->lon_e7;
    o->alt_m   = d->alt_m;
    o->speed_x10 = d->speed_x10;
    o->track_deg = d->track_deg;
    o->has_operator_loc = d->has_op_loc;
    o->op_lat_e7 = d->op_lat_e7;
    o->op_lon_e7 = d->op_lon_e7;
    o->has_operator_id = d->has_op_id;
    memcpy(o->operator_id, d->operator_id, sizeof o->operator_id);
}

int      nocsif_ble_drone_count(void) { return s_drone_cnt; }
uint32_t nocsif_ble_drone_gen(void)   { return s_drone_gen; }

bool nocsif_ble_drone_get(int i, nocsif_ble_drone_t *out)
{
    if (out == NULL) {
        return false;
    }
    bool ok = false;
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_drone_mux);
    if (i >= 0 && i < s_drone_cnt && s_drone[i].used) {
        fill_drone_out(out, &s_drone[i], now);
        ok = true;
    }
    portEXIT_CRITICAL(&s_drone_mux);
    return ok;
}

const char *nocsif_ble_drone_tag_str(void)
{
    static char buf[8];
    int n = s_drone_cnt;
    if (n <= 0) {
        return "clear";
    }
    snprintf(buf, sizeof buf, "%d", n);
    return buf;
}

/* ASTM F3411 UA (unmanned-aircraft) type names. */
const char *nocsif_ble_ua_type_str(uint8_t t)
{
    switch (t) {
        case 1:  return "Aeroplane";
        case 2:  return "Multirotor";
        case 3:  return "Gyroplane";
        case 4:  return "Hybrid Lift";
        case 5:  return "Ornithopter";
        case 6:  return "Glider";
        case 7:  return "Kite";
        case 8:  return "Free Balloon";
        case 9:  return "Captive Balloon";
        case 10: return "Airship";
        case 11: return "Parachute";
        case 12: return "Rocket";
        case 13: return "Tethered";
        case 14: return "Ground Obstacle";
        default: return "Aircraft";
    }
}

/* ---- BLE HID keyboard (M7; request path + typing API, LVGL-task-safe) -------------- */
void nocsif_ble_hid_request_start(void) { post(CMD_HID_START); }

nocsif_ble_hid_state_t nocsif_ble_hid_state(void) { return (nocsif_ble_hid_state_t)s_hid_state; }

bool nocsif_ble_hid_ready(void)
{
    return s_hid_conn != BLE_HS_CONN_HANDLE_NONE && s_hid_subscribed && s_hid_encrypted;
}

/* True while a Controllers screen is open (unified bond) */
bool nocsif_ble_controllers_active(void) { return s_hid_mode != 0; }

/* Notify one 8-byte boot-keyboard report on the input-report handle */
void nocsif_ble_hid_send_report(const uint8_t report[8])
{
    if (report == NULL || s_hid_conn == BLE_HS_CONN_HANDLE_NONE || !s_hid_subscribed) {
        return;
    }
    struct os_mbuf *om = ble_hs_mbuf_from_flat(report, 8);
    if (om == NULL) {
        return;                     /* mbuf pool exhausted — this report is dropped, the run continues */
    }
    int rc = ble_gatts_notify_custom(s_hid_conn, s_hid_input_val, om);
    if (rc != 0) {
        ESP_LOGW(TAG, "hid: notify rc=%d", rc);
    }
}

/* ---- Controllers: composite-HID report notifiers (LVGL-task-safe; no-op unless a host is subscribed) --- */
static void hid_notify(uint16_t val_handle, const uint8_t *data, uint16_t len)
{
    if (val_handle == 0 || s_hid_conn == BLE_HS_CONN_HANDLE_NONE || !s_hid_subscribed) {
        return;
    }
    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
    if (om == NULL) return;                 /* mbuf pool exhausted — drop this report */
    ble_gatts_notify_custom(s_hid_conn, val_handle, om);
}

/* Keyboard tap (press + release) — for the Keynote / Numpad controllers. Modifier mask + one keycode. */
void nocsif_ble_hid_key(uint8_t modifier, uint8_t keycode)
{
    if (!nocsif_ble_hid_ready()) return;
    uint8_t press[8] = { modifier, 0, keycode, 0, 0, 0, 0, 0 };
    uint8_t rel[8]   = {0};
    hid_notify(s_hid_input_val, press, 8);
    hid_notify(s_hid_input_val, rel, 8);
}

/* Mouse report: button bitmap + relative dx/dy + wheel */
void nocsif_ble_hid_mouse(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel)
{
    if (!nocsif_ble_hid_ready()) return;
    uint8_t r[4] = { buttons, (uint8_t)dx, (uint8_t)dy, (uint8_t)wheel };
    hid_notify(s_hid_mouse_val, r, 4);
}

/* Consumer/media usage — sent as a press then release (a momentary tap): play/pause, next, vol, etc. */
void nocsif_ble_hid_consumer(uint16_t usage)
{
    if (!nocsif_ble_hid_ready()) return;
    uint8_t on[2]  = { (uint8_t)(usage & 0xFF), (uint8_t)(usage >> 8) };
    uint8_t off[2] = { 0, 0 };
    hid_notify(s_hid_consumer_val, on, 2);
    hid_notify(s_hid_consumer_val, off, 2);
}

/* Manual iOS on-screen-keyboard toggle — see ble.h. One tap sends the AL Keyboard Layout usage so iOS
 * brings its software keyboard back while the watch stays a connected HID keyboard. Gated only on a live
 * HID link (this control lives on the phone-companion screen). */
bool nocsif_ble_ios_kbd_toggle(void)
{
    if (!nocsif_ble_hid_ready()) {
        ESP_LOGW(TAG, "hid: iOS keyboard toggle ignored — no HID link");
        return false;
    }
    ESP_LOGI(TAG, "hid: manual iOS on-screen keyboard toggle (AL Keyboard Layout)");
    nocsif_ble_hid_consumer(NOCSIF_HID_CC_KBD_LAYOUT);
    return true;
}


const char *nocsif_ble_hid_status_str(void)
{
    if (nocsif_reliability_safe_mode())  return "safe mode " BLE_DOT " controllers off";
    if (s_hid_mode && !s_synced)         return "starting " BLE_DOT " releasing WiFi" BLE_ELL;
    switch (s_hid_state) {
        case NOCSIF_HID_ADVERTISING: return "advertising " BLE_DOT " pair \"NocSif Kbd\" from Bluetooth settings";
        case NOCSIF_HID_CONNECTED:   return "connected " BLE_DOT " confirm pairing on the host";
        case NOCSIF_HID_READY:       return "ready " BLE_DOT " controllers live";
        case NOCSIF_HID_FAILED:      return "failed " BLE_DOT " leave + reopen";
        default:                     return "off";
    }
}

const char *nocsif_ble_hid_tag_str(void)
{
    if (nocsif_reliability_safe_mode()) return "off";
    switch (s_hid_state) {
        case NOCSIF_HID_ADVERTISING: return "pair";
        case NOCSIF_HID_CONNECTED:   return "linking";
        case NOCSIF_HID_READY:       return "ready";
        default:                     return "type";
    }
}

/* ---- Phone companion hub (M7; request path + getters, LVGL-task-safe) --------------- */
void nocsif_ble_phone_connect(void)    { post(CMD_PHONE_CONNECT); }
void nocsif_ble_phone_disconnect(void) { post(CMD_PHONE_DISCONNECT); }

void nocsif_ble_phone_boot_autostart(void)
{
    phone_cfg_load();
    if (s_bt_master && s_phone_cnt > 0) {
        post(CMD_PHONE_CONNECT);        /* arms + advertises so the last phone reconnects on its own */
    }
}

/* ---- boot-time controller reservation (BLE<->WiFi coexistence) ------------------------ *
 * FRAGMENTATION, not shortage, is what blocks BLE once WiFi is up: the BT controller needs ~30 KB
 * in ONE run, and after WiFi has initialized the largest hole stays stuck around 27 KB no matter
 * how much total memory is free (measured: freeing 10 KB moved `free` from 28k to 38k while
 * leaving `largest` at 27648). Allocation ORDER is therefore the fix — claim the controller's
 * block at boot, while the heap is still one big unfragmented run (~86 KB free just before
 * esp_wifi_init), and let WiFi allocate around it afterwards (its lean profile fits in what remains).
 *
 * Called from main.c BEFORE nocsif_wifi_init(). Blocks briefly (up to ~4 s) so the reservation is
 * actually finished before WiFi starts grabbing memory — ordering is the entire point. Claims the
 * controller UNCONDITIONALLY (even when the Bluetooth master is OFF) so it's always resident and
 * "Bluetooth on" is an instant logical re-enable, never a reboot (operator constraint #1); OFF
 * just means the resident controller isn't advertising. Only safe mode skips it (the int-DMA-heavy
 * path a crash-streak avoids). */
void nocsif_ble_boot_reserve(void)
{
    phone_cfg_load();
    s_boot_reserve_ran = true;          /* the reserve step ran BEFORE WiFi — wifi bring_up asserts this */
    if (nocsif_reliability_safe_mode()) {
        return;
    }
    post(CMD_BLE_RESERVE);              /* claims the controller block resident (advertises iff the master is on) */
    for (int i = 0; i < 80 && !s_host_up; i++) {
        vTaskDelay(pdMS_TO_TICKS(50));  /* ~4 s ceiling; normally well under 1 s */
    }
    nocsif_log_dma_free(s_host_up ? "boot reserve: NimBLE UP (controller block claimed, held for session)"
                                  : "boot reserve: NimBLE NOT up (gate refused — check init order)");
}

/* True once nocsif_ble_boot_reserve has run (i.e. the reserve step executed before WiFi). wifi.c's
 * bring_up asserts this so a future init-order regression that starts WiFi first is caught loudly. */
bool nocsif_ble_boot_reserve_ran(void) { return s_boot_reserve_ran; }

bool nocsif_ble_bt_enabled(void) { phone_cfg_load(); return s_bt_master; }
/* Always false now: the controller is reserved at boot and held resident, so enabling Bluetooth
 * never needs a restart (operator constraint #1). Kept for API stability; the "restart the watch"
 * UX itself is gone. */
bool nocsif_ble_needs_restart(void) { return false; }
void nocsif_ble_bt_set_enabled(bool on)
{
    phone_cfg_load();
    s_bt_master = on;                   /* reflects instantly for the toggle tag */
    nocsif_settings_set_i32("bt_master", on ? 1 : 0);
    post_arg(CMD_BT_ENABLE, on ? 1 : 0);
}

bool nocsif_ble_notif_enabled(void) { phone_cfg_load(); return s_notif_enabled; }
void nocsif_ble_notif_set_enabled(bool on)
{
    phone_cfg_load();
    s_notif_enabled = on;               /* reflects instantly; the gate reads it on the next connect */
    nocsif_settings_set_i32("ph_notif", on ? 1 : 0);
    post_arg(CMD_NOTIF_SET, on ? 1 : 0);
}

int nocsif_ble_phone_count(void) { phone_cfg_load(); return s_phone_cnt; }

bool nocsif_ble_phone_get(int i, nocsif_ble_phone_t *out)
{
    if (!out) {
        return false;
    }
    phone_cfg_load();
    bool ok = false;
    portENTER_CRITICAL(&s_phone_mux);
    if (i >= 0 && i < s_phone_cnt) {
        int order[NOCSIF_BLE_PHONE_MAX];
        for (int k = 0; k < s_phone_cnt; k++) order[k] = k;
        for (int a = 0; a < s_phone_cnt - 1; a++) {          /* small n: bubble-sort by seq desc (most-recent first) */
            for (int b = 0; b < s_phone_cnt - 1 - a; b++) {
                if (s_phone[order[b]].seq < s_phone[order[b + 1]].seq) {
                    int t = order[b]; order[b] = order[b + 1]; order[b + 1] = t;
                }
            }
        }
        int sidx = order[i];
        *out = s_phone[sidx];
        out->connected = s_phone_conn_valid && memcmp(s_phone_conn_addr, s_phone[sidx].addr, 6) == 0;
        ok = true;
    }
    portEXIT_CRITICAL(&s_phone_mux);
    return ok;
}

void nocsif_ble_phone_forget(const uint8_t addr[6], uint8_t addr_type)
{
    if (!addr) {
        return;
    }
    phone_meta_remove(addr);            /* instant list refresh + persist (LVGL-task NVS, like the Connect Phone hub) */
    memcpy(s_forget_target.val, addr, 6);
    s_forget_target.type = addr_type;
    s_forget_pending = true;
    post(CMD_PHONE_FORGET);             /* worker: deletes the bond and drops the live link if it's this one */
}

/* ---- saved HID hosts (keyboards) — LVGL-task-safe getters + forget ------------------- */
int nocsif_ble_hid_saved_count(void) { hidbond_cfg_load(); return s_hidbond_cnt; }

bool nocsif_ble_hid_saved_get(int i, nocsif_ble_phone_t *out)
{
    if (!out) {
        return false;
    }
    hidbond_cfg_load();
    bool ok = false;
    portENTER_CRITICAL(&s_phone_mux);
    if (i >= 0 && i < s_hidbond_cnt) {
        int order[NOCSIF_BLE_HIDBOND_MAX];
        for (int k = 0; k < s_hidbond_cnt; k++) order[k] = k;
        for (int a = 0; a < s_hidbond_cnt - 1; a++) {        /* bubble by seq desc (most-recent first) */
            for (int b = 0; b < s_hidbond_cnt - 1 - a; b++) {
                if (s_hidbond[order[b]].seq < s_hidbond[order[b + 1]].seq) {
                    int t = order[b]; order[b] = order[b + 1]; order[b + 1] = t;
                }
            }
        }
        int sidx = order[i];
        *out = s_hidbond[sidx];
        out->connected = s_hid_conn_valid && memcmp(s_hid_conn_addr, s_hidbond[sidx].addr, 6) == 0;
        ok = true;
    }
    portEXIT_CRITICAL(&s_phone_mux);
    return ok;
}

void nocsif_ble_hid_saved_forget(const uint8_t addr[6], uint8_t addr_type)
{
    if (!addr) {
        return;
    }
    hidbond_meta_remove(addr);          /* instant list refresh + persist (LVGL-task NVS) */
    memcpy(s_hidforget_target.val, addr, 6);
    s_hidforget_target.type = addr_type;
    s_hidforget_pending = true;
    post(CMD_HIDBOND_FORGET);           /* worker: delete the bond + drop the live link if it's this one */
}

/* ---- Phone Notifications (M7 ANCS; request path + snapshot getters, LVGL-task-safe) - */
void nocsif_ble_ancs_request_start(void)  { post(CMD_ANCS_START); }
void nocsif_ble_ancs_request_forget(void) { post(CMD_ANCS_FORGET); }

nocsif_ble_ancs_state_t nocsif_ble_ancs_state(void) { return (nocsif_ble_ancs_state_t)s_anc_state; }
int      nocsif_ble_ancs_count(void) { return s_anc_cnt; }
uint32_t nocsif_ble_ancs_gen(void)   { return s_anc_gen; }

bool nocsif_ble_ancs_get(int i, nocsif_ble_ancs_notif_t *out)
{
    if (out == NULL) {
        return false;
    }
    bool ok = false;
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_anc_mux);
    if (i >= 0 && i < s_anc_cnt && s_anc[i].used) {
        out->uid        = s_anc[i].uid;
        out->category   = s_anc[i].category;
        out->age_ms     = (uint32_t)((now - s_anc[i].rx_us) / 1000);
        out->have_attrs = s_anc[i].have_attrs;
        memcpy(out->app,     s_anc[i].app,     sizeof out->app);
        memcpy(out->title,   s_anc[i].title,   sizeof out->title);
        memcpy(out->message, s_anc[i].message, sizeof out->message);
        ok = true;
    }
    portEXIT_CRITICAL(&s_anc_mux);
    return ok;
}

const char *nocsif_ble_ancs_status_str(void)
{
    switch (s_anc_state) {
        case NOCSIF_ANCS_ADVERTISING: return "advertising — connect from a BLE scanner app to pair";
        case NOCSIF_ANCS_CONNECTED:   return "connected " BLE_DOT " pairing" BLE_ELL;
        case NOCSIF_ANCS_READY:       return "connected " BLE_DOT " mirroring notifications";
        case NOCSIF_ANCS_FAILED:      return "failed " BLE_DOT " tap Forget phone + retry";
        default:                      return "off";
    }
}

/* ANCS CategoryID names (ASTM/Apple). */
const char *nocsif_ble_ancs_category_str(uint8_t cat)
{
    switch (cat) {
        case 0:  return "Other";
        case 1:  return "Incoming Call";
        case 2:  return "Missed Call";
        case 3:  return "Voicemail";
        case 4:  return "Social";
        case 5:  return "Schedule";
        case 6:  return "Email";
        case 7:  return "News";
        case 8:  return "Health";
        case 9:  return "Business";
        case 10: return "Location";
        case 11: return "Entertainment";
        default: return "Notification";
    }
}

const char *nocsif_ble_ancs_tag_str(void)
{
    static char buf[12];
    if (s_anc_state == NOCSIF_ANCS_READY) {
        int n = s_anc_cnt;
        if (n > 0) { snprintf(buf, sizeof buf, "%d", n); return buf; }
        return "ready";
    }
    if (s_anc_state == NOCSIF_ANCS_ADVERTISING) return "pair";
    if (s_anc_state == NOCSIF_ANCS_CONNECTED)   return "pairing";
    if (s_anc_state == NOCSIF_ANCS_FAILED)      return "failed";
    return "phone";
}

/* ---- AMS media remote (LVGL-task-safe) -------------------------------------------------------- */
void nocsif_ble_ams_cmd(int cmd_id)     { post_arg(CMD_AMS_CMD, cmd_id); }
void nocsif_ble_ams_set_volume(int pct) { post_arg(CMD_AMS_VOL_SET, pct); }
uint32_t nocsif_ble_ams_gen(void)       { return s_ams_gen; }

bool nocsif_ble_ams_get(nocsif_ble_ams_t *out)
{
    if (!out) return false;
    portENTER_CRITICAL(&s_ams_mux);
    out->connected  = s_ams_ready;
    out->playing    = s_ams_playing;
    out->volume_pct = s_ams_vol_pct;
    strncpy(out->title,  s_ams_title,  sizeof out->title  - 1);  out->title[sizeof out->title - 1]   = '\0';
    strncpy(out->artist, s_ams_artist, sizeof out->artist - 1);  out->artist[sizeof out->artist - 1] = '\0';
    portEXIT_CRITICAL(&s_ams_mux);
    out->gen = s_ams_gen;
    return true;
}

const char *nocsif_ble_ams_tag_str(void)
{
    if (!s_ams_ready) return "—";
    return s_ams_playing ? "playing" : "paused";
}

/* ---- published state (LVGL-task-safe) ---------------------------------------------- */
bool nocsif_ble_available(void)   { return s_available; }
bool nocsif_ble_scan_active(void) { return s_scan_active; }
bool nocsif_ble_starting(void)    { return s_starting; }
int  nocsif_ble_dev_count(void)   { return s_dev_cnt; }
uint32_t nocsif_ble_dev_gen(void) { return s_dev_gen; }

/* Maps an internal table entry (already copied out under the lock) into the public snapshot. No
 * stack access. */
static void fill_dev_out(const ble_dev_t *tmp, nocsif_ble_dev_t *out)
{
    memcpy(out->addr, tmp->addr, 6);
    out->addr_type     = tmp->addr_type;
    memcpy(out->name, tmp->name, sizeof out->name);
    out->rssi          = tmp->rssi;
    out->tx_pwr        = tmp->tx_pwr;
    out->tx_pwr_ok     = tmp->tx_pwr_ok;
    out->appearance    = tmp->appearance;
    out->appearance_ok = tmp->appearance_ok;
    out->company       = tmp->company_ok ? tmp->company : 0xFFFF;
    out->company_ok    = tmp->company_ok;
    out->uuid16        = tmp->uuid16;
    out->n_uuid        = tmp->n_uuid;
    out->connectable   = tmp->connectable;
    out->tracker       = tmp->tracker;
    out->frames        = tmp->frames;
    int64_t age = esp_timer_get_time() - tmp->last_us;
    out->age_ms = age > 0 ? (uint32_t)(age / 1000) : 0;
    /* Raw AD lives in the PSRAM parallel store (s_dev_raw), copied by dev_get which has the index; zero
     * it here so callers that don't fill it (e.g. tracker_get) report "no raw captured". */
    out->adv_len = 0;
    out->rsp_len = 0;
}

bool nocsif_ble_dev_get(int idx, nocsif_ble_dev_t *out)
{
    if (out == NULL) {
        return false;
    }
    ble_dev_t tmp;
    ble_dev_raw_t rawtmp;
    bool ok, have_raw = false;
    portENTER_CRITICAL(&s_dev_mux);
    ok = (idx >= 0 && idx < s_dev_cnt && s_dev[idx].used);
    if (ok) {
        tmp = s_dev[idx];                       /* whole-struct copy under the lock */
        if (s_dev_raw) { rawtmp = s_dev_raw[idx]; have_raw = true; }   /* raw AD from the PSRAM store */
    }
    portEXIT_CRITICAL(&s_dev_mux);
    if (!ok) {
        return false;
    }
    fill_dev_out(&tmp, out);
    if (have_raw) {
        memcpy(out->adv_data, rawtmp.adv_data, NOCSIF_BLE_ADV_MAX); out->adv_len = rawtmp.adv_len;
        memcpy(out->rsp_data, rawtmp.rsp_data, NOCSIF_BLE_ADV_MAX); out->rsp_len = rawtmp.rsp_len;
    }
    return true;
}

/* ---- omit list (persisted; addresses dropped at ingestion) ------------------------- */

/* Rebuild the persisted string from s_omit and commit it. Runs on the LVGL task (the sole writer), so
 * it reads s_omit without the spinlock. Record text: "<12-hex addr MSB-first><2-hex type> <name>\n". */
#define OMIT_BUF_SZ (NOCSIF_BLE_OMIT_MAX * 52)   /* text scratch, allocated on demand from PSRAM */
static void omit_save(void)
{
    char *buf = heap_caps_malloc(OMIT_BUF_SZ, MALLOC_CAP_SPIRAM);   /* transient; off the internal pool */
    if (buf == NULL) { ESP_LOGW(TAG, "omit_save: no scratch — list not persisted this time"); return; }
    int off = 0, n = s_omit_cnt;
    for (int i = 0; i < n && off < OMIT_BUF_SZ - 60; i++) {
        const nocsif_ble_omit_t *o = &s_omit[i];
        off += snprintf(buf + off, OMIT_BUF_SZ - off, "%02x%02x%02x%02x%02x%02x%02x %s\n",
                        o->addr[5], o->addr[4], o->addr[3], o->addr[2], o->addr[1], o->addr[0],
                        o->addr_type, o->name);
    }
    buf[off] = '\0';
    nocsif_settings_set_str("ble_omit", buf);
    heap_caps_free(buf);
}

/* Load the persisted list ONCE, before any scan (called from nocsif_ble_init on a normal task, so no
 * host-task reader can race — no lock needed). Absent / malformed key = empty list. */
static void omit_load(void)
{
    if (s_omit_loaded) {
        return;
    }
    s_omit_loaded = true;                          /* set first: a missing key just leaves it empty */
    char *buf = heap_caps_malloc(OMIT_BUF_SZ, MALLOC_CAP_SPIRAM);   /* transient; off the internal pool */
    if (buf == NULL) return;
    if (nocsif_settings_get_str("ble_omit", buf, OMIT_BUF_SZ, "") != ESP_OK || !buf[0]) {
        heap_caps_free(buf);
        return;
    }
    int cnt = 0;
    char *p = buf;
    while (*p && cnt < NOCSIF_BLE_OMIT_MAX) {
        char *nl = strchr(p, '\n');
        if (nl) *nl = '\0';
        size_t len = strlen(p);
        if (len >= 14) {                            /* 12 hex addr + 2 hex type */
            uint8_t a[6]; unsigned by, ty; bool ok = true;
            for (int i = 0; i < 6; i++) {
                if (sscanf(p + i * 2, "%2x", &by) != 1) { ok = false; break; }
                a[5 - i] = (uint8_t)by;             /* text is MSB-first; table stores LSB-first */
            }
            if (ok && sscanf(p + 12, "%2x", &ty) == 1) {
                nocsif_ble_omit_t *o = &s_omit[cnt];
                memcpy(o->addr, a, 6);
                o->addr_type = (uint8_t)ty;
                const char *nm = (len >= 16) ? p + 15 : "";   /* [14]=' ', name at [15..] */
                strncpy(o->name, nm, sizeof o->name - 1);
                o->name[sizeof o->name - 1] = '\0';
                cnt++;
            }
        }
        if (!nl) break;
        p = nl + 1;
    }
    s_omit_cnt = cnt;
    heap_caps_free(buf);
}

bool nocsif_ble_omit_contains(const uint8_t addr[6], uint8_t addr_type)
{
    if (!addr || !s_omit_loaded) {
        return false;
    }
    bool hit = false;
    portENTER_CRITICAL(&s_omit_mux);
    for (int i = 0; i < s_omit_cnt; i++) {
        if (s_omit[i].addr_type == addr_type && memcmp(s_omit[i].addr, addr, 6) == 0) { hit = true; break; }
    }
    portEXIT_CRITICAL(&s_omit_mux);
    return hit;
}

void nocsif_ble_omit_add(const uint8_t addr[6], uint8_t addr_type, const char *name)
{
    if (!addr) {
        return;
    }
    omit_load();
    bool added = false;
    portENTER_CRITICAL(&s_omit_mux);
    int found = -1;
    for (int i = 0; i < s_omit_cnt; i++) {
        if (s_omit[i].addr_type == addr_type && memcmp(s_omit[i].addr, addr, 6) == 0) { found = i; break; }
    }
    if (found < 0 && s_omit_cnt < NOCSIF_BLE_OMIT_MAX) {
        nocsif_ble_omit_t *o = &s_omit[s_omit_cnt++];
        memcpy(o->addr, addr, 6);
        o->addr_type = addr_type;
        strncpy(o->name, (name && name[0]) ? name : "", sizeof o->name - 1);
        o->name[sizeof o->name - 1] = '\0';
        added = true;
    }
    portEXIT_CRITICAL(&s_omit_mux);
    if (!added) {
        return;                                    /* already omitted (or list full) — nothing to do */
    }
    /* Drop it from the live table + hunt so it disappears at once (memset → used=0, last_us=0 → the
     * slot is the next one reused). Bumps the gen so lists rebuild without the row. */
    portENTER_CRITICAL(&s_dev_mux);
    for (int i = 0; i < s_dev_cnt; i++) {
        if (s_dev[i].used && s_dev[i].addr_type == addr_type && memcmp(s_dev[i].addr, addr, 6) == 0) {
            memset(&s_dev[i], 0, sizeof s_dev[i]);
            if (s_dev_raw) memset(&s_dev_raw[i], 0, sizeof s_dev_raw[i]);
            s_dev_gen++;
        }
    }
    if (s_hunt_active && s_hunt_addr_type == addr_type && memcmp(s_hunt_addr, addr, 6) == 0) {
        s_hunt_active = false;
    }
    portEXIT_CRITICAL(&s_dev_mux);
    omit_save();
}

void nocsif_ble_omit_remove(const uint8_t addr[6], uint8_t addr_type)
{
    if (!addr) {
        return;
    }
    omit_load();
    bool removed = false;
    portENTER_CRITICAL(&s_omit_mux);
    for (int i = 0; i < s_omit_cnt; i++) {
        if (s_omit[i].addr_type == addr_type && memcmp(s_omit[i].addr, addr, 6) == 0) {
            for (int j = i; j < s_omit_cnt - 1; j++) s_omit[j] = s_omit[j + 1];
            s_omit_cnt--;
            removed = true;
            break;
        }
    }
    portEXIT_CRITICAL(&s_omit_mux);
    if (removed) omit_save();
}

void nocsif_ble_omit_clear(void)
{
    omit_load();
    portENTER_CRITICAL(&s_omit_mux);
    s_omit_cnt = 0;
    portEXIT_CRITICAL(&s_omit_mux);
    omit_save();
}

int nocsif_ble_omit_count(void)
{
    return s_omit_loaded ? s_omit_cnt : 0;
}

bool nocsif_ble_omit_get(int i, nocsif_ble_omit_t *out)
{
    if (!out || !s_omit_loaded) {
        return false;
    }
    bool ok;
    portENTER_CRITICAL(&s_omit_mux);
    ok = (i >= 0 && i < s_omit_cnt);
    if (ok) *out = s_omit[i];
    portEXIT_CRITICAL(&s_omit_mux);
    return ok;
}

/* alert-omit set: addresses suppressed from the anti-stalk follow ALERT only */
#define BLE_AOMIT_MAX 24
static uint8_t s_aomit[BLE_AOMIT_MAX][7];   /* [0..5]=addr (LSB-first, as stored) [6]=addr_type */
static int     s_aomit_cnt;
static bool    s_aomit_loaded;

static void aomit_load(void)
{
    if (s_aomit_loaded) return;
    s_aomit_loaded = true;
    char buf[BLE_AOMIT_MAX * 15 + 1];
    if (nocsif_settings_get_str("trk_aomit", buf, sizeof buf, "") != ESP_OK) return;
    const char *p = buf;
    while (*p && s_aomit_cnt < BLE_AOMIT_MAX) {
        /* one record = 14 hex chars: addr MSB-first (12) + type (2) */
        unsigned b[7]; int ok = 1;
        for (int k = 0; k < 7 && ok; k++) {
            if (!isxdigit((int)p[0]) || !isxdigit((int)p[1])) { ok = 0; break; }
            char h[3] = { p[0], p[1], 0 };
            b[k] = (unsigned)strtoul(h, NULL, 16);
            p += 2;
        }
        if (!ok) break;
        for (int k = 0; k < 6; k++) s_aomit[s_aomit_cnt][k] = (uint8_t)b[5 - k];  /* MSB-first -> LSB store */
        s_aomit[s_aomit_cnt][6] = (uint8_t)b[6];
        s_aomit_cnt++;
        while (*p == ' ' || *p == '\n') p++;
    }
}

static void aomit_save(void)
{
    char buf[BLE_AOMIT_MAX * 15 + 1];
    int off = 0;
    for (int i = 0; i < s_aomit_cnt; i++) {
        off += snprintf(buf + off, sizeof buf - off, "%02x%02x%02x%02x%02x%02x%02x",
                        s_aomit[i][5], s_aomit[i][4], s_aomit[i][3], s_aomit[i][2],
                        s_aomit[i][1], s_aomit[i][0], s_aomit[i][6]);
    }
    buf[off] = '\0';
    nocsif_settings_set_str("trk_aomit", buf);
}

bool nocsif_ble_alert_omit_contains(const uint8_t addr[6], uint8_t addr_type)
{
    if (!addr) return false;
    aomit_load();
    for (int i = 0; i < s_aomit_cnt; i++) {
        if (s_aomit[i][6] == addr_type && memcmp(s_aomit[i], addr, 6) == 0) return true;
    }
    return false;
}

void nocsif_ble_alert_omit_add(const uint8_t addr[6], uint8_t addr_type)
{
    if (!addr) return;
    aomit_load();
    if (nocsif_ble_alert_omit_contains(addr, addr_type)) return;
    if (s_aomit_cnt >= BLE_AOMIT_MAX) return;
    memcpy(s_aomit[s_aomit_cnt], addr, 6);
    s_aomit[s_aomit_cnt][6] = addr_type;
    s_aomit_cnt++;
    aomit_save();
}

/* ---- item-tracker filtered view (M7-P4·1) ----------------------------------------- */
const char *nocsif_ble_tracker_str(uint8_t tracker)
{
    switch (tracker) {
        case NOCSIF_BLE_TRACKER_FINDMY:   return "Find My";
        case NOCSIF_BLE_TRACKER_TILE:     return "Tile";
        case NOCSIF_BLE_TRACKER_SMARTTAG: return "SmartTag";
        default:                          return "";
    }
}

int nocsif_ble_tracker_count(void)
{
    int n = 0;
    portENTER_CRITICAL(&s_dev_mux);
    for (int i = 0; i < s_dev_cnt; i++) {
        if (s_dev[i].used && s_dev[i].tracker) n++;
    }
    portEXIT_CRITICAL(&s_dev_mux);
    return n;
}

bool nocsif_ble_tracker_get(int idx, nocsif_ble_dev_t *out)
{
    if (out == NULL || idx < 0) {
        return false;
    }
    ble_dev_t tmp;
    bool ok = false;
    portENTER_CRITICAL(&s_dev_mux);
    int k = 0;
    for (int i = 0; i < s_dev_cnt; i++) {
        if (s_dev[i].used && s_dev[i].tracker) {
            if (k == idx) { tmp = s_dev[i]; ok = true; break; }
            k++;
        }
    }
    portEXIT_CRITICAL(&s_dev_mux);
    if (!ok) {
        return false;
    }
    fill_dev_out(&tmp, out);
    return true;
}

const char *nocsif_ble_tracker_tag_str(void)
{
    static char b[16];
    if (nocsif_reliability_safe_mode()) return "\xE2\x80\x94";   /* — */
    if (s_starting) return BLE_ELL;
    int n = nocsif_ble_tracker_count();
    if (n > 0) { snprintf(b, sizeof b, "%d found", n); return b; }
    if (s_scan_active) return "scan";
    return "clear";
}

/* Anti-stalk: a tracker present (same address) >= min_present_ms and still in range fires one "may be following you" alert. */
bool nocsif_ble_tracker_following(uint32_t min_present_ms, char *label, size_t len,
                                  uint8_t addr_out[6], uint8_t *atype_out)
{
    if (label == NULL || len == 0) return false;
    aomit_load();                                                          /* NVS read — before the lock */
    int64_t now = esp_timer_get_time();
    bool hit = false;
    ble_dev_t tmp = {0};
    portENTER_CRITICAL(&s_dev_mux);
    for (int i = 0; i < s_dev_cnt; i++) {
        ble_dev_t *d = &s_dev[i];
        if (!d->used || !d->tracker || d->follow_alerted) continue;
        if (nocsif_ble_alert_omit_contains(d->addr, d->addr_type)) continue; /* suppressed from alerts     */
        if ((now - d->last_us) > 60000000LL) continue;                    /* must still be in range (<60 s) */
        if ((now - d->first_us) < (int64_t)min_present_ms * 1000) continue; /* present long enough?          */
        d->follow_alerted = true;
        tmp = *d;
        hit = true;
        break;
    }
    portEXIT_CRITICAL(&s_dev_mux);
    if (!hit) return false;
    if (addr_out)  memcpy(addr_out, tmp.addr, 6);
    if (atype_out) *atype_out = tmp.addr_type;
    int mins = (int)((now - tmp.first_us) / 60000000LL);
    snprintf(label, len, "%s %s" BLE_DOT " near you %dm",
             nocsif_ble_tracker_str(tmp.tracker), tmp.name[0] ? tmp.name : "", mins);
    return true;
}

/* ---- card-skimmer detection (filtered view; LVGL-task-safe) ------------------------ */
int nocsif_ble_skimmer_count(void)
{
    int n = 0;
    portENTER_CRITICAL(&s_dev_mux);
    for (int i = 0; i < s_dev_cnt; i++) {
        if (s_dev[i].used && skim_reason(&s_dev[i], s_dev_raw ? &s_dev_raw[i] : NULL)) n++;
    }
    portEXIT_CRITICAL(&s_dev_mux);
    return n;
}

bool nocsif_ble_skimmer_get(int idx, nocsif_ble_dev_t *out)
{
    if (out == NULL || idx < 0) {
        return false;
    }
    ble_dev_t tmp;
    ble_dev_raw_t rawtmp;
    bool ok = false, have_raw = false;
    portENTER_CRITICAL(&s_dev_mux);
    int k = 0;
    for (int i = 0; i < s_dev_cnt; i++) {
        if (s_dev[i].used && skim_reason(&s_dev[i], s_dev_raw ? &s_dev_raw[i] : NULL)) {
            if (k == idx) {
                tmp = s_dev[i];
                if (s_dev_raw) { rawtmp = s_dev_raw[i]; have_raw = true; }
                ok = true;
                break;
            }
            k++;
        }
    }
    portEXIT_CRITICAL(&s_dev_mux);
    if (!ok) {
        return false;
    }
    fill_dev_out(&tmp, out);
    if (have_raw) {
        memcpy(out->adv_data, rawtmp.adv_data, NOCSIF_BLE_ADV_MAX); out->adv_len = rawtmp.adv_len;
        memcpy(out->rsp_data, rawtmp.rsp_data, NOCSIF_BLE_ADV_MAX); out->rsp_len = rawtmp.rsp_len;
    }
    return true;
}

const char *nocsif_ble_skimmer_reason(const nocsif_ble_dev_t *d)
{
    if (d == NULL) return "";
    if (skim_name_match(d->name)) return "serial module name";
    if (d->uuid16 == 0xFFE0)      return "serial svc 0xFFE0";
    if (ad_has_uuid16(d->adv_data, d->adv_len, 0xFFE0) ||
        ad_has_uuid16(d->rsp_data, d->rsp_len, 0xFFE0)) return "serial svc 0xFFE0";
    if (ad_has_nus(d->adv_data, d->adv_len) ||
        ad_has_nus(d->rsp_data, d->rsp_len))            return "Nordic UART";
    return "";
}

const char *nocsif_ble_skimmer_tag_str(void)
{
    static char b[16];
    if (nocsif_reliability_safe_mode()) return "\xE2\x80\x94";   /* — */
    if (s_starting) return BLE_ELL;
    int n = nocsif_ble_skimmer_count();
    if (n > 0) { snprintf(b, sizeof b, "%d found", n); return b; }
    if (s_scan_active) return "scan";
    return "clear";
}

/* ---- Signal Hunt (M7-P4·2) --------------------------------------------------------- */
void nocsif_ble_hunt_set_target(const uint8_t addr[6], uint8_t addr_type, const char *name)
{
    if (addr == NULL) {
        return;
    }
    portENTER_CRITICAL(&s_dev_mux);
    memcpy(s_hunt_addr, addr, 6);
    s_hunt_addr_type = addr_type;
    s_hunt_raw       = 0;
    s_hunt_ema_x100  = 0;
    s_hunt_peak      = -128;
    s_hunt_last_us   = 0;
    s_hunt_frames    = 0;
    if (name && name[0]) {
        strncpy(s_hunt_name, name, sizeof s_hunt_name - 1);
        s_hunt_name[sizeof s_hunt_name - 1] = '\0';
    } else {
        s_hunt_name[0] = '\0';
    }
    s_hunt_active = true;
    portEXIT_CRITICAL(&s_dev_mux);
}

void nocsif_ble_hunt_clear(void)
{
    portENTER_CRITICAL(&s_dev_mux);
    s_hunt_active = false;
    portEXIT_CRITICAL(&s_dev_mux);
}

bool nocsif_ble_hunt_active(void) { return s_hunt_active; }

bool nocsif_ble_hunt_snapshot(nocsif_ble_hunt_t *out)
{
    if (out == NULL) {
        return false;
    }
    bool     active;
    int16_t  raw, sm, peak;
    int64_t  last;
    uint16_t frames;
    portENTER_CRITICAL(&s_dev_mux);
    active = s_hunt_active;
    raw    = s_hunt_raw;
    sm     = (int16_t)(s_hunt_ema_x100 / 100);
    peak   = s_hunt_peak;
    last   = s_hunt_last_us;
    frames = s_hunt_frames;
    portEXIT_CRITICAL(&s_dev_mux);

    memset(out, 0, sizeof *out);
    if (!active) {
        return false;
    }
    out->active   = true;
    out->rssi     = raw;
    out->smoothed = sm;
    out->peak     = (peak == -128) ? 0 : peak;
    out->frames   = frames;
    if (last > 0) {
        int64_t age = esp_timer_get_time() - last;
        out->age_ms = age > 0 ? (uint32_t)(age / 1000) : 0;
    } else {
        out->age_ms = 0xFFFFFFFFu;   /* pinned but not yet heard */
    }
    return true;
}

const char *nocsif_ble_hunt_target_str(void)
{
    return s_hunt_name[0] ? s_hunt_name : "target";
}

const char *nocsif_ble_hunt_tag_str(void)
{
    static char b[16];
    if (nocsif_reliability_safe_mode()) return "\xE2\x80\x94";   /* — */
    if (!s_hunt_active) return "pick";
    nocsif_ble_hunt_t h;
    if (nocsif_ble_hunt_snapshot(&h) && h.age_ms != 0xFFFFFFFFu) {
        snprintf(b, sizeof b, "%d dBm", h.smoothed);
        return b;
    }
    return "hunting";
}

const char *nocsif_ble_status_str(void)
{
    static char b[40];
    if (nocsif_reliability_safe_mode()) return "safe mode " BLE_DOT " BLE off";
    if (s_starting) return "starting " BLE_DOT " releasing WiFi" BLE_ELL;
    int n = s_dev_cnt;
    if (s_scan_active) {
        if (n > 0) snprintf(b, sizeof b, "scanning " BLE_DOT " %d device%s", n, n == 1 ? "" : "s");
        else       snprintf(b, sizeof b, "scanning" BLE_ELL);
        return b;
    }
    if (s_host_up && !s_available) return "starting" BLE_ELL;
    if (n > 0) { snprintf(b, sizeof b, "stopped " BLE_DOT " %d seen", n); return b; }
    return "off";
}

const char *nocsif_ble_scan_tag_str(void)
{
    static char b[16];
    if (nocsif_reliability_safe_mode()) return "\xE2\x80\x94";   /* — */
    if (s_starting) return BLE_ELL;
    int n = s_dev_cnt;
    if (s_scan_active) {
        if (n > 0) { snprintf(b, sizeof b, "%d", n); return b; }
        return "scan";
    }
    if (n > 0) { snprintf(b, sizeof b, "%d seen", n); return b; }
    return "ready";
}

const char *nocsif_ble_addr_type_str(uint8_t addr_type)
{
    switch (addr_type) {
        case BLE_ADDR_PUBLIC:    return "public";
        case BLE_ADDR_RANDOM:    return "random";
        case BLE_ADDR_PUBLIC_ID: return "public-id";
        case BLE_ADDR_RANDOM_ID: return "random-id";
        default:                 return "addr";
    }
}

/* A small table of the Bluetooth SIG company identifiers seen most often in the field — unknown
 * ids just show as a raw hex code in the UI, so only the common ones need an entry here. */
const char *nocsif_ble_company_str(uint16_t company)
{
    switch (company) {
        case 0x004C: return "Apple";
        case 0x0006: return "Microsoft";
        case 0x00E0: return "Google";
        case 0x0075: return "Samsung";
        case 0x0059: return "Nordic";
        case 0x02E5: return "Espressif";
        case 0x0087: return "Garmin";
        case 0x00D7: return "Qualcomm";
        case 0x000F: return "Broadcom";
        case 0x0171: return "Amazon";
        case 0x012D: return "Sony";
        case 0x0157: return "Huami";
        default:     return "";
    }
}

const char *nocsif_ble_appearance_str(uint16_t appearance)
{
    /* Top-level GAP appearance category = value >> 6 (Bluetooth Assigned Numbers). Best-effort human
     * label for the common ones; "" for 0 / unknown (the detail view still shows the raw code). */
    if (appearance == 0) {
        return "";
    }
    switch (appearance >> 6) {
        case 0x001: return "Phone";
        case 0x002: return "Computer";
        case 0x003: return "Watch";
        case 0x004: return "Clock";
        case 0x005: return "Display";
        case 0x006: return "Remote";
        case 0x007: return "Glasses";
        case 0x008: return "Tag";
        case 0x009: return "Keyring";
        case 0x00A: return "Media Player";
        case 0x00B: return "Barcode Scanner";
        case 0x00C: return "Thermometer";
        case 0x00D: return "Heart Rate Sensor";
        case 0x00E: return "Blood Pressure";
        case 0x00F: return "Input Device";      /* HID: keyboard / mouse / joystick */
        case 0x010: return "Glucose Meter";
        case 0x011: return "Running Sensor";
        case 0x012: return "Cycling";
        case 0x031: return "Outdoor Sports";
        case 0x041: return "Audio Device";       /* Wearable Audio (earbuds / headset) */
        default:    return "";
    }
}

/* ============================ GATT explore — public API (M7-P2) ============================ */

void nocsif_ble_gatt_connect(int dev_idx) { post_arg(CMD_GATT_CONNECT, dev_idx); }
void nocsif_ble_gatt_disconnect(void)     { post(CMD_GATT_DISCONNECT); }
void nocsif_ble_gatt_read(int idx)        { post_arg(CMD_GATT_READ, idx); }

nocsif_ble_gatt_state_t nocsif_ble_gatt_state(void) { return s_gatt_state; }
int      nocsif_ble_gatt_item_count(void) { return s_svc_cnt + s_chr_cnt; }
uint32_t nocsif_ble_gatt_gen(void)        { return s_gatt_gen; }
const char *nocsif_ble_gatt_target_str(void) { return s_target; }

const char *nocsif_ble_gatt_state_str(void)
{
    static char b[56];
    switch (s_gatt_state) {
    case NOCSIF_BLE_GATT_CONNECTING:
        snprintf(b, sizeof b, "linking to %s" BLE_ELL, s_target);
        return b;
    case NOCSIF_BLE_GATT_DISCOVERING:
        snprintf(b, sizeof b, "reading GATT " BLE_DOT " %d svc %d chr", s_svc_cnt, s_chr_cnt);
        return b;
    case NOCSIF_BLE_GATT_READY:
        snprintf(b, sizeof b, "%s " BLE_DOT " %d svc %d chr", s_target, s_svc_cnt, s_chr_cnt);
        return b;
    case NOCSIF_BLE_GATT_DISCONNECTING:
        return "disconnecting" BLE_ELL;
    default:
        return s_gatt_fail ? "connect failed " BLE_DOT " tap a device"
                           : "tap a device to connect";
    }
}

const char *nocsif_ble_gatt_tag_str(void)
{
    static char b[16];
    if (nocsif_reliability_safe_mode()) return "\xE2\x80\x94";   /* — */
    switch (s_gatt_state) {
    case NOCSIF_BLE_GATT_CONNECTING:  return "linking";
    case NOCSIF_BLE_GATT_DISCOVERING: return "reading";
    case NOCSIF_BLE_GATT_READY:       snprintf(b, sizeof b, "%d svc", s_svc_cnt); return b;
    default:                          return "connect";
    }
}

const char *nocsif_ble_gatt_uuid_name(uint16_t u)
{
    switch (u) {
    /* --- services --- */
    case 0x1800: return "Generic Access";
    case 0x1801: return "Generic Attribute";
    case 0x1802: return "Immediate Alert";
    case 0x1803: return "Link Loss";
    case 0x1804: return "Tx Power";
    case 0x1805: return "Current Time";
    case 0x180A: return "Device Info";
    case 0x180D: return "Heart Rate";
    case 0x180F: return "Battery";
    case 0x1812: return "HID";
    case 0x1816: return "Cycling Speed";
    case 0xFE59: return "Nordic DFU";
    /* --- characteristics --- */
    case 0x2A00: return "Device Name";
    case 0x2A01: return "Appearance";
    case 0x2A04: return "Conn Params";
    case 0x2A05: return "Service Changed";
    case 0x2A19: return "Battery Level";
    case 0x2A23: return "System ID";
    case 0x2A24: return "Model Number";
    case 0x2A25: return "Serial Number";
    case 0x2A26: return "Firmware Rev";
    case 0x2A27: return "Hardware Rev";
    case 0x2A28: return "Software Rev";
    case 0x2A29: return "Manufacturer";
    case 0x2A2B: return "Current Time";
    case 0x2A37: return "Heart Rate Meas";
    case 0x2A50: return "PnP ID";
    default:     return "";
    }
}

/* Formats a NimBLE UUID into "0xXXXX" (16-bit) or the full dashed string (128-bit). */
static void gatt_format_uuid(const ble_uuid_any_t *u, char *dst, size_t dstlen)
{
    char tmp[BLE_UUID_STR_LEN];
    ble_uuid_to_str(&u->u, tmp);
    strncpy(dst, tmp, dstlen - 1);
    dst[dstlen - 1] = '\0';
}

/* Formats a read value: a couple of friendly decodes, else quoted text if printable, else hex. */
static void gatt_format_value(uint16_t u16, const uint8_t *v, uint8_t n, char *dst, size_t dstlen)
{
    if (n == 0) { snprintf(dst, dstlen, "(empty)"); return; }
    if (u16 == 0x2A19 && n >= 1) { snprintf(dst, dstlen, "%u%%", v[0]); return; }        /* Battery */
    if (u16 == 0x2A01 && n >= 2) {                                                        /* Appearance */
        snprintf(dst, dstlen, "0x%04X", (uint16_t)(v[0] | (v[1] << 8))); return;
    }
    bool printable = true;
    for (int i = 0; i < n; i++) {
        if (v[i] != 0 && (v[i] < 0x20 || v[i] > 0x7e)) { printable = false; break; }
    }
    if (printable) {                                     /* device name / manufacturer / model / … */
        size_t k = 0;
        dst[k++] = '"';
        for (int i = 0; i < n && v[i] && k < dstlen - 2; i++) dst[k++] = (char)v[i];
        if (k < dstlen - 1) dst[k++] = '"';
        dst[k] = '\0';
        return;
    }
    size_t k = 0;                                        /* raw bytes */
    for (int i = 0; i < n && k + 3 < dstlen; i++) {
        k += (size_t)snprintf(dst + k, dstlen - k, "%02x ", v[i]);
    }
    if (k > 0) dst[k - 1] = '\0';                        /* drops the trailing space */
    else       snprintf(dst, dstlen, "-");
}

bool nocsif_ble_gatt_item_get(int idx, nocsif_ble_gatt_item_t *out)
{
    if (out == NULL) {
        return false;
    }
    int si, ci;
    bool ok;
    g_svc_t sv;
    g_chr_t ch;
    portENTER_CRITICAL(&s_gatt_mux);
    ok = gatt_flat_resolve(idx, &si, &ci);
    if (ok) {
        if (ci < 0) sv = s_svc[si];
        else        ch = s_chr[ci];
    }
    portEXIT_CRITICAL(&s_gatt_mux);
    if (!ok) {
        return false;
    }

    memset(out, 0, sizeof *out);
    out->svc_index = (uint8_t)si;
    if (ci < 0) {                                        /* service header */
        out->is_service = true;
        out->handle = sv.start;
        gatt_format_uuid(&sv.uuid, out->uuid, sizeof out->uuid);
        uint16_t u16 = (sv.uuid.u.type == BLE_UUID_TYPE_16) ? ble_uuid_u16(&sv.uuid.u) : 0;
        if (u16) {
            const char *nm = nocsif_ble_gatt_uuid_name(u16);
            strncpy(out->label, nm, sizeof out->label - 1);
        }
    } else {                                             /* characteristic */
        out->is_service = false;
        out->handle = ch.val_handle;
        out->props  = ch.props;
        out->readable   = (ch.props & BLE_GATT_CHR_PROP_READ) != 0;
        out->writable   = (ch.props & (BLE_GATT_CHR_PROP_WRITE | BLE_GATT_CHR_PROP_WRITE_NO_RSP)) != 0;
        out->notifiable = (ch.props & (BLE_GATT_CHR_PROP_NOTIFY | BLE_GATT_CHR_PROP_INDICATE)) != 0;
        gatt_format_uuid(&ch.uuid, out->uuid, sizeof out->uuid);
        uint16_t u16 = (ch.uuid.u.type == BLE_UUID_TYPE_16) ? ble_uuid_u16(&ch.uuid.u) : 0;
        if (u16) {
            const char *nm = nocsif_ble_gatt_uuid_name(u16);
            strncpy(out->label, nm, sizeof out->label - 1);
        }
        if (ch.has_val) {
            out->has_value = true;
            gatt_format_value(u16, ch.val, ch.vlen, out->value, sizeof out->value);
        }
    }
    return true;
}

/* ============================ Advertise / Beacon — public API (M7-P3) ============================ */

void nocsif_ble_adv_start(void) { post(CMD_ADV_START); }
void nocsif_ble_adv_stop(void)  { post(CMD_ADV_STOP); }
bool nocsif_ble_adv_active(void)   { return s_adv_active; }
bool nocsif_ble_adv_starting(void) { return s_adv_starting; }

nocsif_ble_adv_mode_t nocsif_ble_adv_mode(void) { return s_adv_mode; }

void nocsif_ble_adv_set_mode(nocsif_ble_adv_mode_t mode)
{
    s_adv_mode = (mode == NOCSIF_BLE_ADV_IBEACON) ? NOCSIF_BLE_ADV_IBEACON : NOCSIF_BLE_ADV_CUSTOM;
    nocsif_settings_set_i32("adv_mode", (int32_t)s_adv_mode);
    /* if already advertising, restarts so the new format takes effect */
    if (s_adv_active || s_want_adv) {
        post(CMD_ADV_STOP);
        post(CMD_ADV_START);
    }
}

const char *nocsif_ble_adv_name(void) { return s_adv_name; }

void nocsif_ble_adv_set_name(const char *name)
{
    if (name == NULL) {
        return;
    }
    strncpy(s_adv_name, name[0] ? name : "NocSif", sizeof s_adv_name - 1);
    s_adv_name[sizeof s_adv_name - 1] = '\0';
    nocsif_settings_set_str("adv_name", s_adv_name);
    if ((s_adv_active || s_want_adv) && s_adv_mode == NOCSIF_BLE_ADV_CUSTOM) {
        post(CMD_ADV_STOP);
        post(CMD_ADV_START);
    }
}

const char *nocsif_ble_adv_mode_str(void)
{
    return s_adv_mode == NOCSIF_BLE_ADV_IBEACON ? "iBeacon" : "named";
}

const char *nocsif_ble_adv_status_str(void)
{
    static char b[56];
    if (nocsif_reliability_safe_mode()) return "safe mode " BLE_DOT " advertise off";
    if (s_adv_starting) return "starting " BLE_DOT " releasing WiFi" BLE_ELL;
    if (s_adv_active) {
        if (s_adv_mode == NOCSIF_BLE_ADV_IBEACON) return "broadcasting " BLE_DOT " iBeacon (NOCSIF)";
        snprintf(b, sizeof b, "broadcasting " BLE_DOT " \"%s\"", s_adv_name);
        return b;
    }
    return "off " BLE_DOT " tap Broadcast";
}

const char *nocsif_ble_adv_tag_str(void)
{
    if (nocsif_reliability_safe_mode()) return "\xE2\x80\x94";   /* — */
    if (s_adv_starting) return BLE_ELL;
    if (s_adv_active)   return "on air";
    return "off";
}

/* ============================ Advertisement Resilience Test — public API ============================ */
void nocsif_ble_restest_start(void) { post(CMD_RESTEST_START); }
void nocsif_ble_restest_stop(void)  { post(CMD_RESTEST_STOP); }
bool nocsif_ble_restest_active(void) { return s_rt_active; }

bool nocsif_ble_restest_connect(void) { rt_config_load(); return s_rt_connect; }
void nocsif_ble_restest_set_connect(bool on)
{
    rt_config_load();
    s_rt_connect = on;
    nocsif_settings_set_i32("rt_connect", on ? 1 : 0);
}
nocsif_ble_rt_intensity_t nocsif_ble_restest_intensity(void) { rt_config_load(); return s_rt_intensity; }
void nocsif_ble_restest_set_intensity(nocsif_ble_rt_intensity_t i)
{
    rt_config_load();
    s_rt_intensity = (i >= NOCSIF_BLE_RT_LOW && i <= NOCSIF_BLE_RT_HIGH) ? i : NOCSIF_BLE_RT_MED;
    nocsif_settings_set_i32("rt_int", (int32_t)s_rt_intensity);
}
int nocsif_ble_restest_duration_s(void) { rt_config_load(); return s_rt_dur_s; }
void nocsif_ble_restest_set_duration_s(int s)
{
    rt_config_load();
    if (s < NOCSIF_BLE_RT_DUR_MIN_S) s = NOCSIF_BLE_RT_DUR_MIN_S;
    if (s > NOCSIF_BLE_RT_DUR_MAX_S) s = NOCSIF_BLE_RT_DUR_MAX_S;
    s_rt_dur_s = s;
    nocsif_settings_set_i32("rt_dur", s_rt_dur_s);
}

uint32_t nocsif_ble_restest_emitted(void) { return s_rt_emitted; }
int nocsif_ble_restest_remaining_s(void)
{
    if (!s_rt_active) return 0;
    int64_t rem = (s_rt_deadline_us - esp_timer_get_time()) / 1000000;
    return rem < 0 ? 0 : (int)rem;
}
const char *nocsif_ble_restest_intensity_str(void)
{
    rt_config_load();
    return s_rt_intensity == NOCSIF_BLE_RT_LOW ? "low"
         : s_rt_intensity == NOCSIF_BLE_RT_MED ? "medium" : "high";
}
const char *nocsif_ble_restest_variant_str(void) { return s_rt_variant_lbl; }
const char *nocsif_ble_restest_status_str(void)
{
    static char b[64];
    if (nocsif_reliability_safe_mode()) return "safe mode " BLE_DOT " test off";
    if (s_rt_active) {
        snprintf(b, sizeof b, "%s " BLE_DOT " %us left " BLE_DOT " %u sent",
                 s_rt_connect ? "pop-ups (connectable)" : "pop-ups",
                 (unsigned)nocsif_ble_restest_remaining_s(), (unsigned)s_rt_emitted);
        return b;
    }
    return "off " BLE_DOT " authorized bench only";
}
const char *nocsif_ble_restest_tag_str(void)
{
    if (nocsif_reliability_safe_mode()) return "\xE2\x80\x94";   /* — */
    return s_rt_active ? "on air" : "off";
}
