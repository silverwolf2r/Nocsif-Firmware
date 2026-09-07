/*
 * NocSif — WiFi (on-SoC 2.4 GHz, STA) worker + UI glue (M5-P1).
 *
 * A thin worker over ESP-IDF esp_wifi in station mode. All radio work runs on a
 * dedicated worker task; the LVGL callbacks only *request* an action (scan / join /
 * enable), exactly like nfc.cpp / ducky.c. esp_wifi's own event callbacks (scan done,
 * got IP, disconnected) run on the system event task and publish state back.
 *
 *   - Lazy bring-up on the first enable/scan request (esp_netif + default event loop +
 *     esp_wifi_init/start), so boot stays fast and the coexistence risk (WiFi buffers vs
 *     the display-flush DMA path) is isolated to first use.
 *   - Safe-mode gated (nocsif_reliability_safe_mode): the radio is skipped after a boot
 *     loop, and nocsif_wifi_available() stays false.
 *   - Credentials persist in the NVS settings store, so a join survives a reboot and the
 *     radio auto-reconnects when re-enabled.
 *
 * The status / scan-list getters return cached, module-owned data (no radio I/O) so they
 * are safe to call from the LVGL task (Control-Center toggle, scan screen, live labels).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One discovered access point (a snapshot copy for the UI; no radio access). */
typedef struct {
    char    ssid[33];   /* NUL-terminated; "" for a hidden SSID                 */
    int8_t  rssi;       /* dBm (negative; closer to 0 = stronger)               */
    uint8_t authmode;   /* wifi_auth_mode_t; WIFI_AUTH_OPEN(0) = no passphrase   */
    uint8_t channel;    /* primary channel                                       */
} nocsif_wifi_ap_t;

/* Create the idle worker task. Idempotent; safe to call from the LVGL task as the lazy
 * trigger on first WiFi-screen entry / first toggle. In reliability safe mode this is a
 * no-op and nocsif_wifi_available() stays false. Returns ESP_OK once the task exists (or
 * in safe mode). Does NOT bring the radio up — that happens on the first enable/scan. */
esp_err_t nocsif_wifi_init(void);

/* Turn the radio on / off (the Control-Center toggle; Airplane forces off). Non-blocking:
 * only signals the worker. Enabling performs the lazy bring-up on first use and, if a
 * network is saved, auto-reconnects to it. Disabling drops the link and stops the radio. */
void nocsif_wifi_request_enable(bool on);

/* Request one access-point discovery scan (active, all channels). Non-blocking / LVGL-
 * callback-safe. Brings the radio up first if needed. Results land in the AP snapshot and
 * bump nocsif_wifi_scan_gen(); nocsif_wifi_scanning() is true while it runs. */
void nocsif_wifi_request_scan(void);

/* Join a network and persist the credentials (NVS) for reboot-survival. `pass` may be NULL
 * or "" for an open network. Non-blocking: the worker sets the config and connects; the
 * outcome shows in the status/detail strings and nocsif_wifi_connected(). */
void nocsif_wifi_request_connect(const char *ssid, const char *pass);

/* Drop the current link WITHOUT forgetting the saved credentials (the radio stays on and
 * idle). Non-blocking. */
void nocsif_wifi_request_disconnect(void);

/* Forget the saved network (clear NVS creds) and drop the link. Non-blocking. */
void nocsif_wifi_request_forget(void);

/* Reconnect to the ALREADY-SAVED network without a passphrase prompt (the UI holds no
 * password to pass). Used when the user taps a saved network in the list. Forces a connect
 * regardless of the auto-join preference — an explicit tap is explicit intent. Non-blocking. */
void nocsif_wifi_request_reconnect(void);

/* Spoof the station MAC: randomize picks a fresh locally-administered unicast address; restore
 * returns the factory address. Applied on the worker (stop -> set -> start), so a live link
 * blips and re-joins. Non-blocking; the new address lands in nocsif_wifi_mac_str(). */
void nocsif_wifi_request_randomize_mac(void);
void nocsif_wifi_request_restore_mac(void);

/* Re-apply the DHCP/mDNS hostname from the current device name (nocsif_settings_device_name,
 * sanitized to a valid hostname). The radio picks it up on the next DHCP request, so a live link
 * is bounced to refresh how the watch appears on the network. Call after renaming the watch. */
void nocsif_wifi_apply_hostname(void);

/* Set a SPECIFIC station MAC (manual entry). `mac` is 6 bytes and must be unicast (mac[0] bit0=0);
 * a multicast address is rejected. Applied worker-side (stop -> set -> start); a live link blips
 * and re-joins. Non-blocking; the result shows in nocsif_wifi_mac_str(). */
void nocsif_wifi_request_set_mac(const uint8_t mac[6]);

/* ---- saved networks (multiple remembered profiles) --------------------------------- *
 * Every successful join is remembered (SSID + passphrase) so the watch can rejoin without a
 * prompt and offer a known-networks list. The getters are RAM-cached (LVGL-task-safe). */

/* Number of remembered networks (0..). */
int nocsif_wifi_saved_count(void);

/* Copy saved network `idx` (0..count-1) SSID into `out`. False if `idx` is out of range. */
bool nocsif_wifi_saved_ssid_at(int idx, char *out, size_t len);

/* True if `ssid` is in the saved set. */
bool nocsif_wifi_is_saved(const char *ssid);

/* The stored passphrase for `ssid` ("" if not saved or an open network). Module-owned RAM — for
 * the owner's own "show saved password" readout on-device. */
const char *nocsif_wifi_pass_of(const char *ssid);

