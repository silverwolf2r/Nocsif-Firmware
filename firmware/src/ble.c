/*
 * NocSif — Bluetooth LE (on-SoC 2.4 GHz, NimBLE host) worker (M7-P1). See ble.h.
 *
 * Architecture (mirrors wifi.c / nfc.cpp):
 *   - A dedicated worker task owns the stack actions. The LVGL callbacks only post a command to the
 *     worker's queue (never touch the stack) so LVGL stays single-threaded and can't stall.
 *   - NimBLE runs its own host task (nimble_port_freertos_init). Its GAP discovery callback — an
 *     advertisement report arrived — fires on that host task; it does O(1) work: parse the ~31-byte
 *     adv payload, then upsert one row into a BDA-keyed table under a short spinlock. The UI reads a
 *     lock-free snapshot. Nothing here calls LVGL or blocks.
 *   - Bring-up (nimble_port_init + host task) is LAZY on the first scan request and gated on
 *     nocsif_reliability_safe_mode(), so boot stays fast and the coexistence risk (BLE controller RAM
 *     vs the display-flush DMA path) is isolated to first use. The host stays up between scans; Stop
 *     just cancels discovery.
 *
 * Coexistence: WiFi + BLE share the one 2.4 GHz radio via esp_coex software coexistence
 * (CONFIG_ESP_COEX_SW_COEXIST_ENABLE, sdkconfig.defaults). Enabling BT claws back internal DMA RAM,
 * so bring-up logs int-dma free / largest-block for the on-device headroom check (the DMA-hang class).
 *
 * Authorized testing only: this receives the advertisements every nearby device already broadcasts,
 * the same discovery a phone performs. Scope every use to devices you are authorized to test.
 */
#include "ble.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>        /* atof — parse AMS volume ("0.0".."1.0")                            */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nimble/ble.h"            /* BLE_ERR_REM_USER_CONN_TERM (GATT disconnect reason)       */
#include "nimble/hci_common.h"      /* BLE_HCI_ADV_RPT_EVTYPE_* (connectable PDU classification) */
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"       /* BLE_HS_ADV_F_* / _TX_PWR_LVL_AUTO (advertise fields, M7-P3) */
#include "host/ble_gap.h"          /* ble_gap_connect / _terminate / _adv_start (central + bcast) */
#include "host/ble_gatt.h"         /* ble_gattc_* (client) + ble_gatts_* / ble_gatt_svc_def (server) */
#include "host/ble_att.h"          /* BLE_ATT_ERR_* / BLE_ATT_F_READ (GATT server access, HID)  */
#include "host/ble_uuid.h"         /* ble_uuid_to_str / _u16 (UUID formatting)                  */
#include "host/ble_hs_mbuf.h"      /* ble_hs_mbuf_to_flat / _from_flat (read values / notify)   */
#include "host/ble_sm.h"           /* BLE_SM_PAIR_KEY_DIST_* (bonding key distribution, ANCS)   */
#include "host/ble_store.h"        /* ble_store_clear / _util_delete_peer (unpair / re-pair)   */
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"    /* ble_svc_gap_init / device_name_set (HID GATT server)  */
#include "services/gatt/ble_svc_gatt.h"  /* ble_svc_gatt_init — Service Changed (HID GATT server) */
#include "os/os_mbuf.h"            /* os_mbuf_append / OS_MBUF_PKTLEN (HID access callbacks)     */
#include "store/config/ble_store_config.h"  /* ble_store_config_init — NVS-backed bond store (ANCS) */

#include "coex.h"           /* NOCSIF_DMA_CAPS + nocsif_int_dma_largest/_free + per-radio gates   */
#include "reliability.h"    /* nocsif_reliability_safe_mode */
#include "settings.h"       /* nocsif_settings_* — persist the advertise config (M7-P3)         */
#include "wifi.h"           /* wifi state getters (the controller is resident; BLE never yields WiFi)   */
#include "sdcard.h"         /* nocsif_sdcard_lock/unlock — /sd access for the advert PCAP (P4·3) */
#include "usb_gadget.h"     /* nocsif_usb_gadget_claim_sd — own /sd away from USB-MSC while writing */
#include "power.h"          /* nocsif_power_batt_pct — sourced by the HID Battery Service        */

#include <sys/stat.h>       /* mkdir + stat (PCAP output dir / next-free filename, P4·3)         */

static const char *TAG = "ble";

/* Provided by the NimBLE port's config store (libbt.a) but not declared in its public header. */
void ble_store_config_init(void);

/* ---- tunables --------------------------------------------------------------------- */
#define BLE_DEV_MAX     48      /* discovered-device table size (first-seen; stalest evicted) */
#define BLE_CMD_QLEN    6       /* worker command queue depth                                 */
#define BLE_NAME_MAX    32      /* local-name field, incl. NUL                                */

/* Middot "·" (U+00B7) and ellipsis "…" (U+2026) as UTF-8 literals — ble.c pulls in no UI headers. */
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
} ble_cmd_type_t;

typedef struct {
    ble_cmd_type_t type;
    int            arg;    /* CONNECT: scan-table dev index. READ: flattened item index.    */
} ble_cmd_t;

static void post(ble_cmd_type_t type);              /* fwd: host-task callbacks queue worker commands */
static void post_arg(ble_cmd_type_t type, int arg);

/* ---- discovered-device table (BDA-keyed; spinlock-guarded) ------------------------- *
 * Written by the NimBLE host task's GAP callback; read (snapshot copied out) by the LVGL task. The
 * short spinlock only touches the table, so the reader is never blocked long. First-seen order keeps
 * rows stable; when the table is full the stalest entry (oldest last_us) is evicted. */
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
} ble_dev_t;

static ble_dev_t          s_dev[BLE_DEV_MAX];
static volatile int       s_dev_cnt;
static volatile uint32_t  s_dev_gen;
static portMUX_TYPE       s_dev_mux = portMUX_INITIALIZER_UNLOCKED;

/* ---- module state ----------------------------------------------------------------- */
static TaskHandle_t   s_task;
static QueueHandle_t  s_q;
static bool           s_host_up;      /* nimble_port_init done + host task spawned          */
static volatile bool  s_synced;       /* host<->controller synced (sync_cb fired)           */
static volatile bool  s_available;    /* mirror of s_synced for the C getter                */
static volatile bool  s_scan_active;  /* discovery running                                  */
static volatile bool  s_starting;     /* bring-up in progress (releasing WiFi + NimBLE init) */
static bool           s_want_scan;    /* intent: scan when the stack is ready                */

/* ---- GATT explore tables (M7-P2; s_gatt_mux-guarded) ------------------------------- *
 * Once connected to one device, the host task's GATT callbacks append its services + characteristics
 * here (append-only during discovery, zeroed on disconnect), plus a small read cache per
 * characteristic. The UI reads a flattened snapshot (nocsif_ble_gatt_item_get) lock-free. */
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
static int                s_disc_svc_i;    /* service whose chrs are being enumerated         */
static volatile bool      s_gatt_fail;     /* last connect attempt failed (header hint)       */
static char               s_target[BLE_TGT_MAX];   /* connected-peer label                    */

/* ---- advertise / beacon (M7-P3, broadcaster) --------------------------------------- *
 * A non-connectable advertisement we transmit: a plain named advert or an Apple iBeacon. Config is
 * persisted to NVS and applied on the next start. The name buffer is set from the LVGL task before a
 * start is requested (sequential user actions), read by the worker/host at start — a benign race. */
static nocsif_ble_adv_mode_t s_adv_mode;                 /* CUSTOM / IBEACON                 */
static char               s_adv_name[BLE_NAME_MAX] = "NocSif";
static volatile bool      s_adv_active;                  /* advertising running              */
static volatile bool      s_adv_starting;                /* bring-up window (releasing WiFi) */
static bool               s_want_adv;                    /* intent: advertise once synced    */
static bool               s_adv_loaded;                  /* NVS config pulled in once        */

/* ---- Signal Hunt target (M7-P4·2; guarded by s_dev_mux) ---------------------------- *
 * One pinned device whose RSSI we track live (EMA-smoothed) for the proximity gradient. Updated in
 * dev_upsert on the host task when the target's advert arrives; read as a snapshot by the UI. */
static uint8_t            s_hunt_addr[6];
static uint8_t            s_hunt_addr_type;
static volatile bool      s_hunt_active;
static int16_t            s_hunt_raw;         /* last raw RSSI dBm                             */
static int32_t            s_hunt_ema_x100;    /* EMA of RSSI * 100 (smoothed gradient)         */
static int16_t            s_hunt_peak;        /* strongest (closest) RSSI seen since pinned    */
static int64_t            s_hunt_last_us;     /* last time the target was heard                */
static uint16_t           s_hunt_frames;      /* adverts heard from the target since pinned    */
static char               s_hunt_name[BLE_NAME_MAX];

/* ---- advert PCAP export (M7-P4·3) -------------------------------------------------- *
 * Record every received advertisement to /sd as a Wireshark-readable PCAP. The single 2.4 GHz radio
 * is tight on internal RAM during BLE, so this reuses the WiFi PCAP writer *pattern* but NOT a fresh
 * task: the GAP callback (host task, sole producer) copies each raw adv into a PSRAM SPSC ring in
 * O(1); the existing worker task drains the ring to the file on a 100 ms wake (its stack is bumped to
 * fit FATFS — see nocsif_ble_init). Each record is synthesised into a BLE link-layer advertising PDU
 * with a pseudo-header (LINKTYPE_BLUETOOTH_LE_LL_WITH_PHDR = 256) so the RSSI + address + AD payload
 * decode natively in Wireshark. Fidelity note: the observer hands us the assembled adv report, not the
 * raw over-the-air LL bits, so channel is nominal (primary-adv) and the CRC is left unchecked. */
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

static ble_pcap_slot_t   *s_pcap_ring;             /* PSRAM, BLE_PCAP_SLOTS entries (lazy)    */
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
 * Receive-side airspace awareness: a compliant drone is required to broadcast a public Remote ID —
 * identity, its own position, and the operator (pilot) position. Over Bluetooth it rides a Service
 * Data AD (UUID 0xFFFA, app code 0x0D); each BT4-legacy advert carries ONE 25-byte ODID message,
 * cycling Basic-ID / Location / System / Operator-ID across successive adverts, which we accumulate
 * per drone (address-keyed). Parsed on the host task in the GAP callback; the UI reads a lock-free
 * snapshot. No transmit, no interaction — the same identification any Remote-ID receiver shows. */
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
 * The watch advertises connectably soliciting ANCS; the phone connects + bonds, then the watch is
 * the GATT client of the phone's ANCS service. The host task drives the discover→subscribe→fetch
 * chain and parses notifications into a table; the UI reads a lock-free snapshot. The three ANCS
 * characteristic UUIDs are 128-bit (BLE_UUID128_INIT wants little-endian, i.e. the printed UUID
 * reversed byte-by-byte). */
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

