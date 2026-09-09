/*
 * NocSif — Bluetooth LE (on-SoC 2.4 GHz, NimBLE host) worker + UI glue (P1 observer · P2 GATT · P3 adv).
 *
 * A thin wrapper over ESP-IDF's NimBLE stack in the observer + central + broadcaster roles:
 * receive nearby BLE advertisements and list the devices in range (P1), connect to one and browse
 * its GATT attribute database (P2), and transmit our own advertisement/beacon (P3, described
 * further down). Mirrors the shape of wifi.{c,h}: every stack action runs on a dedicated worker
 * task, and LVGL callbacks only *request* an action (like scan on/off) rather than touching the
 * stack directly. NimBLE runs its own host task, and its GAP discovery callback (an advertisement
 * arrived) fires there — it does O(1) work, copies the parsed fields out under a short spinlock,
 * and publishes a snapshot the UI can read lock-free. Nothing here touches LVGL, and nothing here
 * ever blocks the LVGL task on the radio.
 *
 *   - Bring-up (nimble_port_init + the host task) is lazy, triggered by the first scan request, so
 *     boot stays fast and the coexistence risk (BLE controller RAM competing with the display-flush
 *     DMA path) is confined to first use.
 *   - Gated by reliability safe mode: after a boot loop the radio is skipped entirely and
 *     nocsif_ble_available() stays false.
 *   - Shares the single 2.4 GHz radio with WiFi via esp_coex software coexistence.
 *
 * Scope every use to devices you're authorized to test. This receives only the advertisements
 * nearby devices are already broadcasting — the same passive/active discovery a phone performs.
 *
 * The device-table getters below return cached, module-owned snapshots with no stack I/O, so
 * they're safe to call directly from the LVGL task (the scan screen, its live rows, the menu tag).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One discovered BLE device, as a snapshot copy for the UI — no stack access here. */
typedef struct {
    uint8_t  addr[6];      /* device address, as NimBLE delivers it (val[0] = LSB)                */
    uint8_t  addr_type;    /* 0=public 1=random 2=public-id 3=random-id (see nocsif_ble_addr_type_str) */
    char     name[32];     /* NUL-terminated local name from the adv/scan-response; "" if none    */
    int8_t   rssi;         /* last seen, dBm (negative; closer to 0 means stronger)               */
    int8_t   tx_pwr;       /* advertised TX-power level, dBm (only valid when tx_pwr_ok)           */
    bool     tx_pwr_ok;
    uint16_t appearance;   /* GAP appearance code (0 = not advertised)                             */
    bool     appearance_ok;
    uint16_t company;      /* company identifier from manufacturer data (0xFFFF = none)            */
    bool     company_ok;
    uint16_t uuid16;       /* first advertised 16-bit service UUID (0 = none or only 128-bit)      */
    uint8_t  n_uuid;       /* count of advertised service UUIDs (16- + 32- + 128-bit)              */
    bool     connectable;  /* the advertisement invites a connection (from the PDU type)           */
    uint8_t  tracker;      /* item-tracker class if recognized (nocsif_ble_tracker_t; 0 = none)    */
    uint16_t frames;       /* advertisement reports seen from this device                          */
    uint32_t age_ms;       /* time since last seen (staleness)                                     */
} nocsif_ble_dev_t;

/* Creates the idle worker task. Idempotent; safe to call from the LVGL task as the lazy trigger on
 * first entering a BLE screen. In reliability safe mode it's a no-op and nocsif_ble_available()
 * stays false. Returns ESP_OK once the task exists (or in safe mode). Does NOT bring up the radio
 * stack itself — that happens on the first scan request. */
esp_err_t nocsif_ble_init(void);

/* Starts/stops device discovery (the Scan screen's Start/Stop control). Non-blocking — just
 * signals the worker. The controller is already resident (claimed at boot), so this only toggles
 * scanning on it — no radio release, no bring-up race. Gated on reliability safe mode. */
void nocsif_ble_request_scan(bool on);

