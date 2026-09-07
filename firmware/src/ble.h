/*
 * NocSif — Bluetooth LE (on-SoC 2.4 GHz, NimBLE host) worker + UI glue (P1 observer · P2 GATT · P3 adv).
 *
 * A thin worker over ESP-IDF NimBLE in the OBSERVER + CENTRAL + BROADCASTER roles — receive nearby BLE
 * advertisements and present the devices in range (P1), connect to one to explore its GATT attribute
 * database (P2), and transmit our own advertisement / beacon (P3, further down). Mirrors wifi.{c,h}:
 * all stack actions run on a dedicated worker
 * task; the LVGL callbacks only *request* an action (scan on/off). NimBLE runs its own host task,
 * and its GAP discovery callback (an advertisement report arrived) fires there — it does O(1) work,
 * copies the parsed fields out under a short spinlock, and publishes a snapshot the UI reads
 * lock-free. Nothing here touches LVGL; nothing blocks the LVGL task on the radio.
 *
 *   - Lazy bring-up on the first scan request (nimble_port_init + host task), so boot stays fast and
 *     the coexistence risk (BLE controller RAM vs the display-flush DMA path) is isolated to first use.
 *   - Safe-mode gated (nocsif_reliability_safe_mode): the radio is skipped after a boot loop, and
 *     nocsif_ble_available() stays false.
 *   - Single 2.4 GHz radio shared with WiFi via esp_coex (software coexistence).
 *
 * Scope every use to devices you are authorized to test. This is passive/active reception of the
 * advertisements every nearby device already broadcasts — the same discovery a phone does.
 *
 * The device-table getters return cached, module-owned snapshots (no stack I/O) so they are safe to
 * call from the LVGL task (the scan screen, its live rows, the menu-row tag).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One discovered BLE device (a snapshot copy for the UI; no stack access). */
typedef struct {
    uint8_t  addr[6];      /* device address, stored as NimBLE delivers it (val[0] = LSB)         */
    uint8_t  addr_type;    /* 0=public 1=random 2=public-id 3=random-id (see nocsif_ble_addr_type_str) */
    char     name[32];     /* NUL-terminated local name from adv / scan-response; "" if none      */
    int8_t   rssi;         /* last seen, dBm (negative; closer to 0 = stronger)                   */
    int8_t   tx_pwr;       /* advertised TX-power level, dBm (valid only when tx_pwr_ok)          */
    bool     tx_pwr_ok;
    uint16_t appearance;   /* GAP appearance code (0 = not advertised)                            */
    bool     appearance_ok;
    uint16_t company;      /* company identifier from manufacturer data (0xFFFF = none)           */
    bool     company_ok;
    uint16_t uuid16;       /* first advertised 16-bit service UUID (0 = none / only 128-bit)      */
    uint8_t  n_uuid;       /* count of advertised service UUIDs (16- + 32- + 128-bit)             */
    bool     connectable;  /* the advertisement invites a connection (from the PDU type)          */
    uint8_t  tracker;      /* item-tracker class if recognized (nocsif_ble_tracker_t; 0 = none)   */
    uint16_t frames;       /* advertisement reports seen from this device                         */
    uint32_t age_ms;       /* since last seen (staleness)                                         */
} nocsif_ble_dev_t;

/* Create the idle worker task. Idempotent; safe to call from the LVGL task as the lazy trigger on
 * first BLE-screen entry. In reliability safe mode this is a no-op and nocsif_ble_available() stays
 * false. Returns ESP_OK once the task exists (or in safe mode). Does NOT bring the stack up — that
 * happens on the first scan request. */
esp_err_t nocsif_ble_init(void);

/* Start / stop device discovery (the Scan screen's Start/Stop). Non-blocking: only signals the
 * worker. The controller is resident (reserved at boot), so this just toggles a scan on it — no radio
 * release, no bring-up race. Gated on reliability safe mode. */
void nocsif_ble_request_scan(bool on);

/* Leave a BLE screen: QUIESCE — stop the screen's transient recon but KEEP the controller resident (it
 * is never torn down at runtime; the reserved block can't be re-claimed once WiFi is up). The persistent
 * phone link is preserved / re-armed. Call when a BLE screen is left. Non-blocking. */
void nocsif_ble_request_release(void);

/* ---- published state (no stack I/O; safe on the LVGL task) -------------------------- */