/* Join a SAVED network by SSID using its stored passphrase (no prompt). Non-blocking. */
void nocsif_wifi_connect_saved(const char *ssid);

/* Forget one saved network by SSID (drops the link if it is the active one). Non-blocking. */
void nocsif_wifi_forget_ssid(const char *ssid);

/* ---- lean buffer profile (BLE⇄WiFi coexistence) -------------------------------------- *
 * The WiFi driver's internal-DMA footprint is fixed at esp_wifi_init() by its BUFFER COUNTS — it does
 * not shrink when the link goes idle, so slowing traffic down frees nothing. The lean profile inits the
 * driver with a much smaller buffer set (~35-40 KB instead of ~58 KB), leaving the contiguous internal
 * RAM the BLE controller needs while the station stays ASSOCIATED.
 *
 * This is a BOOT-TIME input: main.c arms it before the first WiFi bring-up whenever Bluetooth is on.
 * The buffer set can't change after esp_wifi_init without a driver teardown/re-init, which would
 * re-fragment the pool it just freed (and can't re-widen the contiguous hole anyway — measured), so a
 * runtime call is IGNORED once the driver is up. Since the BLE controller is now reserved for the whole
 * session, lean is always correct while Bluetooth is on. Non-blocking (worker-posted). */
void nocsif_wifi_set_lean(bool lean);
bool nocsif_wifi_is_lean(void);

/* ---- published state (no radio I/O; safe on the LVGL task) --------------------------- */

/* False in safe mode or if esp_wifi failed to initialise; true once the driver is up.
 * (Before the first enable/scan it is false — bring-up is lazy.) */
bool nocsif_wifi_available(void);

/* True while the radio is powered on (between enable and disable), regardless of link. */
bool nocsif_wifi_enabled(void);

/* True once the station has an IP (a usable link). Drives the connected accent. */
bool nocsif_wifi_connected(void);

/* Coarse state of the most recent join attempt — drives the full-screen connecting view.
 * The specific message (which network, "Wrong password", the IP) is in nocsif_wifi_detail_str(). */
typedef enum {
    NOCSIF_WIFI_JOIN_IDLE = 0,   /* no join in progress                 */
    NOCSIF_WIFI_JOIN_JOINING,    /* associating / awaiting an IP        */
    NOCSIF_WIFI_JOIN_CONNECTED,  /* got an IP                           */
    NOCSIF_WIFI_JOIN_FAILED,     /* auth failure or gave up (see detail)*/
} nocsif_wifi_join_state_t;
nocsif_wifi_join_state_t nocsif_wifi_join_state(void);

/* True while a scan is in flight (drives the scan-screen sweep animation). */
bool nocsif_wifi_scanning(void);

/* Compact status for a right-side row tag / the Control Center ("off" / "on" / "scan" /
 * "join" / a short SSID / "online" / "err"). Module-owned, stable between updates. */
const char *nocsif_wifi_status_str(void);

/* Full one-line readout ("online · 192.168.1.42", "joining <ssid>…", "wrong password",
 * "radio off", …) for a detail label. Module-owned. */
const char *nocsif_wifi_detail_str(void);

/* The station IP as a string, or "" when not connected. Module-owned. */
const char *nocsif_wifi_ip_str(void);

/* The SSID of the saved network, or "" if none is saved. Module-owned (RAM cache). */
const char *nocsif_wifi_saved_ssid(void);

/* ---- §4.6 Governor P3 geo-store — PLACES keyed by BSSID --------------------------------- *
 * A place = an access point (BSSID) + the location it was last connected from (micro-degrees) + its
 * SSID — one SSID can exist in many physical places, so the AP is the key. Auto-learned when a fresh
 * GNSS fix coincides with a link (up to 8; oldest evicted; forgetting an SSID drops its places). The
 * Governor draws a fence around each ("near a known AP -> wake WiFi"). Reads are plain (LVGL/timer-
 * safe); the stamp is posted to the WiFi worker, which owns the NVS write. */
int  nocsif_wifi_saved_index(const char *ssid);                      /* profile slot of an SSID, or -1  */
int  nocsif_wifi_place_count(void);
bool nocsif_wifi_place_get(int idx, int32_t *lat_ud, int32_t *lon_ud, char *ssid, size_t ssid_len);
int  nocsif_wifi_connected_place(void);                              /* place of the linked AP's BSSID, or -1 (asks the WiFi task) */
void nocsif_wifi_request_geo_stamp(int32_t lat_ud, int32_t lon_ud);  /* learn/refresh the linked AP's place */

/* Network detail for the live link (module-owned strings; "" when not connected). The subnet
 * mask + gateway (router) come from the DHCP lease; the MAC is the station address and updates
 * after a spoof. All safe to read on the LVGL task. */
const char *nocsif_wifi_netmask_str(void);
const char *nocsif_wifi_gateway_str(void);
const char *nocsif_wifi_mac_str(void);

/* Auto-join preference for the saved network: when on, enabling the radio (the Control-Center
 * toggle) auto-reconnects to it; when off, the radio comes up idle until an explicit tap.
 * Persisted (NVS). RAM-cached, so both are safe on the LVGL task. */
bool nocsif_wifi_autojoin(void);
void nocsif_wifi_set_autojoin(bool on);

/* ---- scan snapshot (lock-free for the reader) --------------------------------------- */

/* Number of APs in the current snapshot (0 before the first scan completes). */
int nocsif_wifi_ap_count(void);

/* A monotonically increasing generation that bumps each time a new scan result is
 * published; the scan screen polls it to know when to rebuild its rows (cheap; no radio). */
uint32_t nocsif_wifi_scan_gen(void);