/* Apple Media Service (AMS) — the media-remote sibling of ANCS, discovered on the SAME bonded phone
 * link. The watch (client) reads now-playing over Entity Update and drives playback over Remote
 * Command. UUIDs stored little-endian, same as ANCS above.
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

static anc_notif_t        s_anc[ANCS_MAX_NOTIF];      /* newest at index 0 (shift on insert) */
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
static uint32_t           s_anc_ds_uid;                  /* uid the current DS response is for */

/* ---- AMS (media remote) — same bonded phone link as ANCS, discovered right after it ---------- */
static uint16_t           s_ams_svc_start, s_ams_svc_end;
static uint16_t           s_ams_rc_val;                  /* Remote Command value handle (write)   */
static uint16_t           s_ams_eu_val, s_ams_eu_cccd;   /* Entity Update value + CCCD (notify)   */
static bool               s_ams_started;                 /* AMS discovery kicked off this link    */
static bool               s_ams_ready;                   /* EU subscribed + attrs registered      */
static portMUX_TYPE       s_ams_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t  s_ams_gen;                     /* bumps on any now-playing change        */
static bool               s_ams_playing;                 /* PlaybackInfo state == Playing          */
static int                s_ams_vol_pct = -1;            /* -1 = unknown                           */
static char               s_ams_title[64];
static char               s_ams_artist[48];
static void ams_start_discovery(void);   /* fwd: ANCS chain kicks AMS once its ATT work is done */

/* ---- Phone companion hub (M7, Connect Phone screen): persisted lifecycle + saved-phone metadata --- *
 * The NimBLE bond store is authoritative for pairing keys but carries no friendly name / recency, so we
 * keep a small parallel table in our own settings NVS. It is slot-based (fixed keys "ph_s0".."ph_sN") so
 * it enumerates WITHOUT bringing the stack up — main.c gates the boot auto-connect on the count. */
static volatile bool      s_bt_master     = true;   /* Bluetooth master (persisted): governs auto-connect */
static volatile bool      s_boot_reserve_ran;       /* nocsif_ble_boot_reserve claimed the block before WiFi */
static volatile bool      s_notif_enabled = true;   /* mirror phone notifications (persisted)              */
static bool               s_phone_cfg_loaded;        /* NVS pulled in once                                 */
static uint32_t           s_phone_seq;               /* monotonic recency counter (persisted)              */
static nocsif_ble_phone_t s_phone[NOCSIF_BLE_PHONE_MAX];   /* RAM cache of the saved-phone metadata        */
static int                s_phone_cnt;
static portMUX_TYPE       s_phone_mux = portMUX_INITIALIZER_UNLOCKED;
/* Identity address of the live link (cached on ENC_CHANGE, cleared on DISCONNECT) so the "connected"
 * flag + recency capture never need a host call from the LVGL task. */
static volatile bool      s_phone_conn_valid;
static uint8_t            s_phone_conn_addr[6];
static uint8_t            s_phone_conn_atype;
/* Pending metadata record (host task fills it on bond, worker consumes it → NVS off the host task). */
static uint8_t            s_meta_addr[6];
static uint8_t            s_meta_atype;
static volatile bool      s_meta_pending;
/* Forget target: set by the LVGL-task request fn, consumed by the worker (serialized user action). */
static ble_addr_t         s_forget_target;
static volatile bool      s_forget_pending;
static void phone_cfg_load(void);        /* fwd: pre-loaded in nocsif_ble_init + guarded elsewhere */

/* ---- BLE HID keyboard (M7, PERIPHERAL + GATT server) ------------------------------- *
 * The watch hosts a HID-over-GATT keyboard service and advertises with the keyboard appearance; a
 * host pairs + subscribes and the watch types by notifying 8-byte boot-keyboard reports (built by the
 * shared hid_kbd keymap, driven by the DuckyScript engine over its BLE sink). Mutually exclusive with
 * the ANCS/AMS phone link at the connection level (MAX_CONNECTIONS=1) — do_hid_start drops the phone
 * link and swaps to the keyboard advert. The HID service is registered PERMANENTLY at boot
 * (gatt_server_register), so s_hid_mode no longer gates registration — it means "currently advertising/
 * linked as a keyboard", NOT "the HID table is registered" (RAM Phase 2 #10). */
static uint8_t            s_hid_mode;                        /* currently advertising/linked as a keyboard */
static bool               s_want_hid;                        /* intent: advertise once synced           */
static volatile int       s_hid_state;                      /* nocsif_ble_hid_state_t                   */
static uint16_t           s_hid_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t           s_hid_input_val;                  /* input report char value handle (notify)  */
static uint16_t           s_hid_batt_val;                   /* battery level char value handle          */
static volatile bool      s_hid_subscribed;                 /* host subscribed to input-report notifies */
static volatile bool      s_hid_encrypted;                  /* link encrypted (bonded)                  */
static uint8_t            s_hid_led;                         /* last LED output report (caps/num/scroll) */
static uint8_t            s_hid_proto = 1;                   /* HID protocol mode (1 = report protocol)  */
static void hid_adv_start(void);          /* fwd: on_sync + a disconnect re-advertise                  */
static void hid_teardown(void);           /* fwd: leave keyboard role (stop advert/link, clear s_hid_mode) */
static void gatt_server_register(void);   /* fwd: bring_up registers the permanent GATT server at boot   */

/* ---- device-table upsert (NimBLE host task) --------------------------------------- *
 * Merge one advertisement report into the table. Fields default to "absent" — a scan response for a
 * device we already have only *adds* what it carries (typically the name), never clears prior data. */
static void dev_upsert(const uint8_t addr[6], uint8_t addr_type, int8_t rssi, bool connectable,
                       const char *name, int8_t tx_pwr, bool tx_pwr_ok,
                       uint16_t appearance, bool appearance_ok,
                       uint16_t company, bool company_ok,
                       uint16_t uuid16, uint8_t n_uuid, uint8_t tracker)
{
    int64_t now = esp_timer_get_time();

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
        } else {                                    /* evict the stalest */
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
        structural = true;
    }
    ble_dev_t *d = &s_dev[idx];
    d->rssi = rssi;
    d->last_us = now;
    if (connectable)  d->connectable = true;        /* sticky: a scan-rsp PDU must not clear it   */
    if (name && name[0]) {                          /* reveal / refresh the name; keep a known one */
        if (d->name[0] == '\0') structural = true;  /* a newly-revealed name (scan-rsp) changes the
                                                     * row's hero → force a UI refresh, else it's
                                                     * stored silently until the entry next churns  */
        size_t k = 0;
        while (name[k] && k < sizeof(d->name) - 1) { d->name[k] = name[k]; k++; }
        d->name[k] = '\0';
    }
    if (tx_pwr_ok)     { d->tx_pwr = tx_pwr; d->tx_pwr_ok = true; }
    if (appearance_ok) { d->appearance = appearance; d->appearance_ok = true; }
    if (company_ok)    { d->company = company; d->company_ok = true; }
    if (uuid16)        d->uuid16 = uuid16;
    if (n_uuid > d->n_uuid) d->n_uuid = n_uuid;
    if (tracker)       d->tracker = tracker;        /* sticky: a later bare PDU must not clear it  */
    if (d->frames < 0xFFFF) d->frames++;
    if (structural) s_dev_gen++;

    /* Signal Hunt (M7-P4·2): if this advert is from the pinned target, drive its live gradient. */
    if (s_hunt_active && addr_type == s_hunt_addr_type && memcmp(addr, s_hunt_addr, 6) == 0) {
        s_hunt_raw = rssi;
        s_hunt_ema_x100 = (s_hunt_ema_x100 == 0)
                              ? (int32_t)rssi * 100
                              : (s_hunt_ema_x100 * 7 + (int32_t)rssi * 100 * 3) / 10;  /* EMA α=0.3 */
        if (rssi > s_hunt_peak) s_hunt_peak = rssi;   /* closer = higher (less negative) */
        s_hunt_last_us = now;
        if (s_hunt_frames < 0xFFFF) s_hunt_frames++;
    }
    portEXIT_CRITICAL(&s_dev_mux);
}

/* Recognize a common item tracker from its advertisement (M7-P4·1). Receive-side classification of
 * signatures the tracker already broadcasts: Apple Find My / AirTag = the offline-finding beacon
 * (company 0x004C, manufacturer message type 0x12); Tile = service UUID 0xFEED/0xFEEC; Samsung
 * SmartTag = service UUID 0xFD5A. `mfg` points at the raw manufacturer AD (mfg[0..1] = company id). */
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

/* Copy one raw advertisement into the PCAP ring (M7-P4·3). Sole producer = the host task's GAP
 * callback; O(1), lock-free (SPSC vs the worker consumer). Stores the raw report fields — the LL/PCAP
 * bytes are synthesised later on the worker so nothing heavy runs on the host task. */
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