/* False in safe mode or if NimBLE failed to initialise; true once the host stack is synced and up.
 * (Before the first scan request it is false — bring-up is lazy.) */
bool nocsif_ble_available(void);

/* True while discovery is running. */
bool nocsif_ble_scan_active(void);

/* True during bring-up (releasing WiFi + initialising NimBLE) — the ~2-5 s window before the first
 * scan. Lets the UI show "starting…" instead of "off". */
bool nocsif_ble_starting(void);

/* Compact status for the Scan screen header ("off" / "scanning…" / "N seen" / "safe mode" / "err"). */
const char *nocsif_ble_status_str(void);

/* Compact live tag for the BLE-menu "Scan Devices" row ("ready" / "scan" / "N seen"). Module-owned. */
const char *nocsif_ble_scan_tag_str(void);

/* ---- discovered-device snapshot (lock-free for the reader) -------------------------- */

/* Number of devices in the table (0 before the first advertisement). Kept first-seen for stable
 * rows; the stalest entry is evicted when full. */
int nocsif_ble_dev_count(void);

/* A generation that bumps on a STRUCTURAL change (a new device appears or one is evicted), so the
 * list screen rebuilds its rows only when the set changes (field-only updates don't churn it). */
uint32_t nocsif_ble_dev_gen(void);

/* Copy device `idx` (0..count-1) into `out`. Returns false if `idx` is out of range. Copies the
 * entry under a short spinlock (the worker/host task never blocks the reader for long). */
bool nocsif_ble_dev_get(int idx, nocsif_ble_dev_t *out);

/* ---- lookups (keep NimBLE + the company table inside ble.c) ------------------------- */

/* Human label for an address type ("public" / "random" / "public-id" / "random-id"). */
const char *nocsif_ble_addr_type_str(uint8_t addr_type);

/* Best-effort short vendor name for a Bluetooth SIG company identifier ("Apple", "Google", "Tile",
 * …), or "" when unknown. A small built-in table of the identifiers seen most in the field. */
const char *nocsif_ble_company_str(uint16_t company);

/* ---- item-tracker detection (M7-P4·1) ---------------------------------------------- *
 * Recognize the advertisement signatures of common item trackers so a "who's following me" view can
 * surface them. Purely receive-side classification of the adverts the tracker already broadcasts. */
typedef enum {
    NOCSIF_BLE_TRACKER_NONE = 0,
    NOCSIF_BLE_TRACKER_FINDMY,     /* Apple Find My / AirTag (offline-finding beacon)    */
    NOCSIF_BLE_TRACKER_TILE,       /* Tile                                              */
    NOCSIF_BLE_TRACKER_SMARTTAG,   /* Samsung Galaxy SmartTag                           */
} nocsif_ble_tracker_t;

/* Short label for a tracker class ("Find My" / "Tile" / "SmartTag"), or "" for NONE. */
const char *nocsif_ble_tracker_str(uint8_t tracker);

/* Number of discovered devices classified as a tracker (a filtered view of the device table). */
int nocsif_ble_tracker_count(void);

/* Copy the `idx`-th tracker device (0..nocsif_ble_tracker_count()-1) into `out`. False if out of
 * range. Same snapshot semantics as nocsif_ble_dev_get. */
bool nocsif_ble_tracker_get(int idx, nocsif_ble_dev_t *out);

/* Compact live tag for the BLE-menu "Nearby Trackers" row ("scan" / "N found" / "clear" / "—"). */
const char *nocsif_ble_tracker_tag_str(void);

/* ---- Signal Hunt (M7-P4·2): live-RSSI proximity hunt ------------------------------- *
 * Pin one scanned device as the hunt target; its RSSI (smoothed) drives a proximity gradient that
 * strengthens as you close in ("warmer / colder"). Receive-side only — the same signal-strength read
 * a phone shows. The haptic (eyes-free) + IMU rotation-sweep bearing layers land with M11. */
typedef struct {
    bool     active;    /* a target is pinned                                */
    int16_t  rssi;      /* last raw RSSI, dBm                                */
    int16_t  smoothed;  /* EMA-smoothed RSSI, dBm (the gradient value)       */
    int16_t  peak;      /* strongest (closest) RSSI seen since pinned, dBm   */
    uint32_t age_ms;    /* since the target was last heard (staleness)       */
    uint16_t frames;    /* adverts heard from the target since pinned        */
} nocsif_ble_hunt_t;

