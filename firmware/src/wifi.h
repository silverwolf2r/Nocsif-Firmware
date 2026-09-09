/*
 * WiFi (on-SoC 2.4 GHz, station mode) worker and UI glue (M5-P1).
 *
 * A thin wrapper over ESP-IDF's esp_wifi running in station mode. All radio work
 * happens on a dedicated worker task; LVGL callbacks only ever *request* an action
 * — scan, join, enable — following the same pattern as nfc.cpp and ducky.c.
 * esp_wifi's own event callbacks (scan done, got IP, disconnected) run on the
 * system event task and publish state back for the UI to read.
 *
 *   - Bring-up (esp_netif, the default event loop, esp_wifi_init/start) is lazy,
 *     happening only on the first enable/scan request, so boot stays fast and any
 *     coexistence risk (WiFi buffers competing with the display-flush DMA path) is
 *     confined to first use.
 *   - Gated by reliability safe mode (nocsif_reliability_safe_mode): the radio is
 *     skipped entirely after a boot loop, and nocsif_wifi_available() stays false.
 *   - Credentials are persisted in the NVS settings store, so a join survives a
 *     reboot and the radio auto-reconnects once re-enabled.
 *
 * The status and scan-list getters all return cached, module-owned data with no
 * radio I/O, so they're safe to call from the LVGL task (the Control Center
 * toggle, the scan screen, live labels).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One discovered access point — a snapshot copy handed to the UI, with no radio access. */
typedef struct {
    char    ssid[33];   /* NUL-terminated; empty for a hidden SSID */
    int8_t  rssi;       /* signal strength in dBm, negative, closer to 0 means stronger */
    uint8_t authmode;   /* a wifi_auth_mode_t value; WIFI_AUTH_OPEN (0) means no passphrase needed */
    uint8_t channel;    /* the AP's primary channel */
} nocsif_wifi_ap_t;

/* Creates the idle worker task. Safe to call more than once, including from
 * the LVGL task as the lazy trigger on first entering the WiFi screen or first
 * toggling it on. In reliability safe mode this is a no-op and
 * nocsif_wifi_available() stays false. Returns ESP_OK once the task exists (or
 * in safe mode). Does not bring the radio itself up — that happens on the first
 * enable or scan. */
esp_err_t nocsif_wifi_init(void);

/* Turns the radio on or off — used by the Control Center toggle; Airplane
 * mode forces it off. Non-blocking: just signals the worker. Enabling performs
 * the lazy bring-up on first use and, if a network is saved, auto-reconnects to
 * it. Disabling drops the link and stops the radio entirely. */
void nocsif_wifi_request_enable(bool on);

/* Requests one active access-point discovery scan across all channels.
 * Non-blocking and safe to call from an LVGL callback. Brings the radio up
 * first if it isn't already. Results land in the AP snapshot and bump
 * nocsif_wifi_scan_gen(); nocsif_wifi_scanning() is true while it's running. */
void nocsif_wifi_request_scan(void);

/* Joins a network and persists the credentials to NVS so it survives a
 * reboot. `pass` may be NULL or "" for an open network. Non-blocking: the
 * worker applies the config and connects, with the outcome reflected in the
 * status/detail strings and nocsif_wifi_connected(). */
void nocsif_wifi_request_connect(const char *ssid, const char *pass);

/* Drops the current link without forgetting the saved credentials — the radio stays on, but idle. Non-blocking. */
void nocsif_wifi_request_disconnect(void);

/* Forgets the saved network entirely (clears its NVS credentials) and drops the link. Non-blocking. */
void nocsif_wifi_request_forget(void);

/* Reconnects to the already-saved network without prompting for a
 * passphrase, since the UI never holds one to pass. Used when the user taps a
 * saved network in the list. Forces a connect regardless of the auto-join
 * preference, since an explicit tap is explicit intent. Non-blocking. */
void nocsif_wifi_request_reconnect(void);

/* Spoofs the station MAC address: randomize picks a fresh
 * locally-administered unicast address, restore reverts to the factory
 * address. Applied on the worker via a stop -> set -> start sequence, so any
 * live link briefly drops and rejoins. Non-blocking; the new address shows up
 * in nocsif_wifi_mac_str(). */
void nocsif_wifi_request_randomize_mac(void);
void nocsif_wifi_request_restore_mac(void);

/* Re-applies the DHCP/mDNS hostname derived from the current device name
 * (nocsif_settings_device_name, sanitized into a valid hostname). The radio
 * only picks this up on its next DHCP request, so a live link is bounced to
 * refresh how the watch appears on the network. Call this after renaming the
 * watch. */
void nocsif_wifi_apply_hostname(void);

/* Sets a specific station MAC address, for manual entry. `mac` is 6 bytes
 * and must be a unicast address (bit 0 of mac[0] clear); a multicast address
 * is rejected. Applied on the worker via a stop -> set -> start sequence, so a
 * live link briefly drops and rejoins. Non-blocking; the result shows up in
 * nocsif_wifi_mac_str(). */
void nocsif_wifi_request_set_mac(const uint8_t mac[6]);

/* ---- saved networks: multiple remembered profiles ---- *
 * Every successful join is remembered — SSID plus passphrase — so the watch
 * can rejoin without prompting again, and so the UI can offer a known-networks
 * list. These getters are RAM-cached and safe to call from the LVGL task. */

/* The number of remembered networks (0 or more). */
int nocsif_wifi_saved_count(void);

/* Copies saved network `idx` (0..count-1)'s SSID into `out`. Returns false if `idx` is out of range. */
bool nocsif_wifi_saved_ssid_at(int idx, char *out, size_t len);

/* True if `ssid` is among the saved networks. */
bool nocsif_wifi_is_saved(const char *ssid);

/* The stored passphrase for `ssid` (empty string if it isn't saved, or is
 * open). Module-owned RAM, meant for the watch's own "show saved password"
 * readout. */
const char *nocsif_wifi_pass_of(const char *ssid);