/* Read a little-endian int32 (ODID lat/lon are deg * 1e7). */
static int32_t odid_le32(const uint8_t *p)
{
    return (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                     ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
}

/* Copy an ASCII ODID text field (fixed width, may be unterminated); non-printables become '?'. */
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

/* Merge one 25-byte ODID message into a drone record (host task; s_drone_mux held by the caller).
 * Only the fields a receiver acts on are decoded: identity, drone position, and operator position. */
static void odid_apply_msg(ble_drone_t *d, const uint8_t *m, int mlen)
{
    if (mlen < ODID_MSG_LEN) {
        return;
    }
    uint8_t type = (m[0] >> 4) & 0x0F;          /* high nibble = message type; low = protocol ver */
    switch (type) {
    case 0x0:   /* Basic ID: identity + aircraft category */
        d->id_type = (m[1] >> 4) & 0x0F;
        d->ua_type = m[1] & 0x0F;
        odid_copy_text(d->uas_id, sizeof d->uas_id, m + 2, 20);
        d->has_basic = true;
        break;
    case 0x1: {  /* Location / Vector: the drone's own position + motion */
        uint8_t flags = m[1];
        bool ew  = flags & 0x02;                /* E/W direction segment (adds 180 to track)      */
        bool mult = flags & 0x01;               /* speed multiplier: 0.25 m/s (clear) vs 0.75      */
        d->track_deg = (uint16_t)(((int)m[2] + (ew ? 180 : 0)) % 360);
        d->speed_x10 = mult ? (uint16_t)((uint32_t)m[3] * 75 / 10 + 638)   /* m/s*10 (0.75 mult)  */
                            : (uint16_t)((uint32_t)m[3] * 25 / 10);        /* m/s*10 (0.25 mult)  */
        d->lat_e7 = odid_le32(m + 5);
        d->lon_e7 = odid_le32(m + 9);
        uint16_t alt_raw = (uint16_t)(m[15] | (m[16] << 8));   /* geodetic altitude */
        d->alt_m = (int16_t)((int)alt_raw / 2 - 1000);         /* encoding: val*0.5 - 1000        */
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
        break;                                  /* Auth (2) / Self-ID (3) ignored for now */
    }
}

/* Merge an ODID advertisement into the drone table (host task; own lock — must NOT nest in
 * s_dev_mux). `msg` points at the first ODID message (after the UUID + app code + counter). */
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
        } else {                                /* evict the stalest */
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

        pcap_push(d);           /* M7-P4·3: record the raw advert (no-op unless capture is armed) */

        struct ble_hs_adv_fields f;
        if (ble_hs_adv_parse_fields(&f, d->data, d->length_data) != 0) {
            /* Malformed adv payload — still record the address + RSSI (name/fields blank). */
            bool conn = (d->event_type == BLE_HCI_ADV_RPT_EVTYPE_ADV_IND ||
                         d->event_type == BLE_HCI_ADV_RPT_EVTYPE_DIR_IND);
            dev_upsert(d->addr.val, d->addr.type, d->rssi, conn,
                       NULL, 0, false, 0, false, 0, false, 0, 0, 0);
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
                   company, company_ok, uuid16, n_uuid, tracker);

        /* OpenDroneID / ASTM F3411 Remote ID (M7-P4·4): a Service Data AD carrying UUID 0xFFFA
         * (bytes FA FF, little-endian) + app code 0x0D, then a 1-byte counter, then ODID message(s).
         * Separate lock from dev_upsert (portMUX is not recursive). */
        if (f.svc_data_uuid16 && f.svc_data_uuid16_len >= 4 + ODID_MSG_LEN &&
            f.svc_data_uuid16[0] == 0xFA && f.svc_data_uuid16[1] == 0xFF &&
            f.svc_data_uuid16[2] == 0x0D) {
            odid_upsert(d->addr.val, d->addr.type, d->rssi,
                        f.svc_data_uuid16 + 4, (int)f.svc_data_uuid16_len - 4);
        }
        return 0;
    }
    case BLE_GAP_EVENT_DISC_COMPLETE:
        /* Only reached if a time-limited discovery ended; we scan BLE_HS_FOREVER, so this means a
         * cancel / internal stop. Reflect it so the UI shows "stopped". */
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
    /* With CONFIG_BT_CTRL_BLE_MAX_ACT=3 (bumped from 2) the controller has a slot for the observer scan
     * ALONGSIDE the persistent ANCS phone advert, so we no longer stop advertising to scan — they
     * coexist, and the phone can still reconnect while a scan runs. (At MAX_ACT=2 adv(1)+scan(1) had no
     * free slot and ble_gap_disc was rejected 0x207 BLE_ERR_MEM_CAPACITY — proven via SCANDBG.) */
    struct ble_gap_disc_params dp = {0};
    dp.passive = 0;             /* active scan: solicit scan responses so we get device names   */
    dp.filter_duplicates = 0;   /* keep repeats: refresh RSSI / last-seen per report            */
    dp.itvl = 0;                /* controller defaults                                          */
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
        s_scan_active = true;   /* already scanning — fine */
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
 * The fixed NocSif iBeacon proximity UUID (leading bytes spell "NOCSIF" so it is recognizable in a
 * beacon scanner). iBeacon major/minor are fixed at 1/1 for this phase. */
static const uint8_t NOCSIF_BEACON_UUID[16] = {
    0x4E, 0x4F, 0x43, 0x53, 0x49, 0x46,   /* "NOCSIF" */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
};

/* Build the raw 30-byte iBeacon advertising payload (flags AD + Apple manufacturer-data AD). */
static int build_ibeacon(uint8_t *o)
{
    int n = 0;
    o[n++] = 0x02; o[n++] = 0x01; o[n++] = 0x06;          /* AD: flags = LE General + BR/EDR unsup   */
    o[n++] = 0x1A; o[n++] = 0xFF;                         /* AD: len 26, manufacturer-specific       */
    o[n++] = 0x4C; o[n++] = 0x00;                         /* company = Apple (0x004C, little-endian)  */
    o[n++] = 0x02; o[n++] = 0x15;                         /* iBeacon type + payload length            */
    memcpy(&o[n], NOCSIF_BEACON_UUID, 16); n += 16;       /* proximity UUID                           */
    o[n++] = 0x00; o[n++] = 0x01;                         /* major = 1 (big-endian)                   */
    o[n++] = 0x00; o[n++] = 0x01;                         /* minor = 1                                */
    o[n++] = 0xC5;                                        /* measured power: -59 dBm at 1 m           */
    return n;                                             /* 30 bytes                                 */
}

/* Non-connectable advertising has no GAP events to handle (no connection, we advertise FOREVER), but
 * pass a stub rather than NULL so the host never dereferences a null callback. */
static int gap_adv_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)event; (void)arg;
    return 0;
}

/* Start advertising with the current config. host- or worker-task (NimBLE API is internally locked).
 * If not yet synced, arms s_want_adv so on_sync starts it. Non-connectable (broadcaster). */
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
        /* Cap the name so flags(3) + tx-power(3) + name-header(2) + name <= 31-byte adv payload;
         * a longer name is advertised truncated + marked incomplete rather than failing the set. */
        size_t nlen = strlen(s_adv_name);
        const size_t NAME_MAX_ADV = 23;
        struct ble_hs_adv_fields fields = {0};
        fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
        fields.name = (const uint8_t *)s_adv_name;
        fields.name_len = (uint8_t)(nlen > NAME_MAX_ADV ? NAME_MAX_ADV : nlen);
        fields.name_is_complete = (nlen <= NAME_MAX_ADV) ? 1 : 0;
        fields.tx_pwr_lvl_is_present = 1;
        fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;   /* stack fills the real level */
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

/* Pull the persisted advertise config (mode + name) from NVS once, so the screen shows the saved
 * values. Idempotent; safe to call from the LVGL task (settings are RAM-cached after first read). */
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

/* ==== Phone Notifications: ANCS notification client (host task) ===================== *
 * After the phone connects + bonds, the watch discovers ANCS, subscribes to the Notification Source
 * (the 8-byte "a notification changed" stream) + Data Source, and for each new notification asks the
 * Control Point for the app id / title / message. Everything below runs on the NimBLE host task. */
static void ancs_adv_start(void);      /* fwd: a disconnect re-advertises */

static const uint8_t ANCS_SUB[2]   = { 0x01, 0x00 }; /* CCCD value: notifications enabled (LE)  */
static const uint8_t ANCS_UNSUB[2] = { 0x00, 0x00 }; /* CCCD value: notifications disabled       */

/* Copy an ANCS text attribute into dst, mapping non-ASCII / control bytes to keep our fonts safe. */
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

/* Insert (newest-first) or refresh a notification by UID. Host task. */
static void anc_notif_upsert(uint32_t uid, uint8_t category)
{
    portENTER_CRITICAL(&s_anc_mux);
    int idx = -1;
    for (int i = 0; i < s_anc_cnt; i++) {
        if (s_anc[i].used && s_anc[i].uid == uid) { idx = i; break; }
    }
    if (idx < 0) {
        int last = (s_anc_cnt < ANCS_MAX_NOTIF) ? s_anc_cnt : ANCS_MAX_NOTIF - 1;
        for (int i = last; i > 0; i--) s_anc[i] = s_anc[i - 1];   /* shift down; drop oldest */
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

/* Ask the Control Point for a notification's App Identifier + Title + Message. */
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
    s_anc_ds_len = 0;                                         /* fresh reassembly for this response    */
    s_anc_ds_uid = uid;
    int rc = ble_gattc_write_flat(s_anc_conn, s_anc_cp_val, cmd, n, NULL, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "ancs: control-point write rc=%d", rc);
    }
}