/* Pin / unpin the hunt target (by address; `name` is a label copy, may be ""). */
void nocsif_ble_hunt_set_target(const uint8_t addr[6], uint8_t addr_type, const char *name);
void nocsif_ble_hunt_clear(void);
bool nocsif_ble_hunt_active(void);

/* Copy the live hunt snapshot. Returns false (and leaves *out zeroed) if no target is pinned. */
bool nocsif_ble_hunt_snapshot(nocsif_ble_hunt_t *out);

/* Label of the pinned target (name or address), and the BLE-menu row tag. */
const char *nocsif_ble_hunt_target_str(void);
const char *nocsif_ble_hunt_tag_str(void);

/* ---- advert PCAP export (M7-P4·3): record adverts to microSD ------------------------ *
 * While the observer scans, write every received advertisement to /sd/nocsif/ble/adv-NNN.pcap as a
 * Bluetooth LE link-layer frame with a pseudo-header (LINKTYPE_BLUETOOTH_LE_LL_WITH_PHDR = 256) so it
 * decodes natively in Wireshark — address, RSSI, and the parsed AD structures. Receive-side only; the
 * card is retrieved over USB File Share. Fidelity note: the observer hands us the assembled adv report
 * (not the raw over-the-air bits), so the channel is nominal and the CRC is left unchecked. */
void nocsif_ble_request_pcap(bool on);      /* arm / stop recording (idempotent)               */
bool     nocsif_ble_pcap_active(void);      /* file open + capturing                            */
uint32_t nocsif_ble_pcap_frames(void);      /* adverts written this session                     */
uint32_t nocsif_ble_pcap_bytes(void);       /* bytes written (incl. headers)                    */
uint32_t nocsif_ble_pcap_dropped(void);     /* adverts dropped (ring full)                      */
const char *nocsif_ble_pcap_path(void);     /* current/last file path ("" until first record)   */
const char *nocsif_ble_pcap_status_str(void);  /* "rec" / "off" / "no card" / "file share" / "err" */
const char *nocsif_ble_pcap_tag_str(void);     /* BLE-menu row tag                              */

/* ---- Drone Detection (M7-P4·4): OpenDroneID / ASTM F3411 Remote ID reception -------- *
 * A compliant drone broadcasts a public Remote ID — identity, its own position, and the operator
 * (pilot) position. Over Bluetooth it rides a Service Data AD (UUID 0xFFFA); BT4-legacy adverts
 * carry one 25-byte ODID message each, cycling identity / location / operator, accumulated per drone
 * here. Receive-side airspace awareness only — no transmit, no interaction. */
typedef struct {
    uint8_t  addr[6];
    int8_t   rssi;
    uint32_t age_ms;             /* since last heard                                    */
    uint16_t msgs;               /* ODID messages parsed from this drone                */
    /* Basic ID */
    bool     has_basic;
    uint8_t  id_type;            /* 1=serial 2=CAA reg 3=UTM(UUID) 4=session            */
    uint8_t  ua_type;            /* UA category (nocsif_ble_ua_type_str)                */
    char     uas_id[21];         /* UAS ID (serial / registration), ASCII               */
    /* Location / Vector — the drone's own position + motion */
    bool     has_loc;
    int32_t  lat_e7, lon_e7;     /* deg * 1e7                                           */
    int16_t  alt_m;              /* geodetic altitude, m                                */
    uint16_t speed_x10;          /* ground speed, m/s * 10                              */
    uint16_t track_deg;          /* course over ground, 0-359                           */
    /* System — the operator (pilot) position */
    bool     has_operator_loc;
    int32_t  op_lat_e7, op_lon_e7;
    /* Operator ID */
    bool     has_operator_id;
    char     operator_id[21];
} nocsif_ble_drone_t;

int  nocsif_ble_drone_count(void);
bool nocsif_ble_drone_get(int i, nocsif_ble_drone_t *out);   /* snapshot by index; false if none */
uint32_t nocsif_ble_drone_gen(void);
const char *nocsif_ble_drone_tag_str(void);                  /* BLE-menu row tag (drone count)   */
const char *nocsif_ble_ua_type_str(uint8_t ua_type);         /* "Multirotor" etc.                */