/* Joins a saved network by SSID, using its stored passphrase — no prompt needed. Non-blocking. */
void nocsif_wifi_connect_saved(const char *ssid);

/* Forgets one saved network by SSID, dropping the link too if it's the currently active one. Non-blocking. */
void nocsif_wifi_forget_ssid(const char *ssid);

/* ---- lean buffer profile (BLE/WiFi coexistence) ---- *
 * The WiFi driver's internal-DMA footprint is fixed at esp_wifi_init() time by
 * its buffer counts — it never shrinks just because the link goes idle, so
 * slowing traffic down doesn't free anything. The lean profile instead
 * initializes the driver with a much smaller buffer set (roughly 35-40 KB
 * instead of ~58 KB), leaving the contiguous internal RAM the BLE controller
 * needs while the station stays associated.
 *
 * This is strictly a boot-time input: main.c arms it before the first WiFi
 * bring-up whenever Bluetooth is on. The buffer set can't be changed after
 * esp_wifi_init without a full driver teardown/re-init, which would just
 * re-fragment the pool it had freed (and, measured, can't re-widen the
 * contiguous hole anyway) — so a runtime call is simply ignored once the
 * driver is already up. Since the BLE controller stays reserved for the
 * whole session, lean is always the correct choice while Bluetooth is on.
 * Non-blocking, posted to the worker. */
void nocsif_wifi_set_lean(bool lean);
bool nocsif_wifi_is_lean(void);

/* ---- published state: no radio I/O, safe on the LVGL task ---- */

/* False in safe mode, or if esp_wifi failed to initialize; true once the
 * driver is actually up. Stays false before the first enable/scan, since
 * bring-up is lazy. */
bool nocsif_wifi_available(void);

/* True while the radio is powered on, from enable to disable, regardless of whether it's actually linked. */
bool nocsif_wifi_enabled(void);

/* True once the station has obtained an IP — a usable link. Drives the connected-state accent color. */
bool nocsif_wifi_connected(void);

/* Coarse state of the most recent join attempt, driving the full-screen
 * connecting view. The specific message — which network, "Wrong password", the
 * IP address — lives in nocsif_wifi_detail_str(). */
typedef enum {
    NOCSIF_WIFI_JOIN_IDLE = 0,   /* no join currently in progress */
    NOCSIF_WIFI_JOIN_JOINING,    /* associating, or waiting on an IP */
    NOCSIF_WIFI_JOIN_CONNECTED,  /* got an IP successfully */
    NOCSIF_WIFI_JOIN_FAILED,     /* an auth failure occurred, or the attempt gave up (see the detail string) */
} nocsif_wifi_join_state_t;
nocsif_wifi_join_state_t nocsif_wifi_join_state(void);

/* True while a scan is in flight; drives the scan screen's sweep animation. */
bool nocsif_wifi_scanning(void);

/* A compact status string for a right-side row tag or the Control Center
 * ("off", "on", "scan", "join", a short SSID, "online", "err"). Module-owned,
 * stable between updates. */
const char *nocsif_wifi_status_str(void);

/* A full one-line readout ("online · 192.168.1.42", "joining <ssid>...",
 * "wrong password", "radio off", etc) for a detail label. Module-owned. */
const char *nocsif_wifi_detail_str(void);

/* The station's IP address as a string, or "" when not connected. Module-owned. */
const char *nocsif_wifi_ip_str(void);

/* The SSID of the currently saved network, or "" if none is saved. Module-owned RAM cache. */
const char *nocsif_wifi_saved_ssid(void);

/* ---- section 4.6 Governor P3 geo-store: places keyed by BSSID ---- *
 * A "place" is an access point (identified by BSSID) plus the location it was
 * last connected from (in micro-degrees) plus its SSID — since one SSID can
 * exist at many physical locations, the AP itself is the actual key. Places
 * are auto-learned whenever a fresh GNSS fix coincides with an active link (up
 * to 8 are kept, oldest evicted first; forgetting an SSID drops its places
 * too). The Governor uses these to draw a fence around each one ("near a known
 * AP, so wake WiFi"). Reads are plain and safe from the LVGL/timer task; the
 * actual location stamp is posted to the WiFi worker, which owns the NVS
 * write. */
int  nocsif_wifi_saved_index(const char *ssid);                      /* the profile slot index for an SSID, or -1 if not found */
int  nocsif_wifi_place_count(void);
bool nocsif_wifi_place_get(int idx, int32_t *lat_ud, int32_t *lon_ud, char *ssid, size_t ssid_len);
int  nocsif_wifi_connected_place(void);                              /* the place record for the linked AP's BSSID, or -1 (asks the WiFi task) */
void nocsif_wifi_request_geo_stamp(int32_t lat_ud, int32_t lon_ud);  /* learns or refreshes the linked AP's place */

/* Network detail for the live link — module-owned strings, empty when not
 * connected. The subnet mask and gateway/router come from the DHCP lease; the
 * MAC is the station's own address, updated after a spoof. All safe to read
 * from the LVGL task. */
const char *nocsif_wifi_netmask_str(void);
const char *nocsif_wifi_gateway_str(void);
const char *nocsif_wifi_mac_str(void);

/* The auto-join preference for the saved network: when on, enabling the
 * radio (the Control Center toggle) auto-reconnects to it; when off, the
 * radio comes up idle until an explicit tap. Persisted to NVS and RAM-cached,
 * so both getter and setter are safe on the LVGL task. */
bool nocsif_wifi_autojoin(void);
void nocsif_wifi_set_autojoin(bool on);

/* ---- scan snapshot, lock-free for the reader ---- */

/* The number of APs in the current snapshot (0 before the first scan completes). */
int nocsif_wifi_ap_count(void);

/* A monotonically increasing generation number, bumped every time a new
 * scan result is published; the scan screen polls this cheaply (no radio
 * access) to know when it needs to rebuild its rows. */
uint32_t nocsif_wifi_scan_gen(void);