/* Try to parse the accumulated Data Source bytes: [CmdID:1][UID:4]{ attrID:1, len:2 LE, value }... */
static bool anc_ds_try_parse(void)
{
    const uint8_t *d = s_anc_ds_buf;
    int len = s_anc_ds_len;
    if (len < 5 || d[0] != 0x00) {
        return false;
    }
    uint32_t uid = (uint32_t)d[1] | ((uint32_t)d[2] << 8) | ((uint32_t)d[3] << 16) | ((uint32_t)d[4] << 24);
    /* static (host task only) — keep these 168 bytes off the tight NimBLE host stack (see NOTIFY_RX). */
    static char app[28], title[40], msg[100];
    app[0] = title[0] = msg[0] = '\0';
    int p = 5, got = 0;
    while (p + 3 <= len) {
        uint8_t  aid  = d[p];
        uint16_t alen = (uint16_t)(d[p + 1] | (d[p + 2] << 8));
        p += 3;
        if (p + alen > len) {
            return false;                                    /* value not fully arrived — wait */
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

/* Notification Source event (8 bytes): EventID, EventFlags, CategoryID, CategoryCount, UID(4 LE). */
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
    if (dsc && ble_uuid_u16(&dsc->uuid.u) == 0x2902) {       /* CCCD → assign to nearest preceding char */
        uint16_t cccd = dsc->handle, best = 0;
        int which = 0;
        if (s_anc_ns_val && s_anc_ns_val < cccd && s_anc_ns_val > best) { best = s_anc_ns_val; which = 1; }
        if (s_anc_ds_val && s_anc_ds_val < cccd && s_anc_ds_val > best) { best = s_anc_ds_val; which = 2; }
        if      (which == 1) s_anc_ns_cccd = cccd;
        else if (which == 2) s_anc_ds_cccd = cccd;
    }
    if (error && error->status != 0) {                       /* discovery finished */
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
 * Discovered on the SAME connection as ANCS (kicked off alongside it on encryption). NimBLE
 * serializes the ATT traffic, so the two discovery chains coexist on one link. Entity Update
 * streams now-playing; Remote Command drives playback. */

/* AMS Entity Update notification: [EntityID][AttributeID][Flags][Value UTF-8...].
 * Entities: 0=Player, 2=Track. Track attrs: 0=Artist, 2=Title. Player attr 1=PlaybackInfo
 * ("state,rate,elapsed" — state 1=Playing), 2=Volume ("0.0".."1.0"). Host task only. */
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
    } else if (entity == 0 && attr == 1) {       /* Player PlaybackInfo → play state */
        bool playing = (vlen > 0 && v[0] == '1');
        portENTER_CRITICAL(&s_ams_mux);
        s_ams_playing = playing;
        portEXIT_CRITICAL(&s_ams_mux);
    } else if (entity == 0 && attr == 2) {       /* Player Volume ("0.0".."1.0") — parse off the mux */
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
        return;                                  /* nothing we track — no gen bump */
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
    if (dsc && ble_uuid_u16(&dsc->uuid.u) == 0x2902) {   /* CCCD → assign to Entity Update */
        uint16_t cccd = dsc->handle, best = 0;
        int which = 0;
        if (s_ams_rc_val && s_ams_rc_val < cccd && s_ams_rc_val > best) { best = s_ams_rc_val; which = 1; }
        if (s_ams_eu_val && s_ams_eu_val < cccd && s_ams_eu_val > best) { best = s_ams_eu_val; which = 2; }
        if (which == 2) s_ams_eu_cccd = cccd;            /* ignore RC's CCCD — we only write RC */
    }
    if (error && error->status != 0) {                   /* discovery finished */
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

/* Reset AMS state when the phone link drops (so the next connection re-discovers cleanly). */
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

/* Send an AMS Remote Command (worker task; NimBLE host APIs are lock-guarded, like do_gatt_read).
 * Command ids: 0 Play · 1 Pause · 2 TogglePlayPause · 3 NextTrack · 4 PreviousTrack · 5 VolumeUp
 * · 6 VolumeDown. */
static void do_ams_cmd(int cmd)
{
    if (!s_ams_ready || s_ams_rc_val == 0 || s_anc_conn == BLE_HS_CONN_HANDLE_NONE) {
        ESP_LOGW(TAG, "ams: cmd %d ignored (ready=%d rc=%u conn=%u)",
                 cmd, s_ams_ready, s_ams_rc_val, s_anc_conn);
        return;
    }
    /* AMS Remote Command is a *write-with-response* characteristic (Apple's spec) — a write-WITHOUT-
     * response is silently dropped by iOS. Use ble_gattc_write_flat (with response). */
    uint8_t c = (uint8_t)cmd;
    int rc = ble_gattc_write_flat(s_anc_conn, s_ams_rc_val, &c, 1, NULL, NULL);
    ESP_LOGI(TAG, "ams: cmd %u -> rc=%d (handle=%u)", c, rc, s_ams_rc_val);
}

/* Step the phone volume toward a target (0..100%). AMS is relative-only (VolumeUp/VolumeDown), and
 * iOS media volume is ~16 discrete steps, so translate the gap into that many paced up/down commands.
 * Paced (short delays) so a big jump doesn't burst NimBLE's GATT-procedure pool. Worker task. */
static void do_ams_vol_set(int target_pct)
{
    if (!s_ams_ready || s_ams_rc_val == 0 || s_anc_conn == BLE_HS_CONN_HANDLE_NONE) {
        return;
    }
    if (target_pct < 0)   target_pct = 0;
    if (target_pct > 100) target_pct = 100;
    int cur   = (s_ams_vol_pct < 0) ? 50 : s_ams_vol_pct;
    int steps = (target_pct - cur) * 16 / 100;       /* iOS ≈ 16 discrete volume steps */
    int n     = steps < 0 ? -steps : steps;
    if (n > 16) n = 16;
    uint8_t cmd = (steps > 0) ? NOCSIF_AMS_CMD_VOL_UP : NOCSIF_AMS_CMD_VOL_DN;
    for (int i = 0; i < n; i++) {
        ble_gattc_write_flat(s_anc_conn, s_ams_rc_val, &cmd, 1, NULL, NULL);
        vTaskDelay(pdMS_TO_TICKS(25));               /* pace: ~one write per 25 ms */
    }
    ESP_LOGI(TAG, "ams: vol %d%% -> %d%% (%d %s)", cur, target_pct, n, steps > 0 ? "up" : "down");
}

/* ---- ANCS GAP events (our connectable advertising) --------------------------------- */
static int ancs_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_anc_conn = event->connect.conn_handle;
            s_anc_state = NOCSIF_ANCS_CONNECTED;
            ESP_LOGI(TAG, "ancs: phone connected (conn=%u) — requesting pairing", s_anc_conn);
            ble_gap_security_initiate(s_anc_conn);           /* prompt iOS to pair/bond */
        } else {
            ESP_LOGW(TAG, "ancs: connect failed status=%d", event->connect.status);
            s_anc_conn = BLE_HS_CONN_HANDLE_NONE;
            if (s_want_ancs) ancs_adv_start();
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "ancs: disconnected (reason=%d)", event->disconnect.reason);
        s_anc_conn = BLE_HS_CONN_HANDLE_NONE;
        s_anc_ns_val = s_anc_cp_val = s_anc_ds_val = 0;
        s_phone_conn_valid = false;                      /* no live peer (Connect Phone "connected" tag) */
        ams_reset();                                     /* drop media-remote state with the link */
        if (s_want_ancs) { s_anc_state = NOCSIF_ANCS_ADVERTISING; ancs_adv_start(); }
        else             { s_anc_state = NOCSIF_ANCS_IDLE; }
        return 0;
    case BLE_GAP_EVENT_ENC_CHANGE:
        ESP_LOGI(TAG, "ancs: encryption status=%d", event->enc_change.status);
        if (event->enc_change.status == 0) {
            /* Bond established: cache the peer's identity for the saved-phone list, and hand the worker
             * a metadata record (recency + fallback name) so the NVS write stays off the host task. */
            struct ble_gap_conn_desc desc;
            if (ble_gap_conn_find(s_anc_conn, &desc) == 0) {
                memcpy(s_phone_conn_addr, desc.peer_id_addr.val, 6);
                s_phone_conn_atype = desc.peer_id_addr.type;
                s_phone_conn_valid = true;
                memcpy(s_meta_addr, desc.peer_id_addr.val, 6);
                s_meta_atype = desc.peer_id_addr.type;
                s_meta_pending = true;
                post(CMD_PHONE_META);
            }
            anc_start_discovery();       /* ANCS first; AMS follows once ANCS's ATT work completes    */
        } else {                         /* (two concurrent disc-by-uuid procs corrupt each other)    */
            s_anc_state = NOCSIF_ANCS_FAILED;
        }
        return 0;
    case BLE_GAP_EVENT_NOTIFY_RX: {
        uint16_t h = event->notify_rx.attr_handle;
        /* static (not stack): this runs on the single NimBLE host task, and a 256-byte stack frame
         * here — plus the deep ble_gattc_write_flat chain anc_request_attrs invokes below — overflowed
         * the 4096-byte host task stack and corrupted NimBLE's own ble_hs_timer callout (crash in
         * npl_freertos_callout_is_active). Off the stack + a bigger host stack (sdkconfig) fixes it. */
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
        struct ble_gap_conn_desc desc;                       /* iOS re-pairs → drop the stale bond */
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

/* Advertise connectable, soliciting ANCS. Name goes in the scan response (the 128-bit solicitation
 * fills most of the 31-byte adv payload). host- or worker-task (NimBLE API is internally locked). */
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

    /* Put the local name in the PRIMARY advertisement (not just the scan response): iOS
     * Settings > Bluetooth > OTHER DEVICES lists a peripheral far more reliably when the name is
     * in the main advert. flags(3) + name "NocSif"(8) + 128-bit ANCS solicitation(18) = 29 B, fits
     * the 31-byte payload. The solicitation stays (it hints iOS we want to be its ANCS client). */
    struct ble_hs_adv_fields adv = {0};
    adv.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    adv.name = (const uint8_t *)"NocSif";
    adv.name_len = 6;
    adv.name_is_complete = 1;
    adv.sol_uuids128 = &ANCS_SVC_UUID;
    adv.sol_num_uuids128 = 1;
    if (ble_gap_adv_set_fields(&adv) != 0) {
        /* If this fires the payload overflowed and the advert is EMPTY -> nothing shows anywhere. */
        ESP_LOGW(TAG, "ancs: adv_set_fields failed (payload too big?) — advert will be empty");
    }
    /* Keep the name in the scan response too (belt-and-suspenders for scanners that read it there). */
    struct ble_hs_adv_fields rsp = {0};
    rsp.name = (const uint8_t *)"NocSif";
    rsp.name_len = 6;
    rsp.name_is_complete = 1;
    ble_gap_adv_rsp_set_fields(&rsp);

    struct ble_gap_adv_params ap = {0};
    ap.conn_mode = BLE_GAP_CONN_MODE_UND;                    /* connectable undirected */
    ap.disc_mode = BLE_GAP_DISC_MODE_GEN;
    int rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &ap, ancs_gap_event, NULL);
    if (rc == 0 || rc == BLE_HS_EALREADY) {
        s_anc_state = NOCSIF_ANCS_ADVERTISING;
        ESP_LOGI(TAG, "ancs: advertising (connectable, ANCS-solicit); int-dma free=%u largest=%u",
                 (unsigned)nocsif_int_dma_free(),
                 (unsigned)nocsif_int_dma_largest());
    } else {
        ESP_LOGW(TAG, "ancs: adv_start rc=%d", rc);
    }
}

/* Stop ANCS: drop the link + advertising + clear the mirror. Host must be up (called on release). */
static void ancs_teardown(void)
{
    s_want_ancs = false;
    if (s_anc_conn != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_anc_conn, BLE_ERR_REM_USER_CONN_TERM);
        s_anc_conn = BLE_HS_CONN_HANDLE_NONE;
    }
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
    int rc = ble_hs_util_ensure_addr(0);   /* generate an identity address if none */
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
        ancs_adv_start();
    }
    if (s_want_hid) {
        hid_adv_start();
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
    nimble_port_run();                 /* blocks until nimble_port_stop() (we never stop) */
    nimble_port_freertos_deinit();
}

/* One-time controller bring-up on the worker. Runs ONLY at boot now (from nocsif_ble_boot_reserve,
 * before WiFi), and once up the controller is held RESIDENT for the whole session — WiFi + BLE coexist
 * on the single 2.4 GHz radio via esp_coex software coexistence (CONFIG_ESP_COEX_SW_COEXIST_ENABLE).
 * The ~31.7 KB CONTIGUOUS int-DMA block is claimed from the pristine boot pool and NEVER freed at
 * runtime; a released block can't be re-claimed once WiFi is up (measured: WiFi-released largest 21,504
 * < 31,744), which is why "make room for BLE" at runtime was structurally impossible and has been
 * removed (see nocsif_ble_boot_reserve + coex.h). The gate below is the measured floor
 * NOCSIF_RADIO_MIN_DMA_BLE:
 *    largest = 27648  -> "BLE_INIT: Malloc failed"   (FAILS -> btController int-WDT panic)
 *    largest = 31736  -> controller up, host synced  (WORKS)
 * so bring-up is attempted only when it will very likely succeed. From the pristine ~90 KB boot pool it
 * clears easily; if it ever refuses, the boot init order regressed (WiFi came up first). */
static bool bring_up(void)
{
    if (s_host_up) {
        return true;
    }
    /* Gate on CONTIGUOUS internal DMA before touching the controller — a failed esp_bt_controller_init
     * blows the int-WDT, so refuse cleanly instead of crashing. NO runtime "make room": once WiFi is up
     * the hole can't be widened, so the old lean-WiFi poll + "turn WiFi off" hint are gone. */
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
    /* Bonding config (M7 ANCS): Just Works pairing (no display/keyboard), Secure Connections, and we
     * exchange the encryption + identity keys so the bond survives + iOS reconnects. Harmless for the
     * scan/central/broadcaster modes (they never pair). The NVS-backed store persists the bond. */
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_our_key_dist   = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_store_config_init();               /* register the NVS bond store (idempotent) */
    /* GATT server: registered PERMANENTLY here (RAM Phase 2 #10/C13), in the one window between host init
     * and host start. bring_up runs ONCE at boot and the controller is held resident, so the
     * GAP/GATT + HID/DIS/Battery attribute table is built exactly once and lives the whole session. Its
     * host tables land in PSRAM (CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL=y), so this is int-DMA-neutral
     * (the NOCSIF_RADIO_MIN_DMA_BLE gate is unchanged). "Keyboard mode" is now purely which connectable
     * advert is broadcast (hid_adv_start vs ancs_adv_start), NOT a NimBLE re-init — so the last runtime
     * controller teardown/re-claim (which could strand BLE) is gone. */
    gatt_server_register();
    nimble_port_freertos_init(ble_host_task);
    s_host_up = true;
    /* Note (M7 ANCS): the PERIPHERAL role leaves the largest free internal-DMA block small here, which
     * used to starve the display flush — fixed for good by the persistent LVGL transport buffer (buf3,
     * ui.c disp_cfg.trans_size), so the display no longer needs any per-flush internal-DMA allocation. */
    ESP_LOGI(TAG, "NimBLE host task started; int-dma free (post-init)=%u largest=%u",
             (unsigned)nocsif_int_dma_free(),
             (unsigned)nocsif_int_dma_largest());
    return true;
}

/* ---- advert PCAP writer (M7-P4·3; worker task) ------------------------------------- *
 * Everything below the ring producer runs on the worker task: it owns the FILE* and the /sd lock so
 * no FATFS I/O ever touches the NimBLE host task. */

/* Map a legacy adv-report event type to its LL advertising-PDU type (header bits 3:0). */
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

/* Synthesise one LINKTYPE_BLUETOOTH_LE_LL_WITH_PHDR (256) packet from a ring slot into o[]:
 * a 10-byte pseudo-header (RF channel + signal + reference access address + flags) followed by the
 * reconstructed LL advertising PDU (access address + header + AdvA + AdvData + CRC). Returns length. */
static int pcap_build_packet(const ble_pcap_slot_t *s, uint8_t *o)
{
    int n = 0;
    /* Pseudo-header (DLT 256). Channel is nominal — the report doesn't say which of 37/38/39 it hit. */
    o[n++] = 37;                                       /* RF channel (primary advertising)     */
    o[n++] = (uint8_t)s->rssi;                         /* signal power (dBm, int8)             */
    o[n++] = 0;                                        /* noise power (marked invalid)          */
    o[n++] = 0;                                        /* access-address offenses               */
    o[n++] = 0xD6; o[n++] = 0xBE; o[n++] = 0x89; o[n++] = 0x8E;   /* ref access addr 0x8E89BED6  */
    uint16_t flags = 0x0013;                           /* dewhitened | signal-valid | ref-AA-valid */
    o[n++] = (uint8_t)(flags & 0xFF); o[n++] = (uint8_t)(flags >> 8);
    /* LL packet: advertising access address (transmitted LSB-first) + header + payload + CRC. */
    o[n++] = 0xD6; o[n++] = 0xBE; o[n++] = 0x89; o[n++] = 0x8E;
    uint8_t txadd = (uint8_t)(s->addr_type & 0x01);    /* NimBLE odd addr types are random      */
    o[n++] = (uint8_t)(pcap_ll_pdu_type(s->evtype) | (txadd << 6));   /* LL header byte 0        */
    o[n++] = (uint8_t)(6 + s->dlen);                   /* LL header byte 1: payload length      */
    memcpy(&o[n], s->addr, 6); n += 6;                 /* AdvA (as received, LE)                */
    if (s->dlen) { memcpy(&o[n], s->data, s->dlen); n += s->dlen; }  /* AdvData / ScanRspData    */
    o[n++] = 0; o[n++] = 0; o[n++] = 0;                /* CRC (not computed; flags say unchecked) */
    return n;                                          /* BLE_PCAP_PHDR + 4 + 2 + 6 + dlen + 3   */
}

/* Choose the next free capture file. Assumes the /sd lock is held. */
static void pcap_pick_path(char *out, size_t outlen)
{
    for (int i = 0; i < 1000; i++) {
        snprintf(out, outlen, "/sd/nocsif/ble/adv-%03d.pcap", i);
        struct stat st;
        if (stat(out, &st) != 0) {
            return;                                    /* first non-existent name */
        }
    }
    snprintf(out, outlen, "/sd/nocsif/ble/adv-999.pcap");   /* fallback: reuse the last */
}

/* Own the card, create the output dir, write the PCAP global header. Returns the FILE* (state set) or
 * NULL on failure (s_pcap_state left with the reason). Worker task only. */
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
    mkdir("/sd/nocsif", 0777);                          /* ignore EEXIST */
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
    /* PCAP global header (LE): magic a1b2c3d4 · v2.4 · zone 0 · sig 0 · snaplen · LINKTYPE=256. */
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

/* Append one advert record (16-byte record header + synthesised LE-LL packet). Lock held by caller. */
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

/* Open the file lazily, then drain a bounded chunk of the ring under one /sd lock. Worker task; called
 * once per loop pass while recording is armed. Skips the lock entirely when nothing is pending. */
static void pcap_service(void)
{
    if (!s_pcap_want) {
        return;
    }
    if (s_pcap_f == NULL) {
        s_pcap_f = pcap_open_file();
        if (s_pcap_f == NULL) {                         /* open failed — disarm, surface the state */
            s_pcap_want = false;
            s_pcap_active = false;
            return;
        }
        s_pcap_head = s_pcap_tail = s_pcap_drop = 0;     /* fresh ring for this session */
        s_pcap_active = true;                            /* NOW the host cb fills the ring */
        return;
    }
    uint32_t head = s_pcap_head;                         /* sole consumer */
    uint32_t tail = __atomic_load_n(&s_pcap_tail, __ATOMIC_ACQUIRE);
    if (head == tail) {
        return;                                          /* nothing pending — don't touch the lock */
    }
    if (!nocsif_sdcard_lock(1000)) {
        return;                                          /* card busy — retry next pass */
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

/* Final-drain + close the capture file (worker task). Safe to call when nothing is open. */
static void pcap_close(void)
{
    s_pcap_active = false;                               /* host cb stops filling immediately */
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
            fclose(s_pcap_f);                            /* lock timeout — best-effort close */
        }
        s_pcap_f = NULL;
        ESP_LOGI(TAG, "pcap: closed (%u adverts, %u bytes, %u dropped)",
                 (unsigned)s_pcap_frames, (unsigned)s_pcap_bytes, (unsigned)s_pcap_drop);
    }
    if (s_pcap_state == BPCAP_REC) {
        s_pcap_state = BPCAP_OFF;
    }
}

/* nimble_teardown() was REMOVED in RAM Phase 2 (#10/C13). It was the last runtime NimBLE
 * host+controller teardown (nimble_port_stop/deinit) — used only to swap the GATT attribute table when
 * entering/leaving HID keyboard mode. That freed the reserved ~31.7 KB controller block, which once WiFi
 * is up can NEVER be re-claimed (measured: WiFi-released largest 21,504 < the 31,744 gate), so it could
 * strand Bluetooth until a reboot. The GATT server (incl. HID) is now registered PERMANENTLY at boot
 * (gatt_server_register) and keyboard mode is a pure advert swap (do_hid_start / hid_teardown), so the
 * controller is held resident for the whole session — the governor invariant now holds for HID too. */

/* Drop the discovered devices / drones / GATT view so the next scan starts clean. Shared by the
 * session-exit quiesce and the full release. Does NOT touch the live GATT connection handle — the
 * caller decides whether to terminate it. */
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

/* Session-exit QUIESCE (BLE⇄WiFi coexistence). Leaving a BLE screen must NOT free the controller: its
 * ~30 KB CONTIGUOUS internal-DMA block was reserved at boot, before WiFi, and once WiFi is up that hole
 * can never be re-formed (fragmentation, not shortage — see nocsif_ble_boot_reserve). Freeing it strands
 * BLE until the next reboot: WiFi fragments the hole and the NOCSIF_RADIO_MIN_DMA_BLE gate then refuses
 * every re-bring-up. So instead of tearing down, we stop the screen's transient recon activity and keep
 * the controller RESIDENT, with WiFi left on its lean profile. The persistent phone link is preserved /
 * re-armed. do_ble_release routes here in ALL cases now (governor invariant — the controller is never
 * torn down at runtime); even a Bluetooth-master-OFF toggle keeps the block reserved (bt_master_off).
 * Runs on the worker. */
static void ble_quiesce(void)
{
    if (!s_host_up) {
        s_want_scan = false;
        s_want_adv  = false;
        return;                         /* controller isn't up (the gate refused earlier) — nothing to keep */
    }
    /* Stop transient recon activity, but leave the controller + phone link running. */
    if (s_pcap_want || s_pcap_f) {      /* close any advert recording first */
        s_pcap_want = false;
        pcap_close();
    }
    scan_stop();                        /* cancels discovery + clears s_want_scan */
    if (s_want_adv) {
        adv_stop();                     /* stop a recon beacon (Advertise / Beacon screen) */
    }
    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {   /* drop a transient GATT-explore link (recon) */
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        vTaskDelay(pdMS_TO_TICKS(30));
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    }
    /* Leaving keyboard mode is now just dropping the keyboard advert/link (RAM Phase 2 #10): the GATT
     * table is permanent and the controller stays resident, so there is NO NimBLE teardown + re-claim
     * here — the old "could not re-claim the controller after HID — restart to restore BLE" failure path
     * is gone. The phone advert is re-armed by the tail below. */
    if (s_hid_mode) {
        hid_teardown();
    }
    ble_clear_recon_tables();
    /* Re-assert the persistent phone advert so a bonded phone reconnects (the master governs this).
     * Skips if a phone link is already live or the controller isn't synced yet (ancs_adv_start then
     * arms s_want_ancs and on_sync starts it). */
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

/* Logical Bluetooth OFF (BT master toggled off). GOVERNOR INVARIANT: never a runtime teardown — the
 * ~31.7 KB controller block stays reserved so turning Bluetooth back ON is instant, never a reboot
 * (operator constraint #1). So OFF stops all activity + drops every link but KEEPS the controller: stop
 * recon + advert + the phone link, clear the tables, disarm re-advertise — and deliberately do NOT
 * nimble_teardown. WiFi stays lean while the block is held (the honest, restart-free cost). Runs on the
 * worker. In safe mode / if the boot reserve was skipped the controller isn't up: just clear intent. */
static void bt_master_off(void)
{
    if (!s_host_up) {
        s_want_ancs = s_want_scan = s_want_adv = false;
        return;
    }
    if (s_pcap_want || s_pcap_f) {      /* stop + flush any advert recording */
        s_pcap_want = false;
        pcap_close();
    }
    scan_stop();
    adv_stop();
    ancs_teardown();                    /* drop the phone ANCS link + advertising + clear the mirror */
    ams_reset();                        /* clear media-remote state with the link */
    hid_teardown();                     /* drop the keyboard link + advertising, if any */
    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {   /* drop a transient GATT-explore link */
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        vTaskDelay(pdMS_TO_TICKS(30));
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    }
    ble_clear_recon_tables();
    s_want_ancs = s_want_scan = s_want_adv = false;
    /* NO nimble_teardown: the reserved controller block stays claimed so ON is instant, no reboot. */
    nocsif_log_dma_free("Bluetooth OFF (logical) — controller resident");
}

/* Leaving a BLE screen. ALWAYS quiesce — the controller is never torn down at runtime (governor
 * invariant), whether the Bluetooth master is on or off. Quiesce stops the screen's transient recon and,
 * while the master is on, re-arms the persistent phone advert; it never frees the reserved block. A freed
 * block can't be re-claimed once WiFi is up (measured), which is what used to strand Bluetooth until a
 * reboot. Runs on the worker. */
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
        hid_teardown();         /* leaving keyboard mode — stop the keyboard advert (GATT table stays) */
    }
    s_starting = true;          /* UI shows "starting…" through the WiFi-release + init window */
    if (!bring_up()) {
        s_starting = false;
        return;
    }
    s_want_scan = true;
    scan_start();               /* if not yet synced this arms s_want_scan; on_sync clears s_starting */
}

static void do_scan_off(void)
{
    scan_stop();
}

/* Arm advert recording (M7-P4·3). Ensures the observer is scanning so there are adverts to capture,
 * allocates the PSRAM ring on first use, then flags the worker loop to open the file + drain. */
static void do_pcap_on(void)
{
    if (nocsif_reliability_safe_mode()) {
        ESP_LOGW(TAG, "safe mode — capture request ignored");
        return;
    }
    if (s_hid_mode) {
        hid_teardown();                         /* leaving keyboard mode — stop the keyboard advert */
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
    if (!s_host_up && !bring_up()) {            /* need the radio up to hear adverts */
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
        hid_teardown();         /* leaving keyboard mode — stop the keyboard advert (GATT table stays) */
    }
    s_adv_starting = true;      /* UI shows "starting…" through the WiFi-release + init window */
    if (!bring_up()) {
        s_adv_starting = false;
        return;
    }
    s_want_adv = true;
    adv_start_now();            /* if not yet synced this arms s_want_adv; on_sync clears s_adv_starting */
}

static void do_adv_stop(void)
{
    adv_stop();
}

/* ---- Phone Notifications worker handlers (M7 ANCS) --------------------------------- */
static void do_ancs_start(void)
{
    if (nocsif_reliability_safe_mode()) {
        ESP_LOGW(TAG, "safe mode — ANCS request ignored");
        return;
    }
    if (s_hid_mode) {
        hid_teardown();             /* leaving keyboard mode — stop the keyboard advert (GATT table stays) */
    }
    if (s_want_ancs) {              /* phone session already active — it now PERSISTS across screens
                                     * (see the notif/media screens). Re-opening a screen must NOT
                                     * re-advertise / reset a live connection — just keep it. */
        return;
    }
    if (!bring_up()) {
        s_anc_state = NOCSIF_ANCS_FAILED;
        return;
    }
    ams_reset();                    /* fresh media-remote state for this session (re-discovers AMS) */
    s_want_ancs = true;
    s_anc_state = NOCSIF_ANCS_ADVERTISING;
    ancs_adv_start();               /* if not yet synced, arms s_want_ancs; on_sync starts it */
}

static void do_ancs_forget(void)
{
    if (!s_host_up) {
        return;
    }
    ble_store_clear();              /* wipe every bond so the next pairing is fresh */
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
        ancs_adv_start();           /* re-advertise so the phone can pair again */
    }
}

/* ---- Phone companion hub: persisted config + saved-phone metadata (M7) -------------- *
 * Slot-based NVS table (keys "ph_s0".."ph_sN"), enumerable without the stack up. Each slot serializes
 * to "<seq>|<12hex identity addr>|<atype>|<name>". Addresses are stored in NimBLE byte order (val[0] =
 * LSB); the round-trip is symmetric so ordering is irrelevant. */
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
    /* the name is the remainder after the 3rd '|' (it never contains one) */
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

/* Record a (re)connection to `addr`: bump recency, set name (fallback if none), add/evict as needed. */
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
        } else {                                     /* evict the least-recent */
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

/* ---- Phone companion hub worker handlers (M7) -------------------------------------- */

/* Boot-time controller reservation (the governor's core). Claim the controller's ~31.7 KB block from the
 * pristine boot pool and hold it RESIDENT for the whole session — UNCONDITIONALLY, regardless of the
 * persisted Bluetooth-master toggle. This is what makes "Bluetooth on" an instant logical re-enable
 * rather than a reboot (operator constraint #1): once WiFi is up the block can never be re-claimed
 * (measured 21,504 < 31,744), so it must be taken now or never. If the master is ON, also arm the phone
 * advert / auto-connect; if OFF, the controller stays resident but idle (logical off — no advert). Safe
 * mode skips the whole int-DMA-heavy bring-up. Worker task (bring_up blocks). */
static void do_ble_reserve(void)
{
    phone_cfg_load();
    if (nocsif_reliability_safe_mode()) {
        return;
    }
    if (!bring_up()) {
        return;                     /* gate refused — should never happen from the pristine boot pool */
    }
    if (s_bt_master) {
        do_ancs_start();            /* master on: advertise (open re-arm) + auto-connect the last phone */
    }
    /* master off: controller resident + idle. Turning Bluetooth on later (do_bt_enable/do_ancs_start)
     * finds s_host_up already true, so bring_up returns instantly — no gate, no reboot. */
}

static void do_phone_connect(void)
{
    phone_cfg_load();
    if (!s_bt_master) {
        s_bt_master = true;
        nocsif_settings_set_i32("bt_master", 1);
    }
    do_ancs_start();                /* arm + advertise (open re-arm); brings NimBLE up if needed */
}

static void do_phone_disconnect(void)
{
    if (s_anc_conn != BLE_HS_CONN_HANDLE_NONE) {
        /* Momentary drop: keep s_want_ancs so the disconnect handler re-advertises → reconnects in
         * range. To stop for good, toggle the Bluetooth master off. */
        ble_gap_terminate(s_anc_conn, BLE_ERR_REM_USER_CONN_TERM);
    }
}

static void do_bt_enable(int on)
{
    if (on) {
        /* The controller was reserved at boot and is held RESIDENT for the whole session (do_ble_release
         * / bt_master_off never free it), so turning Bluetooth back ON is a LOGICAL re-enable: just
         * re-arm the phone advert on the already-up controller. Instant, no gate, no bring-up race, no
         * restart hint — operator constraint #1 (Bluetooth is always available at runtime). If the
         * controller somehow isn't up (safe mode / boot reserve skipped) do_ancs_start brings it up from
         * whatever the pool allows and the gate refuses cleanly rather than crashing. */
        do_ancs_start();
    } else {
        bt_master_off();            /* stop all activity + drop the link; KEEP the controller resident */
    }
}

static void do_notif_set(int on)
{
    /* Apply live if we're connected + already found the Notification-Source CCCD; otherwise the gate in
     * anc_disc_dsc_cb takes effect on the next connect. */
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
    phone_meta_touch(addr, atype, NULL);   /* fallback name; recency bump */
}

static void do_phone_forget(void)
{
    if (!s_forget_pending) {
        return;
    }
    s_forget_pending = false;
    ble_addr_t tgt = s_forget_target;
    /* Metadata was already dropped optimistically by the request fn; delete the pairing keys + drop the
     * live link if this is the connected peer. Needs the host up. */
    if (s_host_up) {
        ble_store_util_delete_peer(&tgt);
        if (s_anc_conn != BLE_HS_CONN_HANDLE_NONE && s_phone_conn_valid &&
            memcmp(s_phone_conn_addr, tgt.val, 6) == 0) {
            ble_gap_terminate(s_anc_conn, BLE_ERR_REM_USER_CONN_TERM);
        }
    }
    ESP_LOGI(TAG, "phone: forgot a saved phone (bond + metadata)");
}

/* ============================== BLE HID keyboard (M7) ================================ *
 * The watch hosts a standard HID-over-GATT keyboard (report protocol) + Device Information + Battery
 * services and advertises with the keyboard appearance. A host pairs (Just Works, bonded) + subscribes
 * to the input report; the DuckyScript engine then types by notifying 8-byte boot-keyboard reports on
 * the input-report value handle. The GATT *server* is registered PERMANENTLY at boot (gatt_server_register)
 * with the controller resident; the input-report notify path is gated on an actual subscribed + encrypted
 * keyboard host, so a registered-but-not-advertised HID service emits nothing. Type on hosts you own. */

/* Standard 8-byte boot-keyboard report descriptor: byte0 = modifier bitmap, byte1 = reserved,
 * bytes2..7 = up to six pressed keycodes; one output byte drives the Caps/Num/Scroll LEDs. Matches the
 * USB report the M4 path emits, so the shared keymap produces identical bytes on both transports. */
static const uint8_t HID_REPORT_MAP[] = {
    0x05, 0x01,  /* Usage Page (Generic Desktop)         */
    0x09, 0x06,  /* Usage (Keyboard)                     */
    0xA1, 0x01,  /* Collection (Application)             */
    0x05, 0x07,  /*   Usage Page (Keyboard/Keypad)       */
    0x19, 0xE0,  /*   Usage Minimum (Left Control)       */
    0x29, 0xE7,  /*   Usage Maximum (Right GUI)          */
    0x15, 0x00,  /*   Logical Minimum (0)                */
    0x25, 0x01,  /*   Logical Maximum (1)                */
    0x75, 0x01,  /*   Report Size (1)                    */
    0x95, 0x08,  /*   Report Count (8)                   */
    0x81, 0x02,  /*   Input (Data,Var,Abs) — modifiers   */
    0x95, 0x01,  /*   Report Count (1)                   */
    0x75, 0x08,  /*   Report Size (8)                    */
    0x81, 0x01,  /*   Input (Const) — reserved byte      */
    0x95, 0x05,  /*   Report Count (5)                   */
    0x75, 0x01,  /*   Report Size (1)                    */
    0x05, 0x08,  /*   Usage Page (LEDs)                  */
    0x19, 0x01,  /*   Usage Minimum (Num Lock)           */
    0x29, 0x05,  /*   Usage Maximum (Kana)               */
    0x91, 0x02,  /*   Output (Data,Var,Abs) — LEDs       */
    0x95, 0x01,  /*   Report Count (1)                   */
    0x75, 0x03,  /*   Report Size (3)                    */
    0x91, 0x01,  /*   Output (Const) — LED padding       */
    0x95, 0x06,  /*   Report Count (6)                   */
    0x75, 0x08,  /*   Report Size (8)                    */
    0x15, 0x00,  /*   Logical Minimum (0)                */
    0x25, 0x65,  /*   Logical Maximum (101)              */
    0x05, 0x07,  /*   Usage Page (Keyboard/Keypad)       */
    0x19, 0x00,  /*   Usage Minimum (0)                  */
    0x29, 0x65,  /*   Usage Maximum (101)                */
    0x81, 0x00,  /*   Input (Data,Array) — 6 keycodes    */
    0xC0,        /* End Collection                       */
};

/* HID Information: bcdHID 0x0111, country 0, flags 0x01 (remote-wake capable). */
static const uint8_t HID_INFO[] = { 0x11, 0x01, 0x00, 0x01 };
/* PnP ID: vendor source 0x02 (USB IF), VID 0x303A (Espressif), PID 0x0001, version 0x0100. */
static const uint8_t HID_PNP_ID[] = { 0x02, 0x3A, 0x30, 0x01, 0x00, 0x00, 0x01 };
/* Report Reference descriptors: {report id 0, type} — 0x01 Input, 0x02 Output. */
static const uint8_t HID_RPT_REF_IN[]  = { 0x00, 0x01 };
static const uint8_t HID_RPT_REF_OUT[] = { 0x00, 0x02 };

/* Access-callback dispatch tags (chr/dsc .arg). */
enum {
    HID_A_INFO = 1, HID_A_REPORT_MAP, HID_A_CTRL, HID_A_PROTO,
    HID_A_INPUT, HID_A_OUTPUT, HID_A_PNP, HID_A_MANUF, HID_A_BATT,
};

/* Append `len` bytes to a read response; map an mbuf-full to the ATT resource error. */
static int hid_chr_read(struct os_mbuf *om, const void *data, uint16_t len)
{
    return os_mbuf_append(om, data, len) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

/* One access callback for every HID/DIS/Battery characteristic; dispatch on the per-chr tag in arg. */
static int hid_access_cb(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn; (void)attr;
    switch ((int)(intptr_t)arg) {
    case HID_A_REPORT_MAP: return hid_chr_read(ctxt->om, HID_REPORT_MAP, sizeof HID_REPORT_MAP);
    case HID_A_INFO:       return hid_chr_read(ctxt->om, HID_INFO, sizeof HID_INFO);
    case HID_A_PNP:        return hid_chr_read(ctxt->om, HID_PNP_ID, sizeof HID_PNP_ID);
    case HID_A_MANUF:      return hid_chr_read(ctxt->om, "NocSif", 6);
    case HID_A_CTRL:       return 0;   /* HID Control Point write (suspend/exit) — accept + ignore */
    case HID_A_PROTO:
        if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
            return hid_chr_read(ctxt->om, &s_hid_proto, 1);
        }
        ble_hs_mbuf_to_flat(ctxt->om, &s_hid_proto, 1, NULL);
        return 0;
    case HID_A_INPUT: {                 /* GET_REPORT(input): keys-up; keystrokes arrive via notify */
        uint8_t zero[8] = {0};
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
            { .uuid = BLE_UUID16_DECLARE(0x2A4D), .access_cb = hid_access_cb,     /* Report (Input)    */
              .arg = (void *)(intptr_t)HID_A_INPUT,
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_READ_ENC,
              .val_handle = &s_hid_input_val,
              .descriptors = (struct ble_gatt_dsc_def[]){
                  { .uuid = BLE_UUID16_DECLARE(0x2908), .att_flags = BLE_ATT_F_READ,
                    .access_cb = hid_dsc_access, .arg = (void *)HID_RPT_REF_IN },
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

/* Register the permanent GATT server (GAP + GATT + HID/DIS/Battery). Called ONCE from bring_up at boot,
 * in the window between host init and host start; the controller is resident so the table is never torn
 * down. The GAP device name is neutral ("NocSif") — the keyboard identity lives only in the keyboard
 * advertisement (hid_adv_start sets appearance=Keyboard + the 0x1812 UUID + "NocSif Kbd" scan response),
 * so a phone connecting for ANCS sees a neutral peripheral even though the HID service is present. */
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

/* GAP events for our keyboard advertising / connection. */
static int hid_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_hid_conn = event->connect.conn_handle;
            s_hid_state = NOCSIF_HID_CONNECTED;
            ESP_LOGI(TAG, "hid: host connected (conn=%u) — requesting pairing", s_hid_conn);
            ble_gap_security_initiate(s_hid_conn);   /* force encryption so the report chars unlock */
        } else {
            ESP_LOGW(TAG, "hid: connect failed status=%d", event->connect.status);
            s_hid_conn = BLE_HS_CONN_HANDLE_NONE;
            if (s_want_hid) hid_adv_start();
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "hid: disconnected (reason=%d)", event->disconnect.reason);
        s_hid_conn = BLE_HS_CONN_HANDLE_NONE;
        s_hid_subscribed = false;
        s_hid_encrypted = false;
        if (s_want_hid) { s_hid_state = NOCSIF_HID_ADVERTISING; hid_adv_start(); }
        else            { s_hid_state = NOCSIF_HID_IDLE; }
        return 0;
    case BLE_GAP_EVENT_ENC_CHANGE:
        s_hid_encrypted = (event->enc_change.status == 0);
        ESP_LOGI(TAG, "hid: encryption status=%d", event->enc_change.status);
        if (s_hid_encrypted && s_hid_subscribed) s_hid_state = NOCSIF_HID_READY;
        return 0;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_hid_input_val) {
            s_hid_subscribed = event->subscribe.cur_notify;
            ESP_LOGI(TAG, "hid: input-report subscribe=%d", (int)s_hid_subscribed);
            if (s_hid_subscribed && s_hid_encrypted) s_hid_state = NOCSIF_HID_READY;
        }
        return 0;
    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        struct ble_gap_conn_desc desc;                       /* host re-pairs → drop the stale bond */
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }
    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "hid: MTU=%d", event->mtu.value);
        return 0;
    default:
        return 0;
    }
}