/* Leaves a BLE screen: quiesces the screen's transient scan/recon but keeps the controller
 * resident — it's never torn down at runtime, since the reserved block can't be re-claimed once
 * WiFi is up. Any persistent phone link is preserved or re-armed. Call this when a BLE screen is
 * left. Non-blocking. */
void nocsif_ble_request_release(void);

/* ---- published state (no stack I/O; safe on the LVGL task) -------------------------- */

/* False in safe mode, or if NimBLE failed to initialize; true once the host stack is synced and
 * up. Stays false before the first scan request too, since bring-up is lazy. */
bool nocsif_ble_available(void);

/* True while discovery is running. */
bool nocsif_ble_scan_active(void);

/* True during bring-up (releasing WiFi and initializing NimBLE) — the ~2-5 s window before the
 * first scan actually starts. Lets the UI show "starting…" instead of "off". */
bool nocsif_ble_starting(void);

/* Compact status string for the Scan screen header ("off" / "scanning…" / "N seen" / "safe mode" / "err"). */
const char *nocsif_ble_status_str(void);

/* Compact live tag for the BLE-menu "Scan Devices" row ("ready" / "scan" / "N seen"). Module-owned. */
const char *nocsif_ble_scan_tag_str(void);

/* ---- discovered-device snapshot (lock-free for the reader) -------------------------- */

/* Number of devices currently in the table (0 before the first advertisement is seen). Entries
 * keep their first-seen order for stable rows; the stalest one is evicted once the table fills. */
int nocsif_ble_dev_count(void);

/* A generation counter that bumps only on a STRUCTURAL change (a device appears or is evicted), so
 * the list screen only rebuilds its rows when the set actually changes — field-only updates don't
 * force a rebuild. */
uint32_t nocsif_ble_dev_gen(void);

/* Copies device `idx` (0..count-1) into `out`. Returns false if idx is out of range. The copy is
 * made under a short spinlock, so the worker/host task is never blocked for long by a reader. */
bool nocsif_ble_dev_get(int idx, nocsif_ble_dev_t *out);

/* ---- lookups (keep NimBLE + the company table inside ble.c) ------------------------- */

/* Human-readable label for an address type ("public" / "random" / "public-id" / "random-id"). */
const char *nocsif_ble_addr_type_str(uint8_t addr_type);

/* Best-effort short vendor name for a Bluetooth SIG company identifier ("Apple", "Google", "Tile",
 * …), or "" when unknown. Backed by a small built-in table of the identifiers seen most often. */
const char *nocsif_ble_company_str(uint16_t company);

/* ---- item-tracker detection (M7-P4·1) ---------------------------------------------- *
 * Recognizes the advertisement signatures common item trackers use, so a "who's following me" view
 * can surface them. Purely a receive-side classification of adverts the tracker is already
 * broadcasting. */
typedef enum {
    NOCSIF_BLE_TRACKER_NONE = 0,
    NOCSIF_BLE_TRACKER_FINDMY,     /* Apple Find My / AirTag (offline-finding beacon)    */
    NOCSIF_BLE_TRACKER_TILE,       /* Tile                                              */
    NOCSIF_BLE_TRACKER_SMARTTAG,   /* Samsung Galaxy SmartTag                           */
} nocsif_ble_tracker_t;

/* Short label for a tracker class ("Find My" / "Tile" / "SmartTag"), or "" for NONE. */
const char *nocsif_ble_tracker_str(uint8_t tracker);

/* Number of discovered devices classified as a tracker — a filtered view over the device table. */
int nocsif_ble_tracker_count(void);

/* Copies the `idx`-th tracker device (0..nocsif_ble_tracker_count()-1) into `out`. Returns false
 * if out of range. Same snapshot semantics as nocsif_ble_dev_get. */
bool nocsif_ble_tracker_get(int idx, nocsif_ble_dev_t *out);

/* Compact live tag for the BLE-menu "Nearby Trackers" row ("scan" / "N found" / "clear" / "—"). */
const char *nocsif_ble_tracker_tag_str(void);