/* Copies AP `idx` (0..count-1) into `out`; returns false if `idx` is out of
 * range. Reads the current snapshot lock-free, since the worker publishes an
 * entirely new snapshot atomically. */
bool nocsif_wifi_ap_get(int idx, nocsif_wifi_ap_t *out);

/* A human-readable label for an AP's authmode ("open", "WEP", "WPA",
 * "WPA2", "WPA3", etc). Keeps esp_wifi's auth enum contained inside wifi.c so
 * the UI code never has to include esp_wifi headers. */
const char *nocsif_wifi_authmode_str(uint8_t authmode);

/* True when an AP needs no passphrase (WIFI_AUTH_OPEN) — the join flow skips the on-screen keyboard for it. */
bool nocsif_wifi_authmode_open(uint8_t authmode);

/* ---- monitor mode: promiscuous packet capture (M5-P2, authorized testing) ---- *
 * A passive 802.11 monitor: the radio drops any station link (there's only
 * one radio) and captures every frame in the air, tallying counts by frame
 * type and by channel while hopping across channels 1..13 (or holding a
 * single locked channel). Nothing here is stored or parsed yet — this is just
 * proving stable capture alongside the live UI; PCAP export and frame parsing
 * land in P3. All getters read RAM/volatile state with no radio I/O, so
 * they're safe on the LVGL task. Requests are non-blocking, posted to the
 * worker. */

/* Frame class buckets, kept here so the UI side never has to include esp_wifi headers. */
typedef enum {
    NOCSIF_WIFI_PKT_MGMT = 0,   /* management frames */
    NOCSIF_WIFI_PKT_CTRL,       /* control frames */
    NOCSIF_WIFI_PKT_DATA,       /* data frames */
    NOCSIF_WIFI_PKT_MISC,       /* anything else, or malformed */
    NOCSIF_WIFI_PKT_KINDS
} nocsif_wifi_pkt_kind_t;

/* Enters or leaves capture mode. Entering suspends any active STA link,
 * remembering it; leaving restores that link if one was up. Brings the radio
 * up first if needed. Gated on reliability safe mode. */
void nocsif_wifi_request_monitor(bool on);

/* Whether to hop across channels 1..13 (true) versus hold the current/locked channel (false). */
void nocsif_wifi_request_monitor_hop(bool hop);

/* Locks capture to a specific channel (1..13); implies hopping is off. */
void nocsif_wifi_request_monitor_channel(int ch);

/* Live capture state, safe to read on the LVGL task. */
bool     nocsif_wifi_monitor_active(void);          /* capture is running */
bool     nocsif_wifi_monitor_hopping(void);         /* hopping across channels versus locked to one */
int      nocsif_wifi_monitor_channel(void);         /* the current or locked channel (1..13) */
uint32_t nocsif_wifi_monitor_total(void);           /* all frames captured so far */
uint32_t nocsif_wifi_monitor_rate(void);            /* frames per second, over a roughly 1s window */
uint32_t nocsif_wifi_monitor_count(nocsif_wifi_pkt_kind_t kind);  /* count of frames in one class */
uint32_t nocsif_wifi_monitor_ch_count(int ch);      /* frames seen on channels 1..13 */
int8_t   nocsif_wifi_monitor_rssi_last(void);       /* the last received frame's RSSI, in dBm */
int8_t   nocsif_wifi_monitor_rssi_peak(void);       /* the strongest RSSI seen this session */

/* A compact live tag for the WiFi menu's Monitor row ("off", "ch 6", "lock 6"). Module-owned. */
const char *nocsif_wifi_monitor_tag_str(void);

/* ---- passive frame parser: nearby AP list (M5-P3, authorized testing) ---- *
 * A parser task decodes captured beacon and probe-response frames off the rx
 * hot path — the rx callback only copies raw bytes into a ring, and the
 * parser drains and decodes them separately — into a BSSID-keyed table of
 * nearby access points: SSID (including hidden ones), home channel, security
 * class, RSSI, frames seen, and a best-effort vendor guess from the OUI.
 * Purely passive — nothing is ever transmitted. Enabling the parser implies
 * capture is also running, starting it if it was idle and suspending any STA
 * link. All getters return snapshot copies, safe on the LVGL task. */

/* A security class derived from the beacon/probe capability bit plus any RSN/WPA information elements. */
typedef enum {
    NOCSIF_WIFI_SEC_OPEN = 0,   /* no privacy bit set, and no RSN/WPA element */
    NOCSIF_WIFI_SEC_WEP,        /* privacy bit set, but no RSN/WPA element */
    NOCSIF_WIFI_SEC_WPA,        /* WPA only, via the vendor IE 00-50-F2 */
    NOCSIF_WIFI_SEC_WPA2,       /* RSN, using a PSK AKM */
    NOCSIF_WIFI_SEC_WPA3,       /* RSN, using an SAE AKM */
    NOCSIF_WIFI_SEC_WPA2E,      /* RSN, using an 802.1X (EAP) AKM */
} nocsif_wifi_sec_t;

/* One passively observed access point — a snapshot copy handed to the UI. */
typedef struct {
    uint8_t  bssid[6];
    char     ssid[33];   /* NUL-terminated; empty means hidden (a zero-length or all-null SSID) */
    uint8_t  channel;    /* the home channel, taken from the DS-parameter IE, or the rx channel if absent */
    int8_t   rssi;       /* last-seen signal strength, in dBm */
    uint8_t  security;   /* a nocsif_wifi_sec_t value */
    uint16_t frames;     /* count of beacon/probe-response frames seen from this BSSID */
    uint32_t age_ms;     /* time since last seen, used for staleness */
    char     vendor[12]; /* a short vendor guess from the OUI, or "" when unknown */
} nocsif_wifi_mon_ap_t;

/* Turns the passive parser on or off. Turning it on implies capture is
 * running, starting it if idle; turning it off stops parsing but leaves
 * capture itself running (stop capture separately from the Monitor screen).
 * Non-blocking. */
void nocsif_wifi_request_parse(bool on);