/* Advertise connectable with the keyboard appearance + HID service UUID (name in the scan response).
 * host- or worker-task safe (NimBLE API is internally locked). Arms s_want_hid if not yet synced. */
static void hid_adv_start(void)
{
    if (!s_synced) {
        s_want_hid = true;                                   /* on_sync will start it */
        return;
    }
    if (s_hid_conn != BLE_HS_CONN_HANDLE_NONE) {
        return;                                              /* already linked */
    }
    uint8_t own_addr_type = 0;
    ble_hs_id_infer_auto(0, &own_addr_type);

    static const ble_uuid16_t hid_svc_uuid = BLE_UUID16_INIT(0x1812);
    struct ble_hs_adv_fields adv = {0};
    adv.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    adv.appearance = 0x03C1;                                 /* GAP appearance: Keyboard */
    adv.appearance_is_present = 1;
    adv.uuids16 = (ble_uuid16_t *)&hid_svc_uuid;
    adv.num_uuids16 = 1;
    adv.uuids16_is_complete = 1;
    if (ble_gap_adv_set_fields(&adv) != 0) {
        ESP_LOGW(TAG, "hid: adv_set_fields failed");
    }
    /* Name in the scan response — the primary advert stays a tidy flags+appearance+UUID payload. */
    struct ble_hs_adv_fields rsp = {0};
    rsp.name = (const uint8_t *)"NocSif Kbd";
    rsp.name_len = 10;
    rsp.name_is_complete = 1;
    ble_gap_adv_rsp_set_fields(&rsp);

    struct ble_gap_adv_params ap = {0};
    ap.conn_mode = BLE_GAP_CONN_MODE_UND;                    /* connectable undirected */
    ap.disc_mode = BLE_GAP_DISC_MODE_GEN;
    int rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &ap, hid_gap_event, NULL);
    if (rc == 0 || rc == BLE_HS_EALREADY) {
        s_hid_state = NOCSIF_HID_ADVERTISING;
        ESP_LOGI(TAG, "hid: advertising as keyboard; int-dma free=%u largest=%u",
                 (unsigned)nocsif_int_dma_free(),
                 (unsigned)nocsif_int_dma_largest());
    } else {
        ESP_LOGW(TAG, "hid: adv_start rc=%d", rc);
    }
}