/* ---- Signal Hunt (M7-P4·2): live-RSSI proximity hunt ------------------------------- *
 * Pins one scanned device as the hunt target; its smoothed RSSI drives a proximity gradient that
 * strengthens as you close in ("warmer / colder"). Receive-side only — the same signal-strength
 * reading a phone would show. The haptic (eyes-free) and IMU rotation-sweep bearing layers arrive
 * with M11. */
typedef struct {
    bool     active;    /* a target is pinned                                */
    int16_t  rssi;      /* last raw RSSI, dBm                                */
    int16_t  smoothed;  /* EMA-smoothed RSSI, dBm (the gradient value)       */
    int16_t  peak;      /* strongest (closest) RSSI seen since pinned, dBm   */
    uint32_t age_ms;    /* time since the target was last heard (staleness)  */
    uint16_t frames;    /* adverts heard from the target since pinned        */
} nocsif_ble_hunt_t;

/* Pins/unpins the hunt target by address (`name` is just a label copy, and may be ""). */
void nocsif_ble_hunt_set_target(const uint8_t addr[6], uint8_t addr_type, const char *name);
void nocsif_ble_hunt_clear(void);
bool nocsif_ble_hunt_active(void);

/* Copies the live hunt snapshot. Returns false (and zeroes *out) if no target is pinned. */
bool nocsif_ble_hunt_snapshot(nocsif_ble_hunt_t *out);

/* Label of the pinned target (name or address), and the BLE-menu row tag. */
const char *nocsif_ble_hunt_target_str(void);
const char *nocsif_ble_hunt_tag_str(void);

/* ---- advert PCAP export (M7-P4·3): record adverts to microSD ------------------------ *
 * While the observer scans, writes every received advertisement to
 * /sd/nocsif/ble/adv-NNN.pcap as a Bluetooth LE link-layer frame with a pseudo-header
 * (LINKTYPE_BLUETOOTH_LE_LL_WITH_PHDR = 256), so the file opens natively in Wireshark with
 * address, RSSI, and the parsed AD structures intact. Receive-side only; the card is retrieved
 * over USB File Share. Fidelity note: since the observer hands us the already-assembled adv
 * report rather than the raw over-the-air bits, the channel is recorded as nominal and the CRC is
 * left unchecked. */
void nocsif_ble_request_pcap(bool on);      /* arms / stops recording (idempotent)              */
bool     nocsif_ble_pcap_active(void);      /* true while a file is open and capturing           */
uint32_t nocsif_ble_pcap_frames(void);      /* adverts written this session                      */
uint32_t nocsif_ble_pcap_bytes(void);       /* bytes written, including headers                  */
uint32_t nocsif_ble_pcap_dropped(void);     /* adverts dropped because the ring was full          */
const char *nocsif_ble_pcap_path(void);     /* current/last file path ("" until first record)    */
const char *nocsif_ble_pcap_status_str(void);  /* "rec" / "off" / "no card" / "file share" / "err" */
const char *nocsif_ble_pcap_tag_str(void);     /* BLE-menu row tag                               */

/* ---- Drone Detection (M7-P4·4): OpenDroneID / ASTM F3411 Remote ID reception -------- *
 * A compliant drone publicly broadcasts a Remote ID — its own identity and position, plus the
 * operator (pilot) position. Over Bluetooth it rides a Service Data AD (UUID 0xFFFA); a
 * BT4-legacy advert carries one 25-byte ODID message at a time, cycling through identity /
 * location / operator, which are accumulated here per drone. Receive-side airspace awareness
 * only — no transmit, no interaction. */