/* ============================ Phone Notifications (M7, ANCS, PERIPHERAL role) ============= *
 * Mirror the phone's notifications over Apple's Notification Center Service. The watch advertises as
 * a connectable peripheral soliciting ANCS; the phone connects + bonds (pair once from iOS Bluetooth
 * settings — the bond persists in NVS) and exposes ANCS; the watch, now the GATT *client*, subscribes
 * to the Notification Source + Data Source and fetches each notification's app / title / message.
 * Receive-only for this phase (no dismiss/actions). Foreground-only: the single radio is held while
 * the screen is open (like GATT explore), so it mirrors while you're on the notifications screen. */
typedef enum {
    NOCSIF_ANCS_IDLE = 0,       /* not started                                     */
    NOCSIF_ANCS_ADVERTISING,    /* waiting for the phone to connect                */
    NOCSIF_ANCS_CONNECTED,      /* linked; pairing + discovering ANCS               */
    NOCSIF_ANCS_READY,          /* subscribed; mirroring notifications              */
    NOCSIF_ANCS_FAILED,         /* pairing / discovery failed                       */
} nocsif_ble_ancs_state_t;

typedef struct {
    uint32_t uid;               /* ANCS NotificationUID                            */
    uint8_t  category;          /* ANCS CategoryID (nocsif_ble_ancs_category_str)   */
    uint32_t age_ms;            /* since received                                   */
    bool     have_attrs;        /* app / title / message fetched from Data Source    */
    char     app[28];           /* app identifier (bundle id)                       */
    char     title[40];
    char     message[100];
} nocsif_ble_ancs_notif_t;

void nocsif_ble_ancs_request_start(void);   /* enter notifications mode (advertise + bond + mirror) */
void nocsif_ble_ancs_request_forget(void);  /* erase the bond (unpair the phone)                   */
nocsif_ble_ancs_state_t nocsif_ble_ancs_state(void);
const char *nocsif_ble_ancs_status_str(void);
int  nocsif_ble_ancs_count(void);
bool nocsif_ble_ancs_get(int i, nocsif_ble_ancs_notif_t *out);   /* newest first */
uint32_t nocsif_ble_ancs_gen(void);
const char *nocsif_ble_ancs_category_str(uint8_t cat);           /* "Message" / "Email" / …        */
const char *nocsif_ble_ancs_tag_str(void);                       /* BLE-menu row tag               */

/* ============================ Media remote (M7, AMS) ====================================== *
 * Apple Media Service — the media-control sibling of ANCS, discovered on the SAME bonded phone link
 * (whichever screen brings the link up gets both). The watch reads now-playing over Entity Update and
 * drives playback over Remote Command. No new pairing; rides the ANCS bond. */
typedef struct {
    bool     connected;         /* AMS discovered + subscribed on the phone link   */
    bool     playing;           /* PlaybackInfo state == Playing                    */
    int      volume_pct;        /* 0..100, or -1 if unknown                         */
    char     title[64];         /* now-playing track title                          */
    char     artist[48];        /* now-playing artist                               */
    uint32_t gen;               /* bumps on any change (poll to refresh the UI)     */
} nocsif_ble_ams_t;

/* AMS Remote Command ids (pass to nocsif_ble_ams_cmd). */
#define NOCSIF_AMS_CMD_PLAY     0
#define NOCSIF_AMS_CMD_PAUSE    1
#define NOCSIF_AMS_CMD_TOGGLE   2   /* play/pause toggle */
#define NOCSIF_AMS_CMD_NEXT     3
#define NOCSIF_AMS_CMD_PREV     4
#define NOCSIF_AMS_CMD_VOL_UP   5
#define NOCSIF_AMS_CMD_VOL_DN   6

void        nocsif_ble_ams_cmd(int cmd_id);        /* send a Remote Command (NOCSIF_AMS_CMD_*)      */
void        nocsif_ble_ams_set_volume(int pct);    /* step the phone volume toward pct (0..100)     */
bool        nocsif_ble_ams_get(nocsif_ble_ams_t *out);   /* now-playing snapshot (LVGL-task-safe)   */
uint32_t    nocsif_ble_ams_gen(void);              /* change counter                                */
const char *nocsif_ble_ams_tag_str(void);          /* BLE-menu row tag ("—" / "playing" / "paused") */