/* Leave the keyboard role — the single "stop being a keyboard" primitive (RAM Phase 2 #10). Drops the
 * keyboard link + its connectable advert + subscription and clears s_hid_mode. Does NOT touch the
 * resident controller or the permanent GATT table: the HID service stays registered, it is simply no
 * longer advertised. Every mode switch and the session-exit quiesce call THIS instead of a NimBLE
 * teardown. Host-up-safe (a down host means the boot reserve was skipped — just clear intent). */
static void hid_teardown(void)
{
    s_want_hid = false;
    s_hid_mode = 0;
    if (!s_host_up) {
        s_hid_state = NOCSIF_HID_IDLE;
        return;
    }
    if (s_hid_conn != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_hid_conn, BLE_ERR_REM_USER_CONN_TERM);
        s_hid_conn = BLE_HS_CONN_HANDLE_NONE;
    }
    ble_gap_adv_stop();
    s_hid_subscribed = false;
    s_hid_encrypted = false;
    s_hid_state = NOCSIF_HID_IDLE;
}

/* Enter keyboard mode. NO NimBLE teardown/re-init (RAM Phase 2 #10/C13): the HID GATT table is permanent
 * and the controller is resident, so entering keyboard mode is purely an advert swap. MAX_CONNECTIONS=1,
 * so the phone/ANCS link (and any transient GATT-explore link) is dropped first, then the keyboard
 * connectable advert is started in place of the ANCS-solicit advert (only one legacy advert at a time). */