typedef struct {
    uint8_t  addr[6];
    int8_t   rssi;
    uint32_t age_ms;             /* time since last heard                               */
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
bool nocsif_ble_drone_get(int i, nocsif_ble_drone_t *out);   /* snapshot by index; false if none exists */
uint32_t nocsif_ble_drone_gen(void);
const char *nocsif_ble_drone_tag_str(void);                  /* BLE-menu row tag (drone count)    */
const char *nocsif_ble_ua_type_str(uint8_t ua_type);         /* "Multirotor" etc.                 */

/* ============================ Phone Notifications (M7, ANCS, PERIPHERAL role) ============= *
 * Mirrors the phone's notifications over Apple's Notification Center Service. The watch
 * advertises as a connectable peripheral soliciting ANCS; the phone connects and bonds (paired
 * once from iOS Bluetooth settings — the bond persists in NVS) and exposes ANCS; the watch, now
 * the GATT client, subscribes to the Notification Source and Data Source characteristics and
 * fetches each notification's app / title / message. Receive-only in this phase (no dismiss or
 * actions). Foreground-only: the single radio is held while the screen is open (like GATT
 * explore), so notifications only mirror while the notifications screen is on-screen. */
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
    uint32_t age_ms;            /* time since received                             */
    bool     have_attrs;        /* app / title / message fetched from Data Source    */
    char     app[28];           /* app identifier (bundle id)                       */
    char     title[40];
    char     message[100];
} nocsif_ble_ancs_notif_t;

void nocsif_ble_ancs_request_start(void);   /* enters notifications mode (advertise + bond + mirror) */
void nocsif_ble_ancs_request_forget(void);  /* erases the bond (unpairs the phone)                  */
nocsif_ble_ancs_state_t nocsif_ble_ancs_state(void);
const char *nocsif_ble_ancs_status_str(void);
int  nocsif_ble_ancs_count(void);
bool nocsif_ble_ancs_get(int i, nocsif_ble_ancs_notif_t *out);   /* newest first */
uint32_t nocsif_ble_ancs_gen(void);
const char *nocsif_ble_ancs_category_str(uint8_t cat);           /* "Message" / "Email" / …        */
const char *nocsif_ble_ancs_tag_str(void);                       /* BLE-menu row tag               */

/* ============================ Media remote (M7, AMS) ====================================== *
 * Apple Media Service — the media-control sibling of ANCS, discovered on the SAME bonded phone
 * link (whichever screen first brings the link up gets both). The watch reads now-playing
 * information over Entity Update and drives playback over Remote Command. Rides the ANCS bond;
 * no separate pairing is needed. */
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

void        nocsif_ble_ams_cmd(int cmd_id);        /* sends a Remote Command (NOCSIF_AMS_CMD_*)     */
void        nocsif_ble_ams_set_volume(int pct);    /* steps the phone volume toward pct (0..100)    */
bool        nocsif_ble_ams_get(nocsif_ble_ams_t *out);   /* now-playing snapshot (LVGL-task-safe)   */
uint32_t    nocsif_ble_ams_gen(void);              /* change counter                                */
const char *nocsif_ble_ams_tag_str(void);          /* BLE-menu row tag ("—" / "playing" / "paused") */

/* ============================ Phone companion hub (M7, "Connect Phone" screen) ============= *
 * Lifecycle and persistence around the ONE bonded iOS link (ANCS notifications and AMS media both
 * ride it). The watch is the BLE peripheral: "connect" means advertising to solicit ANCS so a
 * bonded iPhone reconnects (an open re-arm — whichever saved phone is nearest, no directed
 * advertising). A persisted "Bluetooth" master switch governs whether the phone link auto-connects
 * at all (at boot and whenever in range); it does NOT gate the other BLE recon screens (scan /
 * GATT / advertise / HID), which stay lazy regardless. Up to NOCSIF_BLE_PHONE_MAX phones are
 * remembered (the bond store itself holds MAX_BONDS); only one live link at a time
 * (MAX_CONNECTIONS=1). All getters below are LVGL-task-safe (RAM-cached, no stack I/O); the
 * setters just post to the worker. */
#define NOCSIF_BLE_PHONE_MAX 3      /* saved phones tracked (matches BT_NIMBLE_MAX_BONDS) */