/* True while the parser is actively decoding captured frames. */
bool nocsif_wifi_parse_active(void);

/* The nearby-AP table: an entry count, plus a lock-free snapshot copy of
 * entry `idx` (0..count-1). The table preserves first-seen order for stable
 * rows; the stalest entry is evicted once the table fills up. */
int  nocsif_wifi_mon_ap_count(void);
bool nocsif_wifi_mon_ap_get(int idx, nocsif_wifi_mon_ap_t *out);

/* A generation number that bumps only on a structural change — a new
 * BSSID appearing, or one being evicted — so the list screen can rebuild its
 * rows only when the actual set changes rather than on every field update. */
uint32_t nocsif_wifi_mon_ap_gen(void);

/* A human-readable label for a security class ("open", "WEP", "WPA", "WPA2", "WPA3", "WPA2-E"). */
const char *nocsif_wifi_sec_str(uint8_t sec);

/* A compact live tag for the WiFi menu's "Live Networks" row ("off", "3 seen"). Module-owned. */
const char *nocsif_wifi_mon_ap_tag_str(void);

/* ---- passive station list (M5-P3.2, authorized testing) ---- *
 * The same parser also maps data frames to their sender/receiver, i.e. which
 * client device is associated with which access point. The rx copy-out is
 * widened to include a data frame's MAC header (not its payload); the parser
 * then reads the ToDS/FromDS bits and the addresses, off the hot path, into a
 * station-keyed table. Broadcast/multicast addresses and the AP's own address
 * are excluded. Purely passive. Populated whenever the parser is on (via
 * nocsif_wifi_request_parse). All getters return snapshot copies, safe on the
 * LVGL task. */

/* One passively observed station / client device — a snapshot copy handed to the UI. */
typedef struct {
    uint8_t  mac[6];      /* the client/station's own address */
    uint8_t  bssid[6];    /* the access point it's talking to, all-zero if that couldn't be resolved */
    char     ssid[33];    /* the AP's SSID, if that BSSID is present in the AP table, else "" */
    uint8_t  channel;     /* the rx channel it was last seen on */
    int8_t   rssi;        /* last-seen signal strength, in dBm */
    uint16_t frames;      /* count of data frames seen to/from this station */
    uint32_t age_ms;      /* time since last seen, used for staleness */
    char     vendor[12];  /* a short vendor guess from the OUI, or "random" for a locally-administered MAC */
} nocsif_wifi_mon_sta_t;

int  nocsif_wifi_mon_sta_count(void);
bool nocsif_wifi_mon_sta_get(int idx, nocsif_wifi_mon_sta_t *out);
uint32_t nocsif_wifi_mon_sta_gen(void);   /* bumps only on a structural change: an insert or an eviction */
const char *nocsif_wifi_mon_sta_tag_str(void);   /* e.g. "off" or "5 seen", for the WiFi menu row */

/* ---- passive probe-request harvest (M5-P3.2, authorized testing) ---- *
 * Probe requests (management subtype 4) are already present in the copy-out
 * ring; the parser reads the requesting device's address plus the SSID it's
 * searching for into a (device, SSID) table. An empty SSID means a
 * wildcard/broadcast probe. Purely passive. All getters return snapshot
 * copies, safe on the LVGL task. */

/* One harvested probe request — a snapshot copy handed to the UI. */
typedef struct {
    uint8_t  mac[6];      /* the searching device's address */
    char     ssid[33];    /* the requested SSID; "" means a broadcast/wildcard probe */
    int8_t   rssi;        /* last-seen signal strength, in dBm */
    uint16_t count;       /* count of probe-request frames seen for this (device, SSID) pair */
    uint32_t age_ms;      /* time since last seen, used for staleness */
    char     vendor[12];  /* a short vendor guess from the OUI, or "random" for a locally-administered MAC */
} nocsif_wifi_mon_probe_t;

int  nocsif_wifi_mon_probe_count(void);
bool nocsif_wifi_mon_probe_get(int idx, nocsif_wifi_mon_probe_t *out);
uint32_t nocsif_wifi_mon_probe_gen(void);
const char *nocsif_wifi_mon_probe_tag_str(void);   /* e.g. "off" or "7 seen", for the WiFi menu row */

/* ---- PCAP capture to microSD (M5-P3.3, authorized testing) ---- *
 * Writes every captured frame (all types, snaplen bounded) to a standard PCAP
 * file on the card, with each record prefixed by a minimal radiotap header
 * (channel and signal), so it opens directly in Wireshark
 * (LINKTYPE_IEEE802_11_RADIOTAP). A separate all-frames ring feeds a dedicated
 * big-stack writer task, since FAT file I/O must never run on the lean parser
 * task — it would overflow its stack. The /sd volume is app-owned via
 * nocsif_usb_gadget_claim_sd; recording is refused while File Share has handed
 * the card to a host. Enabling this implies capture is running, starting it
 * if idle and suspending any STA link. All getters read RAM/volatile state,
 * safe on the LVGL task. */

/* Starts or stops writing captured frames to a PCAP file. Non-blocking, posted to the worker. */
void nocsif_wifi_request_pcap(bool on);

/* True while frames are actively being written to the file. */
bool nocsif_wifi_pcap_active(void);

/* Live counters for the current session, safe to read on the LVGL task. */
uint32_t nocsif_wifi_pcap_frames(void);    /* frames written to the file */
uint32_t nocsif_wifi_pcap_bytes(void);     /* bytes written, including headers */
uint32_t nocsif_wifi_pcap_dropped(void);   /* frames dropped because the ring was full */

/* The current or most recent PCAP file's path ("" if there hasn't been one yet). Module-owned. */
const char *nocsif_wifi_pcap_path(void);

/* A compact status ("off", "rec", "no card", "file share", "err",
 * "stream", "no host") plus the WiFi menu's row tag. */
const char *nocsif_wifi_pcap_status_str(void);
const char *nocsif_wifi_pcap_tag_str(void);