/* Copy AP `idx` (0..count-1) into `out`. Returns false if `idx` is out of range. Reads the
 * current snapshot lock-free (the worker publishes a whole new snapshot atomically). */
bool nocsif_wifi_ap_get(int idx, nocsif_wifi_ap_t *out);

/* Human label for an AP's authmode ("open" / "WEP" / "WPA" / "WPA2" / "WPA3" / …). Keeps the
 * esp_wifi auth enum inside wifi.c so the UI never includes esp_wifi headers. */
const char *nocsif_wifi_authmode_str(uint8_t authmode);

/* True when an AP needs no passphrase (WIFI_AUTH_OPEN) — the join flow skips the keyboard. */
bool nocsif_wifi_authmode_open(uint8_t authmode);

/* ---- monitor mode: promiscuous packet capture (M5-P2, authorized testing) ----------- *
 * Passive 802.11 monitor: the radio drops any station link (single radio) and captures every
 * frame in the air, tallying counts by type + by channel while hopping 1..13 (or holding one
 * locked channel). Nothing is stored or parsed here — this proves stable capture alongside the
 * live UI; PCAP export + frame parsing land in P3. All getters are RAM/volatile (no radio I/O),
 * so they are safe on the LVGL task. Requests are non-blocking (posted to the worker). */

/* Frame class buckets (kept UI-side free of esp_wifi headers). */
typedef enum {
    NOCSIF_WIFI_PKT_MGMT = 0,   /* management frames  */
    NOCSIF_WIFI_PKT_CTRL,       /* control frames     */
    NOCSIF_WIFI_PKT_DATA,       /* data frames        */
    NOCSIF_WIFI_PKT_MISC,       /* other / malformed  */
    NOCSIF_WIFI_PKT_KINDS
} nocsif_wifi_pkt_kind_t;

/* Enter / leave capture. Entering suspends any STA link and remembers it; leaving restores the
 * link if one was up. Brings the radio up first if needed. Gated on reliability safe mode. */
void nocsif_wifi_request_monitor(bool on);

/* Hop across channels 1..13 (on) vs hold the current/locked channel (off). */
void nocsif_wifi_request_monitor_hop(bool hop);

/* Lock capture to a specific channel (1..13); implies hop=off. */
void nocsif_wifi_request_monitor_channel(int ch);

/* Live capture state (safe on the LVGL task). */
bool     nocsif_wifi_monitor_active(void);          /* capture running                 */
bool     nocsif_wifi_monitor_hopping(void);         /* hopping vs locked               */
int      nocsif_wifi_monitor_channel(void);         /* current / locked channel (1..13)*/
uint32_t nocsif_wifi_monitor_total(void);           /* all captured frames             */
uint32_t nocsif_wifi_monitor_rate(void);            /* frames per second (~1 s window) */
uint32_t nocsif_wifi_monitor_count(nocsif_wifi_pkt_kind_t kind);  /* frames of one class */
uint32_t nocsif_wifi_monitor_ch_count(int ch);      /* frames seen on channel 1..13    */
int8_t   nocsif_wifi_monitor_rssi_last(void);       /* last frame's RSSI (dBm)         */
int8_t   nocsif_wifi_monitor_rssi_peak(void);       /* strongest RSSI this session     */

/* Compact live tag for the WiFi-menu Monitor row ("off" / "ch 6" / "lock 6"). Module-owned. */
const char *nocsif_wifi_monitor_tag_str(void);

/* ---- passive frame parser: nearby AP list (M5-P3, authorized testing) ---------------- *
 * A parser task decodes captured beacon / probe-response frames OFF the rx hot path (the rx
 * callback only copies raw bytes into a ring; the parser drains + decodes) into a BSSID-keyed
 * table of nearby access points: SSID (incl. hidden), home channel, security class, RSSI,
 * frames-seen, and a best-effort vendor from the OUI. Purely passive — nothing is transmitted.
 * Enabling the parser implies capture is running (it starts if idle, suspending any STA link).
 * All getters are snapshot copies, safe on the LVGL task. */

/* Security class derived from the beacon/probe capability bit + RSN/WPA information elements. */
typedef enum {
    NOCSIF_WIFI_SEC_OPEN = 0,   /* no privacy bit, no RSN/WPA        */
    NOCSIF_WIFI_SEC_WEP,        /* privacy bit, no RSN/WPA           */
    NOCSIF_WIFI_SEC_WPA,        /* WPA (vendor IE 00-50-F2) only     */
    NOCSIF_WIFI_SEC_WPA2,       /* RSN, PSK                          */
    NOCSIF_WIFI_SEC_WPA3,       /* RSN with an SAE AKM               */
    NOCSIF_WIFI_SEC_WPA2E,      /* RSN with an 802.1X (EAP) AKM      */
} nocsif_wifi_sec_t;

/* One passively-seen access point (a snapshot copy for the UI). */
typedef struct {
    uint8_t  bssid[6];
    char     ssid[33];   /* NUL-terminated; "" = hidden (zero-length / all-null SSID)      */
    uint8_t  channel;    /* home channel from the DS-parameter IE, else the rx channel     */
    int8_t   rssi;       /* last seen, dBm                                                 */
    uint8_t  security;   /* nocsif_wifi_sec_t                                              */
    uint16_t frames;     /* beacon / probe-response frames seen from this BSSID            */
    uint32_t age_ms;     /* since last seen (staleness)                                    */
    char     vendor[12]; /* short vendor from the OUI, or "" when unknown                  */
} nocsif_wifi_mon_ap_t;