/* Bluetooth master switch (persisted, default ON). OFF releases the phone link, hands the radio
 * back to WiFi, and disarms auto-connect; ON arms it (advertise + reconnect + boot-connect). */
bool nocsif_ble_bt_enabled(void);
void nocsif_ble_bt_set_enabled(bool on);

/* DEPRECATED — always returns false. The controller is reserved at boot and held resident for the
 * whole session, so turning Bluetooth on at runtime is an instant logical re-enable that never
 * needs a restart. Kept only so old call sites still link; the "restart the watch" UX is gone. */
bool nocsif_ble_needs_restart(void);

/* Manually (re)connects/drops the phone link. Connect turns the master switch ON if needed, then
 * advertises so the nearest saved phone reconnects. Disconnect terminates the current link (with
 * the master still ON, the disconnect handler re-advertises, so it reconnects once back in
 * range). Non-blocking. */
void nocsif_ble_phone_connect(void);
void nocsif_ble_phone_disconnect(void);

/* Boot hook (called from main.c): if the master switch is on and at least one phone is
 * remembered, arms + advertises so the last phone reconnects without the screen ever being
 * opened. Non-blocking; a no-op in safe mode or with the master off. */
void nocsif_ble_phone_boot_autostart(void);

/* Boot hook, called BEFORE nocsif_wifi_init(): claims the BT controller's ~30 KB contiguous
 * internal-DMA block while the heap is still unfragmented. Once WiFi has initialized, the largest
 * free hole stays around 27 KB no matter how much total memory is free, so BLE must come up FIRST
 * or not at all. Blocks for up to ~4 s (the ordering is the point). A no-op when the Bluetooth
 * master is off or in safe mode. */
void nocsif_ble_boot_reserve(void);

/* True once nocsif_ble_boot_reserve has actually run (confirming the reserve happened before
 * WiFi). wifi.c's bring-up asserts this, so an init-order regression that brings WiFi up first
 * gets caught with a loud log rather than silently. */
bool nocsif_ble_boot_reserve_ran(void);

/* Phone-notifications gate (persisted, default ON). OFF keeps the link and media working but
 * stops mirroring notifications (skips or unsubscribes the ANCS Notification Source). Applies
 * live while connected. */
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

/* Count of remembered phones (0..NOCSIF_BLE_PHONE_MAX). Doesn't need the radio stack up (reads
 * our own NVS metadata), so main.c can gate the boot auto-connect on it. */
int  nocsif_ble_phone_count(void);
/* Copies the i-th saved phone (0..count-1), ordered MOST-RECENT FIRST. Returns false if out of range. */
bool nocsif_ble_phone_get(int i, nocsif_ble_phone_t *out);
/* Forgets one phone: deletes its bond and our metadata, and drops the live link if it's this peer. */
void nocsif_ble_phone_forget(const uint8_t addr[6], uint8_t addr_type);

/* ============================ GATT explore (M7-P2, CENTRAL role) ============================ *
 * Connects to one nearby device (chosen from the scan table) and walks its attribute database:
 * discovers every service, then the characteristics under each, then reads a value on request.
 * All NimBLE GATT-client work runs on the worker and host tasks; the LVGL side only *requests*
 * (connect / disconnect / read) and reads the flattened, lock-free snapshot below. This is
 * read-only exploration — the same discovery a phone's Bluetooth settings screen performs. Scope
 * it to devices you're authorized to test.
 *
 * The single-radio rule from P1 still holds: the GATT screen brings NimBLE up (releasing WiFi)
 * through the same request path, connects, explores, and hands the radio back on exit. */

/* Where the GATT screen currently sits in the connect->explore lifecycle. */
typedef enum {
    NOCSIF_BLE_GATT_IDLE = 0,     /* not connected — the device-pick list is live                */
    NOCSIF_BLE_GATT_CONNECTING,   /* central link establishment in progress                      */
    NOCSIF_BLE_GATT_DISCOVERING,  /* connected; discovering services + characteristics           */
    NOCSIF_BLE_GATT_READY,        /* connected; discovery complete — the tree is browsable       */
    NOCSIF_BLE_GATT_DISCONNECTING,/* tearing the link down                                       */
} nocsif_ble_gatt_state_t;