/* ---- live PCAP over USB-CDC (M5-P5+, authorized testing) ---- *
 * Instead of writing to a file, streams captured frames to the host over the
 * USB CDC serial port as a live PCAP feed, so a host tool (Wireshark, via the
 * bundled tools/extcap plugin) can read them in real time. This automatically
 * requests CDC gadget mode plus monitor mode, then waits for the host to open
 * the port (DTR) before actually emitting the stream. It shares the single
 * writer/ring with the file recorder — only one of the two can be active at a
 * time. The nocsif_wifi_pcap_* counters and status above report on the stream
 * too (status "stream" once a host is reading, "no host" while armed and
 * waiting). */

/* Starts or stops streaming captured frames to the host over USB-CDC. Non-blocking, posted. */
void nocsif_wifi_request_pcap_stream(bool on);

/* True while the CDC live stream is armed — either waiting for a host, or actively streaming. */
bool nocsif_wifi_pcap_stream_active(void);

/* ---- WPA key-exchange and PMKID observation (M5-P4.1, authorized testing) ---- *
 * Both the 4-way key handshake and the PMKID an AP advertises ride inside
 * data frames as EAPOL (ethertype 0x888e). The rx copy-out already carries a
 * data frame's MAC header for the station map; when a frame turns out to be
 * EAPOL it's copied in full (the one exception to the otherwise short data
 * snaplen), and the parser decodes it off the hot path: which of messages 1
 * through 4 was seen, plus the PMKID from the RSN key-data element in message
 * 1. Results collect in a BSSID-keyed table, mirroring the AP list. This is
 * purely passive observation — nothing is transmitted, and the watch never
 * solicits a handshake itself. Populated whenever the parser is on (via
 * nocsif_wifi_request_parse). All getters return snapshot copies, safe on the
 * LVGL task.
 *
 * An optional save-to-SD reuses the P3.3 writer with an EAPOL filter: only
 * EAPOL frames, plus the beacon or probe-response that names the network, are
 * written, to /sd/nocsif/wifi/hs-NNN.pcap (ready for Wireshark or hcxtools).
 * This shares the single writer with full PCAP capture — only one can record
 * at a time. */

/* One BSSID's observed key-exchange state — a snapshot copy handed to the UI. */
typedef struct {
    uint8_t  bssid[6];
    char     ssid[33];    /* the AP's SSID, if that BSSID is present in the AP table, else "" */
    uint8_t  msg_mask;    /* which messages of the 4-way exchange were seen: bit0=msg1 through bit3=msg4 */
    bool     has_pmkid;   /* whether an RSN PMKID was observed in message 1 */
    uint8_t  pmkid[16];   /* the PMKID bytes themselves; only valid when has_pmkid is true */
    int8_t   rssi;        /* last-seen signal strength, in dBm */
    uint16_t frames;      /* count of EAPOL frames seen for this BSSID */
    uint32_t age_ms;      /* time since last seen, used for staleness */
    char     vendor[12];  /* a short vendor guess from the OUI */
} nocsif_wifi_mon_hs_t;

int  nocsif_wifi_mon_hs_count(void);
bool nocsif_wifi_mon_hs_get(int idx, nocsif_wifi_mon_hs_t *out);
uint32_t nocsif_wifi_mon_hs_gen(void);            /* bumps only on a structural change: an insert or an eviction */
const char *nocsif_wifi_mon_hs_tag_str(void);     /* e.g. "off" or "2 seen", for the WiFi menu row */

/* True when a captured entry has enough material to attempt an offline
 * recovery: either a PMKID, or both the AP's message 1 and the client's
 * message 2 — the pair the offline check actually needs. */
bool nocsif_wifi_hs_crackable(const nocsif_wifi_mon_hs_t *hs);

/* Exports the captured PMKIDs and 4-way handshakes to a hashcat-22000 file
 * on /sd (part of the M5 passive polish): written to
 * /sd/nocsif/wifi/nocsif.hc22000 as WPA*01 (PMKID) plus WPA*02 (handshake)
 * lines, ready to crack without the usual PC-side hcxpcapngtool step.
 * Non-blocking — a one-shot big-stack writer task does the actual SD I/O.
 * The result is read back through the status/path getters below. */
void nocsif_wifi_request_export_hc22000(void);
const char *nocsif_wifi_hc_status_str(void);   /* e.g. "", "exporting...", "N lines written", "no microSD card" */
const char *nocsif_wifi_hc_path(void);         /* the output file's path ("" until an export has actually run) */

/* Starts or stops writing the observed EAPOL frames (plus their naming
 * beacons) to /sd/nocsif/wifi/hs-NNN.pcap. Reuses the PCAP writer with an
 * EAPOL filter; refused while a full PCAP capture is already recording.
 * Non-blocking, posted to the worker. Live state reads back through the
 * nocsif_wifi_pcap_* getters. */
void nocsif_wifi_request_hs_capture(bool on);

/* ---- passive anomaly detectors (M5-P4.2, authorized testing) ---- *
 * Two purely passive signals, both derived from the capture that's already
 * running — nothing is ever transmitted.
 *
 * (1) Management-frame rate: counts deauthentication (subtype 12) and
 *     disassociation (subtype 10) frames, plus a combined per-second rate
 *     over a roughly 1s monitor window. A sustained elevated rate is the
 *     classic signature of a deauthentication/disassociation flood nearby.
 *     Tallied directly in the rx path, so this works under bare monitor mode
 *     even if the parser isn't on.
 * (2) Duplicate-SSID / twin detection: the same SSID advertised by more than
 *     one BSSID. That's legitimate for a roaming mesh, but a security-class
 *     mismatch among them — e.g. an open clone of what's normally a secured
 *     network — is the strong tell for a twin attack. Derived from the
 *     passive AP table, so the parser must be on. */