/* Turn the passive parser on / off. On implies capture is running (starts it if idle); off
 * stops parsing but leaves capture running (stop capture on the Monitor screen). Non-blocking. */
void nocsif_wifi_request_parse(bool on);

/* True while the parser is decoding captured frames. */
bool nocsif_wifi_parse_active(void);

/* Nearby-AP table: entry count, and a lock-free snapshot copy of entry `idx` (0..count-1).
 * The table is kept in first-seen order (stable rows); the stalest entry is evicted when full. */
int  nocsif_wifi_mon_ap_count(void);
bool nocsif_wifi_mon_ap_get(int idx, nocsif_wifi_mon_ap_t *out);

/* A generation that bumps on a STRUCTURAL change (a new BSSID appears or one is evicted), so the
 * list screen can rebuild its rows only when the set changes (field updates don't churn it). */
uint32_t nocsif_wifi_mon_ap_gen(void);

/* Human label for a security class ("open" / "WEP" / "WPA" / "WPA2" / "WPA3" / "WPA2-E"). */
const char *nocsif_wifi_sec_str(uint8_t sec);

/* Compact live tag for the WiFi-menu "Live Networks" row ("off" / "3 seen"). Module-owned. */
const char *nocsif_wifi_mon_ap_tag_str(void);

/* ---- passive station list (M5-P3·2, authorized testing) ----------------------------- *
 * The same parser also maps DATA frames to their sender/receiver: which client device is
 * associated with which access point. The rx copy-out is widened to include a data frame's
 * MAC header (not its payload); the parser reads the ToDS/FromDS bits + addresses OFF the hot
 * path into a station-keyed table. Broadcast/multicast and the AP's own address are excluded.
 * Purely passive. Populated whenever the parser is on (nocsif_wifi_request_parse). Snapshot
 * copies, safe on the LVGL task. */

/* One passively-seen station / client device (a snapshot copy for the UI). */
typedef struct {
    uint8_t  mac[6];      /* the client / station address                                  */
    uint8_t  bssid[6];    /* the access point it is talking to (all-zero if not resolved)   */
    char     ssid[33];    /* the AP's SSID if that BSSID is in the AP table, else ""        */
    uint8_t  channel;     /* rx channel it was last seen on                                 */
    int8_t   rssi;        /* last seen, dBm                                                 */
    uint16_t frames;      /* data frames seen from/to this station                          */
    uint32_t age_ms;      /* since last seen (staleness)                                    */
    char     vendor[12];  /* short vendor from the OUI, "random" for a locally-admin MAC    */
} nocsif_wifi_mon_sta_t;

int  nocsif_wifi_mon_sta_count(void);
bool nocsif_wifi_mon_sta_get(int idx, nocsif_wifi_mon_sta_t *out);
uint32_t nocsif_wifi_mon_sta_gen(void);   /* bumps on a structural change (insert / evict) */
const char *nocsif_wifi_mon_sta_tag_str(void);   /* "off" / "5 seen" for the WiFi-menu row */

/* ---- passive probe-request harvest (M5-P3·2, authorized testing) --------------------- *
 * Probe requests (management subtype 4) are already in the copy-out ring; the parser reads
 * the requesting device's address + the SSID it is searching for into a (device, SSID) table.
 * An empty SSID is a wildcard/broadcast probe. Purely passive. Snapshot copies, LVGL-safe. */

/* One harvested probe request (a snapshot copy for the UI). */
typedef struct {
    uint8_t  mac[6];      /* the searching device                                           */
    char     ssid[33];    /* the requested SSID; "" = a broadcast/wildcard probe            */
    int8_t   rssi;        /* last seen, dBm                                                 */
    uint16_t count;       /* probe-request frames seen for this (device, SSID)              */
    uint32_t age_ms;      /* since last seen (staleness)                                    */
    char     vendor[12];  /* short vendor from the OUI, "random" for a locally-admin MAC    */
} nocsif_wifi_mon_probe_t;

int  nocsif_wifi_mon_probe_count(void);
bool nocsif_wifi_mon_probe_get(int idx, nocsif_wifi_mon_probe_t *out);
uint32_t nocsif_wifi_mon_probe_gen(void);
const char *nocsif_wifi_mon_probe_tag_str(void);   /* "off" / "7 seen" for the WiFi-menu row */

/* ---- PCAP capture to microSD (M5-P3·3, authorized testing) --------------------------- *
 * Writes every captured frame (all types, bounded snaplen) to a standard PCAP file on the card,
 * each record prefixed with a minimal radiotap header (channel + signal), so it opens straight
 * in Wireshark (LINKTYPE_IEEE802_11_RADIOTAP). A SEPARATE all-frames ring feeds a dedicated
 * big-stack writer task — FAT file I/O must never run on the lean parser task (it overflows its
 * stack). The `/sd` volume is app-owned (nocsif_usb_gadget_claim_sd); recording is refused while
 * File Share hands the card to a host. Enabling implies capture is running (starts it if idle,
 * suspending any STA link). All getters are RAM/volatile, safe on the LVGL task. */

/* Start / stop writing captured frames to a PCAP file. Non-blocking (posted to the worker). */
void nocsif_wifi_request_pcap(bool on);

/* True while frames are being written to the file. */
bool nocsif_wifi_pcap_active(void);

/* Live counters for the session (safe on the LVGL task). */
uint32_t nocsif_wifi_pcap_frames(void);    /* frames written to the file            */
uint32_t nocsif_wifi_pcap_bytes(void);     /* bytes written (incl. headers)         */
uint32_t nocsif_wifi_pcap_dropped(void);   /* frames dropped (ring full)            */