static void do_hid_start(void)
{
    if (nocsif_reliability_safe_mode()) {
        ESP_LOGW(TAG, "safe mode — BLE keyboard request ignored");
        return;
    }
    if (s_want_hid) {
        return;                     /* already keyboard mode — persists while the screen is open */
    }
    if (!s_host_up) {               /* controller resident since boot; down only if the reserve was skipped */
        ESP_LOGW(TAG, "hid: controller not up (boot reserve skipped?) — cannot enter keyboard mode");
        s_hid_state = NOCSIF_HID_FAILED;
        return;
    }
    /* Drop the phone role + any transient link (MAX_CONNECTIONS=1), stop its advert, then swap to the
     * keyboard advert — all GAP-level, no int-DMA churn. ancs_teardown disarms s_want_ancs + stops the
     * ANCS-solicit advert; ams_reset clears the media-remote state that rode the phone link. */
    ancs_teardown();
    ams_reset();
    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        vTaskDelay(pdMS_TO_TICKS(30));
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    }
    scan_stop();
    adv_stop();                     /* stop any recon beacon advert too (single legacy advert) */
    s_hid_mode = 1;
    s_want_hid = true;
    s_hid_state = NOCSIF_HID_ADVERTISING;
    hid_adv_start();                /* arms s_want_hid if not yet synced; on_sync starts it */
}