/* ============================ Phone companion hub (M7, "Connect Phone" screen) ============= *
 * Lifecycle + persistence around the ONE bonded iOS link (ANCS notifications + AMS media ride it). The
 * watch is the BLE *peripheral*: "connect" means advertise soliciting ANCS so a bonded iPhone reconnects
 * (open re-arm — whichever saved phone is nearest, no directed advertising). A persisted "Bluetooth"
 * master governs whether the phone link auto-connects at all (boot + in-range); it does NOT gate the BLE
 * recon screens (scan / GATT / advertise / HID), which stay lazy. Up to NOCSIF_BLE_PHONE_MAX phones are
 * remembered (bond store holds MAX_BONDS); one live link at a time (MAX_CONNECTIONS=1). All getters are
 * LVGL-task-safe (RAM-cached; no stack I/O); the setters post to the worker. */
#define NOCSIF_BLE_PHONE_MAX 3      /* saved phones we track metadata for (matches BT_NIMBLE_MAX_BONDS) */

/* Bluetooth master (persisted, default ON). OFF releases the phone link + hands the radio back to WiFi
 * and disarms auto-connect; ON arms it (advertise + reconnect + boot-connect). */
bool nocsif_ble_bt_enabled(void);
void nocsif_ble_bt_set_enabled(bool on);

/* DEPRECATED — always returns false. The controller is reserved at boot and held resident for the whole
 * session, so turning Bluetooth on at runtime is an instant LOGICAL re-enable that never needs a restart
 * (operator constraint #1). Kept only for call-site/API stability; the "restart the watch" UX is gone. */
bool nocsif_ble_needs_restart(void);

/* Manually (re)connect / drop the phone link. Connect turns the master ON if needed, then advertises so
 * the nearest saved phone reconnects. Disconnect terminates the current link (with the master ON the
 * disconnect handler re-advertises → it reconnects in range). Non-blocking. */
void nocsif_ble_phone_connect(void);
void nocsif_ble_phone_disconnect(void);

/* Boot hook (main.c): if the master is on and ≥1 phone is remembered, arm + advertise so the last phone
 * reconnects without opening the screen. Non-blocking; a no-op in safe mode or with the master off. */
void nocsif_ble_phone_boot_autostart(void);

/* Boot hook, called BEFORE nocsif_wifi_init(): claim the BT controller's ~30 KB CONTIGUOUS internal-DMA
 * block while the heap is still unfragmented. Once WiFi has initialised, the largest free hole stays
 * around 27 KB however much total memory is free — so BLE must be brought up FIRST or not at all.
 * Blocks up to ~4 s (ordering is the point). No-op when the Bluetooth master is off or in safe mode. */
void nocsif_ble_boot_reserve(void);

/* True once nocsif_ble_boot_reserve has executed (the reserve step ran before WiFi). wifi.c bring_up
 * asserts this so a future init-order regression that brings WiFi up first is caught with a loud log. */
bool nocsif_ble_boot_reserve_ran(void);

/* Phone-notifications gate (persisted, default ON). OFF keeps the link + media but stops mirroring
 * notifications (skips / unsubscribes the ANCS Notification Source). Applies live when connected. */
bool nocsif_ble_notif_enabled(void);
void nocsif_ble_notif_set_enabled(bool on);

/* ---- saved phones (bonded peers + our recency / name metadata) ---------------------- */
typedef struct {
    uint8_t  addr[6];      /* identity address (as stored in the bond)                  */
    uint8_t  addr_type;    /* identity address type (0 public / 1 random)               */
    char     name[32];     /* friendly label (phone GAP name, best-effort) or "Phone …" */
    uint32_t seq;          /* recency rank (higher = more recently connected)           */
    bool     connected;    /* this peer is the live link right now                      */
} nocsif_ble_phone_t;

/* Count of remembered phones (0..NOCSIF_BLE_PHONE_MAX). Does NOT need the stack up (reads our NVS
 * metadata), so main.c can gate the boot auto-connect on it. */
int  nocsif_ble_phone_count(void);
/* Copy the i-th saved phone (0..count-1), ordered MOST-RECENT FIRST. False if out of range. */
bool nocsif_ble_phone_get(int i, nocsif_ble_phone_t *out);
/* Forget one phone: delete its bond + our metadata, and drop the live link if it is this peer. */
void nocsif_ble_phone_forget(const uint8_t addr[6], uint8_t addr_type);