/* The current / most-recent PCAP file path ("" if none yet). Module-owned. */
const char *nocsif_wifi_pcap_path(void);

/* Compact status ("off" / "rec" / "no card" / "file share" / "err" / "stream" / "no host") + the
 * WiFi-menu row tag. */
const char *nocsif_wifi_pcap_status_str(void);
const char *nocsif_wifi_pcap_tag_str(void);

/* ---- Live-PCAP over USB-CDC (M5-P5+, authorized testing) ------------------------------- *
 * Instead of writing a file, stream the captured frames to the host over the USB CDC serial port
 * as a live PCAP feed, so a host tool (Wireshark via the bundled tools/extcap plugin) reads them in
 * real time. Auto-requests CDC gadget mode + monitor, then waits for the host to open the port (DTR)
 * before emitting the stream. Shares the single writer/ring with the file recorder — only one may be
 * active. The nocsif_wifi_pcap_* counters/status above report the stream (status "stream" when a
 * host is reading, "no host" while armed and waiting). */

/* Start / stop streaming captured frames to the host over USB-CDC. Non-blocking (posted). */
void nocsif_wifi_request_pcap_stream(bool on);

/* True while the CDC live stream is armed (waiting for a host or actively streaming). */
bool nocsif_wifi_pcap_stream_active(void);

/* ---- WPA key-exchange + PMKID observation (M5-P4·1, authorized testing) --------------- *
 * The 4-way key handshake and the PMKID an AP advertises both ride in DATA frames as EAPOL
 * (ethertype 0x888e). The rx copy-out already carries a data frame's MAC header for the station
 * map; when a frame is EAPOL it is copied in full (the rare exception to the short data snaplen),
 * and the parser decodes it OFF the hot path: which of messages 1..4 was seen, and the PMKID from
 * the RSN key-data element in message 1. Results collect in a BSSID-keyed table (mirroring the AP
 * list). Purely PASSIVE observation — nothing is transmitted; the watch never solicits a handshake.
 * Populated whenever the parser is on (nocsif_wifi_request_parse). Snapshot copies, LVGL-safe.
 *
 * Optional save-to-SD reuses the P3·3 writer with an EAPOL filter: only EAPOL frames + the beacon /
 * probe-response that names the network are written, to /sd/nocsif/wifi/hs-NNN.pcap (Wireshark /
 * hcxtools ready). Shares the single writer with full PCAP — only one may record at a time. */

/* One BSSID's observed key-exchange state (a snapshot copy for the UI). */
typedef struct {
    uint8_t  bssid[6];
    char     ssid[33];    /* the AP's SSID if that BSSID is in the AP table, else ""        */
    uint8_t  msg_mask;    /* bit0=msg1 … bit3=msg4 of the 4-way exchange seen               */
    bool     has_pmkid;   /* an RSN PMKID was observed in message 1                          */
    uint8_t  pmkid[16];   /* the PMKID bytes (valid only when has_pmkid)                     */
    int8_t   rssi;        /* last seen, dBm                                                 */
    uint16_t frames;      /* EAPOL frames seen for this BSSID                                */
    uint32_t age_ms;      /* since last seen (staleness)                                    */
    char     vendor[12];  /* short vendor from the OUI                                       */
} nocsif_wifi_mon_hs_t;

int  nocsif_wifi_mon_hs_count(void);
bool nocsif_wifi_mon_hs_get(int idx, nocsif_wifi_mon_hs_t *out);
uint32_t nocsif_wifi_mon_hs_gen(void);            /* bumps on a structural change (insert / evict) */
const char *nocsif_wifi_mon_hs_tag_str(void);     /* "off" / "2 seen" for the WiFi-menu row */

/* True when a captured entry is enough to attempt an offline recovery: a PMKID, or both the AP's
 * message 1 and the client's message 2 (the pair the offline check needs). */
bool nocsif_wifi_hs_crackable(const nocsif_wifi_mon_hs_t *hs);

/* Export the captured PMKIDs + 4-way handshakes to a hashcat-22000 file on /sd (M5 passive polish):
 * /sd/nocsif/wifi/nocsif.hc22000, WPA*01 (PMKID) + WPA*02 (handshake) lines — crackable without the
 * PC-side hcxpcapngtool step. Non-blocking (a one-shot big-stack writer task does the SD I/O). The
 * result reads back through the status/path getters. */
void nocsif_wifi_request_export_hc22000(void);
const char *nocsif_wifi_hc_status_str(void);   /* "" / "exporting…" / "N lines written" / "no microSD card" / … */
const char *nocsif_wifi_hc_path(void);         /* the output file path ("" until an export runs) */

/* Start / stop writing the observed EAPOL frames (+ naming beacons) to /sd/nocsif/wifi/hs-NNN.pcap.
 * Reuses the PCAP writer with an EAPOL filter; refused while full PCAP is already recording.
 * Non-blocking (posted to the worker). Live state reads back through the nocsif_wifi_pcap_* getters. */
void nocsif_wifi_request_hs_capture(bool on);

/* ---- passive anomaly detectors (M5-P4·2, authorized testing) ------------------------- *
 * Two purely passive signals derived from the capture already running — nothing is transmitted.
 *
 * (1) Management-frame rate: deauthentication (subtype 12) + disassociation (subtype 10) counts and
 *     a per-second combined rate over the ~1 s monitor window. A sustained elevated rate is the
 *     classic signature of a deauthentication / disassociation flood in range. Tallied in the rx
 *     path, so it works under bare monitor (the parser need not be on).
 * (2) Duplicate-SSID / twin: one SSID advertised by more than one BSSID. Legitimate for a roaming
 *     mesh, but a security-class MISMATCH among them (e.g. an open clone of a secured network) is
 *     the strong twin tell. Derived from the passive AP table (parser must be on). */