/* ================================ GATT explore (M7-P2) =============================== *
 * Connect to one device as a central and walk its attribute database. The worker initiates the
 * connection + reads; NimBLE delivers the results on its host task via the callbacks below, which do
 * O(1) work and publish under s_gatt_mux. Read-only exploration of a device you are authorized to
 * test — the same service/characteristic walk a phone performs when it connects. */

static void gatt_disc_chrs(int svc_i);   /* forward: chr discovery is chained per service */

/* Service discovery: one callback per service, then a terminating call (status != 0, typically
 * BLE_HS_EDONE) once all services are enumerated — at which point we start on the characteristics. */
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
    /* Services enumerated — enumerate characteristics service-by-service. */
    if (s_svc_cnt > 0) {
        s_disc_svc_i = 0;
        gatt_disc_chrs(0);
    } else {
        s_gatt_state = NOCSIF_BLE_GATT_READY;   /* no services (unusual) — nothing to browse */
        s_gatt_gen++;
    }
    return 0;
}

/* Characteristic discovery for one service; on its terminating status, advance to the next service
 * (or finish). arg carries the service index this batch belongs to. */
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
    while (rc != 0) {                            /* a start failure: skip so discovery still ends */
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
 * callback (gap_disc_event_cb). */
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
            scan_start();   /* resume device discovery so the pick list repopulates */
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

/* Resolve a flattened item index -> (service index, characteristic index or -1 for the service
 * header). Caller must hold s_gatt_mux. Returns false when idx is past the end. */
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

    scan_stop();                    /* must not be scanning to initiate a connection */
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
        /* the DISCONNECT event completes the transition + resumes the scan */
    } else if (s_gatt_state == NOCSIF_BLE_GATT_CONNECTING) {
        ble_gap_conn_cancel();      /* abort the in-flight connection attempt */
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
        /* Block until a command arrives — but while recording, wake every 100 ms to drain the PCAP
         * ring to /sd (the SD FATFS I/O runs here, never on the NimBLE host task). */
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
    xQueueSend(s_q, &c, 0);     /* non-blocking; a full queue drops the request (UI retries) */
}
static void post(ble_cmd_type_t type) { post_arg(type, 0); }

esp_err_t nocsif_ble_init(void)
{
    adv_config_load();          /* pull the saved advertise config in (harmless in safe mode) */
    phone_cfg_load();           /* + the phone-hub config + saved-phone metadata (LVGL task, once) */
    if (s_task != NULL) {
        return ESP_OK;          /* idempotent */
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
    /* Priority below the UI/system tasks; the host task does the reactive work, so the worker mostly
     * blocks on the queue. Not Task-WDT-subscribed (no unbounded spin). The 6144 stack (up from 4096)
     * gives FATFS room for the P4·3 advert PCAP, which drains to /sd on this task; allocated at boot
     * while internal RAM is plentiful, so it never competes with the tight BLE-active window. */
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

/* Notify one 8-byte boot-keyboard report on the input-report handle. Called from the DuckyScript
 * worker task (NimBLE's API is internally locked, so cross-task is safe). No-op if not subscribed. */
void nocsif_ble_hid_send_report(const uint8_t report[8])
{
    if (report == NULL || s_hid_conn == BLE_HS_CONN_HANDLE_NONE || !s_hid_subscribed) {
        return;
    }
    struct os_mbuf *om = ble_hs_mbuf_from_flat(report, 8);
    if (om == NULL) {
        return;                     /* mbuf pool exhausted — drop this report; the run continues */
    }
    int rc = ble_gatts_notify_custom(s_hid_conn, s_hid_input_val, om);
    if (rc != 0) {
        ESP_LOGW(TAG, "hid: notify rc=%d", rc);
    }
}

const char *nocsif_ble_hid_status_str(void)
{
    if (nocsif_reliability_safe_mode())  return "safe mode " BLE_DOT " keyboard off";
    if (s_want_hid && !s_synced)         return "starting " BLE_DOT " releasing WiFi" BLE_ELL;
    switch (s_hid_state) {
        case NOCSIF_HID_ADVERTISING: return "advertising " BLE_DOT " pair from the host's Bluetooth settings";
        case NOCSIF_HID_CONNECTED:   return "connected " BLE_DOT " confirm pairing on the host";
        case NOCSIF_HID_READY:       return "ready " BLE_DOT " typing works";
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
        post(CMD_PHONE_CONNECT);        /* arm + advertise so the last phone reconnects on its own */
    }
}

/* ---- boot-time controller reservation (BLE⇄WiFi coexistence) ------------------------ *
 * FRAGMENTATION, not shortage, is what blocks BLE once WiFi is up: the BT controller needs ~30 KB in
 * ONE run, and after WiFi has initialised the largest hole is stuck around 27 KB no matter how much
 * total memory is free (measured: freeing 10 KB moved `free` 28k->38k and left `largest` at 27648).
 * Allocation ORDER is therefore the fix — claim the controller's block at boot, while the heap is
 * still one big unfragmented run (~86 KB free just before esp_wifi_init), and let WiFi allocate
 * around it afterwards (its lean profile fits in what remains).
 *
 * Called from main.c BEFORE nocsif_wifi_init(). Blocks briefly (up to ~4 s) so the reservation is
 * actually done before WiFi starts grabbing memory — ordering is the entire point. Claims the controller
 * UNCONDITIONALLY (even when the Bluetooth master is OFF) so it is always resident and "Bluetooth on" is
 * an instant logical re-enable, never a reboot (operator constraint #1); OFF just means the resident
 * controller isn't advertising. Only safe mode skips it (the int-DMA-heavy path a crash-streak avoids). */
void nocsif_ble_boot_reserve(void)
{
    phone_cfg_load();
    s_boot_reserve_ran = true;          /* the reserve step ran BEFORE WiFi — wifi bring_up asserts this */
    if (nocsif_reliability_safe_mode()) {
        return;
    }
    post(CMD_BLE_RESERVE);              /* claim the controller block resident (advertise iff master on) */
    for (int i = 0; i < 80 && !s_host_up; i++) {
        vTaskDelay(pdMS_TO_TICKS(50));  /* ~4 s ceiling; normally well under 1 s */
    }
    nocsif_log_dma_free(s_host_up ? "boot reserve: NimBLE UP (controller block claimed, held for session)"
                                  : "boot reserve: NimBLE NOT up (gate refused — check init order)");
}

/* True once nocsif_ble_boot_reserve has run (i.e. the reserve step executed before WiFi). wifi.c
 * bring_up asserts this so a future init-order regression that starts WiFi first is caught loudly. */
bool nocsif_ble_boot_reserve_ran(void) { return s_boot_reserve_ran; }

bool nocsif_ble_bt_enabled(void) { phone_cfg_load(); return s_bt_master; }
/* Always false now: the controller is reserved at boot and held resident, so enabling Bluetooth never
 * needs a restart (operator constraint #1). Kept for API stability; the "restart the watch" UX is gone. */
bool nocsif_ble_needs_restart(void) { return false; }
void nocsif_ble_bt_set_enabled(bool on)
{
    phone_cfg_load();
    s_bt_master = on;                   /* reflect instantly for the toggle tag */
    nocsif_settings_set_i32("bt_master", on ? 1 : 0);
    post_arg(CMD_BT_ENABLE, on ? 1 : 0);
}

bool nocsif_ble_notif_enabled(void) { phone_cfg_load(); return s_notif_enabled; }
void nocsif_ble_notif_set_enabled(bool on)
{
    phone_cfg_load();
    s_notif_enabled = on;               /* reflect instantly; the gate reads it on the next connect */
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
        for (int a = 0; a < s_phone_cnt - 1; a++) {          /* n≤3: bubble by seq desc (most-recent first) */
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
    phone_meta_remove(addr);            /* instant list refresh + persist (LVGL-task NVS, like CC) */
    memcpy(s_forget_target.val, addr, 6);
    s_forget_target.type = addr_type;
    s_forget_pending = true;
    post(CMD_PHONE_FORGET);             /* worker: delete the bond + drop the live link if it's this one */
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

/* Map an internal table entry (copied out under the lock) into the public snapshot. No stack access. */
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
}

bool nocsif_ble_dev_get(int idx, nocsif_ble_dev_t *out)
{
    if (out == NULL) {
        return false;
    }
    ble_dev_t tmp;
    bool ok;
    portENTER_CRITICAL(&s_dev_mux);
    ok = (idx >= 0 && idx < s_dev_cnt && s_dev[idx].used);
    if (ok) {
        tmp = s_dev[idx];                       /* whole-struct copy under the lock */
    }
    portEXIT_CRITICAL(&s_dev_mux);
    if (!ok) {
        return false;
    }
    fill_dev_out(&tmp, out);
    return true;
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

/* A small table of the Bluetooth SIG company identifiers seen most in the field. Unknown ids show as
 * a raw hex code in the UI, so this only needs the common ones. */
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

/* Format a NimBLE UUID into "0xXXXX" (16-bit) or the full dashed string (128-bit). */
static void gatt_format_uuid(const ble_uuid_any_t *u, char *dst, size_t dstlen)
{
    char tmp[BLE_UUID_STR_LEN];
    ble_uuid_to_str(&u->u, tmp);
    strncpy(dst, tmp, dstlen - 1);
    dst[dstlen - 1] = '\0';
}

/* Format a read value: a couple of friendly decodes, else quoted text if printable, else hex. */
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
    if (k > 0) dst[k - 1] = '\0';                        /* drop the trailing space */
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
    /* if we're already advertising, restart so the new format takes effect */
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