/* Management-frame rate counters — RAM/volatile, safe on the LVGL task. Reset whenever capture (re)starts. */
uint32_t nocsif_wifi_deauth_count(void);       /* deauthentication frames seen this session */
uint32_t nocsif_wifi_disassoc_count(void);     /* disassociation frames seen this session */
uint32_t nocsif_wifi_deauth_rate(void);        /* combined deauth+disassoc rate per second, over a roughly 1s window */
uint32_t nocsif_wifi_deauth_peak_rate(void);   /* the peak combined per-second rate seen this session */

/* A compact live tag for the WiFi menu's "Anomalies" row ("off", "ok", "N/s"). Module-owned. */
const char *nocsif_wifi_anomaly_tag_str(void);

/* One duplicate-SSID group — a snapshot copy handed to the UI. */
typedef struct {
    char    ssid[33];      /* the shared, non-empty SSID */
    uint8_t bssids;        /* the number of distinct BSSIDs advertising it (2 or more to be reported at all) */
    bool    sec_mismatch;  /* true if they don't all share one security class — the strong twin tell */
    bool    has_open;      /* true if at least one advertises fully open (no privacy) — a common cloning tactic */
    uint8_t sec_lo;        /* the weakest security class seen among them (a nocsif_wifi_sec_t value) */
    uint8_t sec_hi;        /* the strongest security class seen among them */
} nocsif_wifi_mon_dup_t;

/* Fills `arr` (up to `max` entries) with the duplicate-SSID groups;
 * returns the total group count (a value greater than `max` means only the
 * first `max` were actually written). Computed fresh from the AP table each
 * call. Safe on the LVGL task: it snapshots the table under the lock, then
 * analyzes it afterward without holding the lock. */
int nocsif_wifi_mon_dup_snapshot(nocsif_wifi_mon_dup_t *arr, int max);

/* ---- management-frame TX (M5-P5.1, active — authorized testing) ---- *
 * The first active WiFi operation: transmits deauthentication or
 * disassociation frames that spoof a chosen access point as their source, to
 * the broadcast address (reaching all its clients), at a bounded rate, while
 * the radio holds the target's channel. Used to solicit a reconnect — which
 * the P4.1 handshake capture then observes — or to exercise the P4.2
 * detectors, as part of an authorized test. Every call here is non-blocking,
 * posted to the worker. Gated on reliability safe mode. */

/* Selects the target AP, whose BSSID gets spoofed as the frame source. `channel` is its home channel; `ssid` is only used for the on-screen label. */
void nocsif_wifi_set_mgmt_target(const uint8_t bssid[6], int channel, const char *ssid);

/* The frame kind: false selects deauthentication (subtype 12), true selects disassociation (subtype 10). */
void nocsif_wifi_set_mgmt_disassoc(bool disassoc);
bool nocsif_wifi_mgmt_disassoc(void);

/* Starts or stops transmitting to the selected target. Requires a target
 * already selected; brings the radio up on the target's channel first.
 * Refused in reliability safe mode. Also stops automatically when monitor
 * mode stops. */
void nocsif_wifi_request_mgmt_tx(bool on);

/* Live TX state — RAM/volatile, safe to read on the LVGL task. */
bool     nocsif_wifi_mgmt_tx_active(void);
bool     nocsif_wifi_mgmt_has_target(void);
uint32_t nocsif_wifi_mgmt_tx_count(void);      /* frames transmitted this session */
uint32_t nocsif_wifi_mgmt_tx_rate(void);       /* frames per second, over a roughly 1s window */
const char *nocsif_wifi_mgmt_target_str(void); /* formatted as "SSID · bssid · ch N", or "none" */
const char *nocsif_wifi_mgmt_tx_tag_str(void); /* "off", "armed", or "N/s" for the menu row */

/* ---- beacon TX (M5-P5.2, active — authorized testing) ---- *
 * Advertises a user-managed list of SSIDs by transmitting beacon frames,
 * each with its own locally-administered BSSID, on the currently held
 * channel. The list itself is edited on-watch — add via keyboard, rename,
 * enable/disable, delete — and persisted to NVS. Only entries marked enabled
 * actually transmit. Reuses the P5.1 raw-TX path underneath. Non-blocking and
 * gated on safe mode. The list API is safe to call on the LVGL task, since
 * it's internally locked. */
void nocsif_wifi_request_beacon(bool on);      /* starts or stops transmitting the currently enabled SSIDs */
bool nocsif_wifi_beacon_active(void);
uint32_t nocsif_wifi_beacon_frames(void);      /* beacon frames transmitted this session */
uint32_t nocsif_wifi_beacon_rate(void);        /* frames per second, over a roughly 1s window */
const char *nocsif_wifi_beacon_tag_str(void);  /* "off", "ready", or "N/s" for the menu row */

/* The managed SSID list, persisted to NVS. Mutations dedup entries, clamp to the maximum, and re-save automatically. */
int  nocsif_wifi_beacon_count(void);           /* the number of entries currently in the list */
uint32_t nocsif_wifi_beacon_gen(void);         /* bumps on any list change */
bool nocsif_wifi_beacon_get(int idx, char *out, size_t len, bool *enabled);
bool nocsif_wifi_beacon_add(const char *ssid); /* appends an entry; returns false if it's empty, a duplicate, or the list is already full */
void nocsif_wifi_beacon_remove(int idx);
void nocsif_wifi_beacon_rename(int idx, const char *ssid);
void nocsif_wifi_beacon_toggle(int idx);       /* flips one entry's enabled flag */
void nocsif_wifi_beacon_add_decoys(int n);     /* bulk-appends up to n generated "NocSif-NN" names */

/* ---- software access point (M5-P5.3, active — authorized testing) ---- *
 * Brings the on-SoC radio up as an open software access point and reports
 * the client devices that join it. Since there's only one radio, this is
 * mutually exclusive with monitor mode and STA mode: entering AP mode
 * suspends any capture and the station link (remembering it, and restoring
 * it on exit); conversely, starting monitor mode, a scan, or a join tears the
 * AP back down. The connected-client list is refreshed off the LVGL task from
 * the driver's association table plus the built-in DHCP server's leases, and
 * published lock-free. Config (SSID, channel, hidden) is cached, persisted to
 * NVS, and applied on Start; a live change while it's already running
 * re-applies immediately. Gated on reliability safe mode. Scope every use to
 * networks you're actually authorized to operate. */