/* Management-frame rate counters (RAM/volatile; LVGL-task-safe). Reset when capture (re)starts. */
uint32_t nocsif_wifi_deauth_count(void);       /* deauthentication frames seen this session */
uint32_t nocsif_wifi_disassoc_count(void);     /* disassociation frames seen this session   */
uint32_t nocsif_wifi_deauth_rate(void);        /* combined deauth+disassoc per second (~1 s window) */
uint32_t nocsif_wifi_deauth_peak_rate(void);   /* peak combined per-second rate this session */

/* Compact live tag for the WiFi-menu "Anomalies" row ("off" / "ok" / "N/s"). Module-owned. */
const char *nocsif_wifi_anomaly_tag_str(void);

/* One duplicate-SSID group (a snapshot copy for the UI). */
typedef struct {
    char    ssid[33];      /* the shared (non-empty) SSID                                       */
    uint8_t bssids;        /* distinct BSSIDs advertising it (>= 2 to be reported)              */
    bool    sec_mismatch;  /* they do NOT all share one security class (the strong twin tell)   */
    bool    has_open;      /* at least one advertises open (no privacy) — a common clone tactic */
    uint8_t sec_lo;        /* weakest security class seen (nocsif_wifi_sec_t)                   */
    uint8_t sec_hi;        /* strongest security class seen                                     */
} nocsif_wifi_mon_dup_t;

/* Fill `arr` (up to `max` entries) with the duplicate-SSID groups; returns the total group count
 * (a value > max means only the first `max` were written). Computed fresh from the AP table.
 * Safe on the LVGL task (snapshots the table under the lock, then analyses off-lock). */
int nocsif_wifi_mon_dup_snapshot(nocsif_wifi_mon_dup_t *arr, int max);

/* ---- management-frame TX (M5-P5·1, active — authorized testing) ---------------------- *
 * The FIRST active WiFi op: transmit deauthentication / disassociation frames that spoof a chosen
 * access point as the source, to the broadcast address (all its clients), at a bounded rate while
 * the radio holds the target's channel. Used to solicit a reconnect — which the P4·1 handshake
 * capture then observes — or to exercise the P4·2 detectors, in an authorized test. Every call is
 * non-blocking (posted to the worker). Gated on reliability safe mode. */

/* Select the target AP (whose BSSID is spoofed as the frame source). `channel` is its home channel;
 * `ssid` is for the on-screen label only. */
void nocsif_wifi_set_mgmt_target(const uint8_t bssid[6], int channel, const char *ssid);

/* Frame kind: false = deauthentication (subtype 12), true = disassociation (subtype 10). */
void nocsif_wifi_set_mgmt_disassoc(bool disassoc);
bool nocsif_wifi_mgmt_disassoc(void);

/* Start / stop transmitting to the selected target. Requires a target; brings the radio up on the
 * target's channel first. Refused in reliability safe mode. Stopped when monitor stops. */
void nocsif_wifi_request_mgmt_tx(bool on);

/* Live TX state (RAM/volatile; safe on the LVGL task). */
bool     nocsif_wifi_mgmt_tx_active(void);
bool     nocsif_wifi_mgmt_has_target(void);
uint32_t nocsif_wifi_mgmt_tx_count(void);      /* frames transmitted this session */
uint32_t nocsif_wifi_mgmt_tx_rate(void);       /* frames per second (~1 s window) */
const char *nocsif_wifi_mgmt_target_str(void); /* "SSID · bssid · ch N" or "none" */
const char *nocsif_wifi_mgmt_tx_tag_str(void); /* "off" / "armed" / "N/s" for the menu row */

/* ---- beacon TX (M5-P5·2, active — authorized testing) -------------------------------- *
 * Advertises a USER-MANAGED list of SSIDs by transmitting beacon frames, each with its own locally-
 * administered BSSID, on the held channel. The list is edited on-watch (add via keyboard · rename ·
 * enable/disable · delete) and persisted to NVS. Only ENABLED entries transmit. Reuses the P5·1
 * raw-TX path. Non-blocking / safe-mode gated. The list API is safe on the LVGL task (locked). */
void nocsif_wifi_request_beacon(bool on);      /* start / stop transmitting the enabled SSIDs */
bool nocsif_wifi_beacon_active(void);
uint32_t nocsif_wifi_beacon_frames(void);      /* beacon frames transmitted this session */
uint32_t nocsif_wifi_beacon_rate(void);        /* frames per second (~1 s window)        */
const char *nocsif_wifi_beacon_tag_str(void);  /* "off" / "ready" / "N/s" for the menu row */

/* Managed SSID list (persisted). Mutations dedup, clamp to the max, and re-save. */
int  nocsif_wifi_beacon_count(void);           /* entries in the list */
uint32_t nocsif_wifi_beacon_gen(void);         /* bumps on any list change */
bool nocsif_wifi_beacon_get(int idx, char *out, size_t len, bool *enabled);
bool nocsif_wifi_beacon_add(const char *ssid); /* append (false if empty/duplicate/full) */
void nocsif_wifi_beacon_remove(int idx);
void nocsif_wifi_beacon_rename(int idx, const char *ssid);
void nocsif_wifi_beacon_toggle(int idx);       /* flip an entry's enabled flag */
void nocsif_wifi_beacon_add_decoys(int n);     /* bulk-append up to n generated "NocSif-NN" names */