/* One row of the flattened GATT view — either a service header or a characteristic under it. The
 * UI renders these top-to-bottom, each service immediately followed by its characteristics. */
typedef struct {
    bool     is_service;    /* true = service header; false = characteristic                      */
    uint8_t  svc_index;     /* owning service (0-based) — the same value on the header and its chrs */
    uint16_t handle;        /* service start-handle, or characteristic value-handle              */
    uint8_t  props;         /* characteristic properties bitfield (0 on a service header)         */
    bool     readable;      /* props includes READ, so "tap to read" applies                     */
    bool     writable;      /* props includes WRITE or WRITE-without-response                    */
    bool     notifiable;    /* props includes NOTIFY or INDICATE                                 */
    char     uuid[40];      /* formatted UUID ("0x180F" for 16-bit, full string for 128-bit)     */
    char     label[28];     /* well-known SIG name, or "" if not in the table                    */
    bool     has_value;     /* a read has completed for this characteristic                      */
    char     value[48];     /* formatted last-read value (decoded, else hex + printable ASCII)   */
} nocsif_ble_gatt_item_t;

/* Connects to scan-table device `dev_idx` (0..nocsif_ble_dev_count()-1) and explores its GATT
 * server. Cancels discovery and starts a central connection; non-blocking (the worker does the
 * actual work). */
void nocsif_ble_gatt_connect(int dev_idx);

/* Terminates the GATT connection and returns to the device-pick list, resuming discovery. Non-blocking. */
void nocsif_ble_gatt_disconnect(void);

/* Requests a read of the characteristic value at flattened item `idx`. A no-op if `idx` isn't a
 * readable characteristic. The value lands in that item's `value` field, flipping has_value true. */
void nocsif_ble_gatt_read(int idx);

/* Current lifecycle state, plus compact strings for the screen header and the connected peer's label. */
nocsif_ble_gatt_state_t nocsif_ble_gatt_state(void);
const char *nocsif_ble_gatt_state_str(void);
const char *nocsif_ble_gatt_target_str(void);

/* Flattened GATT view (services interleaved with their characteristics). Lock-free for the reader. */
int  nocsif_ble_gatt_item_count(void);
/* Bumps on any STRUCTURAL change — a service/characteristic appearing, a read completing, a
 * connect/disconnect — so the list screen refreshes only when the view actually changed. */
uint32_t nocsif_ble_gatt_gen(void);
/* Copies flattened item `idx` into `out` (short spinlock). Returns false if `idx` is out of range. */
bool nocsif_ble_gatt_item_get(int idx, nocsif_ble_gatt_item_t *out);

/* Compact live tag for the BLE-menu "GATT Explore" row ("connect" / "linking" / "N services" / "—"). */
const char *nocsif_ble_gatt_tag_str(void);

/* Best-effort short name for a well-known 16-bit SIG UUID (service or characteristic), or "" when
 * it isn't in the table (an unknown UUID just shows its raw value in the UI). */
const char *nocsif_ble_gatt_uuid_name(uint16_t uuid16);

/* ============================ Advertise / Beacon (M7-P3, BROADCASTER role) ============================ *
 * Transmits a non-connectable BLE advertisement — the watch itself becomes a beacon other
 * scanners can see. Two formats: a plain NAMED advertisement (a chosen local name, visible in any
 * BLE scanner or phone) and Apple's iBeacon (a fixed NocSif proximity UUID plus major/minor,
 * detectable by iBeacon apps). This is the first BLE *transmitting* operation — the same kind of
 * broadcast a fitness tag or store beacon emits. Scope it to your own space. Brings NimBLE up
 * (releasing WiFi) on start, the same way the scan path does (single radio). */