/* ============================ GATT explore (M7-P2, CENTRAL role) ============================ *
 * Connect to one nearby device (chosen from the scan table) and walk its attribute database:
 * discover every service, then the characteristics under each, then read a value on request. All
 * NimBLE GATT-client work runs on the worker + host tasks; the LVGL side only *requests* (connect /
 * disconnect / read) and reads the flattened, lock-free snapshot below. Read-only exploration — the
 * same discovery a phone's Bluetooth settings performs. Scope to devices you are authorized to test.
 *
 * The single-radio rule from P1 still holds: the GATT screen brings NimBLE up (releasing WiFi) via the
 * same request path, connects, explores, and hands the radio back on exit. */

/* Where the GATT screen is in the connect→explore lifecycle. */
typedef enum {
    NOCSIF_BLE_GATT_IDLE = 0,     /* not connected — the device-pick list is live                */
    NOCSIF_BLE_GATT_CONNECTING,   /* central link establishment in progress                      */
    NOCSIF_BLE_GATT_DISCOVERING,  /* connected; discovering services + characteristics           */
    NOCSIF_BLE_GATT_READY,        /* connected; discovery complete — the tree is browsable       */
    NOCSIF_BLE_GATT_DISCONNECTING,/* tearing the link down                                       */
} nocsif_ble_gatt_state_t;

/* One row of the flattened GATT view: either a service header or a characteristic beneath it. The UI
 * renders these top-to-bottom (each service immediately followed by its characteristics). */
typedef struct {
    bool     is_service;    /* true = service header; false = characteristic                      */
    uint8_t  svc_index;     /* owning service (0-based) — same value on the header and its chrs   */
    uint16_t handle;        /* service start-handle, or characteristic value-handle              */
    uint8_t  props;         /* characteristic properties bitfield (0 on a service header)         */
    bool     readable;      /* props includes READ (so "tap to read" applies)                    */
    bool     writable;      /* props includes WRITE or WRITE-without-response                    */
    bool     notifiable;    /* props includes NOTIFY or INDICATE                                 */
    char     uuid[40];      /* formatted UUID ("0x180F" for 16-bit, full string for 128-bit)     */
    char     label[28];     /* well-known SIG name, or "" if not in the table                    */
    bool     has_value;     /* a read has completed for this characteristic                      */
    char     value[48];     /* formatted last-read value (decoded, else hex + printable ASCII)   */
} nocsif_ble_gatt_item_t;

/* Connect to scan-table device `dev_idx` (0..nocsif_ble_dev_count()-1) and explore its GATT server.
 * Cancels discovery and initiates a central connection; non-blocking (the worker does the work). */
void nocsif_ble_gatt_connect(int dev_idx);

/* Terminate the GATT connection and return to the device-pick list (resumes discovery). Non-blocking. */
void nocsif_ble_gatt_disconnect(void);

/* Request a read of the characteristic value at flattened item `idx`. A no-op if `idx` is not a
 * readable characteristic. The value lands in that item's `value` field (has_value flips true). */
void nocsif_ble_gatt_read(int idx);

/* Current lifecycle state, and compact strings for the screen header + the connected peer's label. */
nocsif_ble_gatt_state_t nocsif_ble_gatt_state(void);
const char *nocsif_ble_gatt_state_str(void);
const char *nocsif_ble_gatt_target_str(void);

/* Flattened GATT view (services interleaved with their characteristics). Lock-free for the reader. */
int  nocsif_ble_gatt_item_count(void);
/* Bumps on any STRUCTURAL change (a service/characteristic appears, a read completes, connect/
 * disconnect). Lets the list screen refresh only when the view actually changed. */
uint32_t nocsif_ble_gatt_gen(void);
/* Copy flattened item `idx` into `out` (short spinlock). Returns false if `idx` is out of range. */
bool nocsif_ble_gatt_item_get(int idx, nocsif_ble_gatt_item_t *out);

/* Compact live tag for the BLE-menu "GATT Explore" row ("connect" / "linking" / "N services" / "—"). */
const char *nocsif_ble_gatt_tag_str(void);

/* Best-effort short name for a well-known 16-bit SIG UUID (service or characteristic), or "" when not
 * in the table (unknown UUIDs show their raw value in the UI). */
const char *nocsif_ble_gatt_uuid_name(uint16_t uuid16);