/* ---- software access point (M5-P5·3, active — authorized testing) -------------------- *
 * Brings the on-SoC radio up as an OPEN software access point and reports the client devices that
 * join. A single radio makes this mutually exclusive with monitor / STA: entering AP suspends any
 * capture + the station link (remembered, and restored on exit); starting monitor / a scan / a join
 * conversely tears the AP down. The connected-client list is refreshed off the LVGL task from the
 * driver's association table + the built-in DHCP-server leases, and published lock-free. Config
 * (SSID · channel · hidden) is cached, persisted to NVS, and applied on Start (a live change while
 * running re-applies immediately). Gated on reliability safe mode. Scope every use to networks you
 * are authorized to operate. */

/* Start / stop the software AP. Non-blocking (posted to the worker). Start applies the cached config
 * and suspends monitor/STA; stop returns the radio to idle STA (and rejoins the pre-AP link if one
 * was up and auto-join is on). Refused in reliability safe mode. */
void nocsif_wifi_request_ap(bool on);

/* Config setters (LVGL-task-safe): cache + persist to NVS. If the AP is already running they also
 * re-apply live (the SSID/channel change takes effect on the next beacon). `ssid` is clamped to 32
 * chars; `channel` to 1..13. */
void nocsif_wifi_ap_set_ssid(const char *ssid);
void nocsif_wifi_ap_set_channel(int channel);
void nocsif_wifi_ap_set_hidden(bool hidden);

/* Published AP state (no radio I/O; safe on the LVGL task). */
bool        nocsif_wifi_ap_active(void);      /* the AP is up                                  */
const char *nocsif_wifi_ap_ssid(void);        /* configured SSID (module-owned)                */
int         nocsif_wifi_ap_channel(void);     /* configured channel (1..13)                    */
bool        nocsif_wifi_ap_hidden(void);      /* SSID hidden from beacons                       */
const char *nocsif_wifi_ap_ip_str(void);      /* the AP's own IPv4 (gateway), "" until started  */

/* Connected-client table (lock-free snapshot). `mac` (6 bytes) and `ip` ("" until the DHCP lease
 * lands) plus last-seen RSSI. `nocsif_wifi_ap_gen` bumps on a membership change so the list screen
 * rebuilds its rows only when the set changes. */
int  nocsif_wifi_ap_client_count(void);
bool nocsif_wifi_ap_client_get(int idx, uint8_t mac[6], char *ip, size_t iplen, int8_t *rssi);
uint32_t nocsif_wifi_ap_gen(void);

/* Compact live tag for the WiFi-menu "Access Point" row ("off" / "on" / "N joined"). Module-owned. */
const char *nocsif_wifi_ap_tag_str(void);

/* ---- captive portal (M5-P5·4, active — authorized testing) --------------------------- *
 * Layers a captive-portal presentation on the OPEN software AP: a UDP:53 DNS redirector answers every
 * query with the AP's own IP so a joining client's connectivity check resolves to the watch, and an
 * esp_http_server serves a landing page (from /sd/nocsif/wifi/portal.html, or a built-in notice if
 * absent) for every request — which makes client OSes surface their captive-portal sheet. Each request
 * is recorded (client IP · method · path · any submitted fields) to a RAM ring for the on-watch live
 * log and appended to /sd/nocsif/wifi/portal-NNN.log for the operator's authorized-testing review.
 * Starting the portal brings the AP up if it is not already; stopping it leaves the AP running. The
 * portal cannot outlive the AP (stopping the AP stops the portal). Gated on reliability safe mode. */

/* Start / stop the captive portal. Non-blocking (posted to the worker). Start ensures the AP is up
 * first. Refused in reliability safe mode. */
void nocsif_wifi_request_portal(bool on);

/* Published portal state (no radio I/O; safe on the LVGL task). */
bool     nocsif_wifi_portal_active(void);
uint32_t nocsif_wifi_portal_hits(void);          /* total requests served this session       */
uint32_t nocsif_wifi_portal_gen(void);           /* bumps on each new request (UI refresh)    */
const char *nocsif_wifi_portal_page_src(void);   /* "sd" / "built-in" / "" (before first hit) */

/* Recent client-interaction ring (newest first). `idx` 0 = the most recent request. */
int  nocsif_wifi_portal_log_count(void);
bool nocsif_wifi_portal_log_get(int idx, char *out, size_t len);

/* Landing-page selection: pick which HTML file under /sd/nocsif/wifi/portals/ is served (or "" for the
 * built-in notice). The choice is NVS-persisted and applied at once on a running portal. The list of
 * available pages is enumerated UI-side (SD directory listing). */
const char *nocsif_wifi_portal_selected_page(void);   /* "" = built-in */
void        nocsif_wifi_portal_set_page(const char *name);

/* Compact live tag for the WiFi-menu "Captive Portal" row ("off" / "on" / "N hits"). Module-owned. */
const char *nocsif_wifi_portal_tag_str(void);

/* ---- §4.8a Companion control surface (L4) — the on-network web remote — P1 transport ------ *
 * Raises an app-free control surface: an OPEN SoftAP (device-name SSID, no join gate — operator call)
 * hosting an esp_http_server on :80 plus an mDNS responder, so any phone/laptop on the AP browses to
 * `nocsif.local` and controls the watch — no app, no pairing code. Anyone on the AP can control the
 * watch; the off-by-default toggle + the on-watch "linked" indicator are the guardrails (P2/P3 add the
 * command channel + live view). Single-radio reality: raising the surface first stops the promiscuous
 * monitor/parser and the captive portal (mutually exclusive on the one radio + port 80), and the UI
 * releases the BLE controller so the WiFi surface gets the contiguous internal-DMA it needs. Gated on
 * reliability safe mode. Non-blocking (posted to the worker). Turning it off tears the AP/mDNS/HTTP
 * down and restores the prior STA link. */