typedef enum {
    NOCSIF_BLE_ADV_CUSTOM = 0,   /* named non-connectable advertisement (the chosen local name)   */
    NOCSIF_BLE_ADV_IBEACON,      /* Apple iBeacon (fixed NocSif UUID + major/minor)               */
} nocsif_ble_adv_mode_t;

/* Starts/stops broadcasting with the current config. Start brings NimBLE up on first use
 * (releasing WiFi); non-blocking (the worker does the actual work). */
void nocsif_ble_adv_start(void);
void nocsif_ble_adv_stop(void);
bool nocsif_ble_adv_active(void);
/* True during the bring-up window (releasing WiFi + initializing NimBLE), before the advert
 * actually begins transmitting. */
bool nocsif_ble_adv_starting(void);

/* Config (persisted to NVS; applied the next time Start is pressed — change it, then Start). */
nocsif_ble_adv_mode_t nocsif_ble_adv_mode(void);
void        nocsif_ble_adv_set_mode(nocsif_ble_adv_mode_t mode);
const char *nocsif_ble_adv_name(void);              /* advertised local name (Custom mode)          */
void        nocsif_ble_adv_set_name(const char *name);

const char *nocsif_ble_adv_mode_str(void);          /* "named" / "iBeacon"                          */
const char *nocsif_ble_adv_status_str(void);        /* screen header                                */
const char *nocsif_ble_adv_tag_str(void);           /* BLE-menu "Advertise / Beacon" row tag        */

/* ============================ BLE HID keyboard (M7, PERIPHERAL + GATT server) ============== *
 * Turns the watch into a Bluetooth keyboard (HID-over-GATT / HOGP): it advertises with the
 * keyboard appearance and HID service, a host (PC / Mac / phone) pairs and bonds (Just Works),
 * and the watch types by notifying 8-byte boot-keyboard reports — the wireless counterpart to the
 * M4 USB DuckyScript. The same DuckyScript engine (ducky.c plus the hid_kbd keymap) is reused
 * unchanged over a BLE transport sink instead.
 *
 * Single-radio and single-connection rule: MAX_CONNECTIONS=1, so the keyboard link is mutually
 * exclusive with the phone-companion (ANCS/AMS) link and with WiFi. Entering keyboard mode drops
 * the phone link and takes the radio; leaving the screen hands it back to WiFi. This is the
 * device's first GATT *server* role (ANCS/AMS make the watch a GATT client instead). Scope it to
 * hosts you own. */
typedef enum {
    NOCSIF_HID_IDLE = 0,       /* not started                                     */
    NOCSIF_HID_ADVERTISING,    /* advertising as a keyboard, waiting for a host    */
    NOCSIF_HID_CONNECTED,      /* linked; pairing / subscribing                    */
    NOCSIF_HID_READY,          /* bonded + subscribed — typing works              */
    NOCSIF_HID_FAILED,         /* pairing failed                                   */
} nocsif_ble_hid_state_t;

/* Enters keyboard mode: releases the phone link and WiFi, then advertises as a BLE keyboard.
 * Non-blocking (the worker does the bring-up). Gated on reliability safe mode. */
void nocsif_ble_hid_request_start(void);

nocsif_ble_hid_state_t nocsif_ble_hid_state(void);

/* True once a host is connected, bonded (encrypted), and subscribed to the input report — meaning
 * a keystroke sent right now would actually be delivered. The typing engine (the BLE HID sink)
 * polls this. */
bool nocsif_ble_hid_ready(void);

/* Notifies one 8-byte boot-keyboard report ([modifier, 0, k1..k6]) to the subscribed host. Called
 * from the DuckyScript worker task (NimBLE's API is internally locked); a no-op if not ready. */
void nocsif_ble_hid_send_report(const uint8_t report[8]);

const char *nocsif_ble_hid_status_str(void);   /* screen header                                    */
const char *nocsif_ble_hid_tag_str(void);      /* BLE-menu "BLE Keyboard" row tag                  */

#ifdef __cplusplus
}
#endif