/* Starts or stops the software AP. Non-blocking, posted to the worker.
 * Starting applies the cached config and suspends monitor/STA mode; stopping
 * returns the radio to idle STA mode (and rejoins the pre-AP link if one was
 * up and auto-join is enabled). Refused in reliability safe mode. */
void nocsif_wifi_request_ap(bool on);

/* Config setters, safe on the LVGL task: cache the value and persist it to
 * NVS. If the AP is already running, they also re-apply live — an SSID or
 * channel change takes effect starting with the next beacon. `ssid` is
 * clamped to 32 characters; `channel` to the range 1..13. */
void nocsif_wifi_ap_set_ssid(const char *ssid);
void nocsif_wifi_ap_set_channel(int channel);
void nocsif_wifi_ap_set_hidden(bool hidden);

/* Published AP state, no radio I/O, safe on the LVGL task. */
bool        nocsif_wifi_ap_active(void);      /* the AP is currently up */
const char *nocsif_wifi_ap_ssid(void);        /* the configured SSID, module-owned */
int         nocsif_wifi_ap_channel(void);     /* the configured channel (1..13) */
bool        nocsif_wifi_ap_hidden(void);      /* whether the SSID is hidden from beacons */
const char *nocsif_wifi_ap_ip_str(void);      /* the AP's own IPv4 address (its gateway), "" until it's started */

/* The connected-client table, a lock-free snapshot: `mac` (6 bytes) and
 * `ip` ("" until the DHCP lease actually lands), plus the last-seen RSSI.
 * nocsif_wifi_ap_gen bumps on any membership change, so the list screen only
 * rebuilds its rows when the actual client set changes. */
int  nocsif_wifi_ap_client_count(void);
bool nocsif_wifi_ap_client_get(int idx, uint8_t mac[6], char *ip, size_t iplen, int8_t *rssi);
uint32_t nocsif_wifi_ap_gen(void);

/* A compact live tag for the WiFi menu's "Access Point" row ("off", "on", "N joined"). Module-owned. */
const char *nocsif_wifi_ap_tag_str(void);

/* ---- captive portal (M5-P5.4, active — authorized testing) ---- *
 * Layers a captive-portal presentation on top of the open software AP: a
 * UDP:53 DNS redirector answers every query with the AP's own IP, so a
 * joining client's connectivity check resolves to the watch, and an
 * esp_http_server serves a landing page (from /sd/nocsif/wifi/portal.html, or
 * a built-in notice if that's absent) for every request — which makes client
 * OSes surface their normal captive-portal sheet. Each request is recorded
 * (client IP, method, path, any submitted fields) both to a RAM ring for the
 * on-watch live log, and appended to /sd/nocsif/wifi/portal-NNN.log for the
 * operator's own authorized-testing review. Starting the portal brings the AP
 * up first if it isn't already; stopping the portal leaves the AP running.
 * The portal can never outlive the AP — stopping the AP also stops the
 * portal. Gated on reliability safe mode. */

/* Starts or stops the captive portal. Non-blocking, posted to the worker.
 * Starting ensures the AP is up first. Refused in reliability safe mode. */
void nocsif_wifi_request_portal(bool on);

/* Published portal state, no radio I/O, safe on the LVGL task. */
bool     nocsif_wifi_portal_active(void);
uint32_t nocsif_wifi_portal_hits(void);          /* total requests served this session */
uint32_t nocsif_wifi_portal_gen(void);           /* bumps on every new request, for UI refresh */
const char *nocsif_wifi_portal_page_src(void);   /* "sd", "built-in", or "" before the first hit */

/* The recent client-interaction ring, newest first; `idx` 0 is the most recent request. */
int  nocsif_wifi_portal_log_count(void);
bool nocsif_wifi_portal_log_get(int idx, char *out, size_t len);

/* Landing-page selection: picks which HTML file under
 * /sd/nocsif/wifi/portals/ is served (or "" for the built-in notice). The
 * choice is persisted to NVS and applied immediately, even on an already
 * running portal. The list of available pages itself is enumerated UI-side
 * via an SD directory listing. */
const char *nocsif_wifi_portal_selected_page(void);   /* "" selects the built-in page */
void        nocsif_wifi_portal_set_page(const char *name);

/* A compact live tag for the WiFi menu's "Captive Portal" row ("off", "on", "N hits"). Module-owned. */
const char *nocsif_wifi_portal_tag_str(void);

/* ---- section 4.8a companion control surface (L4), the on-network web
 * remote — P1 transport ---- * Raises an app-free control surface: an open
 * SoftAP (SSID is the device name, no join gate — an operator decision)
 * hosting an esp_http_server on port 80 plus an mDNS responder, so any
 * phone or laptop on that AP can browse to nocsif.local and control the
 * watch — no app, no pairing code needed. Anyone on the AP can control the
 * watch; the off-by-default toggle plus the on-watch "linked" indicator are
 * the intended guardrails (P2/P3 add the actual command channel and live
 * view). Given the single-radio reality, raising this surface first stops
 * the promiscuous monitor/parser and the captive portal, since they're
 * mutually exclusive on the one radio and on port 80. Bluetooth is left
 * alone — the controller has been resident since RAM Phase 2, so this
 * surface coexists fine with the phone link. Gated on reliability safe
 * mode. Non-blocking, posted to the worker. Turning it off tears the
 * AP/mDNS/HTTP stack down and restores whatever STA link was active before. */
void nocsif_wifi_companion_set(bool on);

/* Published companion state, no radio I/O, safe on the LVGL task. */
bool        nocsif_wifi_companion_active(void);
const char *nocsif_wifi_companion_ssid(void);        /* the open-AP SSID clients join, "" until it's raised */
const char *nocsif_wifi_companion_url(void);         /* either "nocsif.local" via mDNS, or the AP's raw IP as a fallback */
const char *nocsif_wifi_companion_status_str(void);  /* a one-line status string for the screen */
int         nocsif_wifi_companion_clients(void);      /* the number of joined clients, mirroring the AP client count */