void nocsif_wifi_companion_set(bool on);

/* Published companion state (no radio I/O; safe on the LVGL task). */
bool        nocsif_wifi_companion_active(void);
const char *nocsif_wifi_companion_ssid(void);        /* open-AP SSID clients join ("" until raised)    */
const char *nocsif_wifi_companion_url(void);         /* "nocsif.local" (mDNS) or the AP IP fallback    */
const char *nocsif_wifi_companion_status_str(void);  /* one-line screen status                          */
int         nocsif_wifi_companion_clients(void);      /* joined clients (mirrors the AP client count)   */

/* Compact live tag for the System "Companion" row ("off" / "on" / "on · N"). Module-owned. */
const char *nocsif_wifi_companion_tag_str(void);

/* ---- §4.8a Companion — P2 command channel (phone → watch) --------------------------------- *
 * The companion HTTP server accepts POST commands and forwards each as one of these to a handler the
 * UI registers (`nocsif_wifi_companion_set_cmd_handler`). wifi.c owns transport + JSON parsing on the
 * httpd task; the UI handler marshals the action onto the LVGL task (it must never touch LVGL from the
 * httpd task). Endpoints: POST /api/launch {id} · /api/back · /api/home · /api/type {text} ·
 * /api/key {key:"backspace"|"enter"} · /api/brightness {v} · /api/volume {v}. `arg` carries the app
 * id / text / key name; for brightness/volume it carries the 0-255 level as a decimal string. */
typedef enum {
    NOCSIF_COMPANION_CMD_LAUNCH = 1,   /* arg = app id (a k_screens id, incl. action rows flash/dnd) */
    NOCSIF_COMPANION_CMD_BACK,         /* nav back one screen                                        */
    NOCSIF_COMPANION_CMD_HOME,         /* jump to Home                                               */
    NOCSIF_COMPANION_CMD_TYPE,         /* arg = text to append into the focused watch text field     */
    NOCSIF_COMPANION_CMD_KEY,          /* arg = "backspace" / "enter" (edit the focused field)       */
    NOCSIF_COMPANION_CMD_BRIGHTNESS,   /* arg = "0".."255" — Control-Center brightness level          */
    NOCSIF_COMPANION_CMD_VOLUME,       /* arg = "0".."255" — Control-Center volume level              */
    NOCSIF_COMPANION_CMD_BUTTON,       /* arg = "fn.short|fn.long|pwr.short|pwr.long" — side buttons  */
    NOCSIF_COMPANION_CMD_CAST,         /* arg = "1"/"0" — casting (blank watch panel, phone-as-display)*/
} nocsif_companion_cmd_type_t;

typedef struct {
    nocsif_companion_cmd_type_t type;
    char arg[192];
} nocsif_companion_cmd_t;

typedef void (*nocsif_companion_cmd_fn_t)(const nocsif_companion_cmd_t *cmd);

/* Register the UI-side command handler. Called once at UI init. The handler runs on the HTTP server's
 * task, so it must marshal any LVGL work onto the LVGL task (lvgl_port_lock + lv_async_call). */
void nocsif_wifi_companion_set_cmd_handler(nocsif_companion_cmd_fn_t fn);

/* ---- §4.8a Companion — P2 menu mirror + P3 live state (watch → phone) ---------------------- *
 * Two read-only JSON providers the UI registers so the companion server can mirror the watch WITHOUT
 * hardcoding its layout. Both run on the HTTP/timer task, so each provider MUST touch only immutable
 * registry data or plain cached scalars (never LVGL objects).
 *   - menu provider  → GET /api/menu : the 3 Home categories and their real rows {id,label,en,warn}.
 *   - state provider → the /ws WebSocket push (~2×/s) and GET /api/ping: live watch state (screen
 *     title, focused-field flag, toggles, brightness, volume) so the page can sync + raise the native
 *     phone keyboard when a watch text field is focused. Each fills `buf` (NUL-terminated JSON). */
typedef void (*nocsif_companion_json_fn_t)(char *buf, size_t len);
void nocsif_wifi_companion_set_menu_fn(nocsif_companion_json_fn_t fn);
void nocsif_wifi_companion_set_state_fn(nocsif_companion_json_fn_t fn);

/* ---- §4.8a Companion — P3 screen mirror (watch → phone) ------------------------------------ *
 * The UI captures the live screen (~1×/s, only while a /ws client is connected), downsamples it to a
 * small RGB565 thumbnail, and hands it here; the companion server copies it under a mutex and pushes it
 * as a BINARY /ws frame (an 8-byte header 'N','F',w16,h16,fmt,rsv + w*h*2 RGB565-LE bytes) when it is
 * new. `nocsif_wifi_companion_ws_clients()` lets the UI skip the (heavy) capture when nobody is watching. */
void nocsif_wifi_companion_publish_thumb(const uint8_t *rgb565_le, int w, int h);
int  nocsif_wifi_companion_ws_clients(void);

/* §4.8a interactive control — the phone streams touch points up the /ws socket; wifi.c parses them on
 * the httpd task and calls this UI-registered handler (which feeds the watch's remote pointer indev).
 * (x,y) are watch-space pixels; pressed = finger down. High-frequency + lock-free (a packed scalar). */
typedef void (*nocsif_companion_touch_fn_t)(int x, int y, int pressed);
void nocsif_wifi_companion_set_touch_fn(nocsif_companion_touch_fn_t fn);

#ifdef __cplusplus
}
#endif