/* ============================ Advertise / Beacon (M7-P3, BROADCASTER role) ============================ *
 * Transmit a non-connectable BLE advertisement — the watch becomes a beacon other scanners see. Two
 * formats: a plain NAMED advertisement (a chosen local name, visible in any BLE scanner / phone) and
 * Apple's iBeacon (a fixed NocSif proximity UUID + major/minor, detectable by iBeacon apps). This is
 * the first BLE *transmitting* op — the same broadcast a fitness tag or store beacon emits. Scope to
 * your own space. Brings NimBLE up (releasing WiFi) on start, like the scan path (single radio). */

typedef enum {
    NOCSIF_BLE_ADV_CUSTOM = 0,   /* named non-connectable advertisement (the chosen local name)   */
    NOCSIF_BLE_ADV_IBEACON,      /* Apple iBeacon (fixed NocSif UUID + major/minor)               */
} nocsif_ble_adv_mode_t;

/* Start / stop broadcasting with the current config. Start brings NimBLE up on first use (releasing
 * WiFi); non-blocking (the worker does the work). */
void nocsif_ble_adv_start(void);
void nocsif_ble_adv_stop(void);
bool nocsif_ble_adv_active(void);
/* True during the bring-up window (releasing WiFi + NimBLE init) before the advert actually starts. */
bool nocsif_ble_adv_starting(void);

/* Config (persisted to NVS; applied on the next start — change it, then Start). */
nocsif_ble_adv_mode_t nocsif_ble_adv_mode(void);
void        nocsif_ble_adv_set_mode(nocsif_ble_adv_mode_t mode);
const char *nocsif_ble_adv_name(void);              /* advertised local name (Custom mode)          */
void        nocsif_ble_adv_set_name(const char *name);

const char *nocsif_ble_adv_mode_str(void);          /* "named" / "iBeacon"                          */
const char *nocsif_ble_adv_status_str(void);        /* screen header                                */
const char *nocsif_ble_adv_tag_str(void);           /* BLE-menu "Advertise / Beacon" row tag        */

/* ============================ BLE HID keyboard (M7, PERIPHERAL + GATT server) ============== *
 * The watch becomes a Bluetooth keyboard (HID-over-GATT / HOGP): it advertises with the keyboard
 * appearance + HID service, a host (PC / Mac / phone) pairs and bonds (Just Works), and the watch
 * types by notifying 8-byte boot-keyboard reports — the wireless twin of the M4 USB DuckyScript. The
 * DuckyScript engine (ducky.c + the hid_kbd keymap) is reused unchanged over a BLE transport sink.
 *
 * Single-radio + single-connection rule: MAX_CONNECTIONS=1, so the keyboard link is MUTUALLY
 * EXCLUSIVE with the phone-companion (ANCS/AMS) link and with WiFi. Entering keyboard mode tears the
 * phone link down and takes the radio; leaving the screen releases it back to WiFi. This is the first
 * GATT *server* on the device (ANCS/AMS make the watch a GATT client). Scope to hosts you own. */
typedef enum {
    NOCSIF_HID_IDLE = 0,       /* not started                                     */
    NOCSIF_HID_ADVERTISING,    /* advertising as a keyboard, waiting for a host    */
    NOCSIF_HID_CONNECTED,      /* linked; pairing / subscribing                    */
    NOCSIF_HID_READY,          /* bonded + subscribed — typing works              */
    NOCSIF_HID_FAILED,         /* pairing failed                                   */
} nocsif_ble_hid_state_t;

/* Enter keyboard mode: release the phone link + WiFi and advertise as a BLE keyboard. Non-blocking
 * (the worker does the bring-up). Gated on reliability safe mode. */
void nocsif_ble_hid_request_start(void);

nocsif_ble_hid_state_t nocsif_ble_hid_state(void);

/* True when a host is connected, bonded (encrypted), and subscribed to the input report — i.e. a
 * keystroke sent now will be delivered. The typing engine (hid_kbd BLE sink) polls this. */
bool nocsif_ble_hid_ready(void);

/* Notify one 8-byte boot-keyboard report ([modifier, 0, k1..k6]) to the subscribed host. Called from
 * the DuckyScript worker task (NimBLE's API is internally locked); a no-op if not ready. */
void nocsif_ble_hid_send_report(const uint8_t report[8]);

const char *nocsif_ble_hid_status_str(void);   /* screen header                                    */
const char *nocsif_ble_hid_tag_str(void);      /* BLE-menu "BLE Keyboard" row tag                  */

#ifdef __cplusplus
}
#endif