/* A compact live tag for the System menu's "Companion" row ("off", "on", "on · N"). Module-owned. */
const char *nocsif_wifi_companion_tag_str(void);

/* ---- section 4.8a companion, P2 command channel (phone to watch) ---- *
 * The companion HTTP server accepts POST commands and forwards each as one
 * of these to a handler the UI registers via
 * nocsif_wifi_companion_set_cmd_handler. wifi.c owns the transport and JSON
 * parsing, running on the httpd task; the UI-side handler is responsible for
 * marshaling the actual action onto the LVGL task, since it must never touch
 * LVGL directly from the httpd task. Endpoints: POST /api/launch {id},
 * /api/back, /api/home, /api/type {text}, /api/key
 * {key:"backspace"|"enter"}, /api/brightness {v}, /api/volume {v}. `arg`
 * carries the app id, text, or key name; for brightness/volume it carries
 * the 0-255 level as a decimal string. */
typedef enum {
    NOCSIF_COMPANION_CMD_LAUNCH = 1,   /* arg = app id, a k_screens id, including action rows like flash/dnd */
    NOCSIF_COMPANION_CMD_BACK,         /* navigate back one screen */
    NOCSIF_COMPANION_CMD_HOME,         /* jump straight to Home */
    NOCSIF_COMPANION_CMD_TYPE,         /* arg = text to append into whatever watch text field currently has focus */
    NOCSIF_COMPANION_CMD_KEY,          /* arg = "backspace" or "enter", editing the focused field */
    NOCSIF_COMPANION_CMD_BRIGHTNESS,   /* arg = "0".."255", the Control Center's brightness level */
    NOCSIF_COMPANION_CMD_VOLUME,       /* arg = "0".."255", the Control Center's volume level */
    NOCSIF_COMPANION_CMD_BUTTON,       /* arg = "fn.short"|"fn.long"|"pwr.short"|"pwr.long", the physical side buttons */
    NOCSIF_COMPANION_CMD_CAST,         /* arg = "1"/"0", toggling casting (blanking the watch panel, phone acts as the display) */
} nocsif_companion_cmd_type_t;

typedef struct {
    nocsif_companion_cmd_type_t type;
    char arg[192];
} nocsif_companion_cmd_t;

typedef void (*nocsif_companion_cmd_fn_t)(const nocsif_companion_cmd_t *cmd);

/* Registers the UI-side command handler. Called once at UI init. The
 * handler itself runs on the HTTP server's own task, so it must marshal any
 * LVGL work onto the LVGL task (via lvgl_port_lock plus lv_async_call). */
void nocsif_wifi_companion_set_cmd_handler(nocsif_companion_cmd_fn_t fn);

/* ---- section 4.8a companion, P2 menu mirror plus P3 live state (watch
 * to phone) ---- * Two read-only JSON providers the UI registers, so the
 * companion server can mirror the watch without hardcoding its layout
 * itself. Both run on the HTTP/timer task, so each provider must only ever
 * touch immutable registry data or plain cached scalars — never LVGL
 * objects.
 *   - the menu provider backs GET /api/menu: the 3 Home categories and their
 *     actual rows, as {id,label,en,warn}.
 *   - the state provider backs the /ws WebSocket push (roughly 2x/s) and GET
 *     /api/ping: live watch state — screen title, whether a field has focus,
 *     toggles, brightness, volume — so the web page can stay in sync and
 *     raise the phone's native keyboard whenever a watch text field is
 *     focused. Each provider fills `buf` with NUL-terminated JSON. */
typedef void (*nocsif_companion_json_fn_t)(char *buf, size_t len);
void nocsif_wifi_companion_set_menu_fn(nocsif_companion_json_fn_t fn);
void nocsif_wifi_companion_set_state_fn(nocsif_companion_json_fn_t fn);

/* ---- section 4.8a companion, P3 screen mirror (watch to phone) ---- *
 * The UI captures the live screen roughly once a second, but only while a
 * /ws client is actually connected, downsamples it into a small RGB565
 * thumbnail, and hands it here; the companion server then copies it under a
 * mutex and pushes it as a binary /ws frame — an 8-byte header of
 * 'N','F',w16,h16,fmt,rsv followed by w*h*2 RGB565-LE bytes — whenever it's
 * new. nocsif_wifi_companion_ws_clients() lets the UI skip the relatively
 * heavy capture work entirely when nobody is actually watching. */
void nocsif_wifi_companion_publish_thumb(const uint8_t *rgb565_le, int w, int h);
int  nocsif_wifi_companion_ws_clients(void);

/* Section 4.8a interactive control: the phone streams touch points up
 * the /ws socket; wifi.c parses them on the httpd task and calls this
 * UI-registered handler, which feeds the watch's remote pointer input
 * device. (x,y) are in watch-space pixels; pressed means finger-down.
 * High-frequency and lock-free, backed by a single packed scalar. */
typedef void (*nocsif_companion_touch_fn_t)(int x, int y, int pressed);
void nocsif_wifi_companion_set_touch_fn(nocsif_companion_touch_fn_t fn);

/* Section 4.15 desktop bridge: the very same UI hooks, but reached over a
 * second transport (the USB console). Each of these returns false until the
 * UI has actually registered its handler. Unlike the HTTP path, these do not
 * require the companion surface to be up at all, since the bridge is a
 * wired, owner-only channel. Safe to call from any task — the handlers
 * either marshal onto the LVGL task, or just fill from cached scalars. */
bool nocsif_wifi_companion_dispatch(const nocsif_companion_cmd_t *cmd);
bool nocsif_wifi_companion_menu_json(char *buf, size_t len);
bool nocsif_wifi_companion_state_json(char *buf, size_t len);
bool nocsif_wifi_companion_touch(int x, int y, int pressed);

#ifdef __cplusplus
}
#endif
