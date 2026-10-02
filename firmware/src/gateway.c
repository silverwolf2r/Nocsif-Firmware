/*
 * NocSif — network gateway (travel-router subsystem). See gateway.h for the capability overview
 * and the single-radio integration constraint. Owner's-own-device networking infrastructure.
 *
 * ESP-IDF assumption: v5.5.x (this tree pins idf ">=5.4"; developed/verified against 5.5.4).
 * lwIP NAPT (esp_netif_napt_enable) needs CONFIG_LWIP_IP_FORWARD=y + CONFIG_LWIP_IPV4_NAPT=y —
 * added in sdkconfig.defaults (see the integration note). WireGuard uses the managed component
 * `trombik/esp_wireguard` (a wrapper over smartalock/wireguard-lwip); the code is compiled only
 * when its header is present (__has_include) and otherwise reports the tunnel as unavailable, so
 * the module always builds.
 *
 * Design = wifi.c's worker pattern, plus a DESIRED-vs-ACTUAL reconcile so the five toggles are
 * orthogonal, reusable state rather than one hardcoded path:
 *
 *   request_*()  -> set desired flag (+ persist on the caller task) -> post CMD_RECONCILE
 *   gw_task      -> gw_reconcile(): bring ACTUAL state to match DESIRED, in dependency order:
 *                     uplink (STA or dnst) -> SoftAP+NAPT (share) -> WireGuard default route
 *                     (tunnel) -> DNS server (filter and/or portal gate) -> HTTP splash (portal)
 *   getters      -> module-owned cached scalars/strings (LVGL-task-safe)
 *
 * SD ownership: while the DNS filter or the portal needs the card, the gateway CLAIMS /sd
 * (nocsif_usb_gadget_claim_sd) for its session, exactly like the PCAP writer / captive portal, so
 * it is mutually exclusive with USB File Share. Blocklist reads use a STATIC INTERNAL line buffer
 * (never a PSRAM-stack local) to avoid the SD int-DMA bounce class (docs/LESSONS.md).
 */
#include "gateway.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <sys/stat.h>
#include <dirent.h>        /* opendir/readdir — scan the Pihole/wireguard folders */
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"          /* blocklist rebuild/lookup mutex */
#include "freertos/idf_additions.h"   /* xTaskCreateWithCaps / vTaskDeleteWithCaps — PSRAM stacks */

#include "esp_attr.h"                 /* DRAM_ATTR — force internal buffers off any PSRAM stack */
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_server.h"
#include "esp_http_client.h"          /* uplink captive-portal probe / auto-accept / passthrough */
#if defined(CONFIG_MBEDTLS_CERTIFICATE_BUNDLE)
#  include "esp_crt_bundle.h"         /* HTTPS portal fetch (auto-accept)                        */
#endif
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "lwip/ip4_addr.h"            /* ip4addr_aton — upstream/override parse */
#include "lwip/stats.h"              /* lwip_stats.ip.fw — forwarded-packet throughput sampler */

#include "coex.h"            /* int-DMA telemetry (shared gauge)                 */
#include "wifi.h"            /* uplink preconditions: nocsif_wifi_connected / _ip_str */
#include "settings.h"        /* NVS desired-flag persistence (caller task only)  */
#include "reliability.h"     /* safe-mode gate                                   */
#include "sdcard.h"          /* /sd access lock (blocklist + config reads)       */
#include "usb_gadget.h"      /* claim/release the card for the gateway session   */

/* WireGuard-over-lwIP (managed component). Present -> real tunnel; absent -> reported unavailable. */
#if __has_include("esp_wireguard.h")
#  include "esp_wireguard.h"
#  define GW_HAVE_WIREGUARD 1
#else
#  define GW_HAVE_WIREGUARD 0
#endif

static const char *TAG = "gateway";

/* ---- tunables --------------------------------------------------------------------------- */
#define GW_CMD_QLEN       6
#define GW_SSID_MAX       33
#define GW_PASS_MAX       64
#define GW_STR            48       /* short published-string buffers                          */
#define GW_LINE           320      /* one config / blocklist line incl. NUL                   */
#define GW_PATH           256

#define GW_DNS_PORT       53
#define GW_HTTP_PORT      80

/* DNS blocklist: EVERY *.txt in /sd/nocsif/Pihole/ is parsed (any format — plain domains, hosts
 * "0.0.0.0 domain", AdBlock "||domain^...", dnsmasq "address=/domain/...", wildcard "*.domain") and
 * merged, de-duplicated, into ONE set held in a PSRAM hash table: an arena of NUL-terminated
 * domains + an open-addressing index of arena offsets. No sorting or fixed filename required.
 * allowlist.txt (if present) is the exception file. Lookups are O(1) in RAM (no SD at query time). */
#define BL_DOM_MAX        128            /* longest domain kept (incl. NUL)                       */
#define BL_ALLOW_MAX      512            /* allowlist exceptions (PSRAM)                          */
#define BL_SUFFIX_MAX     8              /* parent suffixes checked per query (sub-domain match)  */
#define BL_ARENA_CAP      (3*1024*1024)  /* PSRAM domain-text arena cap (grown on demand)         */
#define BL_HASH_SLOTS     (1u << 18)     /* 262144 open-addressing slots (1 MB PSRAM), power of 2 */
#define BL_MAX_DOMAINS    150000         /* stop inserting past this (load-factor + arena safety) */
#define BL_MAX_FILES      32             /* .txt files scanned in the folder                      */

#define GW_SIGNED_MAX     16       /* downstream clients that have signed in (portal gate)    */
#define GW_UPSTREAM_TMO_MS 2000    /* upstream resolver reply timeout                         */

/* DNS reply cache (latency win): repeated lookups during a browsing session skip the upstream round-trip.
 * dns_task-only (no lock); entries live in PSRAM. Filtering runs BEFORE the cache, so a domain that becomes
 * blocked after being cached is still sinkholed (order saves us). Only NOERROR replies with answers are
 * cached. TTL is parsed from the reply and clamped. */
#define DNS_CACHE_ENTRIES  64      /* cached (name,qtype) replies (~42 KB PSRAM total)         */
#define DNS_CACHE_RESP_MAX 512     /* max cached reply payload (our rx path handles <=600)     */
#define DNS_CACHE_TTL_MIN  30u     /* clamp a cached reply's life to [30, 600] s               */
#define DNS_CACHE_TTL_MAX  600u

/* NVS desired-flag keys (<=15 chars, "nocsif" namespace via settings.c). */
#define K_SHARE   "gw_share"
#define K_FILTER  "gw_filter"
#define K_TUNNEL  "gw_tunnel"
#define K_PORTAL  "gw_portal"
#define K_DNST    "gw_dnst"
/* WireGuard on-watch config selection + minor-field overrides (keys stay in the SD file). */
#define K_WGFILE  "gw_wgfile"      /* selected .conf name under /sd/nocsif/wireguard/            */
#define K_WGEP    "gw_wgep"        /* endpoint host override ("" = use the file)                 */
#define K_WGPORT  "gw_wgport"      /* endpoint port override (0 = use the file)                  */
#define K_WGDNS   "gw_wgdns"       /* DNS override ("" = use the file)                           */
/* dnst editable settings (scaffold — persisted for later, tunnelling not implemented). */
#define K_DNSRV   "gw_dnsrv"       /* dnst server domain                                         */
#define K_DNKEY   "gw_dnkey"       /* dnst shared key                                            */
#define K_DNMTU   "gw_dnmtu"       /* dnst MTU                                                    */
/* SoftAP name / passphrase edited on-watch (override gateway.conf). */
#define K_APSSID  "gw_apssid"      /* hotspot SSID                                                */
#define K_APPASS  "gw_appass"      /* hotspot passphrase ("" = open)                             */
/* Uplink captive-portal sign-in (the watch's STA behind a hotel/cafe portal). */
#define K_AUTOACC "gw_autoacc"     /* 1 = auto-accept simple Terms/Continue portals               */
#define K_PXY     "gw_pxy"         /* 1 = passthrough the portal to a downstream phone (Tier 2)    */

/* ---- worker commands -------------------------------------------------------------------- */
typedef enum { CMD_RECONCILE, CMD_RELOAD } gw_cmd_t;

static QueueHandle_t s_q;
static TaskHandle_t  s_task;

/* ---- desired state (owner intent; NVS-persisted, RAM-mirrored) -------------------------- */
static bool s_want_share, s_want_filter, s_want_tunnel, s_want_portal, s_want_dnst;

/* ---- actual state (owned by the worker; read cached by the UI) -------------------------- */
static bool s_share_active, s_filter_active, s_tunnel_active, s_portal_active, s_dnst_active;
static bool s_available;

/* ---- SoftAP -------------------------------------------------------------------------------- */
static esp_netif_t *s_ap_netif;
static bool          s_ap_up;          /* the downstream SoftAP is beaconing (any capability)  */
static bool          s_napt_on;        /* NAPT forwarding is enabled on it (the "share" layer)  */
static char          s_ap_ssid[GW_SSID_MAX];
static char          s_ap_ip[16];
static char          s_uplink_ip[16];
static int           s_client_cnt;
static uint32_t      s_client_gen;
static esp_timer_handle_t s_tick;         /* 1 s: client count + tunnel liveness + strings   */
static bool          s_sd_claimed;        /* gateway holds /sd for filter/portal              */
static bool          s_ap_dirty;          /* SSID/pass changed -> re-raise a running AP        */

/* ---- published strings (module-owned; snprintf on the worker, benign torn read on UI) --- */
static char s_status[GW_STR], s_detail[GW_STR], s_tag[GW_STR];
static char s_filter_status[GW_STR], s_tunnel_status[GW_STR];
static char s_portal_status[GW_STR], s_dnst_status[GW_STR];
static char s_tunnel_ep[GW_STR];

/* ---- gateway.conf (SoftAP + master options) --------------------------------------------- */
static struct {
    char ssid[GW_SSID_MAX];
    char pass[GW_PASS_MAX];        /* "" -> open AP                                            */
    int  channel;                  /* 0 = follow the STA channel (required in APSTA)           */
    bool hidden;
    int  maxconn;                  /* 1..8                                                     */
    char dns_upstream[16];         /* override resolver, else STA DHCP DNS, else 1.1.1.1       */
    char portal_page[64];          /* filename under /sd/nocsif/portals/, "" = built-in        */
    int  tunnel_port;              /* 0 = use the Endpoint port from wg0.conf                  */
    bool loaded;
} s_cfg;

/* ---- DNS filter state (PSRAM hash set built from all *.txt in the folder) ---------------- */
static char       (*s_bl_allow)[BL_DOM_MAX]; /* PSRAM allowlist exceptions                     */
static int          s_bl_allow_n;
static char        *s_bl_arena;        /* PSRAM: NUL-terminated lowercased domains back-to-back */
static size_t       s_bl_arena_len, s_bl_arena_cap;
static uint32_t    *s_bl_hash;         /* PSRAM: open-addressing table of (arena offset + 1)    */
static int          s_bl_count;        /* distinct domains inserted                             */
static bool         s_bl_trunc;        /* hit a cap (arena / max domains)                       */
static int          s_bl_size = -1;    /* domains loaded, -1 = not loaded                       */
static SemaphoreHandle_t s_bl_mutex;   /* guards rebuild vs the dns_task lookups                */
static DRAM_ATTR char s_bl_linebuf[GW_LINE]; /* INTERNAL buffer for SD reads (no PSRAM bounce) */

static volatile uint32_t s_dns_q, s_dns_blocked, s_dns_fwd, s_dns_cached;

/* DNS reply cache — dns_task-only (single reader+writer, no lock). PSRAM, allocated in dns_start. */
typedef struct {
    int64_t  exp_us;                    /* 0 = empty slot; else esp_timer expiry                     */
    uint16_t qtype;
    uint16_t len;                       /* cached reply payload length                                */
    char     name[BL_DOM_MAX];          /* lowercased query name (cache key with qtype)               */
    uint8_t  resp[DNS_CACHE_RESP_MAX];  /* the upstream reply bytes (replayed with the client's id)   */
} dns_cache_ent_t;
static dns_cache_ent_t *s_dnsc;         /* [DNS_CACHE_ENTRIES] in PSRAM (NULL = cache disabled)       */
static uint32_t         s_dnsc_hand;    /* round-robin eviction hand                                  */

/* ---- uplink captive-portal sign-in state (the watch's STA behind a hotel/cafe portal) ---------- *
 * Declared here so the DNS gate (dns_task) + reconcile can read it. Set by the transient portal task. */
typedef enum {
    UP_NONE = 0,        /* not checked / no uplink                                                    */
    UP_CHECKING,        /* probing connectivity                                                       */
    UP_ONLINE,          /* internet reachable, no portal                                              */
    UP_SIGNIN,          /* captive portal detected — sign-in required                                 */
    UP_SIGNING,         /* auto-accept attempt in progress                                            */
    UP_SIGNED,          /* auto-accept succeeded (online now)                                         */
    UP_LOGIN,           /* portal needs a human login/code — use the phone passthrough                */
    UP_FAILED,          /* auto-accept tried, still blocked                                           */
} up_state_t;
static volatile up_state_t s_up_state;
static char       s_up_url[256];        /* detected portal URL                                        */
static char       s_up_host[96];        /* portal host (Tier 2 passthrough default target)            */
static char       s_up_detail[GW_STR];  /* short status line for the UI                               */
static volatile bool s_up_autoacc;      /* setting: auto-accept simple Terms/Continue portals         */
static volatile bool s_up_passthru;     /* setting: proxy the portal to a downstream phone (Tier 2)   */
static volatile uint32_t s_up_gen;      /* bumps on state change (UI refresh)                         */
static bool       s_up_link_last;       /* STA link edge tracker (gw_tick)                            */
static TaskHandle_t s_up_task;          /* transient PSRAM-stacked probe/auto-accept task             */
/* True while a downstream phone should be steered to the watch to complete the UPSTREAM portal. */
static inline bool up_passthru_active(void)
{
    return s_up_passthru && (s_up_state == UP_SIGNIN || s_up_state == UP_LOGIN || s_up_state == UP_FAILED);
}

/* Forwarded-traffic throughput (NAPT), sampled 1 Hz in gw_tick from lwIP's ip.fw counter. */
static volatile uint32_t s_fwd_pps;    /* forwarded packets/s (both directions)                      */
static volatile uint32_t s_fwd_kbps;   /* rough estimate: pps * 1500 B * 8 (assumes full frames)     */
#if LWIP_STATS && IP_STATS
static uint16_t          s_fwd_last;   /* previous ip.fw sample (STAT_COUNTER is u16 by default)     */
static bool              s_fwd_have;
#endif

/* DNS + splash tasks/sockets */
static TaskHandle_t s_dns_task;
static int          s_dns_sock = -1;
static volatile bool s_dns_run;
static httpd_handle_t s_httpd;

/* portal sign-in allowlist (network-order client IPs) */
static portMUX_TYPE  s_signed_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t      s_signed_ip[GW_SIGNED_MAX];
static int           s_signed_n;
static uint32_t      s_portal_gen;
static char         *s_portal_html;    /* cached splash page (PSRAM), or NULL -> built-in      */
static size_t        s_portal_html_len;

/* ---- WireGuard config (selection + on-watch minor overrides) ---------------------------- *
 * These are available with OR without the esp_wireguard component so the UI can always list /
 * import / view / edit a tunnel. The keys come only from the SD file (too long to type on-watch);
 * endpoint host / port / DNS can be overridden on-watch (persisted in NVS). The "effective" values
 * (file, then overrides applied) are published to the UI and fed to esp_wireguard on connect. */
static char s_wg_file[64] = "wg0.conf";    /* selected .conf under /sd/nocsif/wireguard/            */
static char s_wg_ov_ep[96];                /* endpoint host override ("" = from file)               */
static int  s_wg_ov_port;                  /* endpoint port override (0 = from file)                */
static char s_wg_ov_dns[16];               /* DNS override ("" = from file)                          */
static char s_wg_priv[64], s_wg_pub[64], s_wg_psk[64];   /* keys (from file only)                    */
static char s_wg_ep[96], s_wg_addr[24], s_wg_netmask[24], s_wg_dns[16];   /* effective              */
static int  s_wg_port, s_wg_keepalive;
static bool s_wg_conf_ok;                  /* keys + endpoint + address all present                 */
#if GW_HAVE_WIREGUARD
static wireguard_ctx_t s_wg_ctx;
static bool            s_wg_inited;
#endif

/* ================================ small helpers ========================================== */

static void publish_strings(void);   /* fwd */
static void post(gw_cmd_t c);        /* fwd (portal_task reconciles when it changes online/passthru) */
static esp_err_t h_proxy(httpd_req_t *req);   /* fwd: Tier 2 portal passthrough (defined below)      */
static void portal_kick(bool force);          /* fwd: spawn the uplink-portal probe/auto-accept task */

static bool safe_mode(void) { return nocsif_reliability_safe_mode(); }

static void str_lower(char *s) { for (; *s; ++s) *s = (char)tolower((unsigned char)*s); }

/* Trim CR/LF/space in place. */
static void rstrip(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t'))
        s[--n] = '\0';
}

/* Ensure the SD config dirs exist (create-on-first-use; ignore EEXIST). */
static void ensure_dirs(void)
{
    mkdir("/sd/nocsif", 0777);
    mkdir(NOCSIF_GW_DIR, 0777);
    mkdir(NOCSIF_GW_PIHOLE_DIR, 0777);
    mkdir(NOCSIF_GW_WG_DIR, 0777);
    mkdir(NOCSIF_GW_PORTAL_DIR, 0777);
    mkdir(NOCSIF_GW_DNST_DIR, 0777);
}

/* Claim the card for the gateway session (idempotent). Returns true if the gateway owns /sd. */
static bool sd_claim(void)
{
    if (s_sd_claimed) return true;
    s_sd_claimed = (nocsif_usb_gadget_claim_sd(1500) == ESP_OK);
    if (s_sd_claimed) ensure_dirs();
    return s_sd_claimed;
}
static void sd_release(void)
{
    if (!s_sd_claimed) return;
    nocsif_usb_gadget_release_sd();
    s_sd_claimed = false;
}

/* ============================ config-file parsers ======================================== *
 * All parsers read under nocsif_sdcard_lock into the STATIC INTERNAL line buffer. The caller
 * must already hold the card (sd_claim). Missing file -> defaults / that toggle stays off. */

static void cfg_defaults(void)
{
    memset(&s_cfg, 0, sizeof s_cfg);
    snprintf(s_cfg.ssid, sizeof s_cfg.ssid, "NocSif-Gateway");
    s_cfg.pass[0]  = '\0';        /* open by default; a passphrase in gateway.conf enables WPA2 */
    s_cfg.channel  = 0;           /* follow STA                                                */
    s_cfg.maxconn  = 4;
    s_cfg.hidden   = false;
    s_cfg.tunnel_port = 0;
}

/* key=value line reader shared by gateway.conf + dnstunnel.conf. */
static bool kv_split(char *line, char **k, char **v)
{
    rstrip(line);
    if (line[0] == '#' || line[0] == ';' || line[0] == '\0') return false;
    char *eq = strchr(line, '=');
    if (!eq) return false;
    *eq = '\0';
    *k = line; *v = eq + 1;
    while (**k == ' ' || **k == '\t') (*k)++;
    while (**v == ' ' || **v == '\t') (*v)++;
    rstrip(*k);
    return **k != '\0';
}

/* On-watch SSID/passphrase edits (NVS) override gateway.conf so the Travel Router screen is the
 * source of truth. Safe to read NVS from any task. */
static void cfg_apply_nvs(void)
{
    char sv[GW_SSID_MAX]; nocsif_settings_get_str(K_APSSID, sv, sizeof sv, "");
    if (sv[0]) snprintf(s_cfg.ssid, sizeof s_cfg.ssid, "%s", sv);
    char pv[GW_PASS_MAX]; nocsif_settings_get_str(K_APPASS, pv, sizeof pv, "\x01");   /* sentinel = unset */
    if (pv[0] != '\x01') snprintf(s_cfg.pass, sizeof s_cfg.pass, "%s", pv);           /* "" = deliberately open */
    if (s_cfg.maxconn < 1) s_cfg.maxconn = 1;
    if (s_cfg.maxconn > 8) s_cfg.maxconn = 8;
    if (s_cfg.ssid[0] == '\0') snprintf(s_cfg.ssid, sizeof s_cfg.ssid, "NocSif-Gateway");
}

static void load_gateway_conf(void)
{
    cfg_defaults();
    s_cfg.loaded = true;
    char path[GW_PATH];
    snprintf(path, sizeof path, "%s/gateway.conf", NOCSIF_GW_DIR);
    if (nocsif_sdcard_lock(500)) {
        FILE *f = fopen(path, "r");
        if (f) {
            while (fgets(s_bl_linebuf, sizeof s_bl_linebuf, f)) {
                char *k, *v;
                if (!kv_split(s_bl_linebuf, &k, &v)) continue;
                if      (!strcasecmp(k, "ap_ssid"))     snprintf(s_cfg.ssid, sizeof s_cfg.ssid, "%s", v);
                else if (!strcasecmp(k, "ap_pass"))     snprintf(s_cfg.pass, sizeof s_cfg.pass, "%s", v);
                else if (!strcasecmp(k, "ap_channel"))  s_cfg.channel = atoi(v);
                else if (!strcasecmp(k, "ap_hidden"))   s_cfg.hidden  = (atoi(v) != 0);
                else if (!strcasecmp(k, "ap_maxconn"))  s_cfg.maxconn = atoi(v);
                else if (!strcasecmp(k, "dns_upstream"))snprintf(s_cfg.dns_upstream, sizeof s_cfg.dns_upstream, "%s", v);
                else if (!strcasecmp(k, "portal_page")) snprintf(s_cfg.portal_page, sizeof s_cfg.portal_page, "%s", v);
                else if (!strcasecmp(k, "tunnel_port")) s_cfg.tunnel_port = atoi(v);
            }
            fclose(f);
        }
        nocsif_sdcard_unlock();
    }
    cfg_apply_nvs();   /* on-watch edits win over the file */
}

/* ---- blocklist: multi-file, any-format, PSRAM hash set (requirement 2) ------------------- */

static void bl_unload(void)   /* caller holds s_bl_mutex (or no reader can run) */
{
    if (s_bl_arena) { heap_caps_free(s_bl_arena); s_bl_arena = NULL; }
    if (s_bl_hash)  { heap_caps_free(s_bl_hash);  s_bl_hash = NULL; }
    if (s_bl_allow) { heap_caps_free(s_bl_allow); s_bl_allow = NULL; }
    s_bl_arena_len = s_bl_arena_cap = 0;
    s_bl_count = s_bl_allow_n = 0;
    s_bl_trunc = false;
    s_bl_size = -1;
}

static uint32_t bl_hash_str(const char *s)   /* FNV-1a */
{
    uint32_t h = 2166136261u;
    for (; *s; s++) { h ^= (uint8_t)*s; h *= 16777619u; }
    return h;
}

/* Membership test against the loaded set (exact). */
static bool bl_hash_has(const char *name)
{
    if (!s_bl_hash || !s_bl_arena) return false;
    uint32_t mask = BL_HASH_SLOTS - 1;
    uint32_t i = bl_hash_str(name) & mask;
    for (uint32_t p = 0; p < BL_HASH_SLOTS; p++) {
        uint32_t slot = s_bl_hash[i];
        if (slot == 0) return false;                              /* empty -> absent */
        if (strcmp(s_bl_arena + (slot - 1), name) == 0) return true;
        i = (i + 1) & mask;
    }
    return false;
}

/* Grow the PSRAM arena to hold `need` more bytes (offsets stay valid across realloc). */
static bool bl_arena_ensure(size_t need)
{
    if (s_bl_arena_len + need <= s_bl_arena_cap) return true;
    if (s_bl_arena_cap >= BL_ARENA_CAP) return false;
    size_t ncap = s_bl_arena_cap ? s_bl_arena_cap * 2 : (256 * 1024);
    while (ncap < s_bl_arena_len + need) ncap *= 2;
    if (ncap > BL_ARENA_CAP) ncap = BL_ARENA_CAP;
    char *n = heap_caps_realloc(s_bl_arena, ncap, MALLOC_CAP_SPIRAM);
    if (!n) return false;
    s_bl_arena = n; s_bl_arena_cap = ncap;
    return s_bl_arena_len + need <= ncap;
}

static void bl_insert(const char *dom)
{
    if (!s_bl_hash) return;
    if (s_bl_count >= BL_MAX_DOMAINS) { s_bl_trunc = true; return; }
    uint32_t mask = BL_HASH_SLOTS - 1;
    uint32_t i = bl_hash_str(dom) & mask;
    for (uint32_t p = 0; p < BL_HASH_SLOTS; p++) {
        uint32_t slot = s_bl_hash[i];
        if (slot == 0) {
            size_t len = strlen(dom) + 1;
            if (!bl_arena_ensure(len)) { s_bl_trunc = true; return; }
            memcpy(s_bl_arena + s_bl_arena_len, dom, len);
            s_bl_hash[i] = (uint32_t)(s_bl_arena_len + 1);
            s_bl_arena_len += len;
            s_bl_count++;
            return;
        }
        if (strcmp(s_bl_arena + (slot - 1), dom) == 0) return;    /* duplicate */
        i = (i + 1) & mask;
    }
    s_bl_trunc = true;                                            /* table full */
}

/* Is `s` a plausible bare domain? (lowercased already). Rejects IPs, junk, localhost. */
static bool bl_valid_domain(const char *s)
{
    size_t n = strlen(s);
    if (n < 3 || n >= BL_DOM_MAX) return false;
    if (s[0] == '.' || s[0] == '-' || s[n - 1] == '.' || s[n - 1] == '-') return false;
    bool dot = false, alpha = false;
    for (const char *p = s; *p; p++) {
        char c = *p;
        if (c == '.') dot = true;
        else if (c >= 'a' && c <= 'z') alpha = true;
        else if (!((c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
    }
    if (!dot || !alpha) return false;               /* need a dot + a letter (rejects pure IPs) */
    if (strcmp(s, "localhost") == 0) return false;
    return true;
}

/* Clean one whitespace-token (strip ||, *, leading dots, and trailing ^/$/,/path) to a domain. */
static bool bl_clean_token(const char *tok, char *out, size_t outsz)
{
    char t[BL_DOM_MAX * 2];
    snprintf(t, sizeof t, "%s", tok);
    char *p = t;
    while (*p == '|' || *p == '*' || *p == '.') p++;              /* AdBlock/wildcard anchors */
    for (char *q = p; *q; q++) {
        if (*q == '^' || *q == '/' || *q == '$' || *q == ',' || *q == '\r' || *q == '\n') { *q = '\0'; break; }
    }
    size_t o = 0;
    for (char *q = p; *q && o < outsz - 1; q++) out[o++] = (char)tolower((unsigned char)*q);
    out[o] = '\0';
    return bl_valid_domain(out);
}

/* Extract one bare domain from a line of ANY common blocklist format. Returns true + fills out.
 * Handles: plain "domain", hosts "0.0.0.0 domain", AdBlock "||domain^...", "*.domain",
 * dnsmasq "address=/domain/..."; skips comments (# ! ; [ <), AdBlock exceptions (@@). */
static bool bl_extract(const char *line_in, char *out, size_t outsz)
{
    char buf[GW_LINE];
    snprintf(buf, sizeof buf, "%s", line_in);
    char *s = buf;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '\0' || *s == '#' || *s == '!' || *s == ';' || *s == '[' || *s == '<') return false;
    if (s[0] == '@' && s[1] == '@') return false;                /* AdBlock exception rule */
    char *h = strchr(s, '#'); if (h) *h = '\0';                  /* strip inline hosts-style comment */
    char *eq = strstr(s, "=/");                                   /* dnsmasq address=/domain/... */
    if (eq) { char *dom = eq + 2; char *sl = strchr(dom, '/'); if (sl) *sl = '\0'; return bl_clean_token(dom, out, outsz); }
    char *save = NULL;
    for (char *tok = strtok_r(s, " \t", &save); tok; tok = strtok_r(NULL, " \t", &save)) {
        if (bl_clean_token(tok, out, outsz)) return true;        /* first valid token wins (hosts/plain/ABP) */
    }
    return false;
}

/* Build the set from EVERY *.txt in the folder (allowlist.txt is the exception file). Rebuild is
 * guarded by s_bl_mutex; lookups take it briefly, so a rebuild's SD reads just make lookups forward
 * (unfiltered) for the second or two they run. */
static void bl_load(void)
{
    if (s_bl_mutex) xSemaphoreTake(s_bl_mutex, portMAX_DELAY);
    bl_unload();
    s_bl_hash  = heap_caps_calloc(BL_HASH_SLOTS, sizeof(uint32_t), MALLOC_CAP_SPIRAM);
    s_bl_allow = heap_caps_calloc(BL_ALLOW_MAX, BL_DOM_MAX, MALLOC_CAP_SPIRAM);
    if (!s_bl_hash || !s_bl_allow) {
        bl_unload(); snprintf(s_filter_status, GW_STR, "no memory");
        if (s_bl_mutex) xSemaphoreGive(s_bl_mutex);
        return;
    }
    if (!nocsif_sdcard_lock(1500)) {
        bl_unload(); snprintf(s_filter_status, GW_STR, "card busy");
        if (s_bl_mutex) xSemaphoreGive(s_bl_mutex);
        return;
    }

    static char names[BL_MAX_FILES][64];
    int nn = 0;
    DIR *d = opendir(NOCSIF_GW_PIHOLE_DIR);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (e->d_type == DT_DIR) continue;
            if (e->d_name[0] == '.') continue;
            size_t L = strlen(e->d_name);
            if (L < 4 || strcasecmp(e->d_name + L - 4, ".txt") != 0) continue;
            if (nn >= BL_MAX_FILES) break;
            strncpy(names[nn], e->d_name, 63); names[nn][63] = '\0'; nn++;
        }
        closedir(d);
    }

    int files = 0;
    for (int fi = 0; fi < nn; fi++) {
        bool is_allow = (strcasecmp(names[fi], "allowlist.txt") == 0);
        char path[GW_PATH];
        snprintf(path, sizeof path, "%s/%s", NOCSIF_GW_PIHOLE_DIR, names[fi]);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        files++;
        char dom[BL_DOM_MAX];
        while (fgets(s_bl_linebuf, sizeof s_bl_linebuf, f)) {
            if (!bl_extract(s_bl_linebuf, dom, sizeof dom)) continue;
            if (is_allow) { if (s_bl_allow_n < BL_ALLOW_MAX) snprintf(s_bl_allow[s_bl_allow_n++], BL_DOM_MAX, "%s", dom); }
            else          bl_insert(dom);
        }
        fclose(f);
        if (s_bl_trunc) break;
    }
    nocsif_sdcard_unlock();

    s_bl_size = s_bl_count;
    if (s_bl_count == 0) snprintf(s_filter_status, GW_STR, files ? "empty list" : "no list");
    else                 snprintf(s_filter_status, GW_STR, s_bl_trunc ? "%d domains (max)" : "%d domains", s_bl_count);
    ESP_LOGI(TAG, "blocklist: %d domains from %d file(s), %d allow%s",
             s_bl_count, files, s_bl_allow_n, s_bl_trunc ? " (truncated)" : "");
    if (s_bl_mutex) xSemaphoreGive(s_bl_mutex);
}

/* Full decision: allowlist wins; else the name + its parent suffixes are tested (sub-domain match).
 * All in PSRAM — no SD at query time. Runs on the dns_task; brief mutex vs a rebuild. */
static bool bl_is_blocked(const char *name_in)
{
    char name[BL_DOM_MAX];
    snprintf(name, sizeof name, "%s", name_in);
    str_lower(name);
    rstrip(name);
    if (name[0] == '\0') return false;
    if (!s_bl_mutex || xSemaphoreTake(s_bl_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return false;   /* rebuild in progress -> forward */

    bool blocked = false;
    for (int i = 0; i < s_bl_allow_n; i++) {
        if (strcmp(s_bl_allow[i], name) == 0) { xSemaphoreGive(s_bl_mutex); return false; }
    }
    const char *suffix = name;
    for (int depth = 0; depth < BL_SUFFIX_MAX && suffix && *suffix; depth++) {
        if (bl_hash_has(suffix)) { blocked = true; break; }
        const char *dot = strchr(suffix, '.');
        suffix = dot ? dot + 1 : NULL;
    }
    xSemaphoreGive(s_bl_mutex);
    return blocked;
}

/* ---- portal sign-in allowlist ----------------------------------------------------------- */
static bool signed_has(uint32_t ip)
{
    bool r = false;
    portENTER_CRITICAL(&s_signed_mux);
    for (int i = 0; i < s_signed_n; i++) if (s_signed_ip[i] == ip) { r = true; break; }
    portEXIT_CRITICAL(&s_signed_mux);
    return r;
}
static void signed_add(uint32_t ip)
{
    portENTER_CRITICAL(&s_signed_mux);
    bool found = false;
    for (int i = 0; i < s_signed_n; i++) if (s_signed_ip[i] == ip) { found = true; break; }
    if (!found) {
        if (s_signed_n < GW_SIGNED_MAX) s_signed_ip[s_signed_n++] = ip;
        else { memmove(&s_signed_ip[0], &s_signed_ip[1], (GW_SIGNED_MAX - 1) * sizeof(uint32_t));
               s_signed_ip[GW_SIGNED_MAX - 1] = ip; }
    }
    portEXIT_CRITICAL(&s_signed_mux);
    s_portal_gen++;
}
static void signed_clear(void)
{
    portENTER_CRITICAL(&s_signed_mux);
    s_signed_n = 0;
    portEXIT_CRITICAL(&s_signed_mux);
}

/* ============================ DNS resolver task ========================================== *
 * Bound to UDP:53 on the AP netif. For each downstream query: parse the QNAME; if the client is
 * unsigned and the portal gate is on, answer A with the AP IP (-> the client's captive check hits
 * the splash) and drop others; else if filter is on and the name is blocked, answer 0.0.0.0; else
 * forward the raw query to the upstream resolver and relay the reply. Mirrors wifi.c's redirector. */

static uint32_t upstream_resolver(void)
{
    /* Config override > (tunnel DNS while tunnelling) > STA DHCP DNS > 1.1.1.1.
     * When the tunnel is the egress, the STA's local DHCP DNS (e.g. 192.168.1.1) is UNREACHABLE
     * through it, so client lookups must go to the tunnel's own DNS (wg0.conf's DNS =). */
    if (s_cfg.dns_upstream[0]) {
        ip4_addr_t a; if (ip4addr_aton(s_cfg.dns_upstream, &a)) return a.addr;
    }
    if (s_tunnel_active && s_wg_dns[0]) {
        ip4_addr_t a; if (ip4addr_aton(s_wg_dns, &a)) return a.addr;
    }
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_dns_info_t d;
    if (sta && esp_netif_get_dns_info(sta, ESP_NETIF_DNS_MAIN, &d) == ESP_OK &&
        d.ip.type == ESP_IPADDR_TYPE_V4 && d.ip.u_addr.ip4.addr) {
        return d.ip.u_addr.ip4.addr;
    }
    ip4_addr_t f; ip4addr_aton("1.1.1.1", &f); return f.addr;   /* last resort */
}

/* Parse the first QNAME into dotted form. Returns bytes consumed (offset of qtype) or -1. */
static int dns_qname(const uint8_t *buf, int n, char *out, int outlen)
{
    int p = 12, o = 0;                                   /* header is 12 bytes */
    while (p < n) {
        uint8_t len = buf[p++];
        if (len == 0) break;
        if ((len & 0xC0) || p + len > n) return -1;      /* no compression in a question */
        if (o && o < outlen - 1) out[o++] = '.';
        for (int i = 0; i < len && o < outlen - 1; i++) out[o++] = (char)buf[p + i];
        p += len;
    }
    out[o] = '\0';
    return (p + 4 <= n) ? p : -1;                        /* leave room for qtype+qclass */
}

/* Build a single-A response in place: keep the header + question, DROP anything after the question
 * (e.g. an EDNS OPT record), then append one A answer for `ip` (network order). `qtype_off` is the
 * offset of qtype from dns_qname; the question ends 4 bytes later. Returns the response length. */
static int dns_answer_a(uint8_t *buf, int qtype_off, uint32_t ip)
{
    buf[2] |= 0x80; buf[3] = 0x00;                       /* QR=1, RA/RCODE=0 */
    /* QDCOUNT (buf[4..5]) stays 0x0001 from the query. */
    buf[6] = 0x00; buf[7] = 0x01;                        /* ANCOUNT=1 */
    buf[8] = buf[9] = buf[10] = buf[11] = 0x00;          /* NSCOUNT=ARCOUNT=0 (strips EDNS) */
    int p = qtype_off + 4;                               /* append right after qtype+qclass */
    buf[p++] = 0xC0; buf[p++] = 0x0C;                    /* NAME -> question */
    buf[p++] = 0x00; buf[p++] = 0x01;                    /* TYPE A */
    buf[p++] = 0x00; buf[p++] = 0x01;                    /* CLASS IN */
    buf[p++] = 0x00; buf[p++] = 0x00; buf[p++] = 0x00; buf[p++] = 0x1E;   /* TTL 30 s */
    buf[p++] = 0x00; buf[p++] = 0x04;                    /* RDLENGTH 4 */
    memcpy(&buf[p], &ip, 4); p += 4;
    return p;
}

/* ---- DNS reply cache helpers (dns_task-only) -------------------------------------------- */

/* Skip a DNS name at offset p (labels or a compression pointer). Returns the offset after it. */
static int dns_skip_name(const uint8_t *b, int n, int p)
{
    while (p < n) {
        uint8_t len = b[p];
        if (len == 0) return p + 1;
        if ((len & 0xC0) == 0xC0) return p + 2;      /* pointer terminates the name */
        p += len + 1;
    }
    return n;
}

/* Min TTL (s) across the answer RRs, clamped to [MIN,MAX]. Bounded, best-effort. */
static uint32_t dns_reply_ttl(const uint8_t *b, int n, int ancount, int ans_off)
{
    uint32_t best = DNS_CACHE_TTL_MAX;
    int p = ans_off;
    for (int a = 0; a < ancount; a++) {
        p = dns_skip_name(b, n, p);
        if (p + 10 > n) break;                       /* type(2) class(2) ttl(4) rdlen(2) */
        uint32_t ttl = ((uint32_t)b[p+4] << 24) | ((uint32_t)b[p+5] << 16) |
                       ((uint32_t)b[p+6] << 8) | b[p+7];
        uint16_t rdlen = ((uint16_t)b[p+8] << 8) | b[p+9];
        if (ttl < best) best = ttl;
        p += 10 + rdlen;
    }
    if (best < DNS_CACHE_TTL_MIN) best = DNS_CACHE_TTL_MIN;
    if (best > DNS_CACHE_TTL_MAX) best = DNS_CACHE_TTL_MAX;
    return best;
}

/* Return a live cache slot for (name,qtype), or -1. Frees expired slots it passes. */
static int dns_cache_find(const char *name, uint16_t qtype)
{
    if (!s_dnsc) return -1;
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < DNS_CACHE_ENTRIES; i++) {
        dns_cache_ent_t *e = &s_dnsc[i];
        if (e->exp_us == 0) continue;
        if (e->exp_us <= now) { e->exp_us = 0; continue; }   /* expired -> reclaim */
        if (e->qtype == qtype && strcmp(e->name, name) == 0) return i;
    }
    return -1;
}

/* Store (or refresh) a forwarded reply. Reuses an empty/expired slot, else round-robin evicts. */
static void dns_cache_store(const char *name, uint16_t qtype, const uint8_t *resp, int len, uint32_t ttl_s)
{
    if (!s_dnsc || len <= 0 || len > DNS_CACHE_RESP_MAX || !name[0]) return;
    int idx = dns_cache_find(name, qtype);
    if (idx < 0) {
        int64_t now = esp_timer_get_time();
        for (int i = 0; i < DNS_CACHE_ENTRIES; i++) {
            if (s_dnsc[i].exp_us == 0 || s_dnsc[i].exp_us <= now) { idx = i; break; }
        }
        if (idx < 0) { idx = (int)s_dnsc_hand; s_dnsc_hand = (s_dnsc_hand + 1) % DNS_CACHE_ENTRIES; }
    }
    dns_cache_ent_t *e = &s_dnsc[idx];
    e->qtype = qtype;
    snprintf(e->name, sizeof e->name, "%s", name);
    e->len = (uint16_t)len;
    memcpy(e->resp, resp, (size_t)len);
    e->exp_us = esp_timer_get_time() + (int64_t)ttl_s * 1000000;
}

static void dns_task(void *arg)
{
    (void)arg;
    int sock = s_dns_sock;
    struct timeval tv = { .tv_sec = 0, .tv_usec = 400000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    /* One upstream socket for forwarding. */
    int up = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct timeval ut = { .tv_sec = GW_UPSTREAM_TMO_MS / 1000, .tv_usec = (GW_UPSTREAM_TMO_MS % 1000) * 1000 };
    if (up >= 0) setsockopt(up, SOL_SOCKET, SO_RCVTIMEO, &ut, sizeof ut);

    uint32_t apip = 0;
    esp_netif_ip_info_t ip;
    if (s_ap_netif && esp_netif_get_ip_info(s_ap_netif, &ip) == ESP_OK) apip = ip.ip.addr;

    static DRAM_ATTR uint8_t rx[600];       /* internal buffer for the query/response */
    static DRAM_ATTR uint8_t up_rx[600];
    while (s_dns_run) {
        struct sockaddr_in cli; socklen_t cl = sizeof cli;
        int n = recvfrom(sock, rx, sizeof rx, 0, (struct sockaddr *)&cli, &cl);
        if (n < 12) continue;                            /* timeout / runt */
        s_dns_q++;

        char name[BL_DOM_MAX];
        int qend = dns_qname(rx, n, name, sizeof name);
        if (qend > 0) str_lower(name);                   /* canonical cache/filter key */
        uint16_t qtype = (qend > 0) ? (((uint16_t)rx[qend] << 8) | rx[qend + 1]) : 0;
        bool client_signed = !s_want_portal || signed_has(cli.sin_addr.s_addr);

        bool room = (qend > 0 && qend + 20 <= (int)sizeof rx);   /* qtype+qclass(4) + answer(16) */

        /* Uplink portal passthrough (Tier 2): steer EVERY downstream host to the watch so the phone
         * reaches the venue portal through our proxy (h_proxy resolves the real host from the Host hdr). */
        if (room && up_passthru_active()) {
            int r = dns_answer_a(rx, qend, apip);
            sendto(sock, rx, r, 0, (struct sockaddr *)&cli, cl);
            continue;
        }

        /* Portal gate: unsigned client -> steer everything to the splash. */
        if (room && !client_signed) {
            int r = dns_answer_a(rx, qend, apip);
            sendto(sock, rx, r, 0, (struct sockaddr *)&cli, cl);
            continue;
        }

        /* Filter: blocked name -> 0.0.0.0 (sinkhole). RAM-only lookup (no SD on the hot path). */
        if (room && s_filter_active) {
            if (bl_is_blocked(name)) {
                int r = dns_answer_a(rx, qend, 0);       /* 0.0.0.0 */
                sendto(sock, rx, r, 0, (struct sockaddr *)&cli, cl);
                s_dns_blocked++;
                continue;
            }
        }

        /* Cache: serve a fresh forwarded reply without the upstream round-trip (the latency win). */
        if (qend > 0) {
            int ci = dns_cache_find(name, qtype);
            if (ci >= 0) {
                int L = s_dnsc[ci].len;
                memcpy(up_rx, s_dnsc[ci].resp, (size_t)L);
                up_rx[0] = rx[0]; up_rx[1] = rx[1];      /* replay with the client's txn id */
                sendto(sock, up_rx, L, 0, (struct sockaddr *)&cli, cl);
                s_dns_cached++;
                continue;
            }
        }

        /* Forward upstream and relay (and cache a good reply for next time). */
        if (up >= 0) {
            struct sockaddr_in us = { 0 };
            us.sin_family = AF_INET; us.sin_port = htons(53);
            us.sin_addr.s_addr = upstream_resolver();
            if (sendto(up, rx, n, 0, (struct sockaddr *)&us, sizeof us) == n) {
                int un = recvfrom(up, up_rx, sizeof up_rx, 0, NULL, NULL);
                if (un > 0) {
                    if (qend > 0 && un <= DNS_CACHE_RESP_MAX && (up_rx[3] & 0x0F) == 0) {
                        int anc = ((int)up_rx[6] << 8) | up_rx[7];   /* ANCOUNT */
                        if (anc > 0) dns_cache_store(name, qtype, up_rx, un,
                                                     dns_reply_ttl(up_rx, un, anc, qend + 4));
                    }
                    sendto(sock, up_rx, un, 0, (struct sockaddr *)&cli, cl); s_dns_fwd++; continue;
                }
            }
        }
        /* Upstream failed: SERVFAIL so the client retries rather than hangs. */
        if (n >= 12) { rx[2] |= 0x80; rx[3] = 0x02; rx[7] = 0; sendto(sock, rx, n, 0, (struct sockaddr *)&cli, cl); }
    }
    if (up >= 0) close(up);
    close(sock); s_dns_sock = -1; s_dns_task = NULL;
    vTaskDeleteWithCaps(NULL);
}

static bool dns_start(void)
{
    if (s_dns_task) return true;
    s_dns_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_dns_sock < 0) return false;
    struct sockaddr_in sa = { 0 };
    sa.sin_family = AF_INET; sa.sin_port = htons(GW_DNS_PORT); sa.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s_dns_sock, (struct sockaddr *)&sa, sizeof sa) != 0) { close(s_dns_sock); s_dns_sock = -1; return false; }
    if (!s_dnsc) {                                    /* lazy PSRAM cache (best-effort; NULL = disabled) */
        s_dnsc = heap_caps_calloc(DNS_CACHE_ENTRIES, sizeof(dns_cache_ent_t), MALLOC_CAP_SPIRAM);
        s_dnsc_hand = 0;
    }
    s_dns_run = true;
    if (xTaskCreateWithCaps(dns_task, "gwdns", 4096, NULL, 4, &s_dns_task, MALLOC_CAP_SPIRAM) != pdPASS) {
        s_dns_run = false; close(s_dns_sock); s_dns_sock = -1; s_dns_task = NULL; return false;
    }
    return true;
}
static void dns_stop(void)
{
    if (!s_dns_task && s_dns_sock < 0) return;
    s_dns_run = false;
    for (int i = 0; i < 20 && s_dns_task; i++) vTaskDelay(pdMS_TO_TICKS(50));
    if (s_dns_sock >= 0) { close(s_dns_sock); s_dns_sock = -1; }
    if (s_dnsc) { heap_caps_free(s_dnsc); s_dnsc = NULL; }   /* task has exited: safe to free the cache */
}

/* ============================ sign-in HTTP splash ======================================= */
static const char PORTAL_DEFAULT_HTML[] =
    "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>NocSif Gateway</title>"
    "<body style='font-family:sans-serif;max-width:30em;margin:3em auto;text-align:center'>"
    "<h2>NocSif Gateway</h2><p>Tap continue to access the network.</p>"
    "<form method=POST action=/signin><button style='font-size:1.2em;padding:.6em 2em'>Continue</button></form>";

static uint32_t req_peer_ip(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);
    struct sockaddr_in6 sa; socklen_t sl = sizeof sa;
    if (getpeername(fd, (struct sockaddr *)&sa, &sl) == 0 && sa.sin6_family == AF_INET)
        return ((struct sockaddr_in *)&sa)->sin_addr.s_addr;
    return 0;
}

static esp_err_t h_signin(httpd_req_t *req)
{
    signed_add(req_peer_ip(req));
    httpd_resp_set_status(req, "200 OK");
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, "<meta http-equiv=refresh content='1;url=http://neverssl.com'>"
                            "<body style='font-family:sans-serif;text-align:center;margin-top:3em'>"
                            "Connected. You may now browse.");
    s_portal_gen++;
    return ESP_OK;
}

static esp_err_t h_splash(httpd_req_t *req)
{
    /* Tier 2: while the UPSTREAM uplink is behind a portal and passthrough is on, relay the venue portal
     * to this downstream client instead of the local sign-in notice. */
    if (up_passthru_active()) return h_proxy(req);
    s_portal_gen++;
    httpd_resp_set_status(req, "200 OK");
    httpd_resp_set_type(req, "text/html");
    if (s_portal_html && s_portal_html_len) httpd_resp_send(req, s_portal_html, s_portal_html_len);
    else httpd_resp_send(req, PORTAL_DEFAULT_HTML, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* Load the sign-in splash into PSRAM once (best-effort). The page library is SHARED with wifi.c's
 * captive portal: /sd/nocsif/wifi/portals/<selected>, chosen by nocsif_wifi_portal_selected_page()
 * ("" = built-in notice), so Travel Router and the Captive Portal screen pick from one list. */
static void portal_load_page(void)
{
    if (s_portal_html) { free(s_portal_html); s_portal_html = NULL; s_portal_html_len = 0; }
    const char *page = nocsif_wifi_portal_selected_page();
    if (!page || !page[0]) return;                 /* "" = the built-in notice */
    char path[GW_PATH];
    snprintf(path, sizeof path, "/sd/nocsif/wifi/portals/%s", page);
    if (!nocsif_sdcard_lock(500)) return;
    FILE *f = fopen(path, "rb");
    if (f) {
        fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
        if (sz > 0 && sz < 64 * 1024) {
            char *buf = heap_caps_malloc(sz + 1, MALLOC_CAP_SPIRAM);
            if (buf) {
                /* Stage through the internal buffer to avoid a PSRAM-dest SD bounce. */
                long got = 0;
                while (got < sz) {
                    size_t want = (sz - got) < (long)sizeof s_bl_linebuf ? (size_t)(sz - got) : sizeof s_bl_linebuf;
                    size_t r = fread(s_bl_linebuf, 1, want, f);
                    if (!r) break;
                    memcpy(buf + got, s_bl_linebuf, r); got += r;
                }
                buf[got] = '\0'; s_portal_html = buf; s_portal_html_len = got;
            }
        }
        fclose(f);
    }
    nocsif_sdcard_unlock();
}

static bool http_start(void)
{
    if (s_httpd) return true;
    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.server_port = GW_HTTP_PORT;
    hc.stack_size  = 7168;              /* +1 KB: the Tier 2 proxy runs esp_http_client on this task */
    hc.max_uri_handlers = 5;
    hc.uri_match_fn = httpd_uri_match_wildcard;
    if (httpd_start(&s_httpd, &hc) != ESP_OK) { s_httpd = NULL; return false; }
    httpd_uri_t u_signin  = { .uri = "/signin", .method = HTTP_POST, .handler = h_signin };
    httpd_uri_t u_all_get = { .uri = "/*",      .method = HTTP_GET,  .handler = h_splash };
    /* POST wildcard -> the Tier 2 passthrough (form submits to the venue portal); h_proxy is only
     * reached when up_passthru_active(), otherwise it 404s via the router since no local POST exists. */
    httpd_uri_t u_all_post = { .uri = "/*",     .method = HTTP_POST, .handler = h_proxy };
    httpd_register_uri_handler(s_httpd, &u_signin);
    httpd_register_uri_handler(s_httpd, &u_all_get);
    httpd_register_uri_handler(s_httpd, &u_all_post);
    return true;
}
static void http_stop(void)
{
    if (s_httpd) { httpd_stop(s_httpd); s_httpd = NULL; }
    if (s_portal_html) { free(s_portal_html); s_portal_html = NULL; s_portal_html_len = 0; }
}

/* ============================ Uplink captive-portal sign-in ============================= *
 * When the watch's STA joins a network behind a captive portal (hotel/cafe), it associates + gets an IP
 * but the internet is blocked behind a "tap to agree" page, so the shared uplink is dead. This detects
 * that, and (opt-in) either AUTO-ACCEPTS a simple Terms/Continue portal (Tier 1) or PASSES the portal
 * through to a downstream phone so the user completes any sign-in on the phone, through the watch (Tier 2).
 * Because the venue authorises the device that submits the form, and every request here is made from the
 * WATCH's STA, completing the portal (either tier) authorises the WATCH — then NAPT shares the live uplink.
 * The probe/auto-accept run on a transient PSRAM-stacked task (esp_http_client is sockets + software-AES
 * TLS: no DMA, no flash writes -> a PSRAM stack is safe and keeps internal RAM free). SAFETY: auto-accept
 * only ever clicks agree/continue-type buttons; it REFUSES any form asking for a password / email / phone /
 * login / code / payment (-> UP_LOGIN, which the phone passthrough handles). */
#define UP_PROBE_URL   "http://connectivitycheck.gstatic.com/generate_204"
#define UP_HTML_CAP    (32 * 1024)          /* max portal page / proxied body we buffer (PSRAM)         */
#define UP_POST_MAX    8192                  /* max proxied POST body                                    */

typedef struct {
    char *buf; int cap; int len;            /* response body accumulator (PSRAM)                        */
    char cookie[256];                       /* last Set-Cookie (name=value, attrs stripped)             */
    char setcookie[256];                    /* raw Set-Cookie to relay downstream (proxy)               */
    char ctype[96];                         /* Content-Type to relay downstream (proxy)                 */
} up_fetch_t;

/* esp_http_client event handler: accumulate the body, capture Set-Cookie + Content-Type. */
static esp_err_t up_http_evt(esp_http_client_event_t *e)
{
    up_fetch_t *f = (up_fetch_t *)e->user_data;
    if (!f) return ESP_OK;
    if (e->event_id == HTTP_EVENT_ON_HEADER && e->header_key && e->header_value) {
        if (!strcasecmp(e->header_key, "Set-Cookie")) {
            snprintf(f->setcookie, sizeof f->setcookie, "%s", e->header_value);
            snprintf(f->cookie, sizeof f->cookie, "%s", e->header_value);
            char *sc = strchr(f->cookie, ';'); if (sc) *sc = '\0';   /* keep just name=value */
        } else if (!strcasecmp(e->header_key, "Content-Type")) {
            snprintf(f->ctype, sizeof f->ctype, "%s", e->header_value);
        }
    } else if (e->event_id == HTTP_EVENT_ON_DATA && f->buf && f->len < f->cap - 1) {
        int n = e->data_len;
        if (n > f->cap - 1 - f->len) n = f->cap - 1 - f->len;
        memcpy(f->buf + f->len, e->data, n);
        f->len += n; f->buf[f->len] = '\0';
    }
    return ESP_OK;
}

static void up_set(up_state_t st, const char *detail)
{
    if (detail) snprintf(s_up_detail, sizeof s_up_detail, "%s", detail);
    if (st != s_up_state) { s_up_state = st; }
    s_up_gen++;
}

/* Case-insensitive bounded substring search in [hay,end). */
static const char *ci_find(const char *hay, const char *end, const char *needle)
{
    size_t nl = strlen(needle);
    if (!nl) return hay;
    for (const char *p = hay; p + nl <= end; p++)
        if (strncasecmp(p, needle, nl) == 0) return p;
    return NULL;
}

/* Read attribute `name`'s value from an HTML tag in [tag,tagend). Handles "..", '..', or bare. */
static bool tag_attr(const char *tag, const char *tagend, const char *name, char *out, size_t outsz)
{
    char pat[24]; snprintf(pat, sizeof pat, "%s", name);
    for (const char *p = tag; p < tagend; ) {
        const char *a = ci_find(p, tagend, pat);
        if (!a) return false;
        const char *q = a + strlen(pat);
        while (q < tagend && (*q == ' ' || *q == '\t')) q++;
        if (q >= tagend || *q != '=') { p = a + 1; continue; }   /* attr name must be followed by '=' */
        /* require the char before `a` to be a boundary so "action" != "reaction" */
        if (a > tag && (isalnum((unsigned char)a[-1]) || a[-1] == '-' || a[-1] == '_')) { p = a + 1; continue; }
        q++;
        while (q < tagend && (*q == ' ' || *q == '\t')) q++;
        char quote = 0;
        if (q < tagend && (*q == '"' || *q == '\'')) { quote = *q; q++; }
        size_t o = 0;
        while (q < tagend && o < outsz - 1) {
            if (quote && *q == quote) break;
            if (!quote && (*q == ' ' || *q == '>' || *q == '\t' || *q == '/')) break;
            out[o++] = *q++;
        }
        out[o] = '\0';
        return true;
    }
    return false;
}

/* Does this token read like an "accept/continue" control (and not a reject)? */
static bool up_is_accept(const char *s)
{
    if (!s || !s[0]) return false;
    char t[80]; snprintf(t, sizeof t, "%s", s); str_lower(t);
    const char *no[] = { "cancel", "decline", "reject", "deny", "back", "logout", "sign out", "exit", NULL };
    for (int i = 0; no[i]; i++) if (strstr(t, no[i])) return false;
    const char *yes[] = { "continue", "accept", "agree", "connect", "proceed", "start", "enter",
                          "online", "free", "guest", "submit", "ok", "confirm", "allow", NULL };
    for (int i = 0; yes[i]; i++) if (strstr(t, yes[i])) return true;
    return false;
}

/* Would this input field require a credential / personal detail? (auto-accept safety gate) */
static bool up_is_sensitive(const char *type, const char *name)
{
    char ty[24]; snprintf(ty, sizeof ty, "%s", type ? type : ""); str_lower(ty);
    if (!strcmp(ty, "password") || !strcmp(ty, "email") || !strcmp(ty, "tel")) return true;
    char nm[64]; snprintf(nm, sizeof nm, "%s", name ? name : ""); str_lower(nm);
    const char *bad[] = { "pass", "user", "login", "mail", "phone", "mobile", "otp", "code", "pin",
                          "card", "cvv", "cvc", "account", "ssn", "credit", NULL };
    for (int i = 0; bad[i]; i++) if (strstr(nm, bad[i])) return true;
    return false;
}

/* Percent-encode + append "name=value&" to a urlencoded body. */
static void up_urlenc(char *dst, size_t dstsz, const char *name, const char *val)
{
    size_t o = strlen(dst);
    const char *srcs[2] = { name, val };
    for (int s = 0; s < 2; s++) {
        for (const char *p = srcs[s]; *p && o < dstsz - 4; p++) {
            unsigned char c = (unsigned char)*p;
            if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') dst[o++] = (char)c;
            else { static const char hx[] = "0123456789ABCDEF"; dst[o++] = '%'; dst[o++] = hx[c >> 4]; dst[o++] = hx[c & 15]; }
        }
        if (s == 0 && o < dstsz - 1) dst[o++] = '=';
    }
    if (o < dstsz - 1) dst[o++] = '&';
    dst[o] = '\0';
}

/* Parse the host out of an http(s):// URL into out. */
static void up_host_of(const char *url, char *out, size_t outsz)
{
    out[0] = '\0';
    const char *h = strstr(url, "://");
    h = h ? h + 3 : url;
    size_t o = 0;
    while (*h && *h != '/' && *h != ':' && o < outsz - 1) out[o++] = *h++;
    out[o] = '\0';
}

/* Resolve a form action (absolute / root-relative / relative) against the portal base URL. */
static void up_url_join(const char *base, const char *action, char *out, size_t outsz)
{
    if (!action || !action[0]) { snprintf(out, outsz, "%s", base); return; }
    if (!strncasecmp(action, "http://", 7) || !strncasecmp(action, "https://", 8)) {
        snprintf(out, outsz, "%s", action); return;
    }
    char scheme[8] = "http", host[96]; up_host_of(base, host, sizeof host);
    if (!strncasecmp(base, "https", 5)) snprintf(scheme, sizeof scheme, "https");
    if (action[0] == '/') { snprintf(out, outsz, "%s://%s%s", scheme, host, action); return; }
    /* relative to the base directory */
    char dir[256]; snprintf(dir, sizeof dir, "%s", base);
    char *sl = strrchr(dir + 8, '/'); if (sl) sl[1] = '\0'; else snprintf(dir, sizeof dir, "%s://%s/", scheme, host);
    snprintf(out, outsz, "%s%s", dir, action);
}

/* Connectivity probe: GET generate_204. Returns HTTP status; fills loc with a redirect target if any. */
static int up_probe(char *loc, size_t locsz)
{
    esp_http_client_config_t cfg = {
        .url = UP_PROBE_URL, .method = HTTP_METHOD_GET,
        .timeout_ms = 4000, .disable_auto_redirect = true,
        .buffer_size = 1024, .buffer_size_tx = 512,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return -1;
    loc[0] = '\0';
    int status = -1;
    if (esp_http_client_open(c, 0) == ESP_OK) {
        esp_http_client_fetch_headers(c);
        status = esp_http_client_get_status_code(c);
        char *v = NULL;
        if (esp_http_client_get_header(c, "Location", &v) == ESP_OK && v && v[0]) snprintf(loc, locsz, "%s", v);
        esp_http_client_close(c);
    }
    esp_http_client_cleanup(c);
    return status;
}

/* Tier 1: fetch the portal page, find a simple accept form, submit it. Returns true if submitted (still
 * re-probe after to confirm). Sets UP_LOGIN if the form needs a credential. */
static bool up_auto_accept(void)
{
    char *html = heap_caps_malloc(UP_HTML_CAP, MALLOC_CAP_SPIRAM);
    if (!html) return false;
    up_fetch_t f = { .buf = html, .cap = UP_HTML_CAP, .len = 0 };
    f.cookie[0] = f.setcookie[0] = f.ctype[0] = '\0';

    esp_http_client_config_t cfg = {
        .url = s_up_url, .method = HTTP_METHOD_GET,
        .timeout_ms = 6000, .disable_auto_redirect = false,
        .event_handler = up_http_evt, .user_data = &f,
        .buffer_size = 2048, .buffer_size_tx = 1024,
#if defined(CONFIG_MBEDTLS_CERTIFICATE_BUNDLE)
        .crt_bundle_attach = esp_crt_bundle_attach,
#endif
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) { free(html); return false; }
    esp_err_t er = esp_http_client_perform(c);
    esp_http_client_cleanup(c);
    if (er != ESP_OK || f.len <= 0) { free(html); return false; }

    const char *end = html + f.len;
    const char *form = ci_find(html, end, "<form");
    if (!form) { free(html); return false; }
    const char *ftag_end = memchr(form, '>', end - form);
    if (!ftag_end) { free(html); return false; }
    const char *form_end = ci_find(ftag_end, end, "</form");
    if (!form_end) form_end = end;

    char action[256] = "", method[8] = "";
    tag_attr(form, ftag_end, "action", action, sizeof action);
    tag_attr(form, ftag_end, "method", method, sizeof method);
    bool post = (method[0] == 0) || !strcasecmp(method, "post");

    /* Walk the inputs: build the hidden/default body, run the safety gate, find the accept control. */
    static char body[1024];   /* static: keep it off the 8 KB PSRAM task stack */
    body[0] = '\0';
    char accept_name[64] = "", accept_val[64] = "";
    bool have_accept = false, needs_login = false;

    for (const char *p = form; (p = ci_find(p, form_end, "<input")) != NULL; ) {
        const char *tend = memchr(p, '>', form_end - p);
        if (!tend) break;
        char type[24] = "text", name[64] = "", val[128] = "";
        tag_attr(p, tend, "type", type, sizeof type);
        tag_attr(p, tend, "name", name, sizeof name);
        tag_attr(p, tend, "value", val, sizeof val);
        if (up_is_sensitive(type, name)) needs_login = true;
        bool is_btn = !strcasecmp(type, "submit") || !strcasecmp(type, "button") || !strcasecmp(type, "image");
        if (is_btn) {
            if (!have_accept && (up_is_accept(val) || up_is_accept(name))) {
                snprintf(accept_name, sizeof accept_name, "%s", name);
                snprintf(accept_val, sizeof accept_val, "%s", val);
                have_accept = true;
            }
        } else if (name[0] && (!strcasecmp(type, "hidden") || !strcasecmp(type, "text") ||
                               !strcasecmp(type, "checkbox") || !strcasecmp(type, "radio"))) {
            up_urlenc(body, sizeof body, name, val);   /* include hidden/default fields */
        }
        p = tend + 1;   /* advance past this <input ...> (ci_find would otherwise re-match it) */
    }
    /* A <button> (no type) with accept text also counts. */
    if (!have_accept) {
        for (const char *p = form; (p = ci_find(p, form_end, "<button")) != NULL; p++) {
            const char *tend = memchr(p, '>', form_end - p);
            const char *bclose = tend ? ci_find(tend, form_end, "</button") : NULL;
            char txt[80] = "";
            if (tend && bclose) { size_t L = (size_t)(bclose - tend - 1); if (L >= sizeof txt) L = sizeof txt - 1; memcpy(txt, tend + 1, L); txt[L] = '\0'; }
            char bname[64] = "", bval[64] = "";
            if (tend) { tag_attr(p, tend, "name", bname, sizeof bname); tag_attr(p, tend, "value", bval, sizeof bval); }
            if (up_is_accept(txt) || up_is_accept(bval) || up_is_accept(bname)) {
                snprintf(accept_name, sizeof accept_name, "%s", bname);
                snprintf(accept_val, sizeof accept_val, "%s", bval);
                have_accept = true; break;
            }
        }
    }
    free(html);

    if (needs_login) { up_set(UP_LOGIN, "sign-in needs a login - use phone passthrough"); return false; }
    if (!have_accept) { return false; }
    if (accept_name[0]) up_urlenc(body, sizeof body, accept_name, accept_val);

    char url[512]; up_url_join(s_up_url, action, url, sizeof url);
    up_fetch_t f2 = { .buf = NULL, .cap = 0, .len = 0 };
    esp_http_client_config_t sc = {
        .url = url, .method = post ? HTTP_METHOD_POST : HTTP_METHOD_GET,
        .timeout_ms = 6000, .disable_auto_redirect = false,
        .event_handler = up_http_evt, .user_data = &f2,
        .buffer_size = 2048, .buffer_size_tx = 1024,
#if defined(CONFIG_MBEDTLS_CERTIFICATE_BUNDLE)
        .crt_bundle_attach = esp_crt_bundle_attach,
#endif
    };
    esp_http_client_handle_t sh = esp_http_client_init(&sc);
    if (!sh) return false;
    if (f.cookie[0]) esp_http_client_set_header(sh, "Cookie", f.cookie);
    esp_http_client_set_header(sh, "Referer", s_up_url);
    if (post) {
        esp_http_client_set_header(sh, "Content-Type", "application/x-www-form-urlencoded");
        esp_http_client_set_post_field(sh, body, (int)strlen(body));
    }
    esp_err_t se = esp_http_client_perform(sh);
    esp_http_client_cleanup(sh);
    return se == ESP_OK;
}

/* The transient worker: probe, publish state, optionally auto-accept, re-probe. */
static void portal_task(void *arg)
{
    bool force_accept = (bool)(intptr_t)arg;
    up_set(UP_CHECKING, "checking uplink");
    char loc[256] = "";
    int st = up_probe(loc, sizeof loc);

    if (st == 204) { up_set(UP_ONLINE, "online"); s_up_url[0] = s_up_host[0] = '\0'; goto done; }
    if (st < 0)    { up_set(UP_NONE, "no uplink"); goto done; }

    /* Portal detected. Record its URL/host. */
    if (loc[0]) snprintf(s_up_url, sizeof s_up_url, "%s", loc);
    else if (!s_up_url[0]) snprintf(s_up_url, sizeof s_up_url, "%s", UP_PROBE_URL);
    up_host_of(s_up_url, s_up_host, sizeof s_up_host);
    up_set(UP_SIGNIN, "sign-in required");

    if (force_accept || s_up_autoacc) {
        up_set(UP_SIGNING, "auto-accepting");
        bool submitted = up_auto_accept();
        if (s_up_state == UP_LOGIN) goto done;   /* safety gate tripped */
        if (submitted) {
            vTaskDelay(pdMS_TO_TICKS(600));
            char l2[256]; int st2 = up_probe(l2, sizeof l2);
            if (st2 == 204) up_set(UP_SIGNED, "signed in");
            else            up_set(UP_FAILED, "auto-accept didn't take - try phone passthrough");
        } else {
            up_set(UP_FAILED, "no simple accept found - use phone passthrough");
        }
    }
done:
    /* If we turned a portal into online, or need the passthrough AP/DNS up, reconcile. */
    post(CMD_RECONCILE);
    s_up_task = NULL;
    vTaskDeleteWithCaps(NULL);
}

/* Spawn the transient portal task (idempotent while one runs). force = attempt auto-accept regardless
 * of the setting (the on-watch "Sign in" button). */
static void portal_kick(bool force)
{
    if (s_up_task) return;
    if (!nocsif_wifi_connected()) { up_set(UP_NONE, "no uplink"); return; }
    xTaskCreateWithCaps(portal_task, "gwportal", 8192, (void *)(intptr_t)force, 4, &s_up_task, MALLOC_CAP_SPIRAM);
}

/* Tier 2: reverse-proxy a downstream request to the venue portal over the STA uplink. Because the DNS
 * gate points every downstream host at the watch while a portal is pending, the phone reaches us with the
 * real target in its Host header — we fetch http://<Host><uri> from the watch (which the venue authorises)
 * and relay it back. HTTP only (a portal that jumps to HTTPS assets is a known limit; auto-accept, which
 * runs client-side on the watch, does handle HTTPS). */
static esp_err_t h_proxy(httpd_req_t *req)
{
    if (!up_passthru_active()) {                     /* not in passthrough -> never proxy arbitrary reqs */
        httpd_resp_set_status(req, "200 OK");
        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, PORTAL_DEFAULT_HTML, HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    char host[96];
    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof host) != ESP_OK || !host[0]) {
        /* no host -> show the local notice */
        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, PORTAL_DEFAULT_HTML, HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    char url[600];
    snprintf(url, sizeof url, "http://%s%s", host, req->uri);

    up_fetch_t f = { .buf = heap_caps_malloc(UP_HTML_CAP, MALLOC_CAP_SPIRAM), .cap = UP_HTML_CAP, .len = 0 };
    if (!f.buf) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem"); return ESP_FAIL; }
    f.cookie[0] = f.setcookie[0] = f.ctype[0] = '\0';

    esp_http_client_config_t cfg = {
        .url = url, .method = (req->method == HTTP_POST) ? HTTP_METHOD_POST : HTTP_METHOD_GET,
        .timeout_ms = 8000, .disable_auto_redirect = false,
        .event_handler = up_http_evt, .user_data = &f,
        .buffer_size = 2048, .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) { free(f.buf); httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "init"); return ESP_FAIL; }

    char cookie[512];
    if (httpd_req_get_hdr_value_str(req, "Cookie", cookie, sizeof cookie) == ESP_OK && cookie[0])
        esp_http_client_set_header(c, "Cookie", cookie);

    char *pbody = NULL;
    if (req->method == HTTP_POST && req->content_len > 0 && req->content_len <= UP_POST_MAX) {
        int blen = req->content_len;
        pbody = malloc(blen + 1);
        if (pbody) {
            int got = 0, r;
            while (got < blen && (r = httpd_req_recv(req, pbody + got, blen - got)) > 0) got += r;
            pbody[got] = '\0';
            char ct[96];
            if (httpd_req_get_hdr_value_str(req, "Content-Type", ct, sizeof ct) == ESP_OK)
                esp_http_client_set_header(c, "Content-Type", ct);
            esp_http_client_set_post_field(c, pbody, got);
        }
    }

    esp_err_t er = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);
    if (pbody) free(pbody);

    if (er != ESP_OK) {
        free(f.buf);
        httpd_resp_set_status(req, "502 Bad Gateway");
        httpd_resp_sendstr(req, "portal fetch failed");
        return ESP_OK;
    }
    char stbuf[20]; snprintf(stbuf, sizeof stbuf, "%d Portal", status);
    httpd_resp_set_status(req, status == 200 ? "200 OK" : stbuf);
    httpd_resp_set_type(req, f.ctype[0] ? f.ctype : "text/html");
    if (f.setcookie[0]) httpd_resp_set_hdr(req, "Set-Cookie", f.setcookie);
    httpd_resp_send(req, f.buf, f.len);
    free(f.buf);
    if (!s_up_task) portal_kick(false);            /* the sign-in may have just authorised us */
    return ESP_OK;
}

/* ============================ WireGuard tunnel (req 3) =================================== */
/* Parse the SELECTED wg-quick INI (/sd/nocsif/wireguard/<s_wg_file>) into the effective fields, then
 * apply the on-watch overrides. Unconditional (runs with or without the WG component) so the UI can
 * always display the current tunnel. Refreshed on every reconcile + reload. */
static void wg_refresh(void)
{
    s_wg_conf_ok = false;
    s_wg_priv[0] = s_wg_pub[0] = s_wg_psk[0] = s_wg_ep[0] = s_wg_addr[0] = s_wg_dns[0] = '\0';
    s_wg_port = 51820; s_wg_keepalive = 25;
    snprintf(s_wg_netmask, sizeof s_wg_netmask, "255.255.255.255");   /* device address = /32 host */
    if (!s_wg_file[0]) snprintf(s_wg_file, sizeof s_wg_file, "wg0.conf");

    char path[GW_PATH];
    snprintf(path, sizeof path, "%s/%s", NOCSIF_GW_WG_DIR, s_wg_file);
    if (nocsif_sdcard_lock(500)) {
        FILE *f = fopen(path, "r");
        if (!f) {
            /* Selected file missing -> fall back to the first .conf present, so just dropping one
             * config onto the card works without the Import tap (drop-in convenience). */
            DIR *dd = opendir(NOCSIF_GW_WG_DIR);
            if (dd) {
                struct dirent *e;
                while ((e = readdir(dd)) != NULL) {
                    if (e->d_type == DT_DIR || e->d_name[0] == '.') continue;
                    size_t L = strlen(e->d_name);
                    if (L >= 5 && strcasecmp(e->d_name + L - 5, ".conf") == 0) {
                        snprintf(s_wg_file, sizeof s_wg_file, "%s", e->d_name);
                        break;
                    }
                }
                closedir(dd);
            }
            snprintf(path, sizeof path, "%s/%s", NOCSIF_GW_WG_DIR, s_wg_file);
            f = fopen(path, "r");
        }
        if (f) {
            while (fgets(s_bl_linebuf, sizeof s_bl_linebuf, f)) {
                char *k, *v;
                if (!kv_split(s_bl_linebuf, &k, &v)) continue;
                if      (!strcasecmp(k, "PrivateKey"))   snprintf(s_wg_priv, sizeof s_wg_priv, "%s", v);
                else if (!strcasecmp(k, "PublicKey"))    snprintf(s_wg_pub,  sizeof s_wg_pub,  "%s", v);
                else if (!strcasecmp(k, "PresharedKey")) snprintf(s_wg_psk,  sizeof s_wg_psk,  "%s", v);
                else if (!strcasecmp(k, "Address")) {    /* "10.9.0.2/32" -> host addr (/mask ignored) */
                    char *sl = strchr(v, '/'); if (sl) *sl = '\0';
                    snprintf(s_wg_addr, sizeof s_wg_addr, "%s", v);
                }
                else if (!strcasecmp(k, "DNS")) {        /* keep only the FIRST server (buffer is one IPv4) */
                    char *comma = strchr(v, ','); if (comma) *comma = '\0';
                    rstrip(v);
                    snprintf(s_wg_dns, sizeof s_wg_dns, "%s", v);
                }
                else if (!strcasecmp(k, "PersistentKeepalive")) s_wg_keepalive = atoi(v);
                else if (!strcasecmp(k, "Endpoint")) {   /* "host:port" */
                    char *col = strrchr(v, ':');
                    if (col) { *col = '\0'; s_wg_port = atoi(col + 1); }
                    snprintf(s_wg_ep, sizeof s_wg_ep, "%s", v);
                }
                /* AllowedIPs is not used: the trombik port routes all forwarded traffic via
                 * esp_wireguard_set_default(), not a peer-allowed-ips field. */
            }
            fclose(f);
        }
        nocsif_sdcard_unlock();
    }

    /* On-watch overrides win over the file; then the legacy gateway.conf tunnel_port. */
    if (s_wg_ov_ep[0])    snprintf(s_wg_ep, sizeof s_wg_ep, "%s", s_wg_ov_ep);
    if (s_wg_ov_dns[0])   snprintf(s_wg_dns, sizeof s_wg_dns, "%s", s_wg_ov_dns);
    if (s_wg_ov_port > 0) s_wg_port = s_wg_ov_port;
    else if (s_cfg.tunnel_port > 0) s_wg_port = s_cfg.tunnel_port;

    s_wg_conf_ok = (s_wg_priv[0] && s_wg_pub[0] && s_wg_ep[0] && s_wg_addr[0]);
    if (s_wg_ep[0]) snprintf(s_tunnel_ep, GW_STR, "%s:%d", s_wg_ep, s_wg_port);
    else            s_tunnel_ep[0] = '\0';
}

#if GW_HAVE_WIREGUARD
static bool wg_up(void)
{
    return s_wg_inited && esp_wireguardif_peer_is_up(&s_wg_ctx) == ESP_OK;
}

static bool wg_start(void)
{
    wg_refresh();
    if (!s_wg_conf_ok) { snprintf(s_tunnel_status, GW_STR, s_wg_ep[0] ? "bad config" : "no config"); return false; }

    wireguard_config_t wc = ESP_WIREGUARD_CONFIG_DEFAULT();
    wc.private_key     = s_wg_priv;
    wc.public_key      = s_wg_pub;
    wc.preshared_key   = s_wg_psk[0] ? s_wg_psk : NULL;
    wc.endpoint        = s_wg_ep;
    wc.port            = s_wg_port;
    /* trombik/esp_wireguard quirk: allowed_ip / allowed_ip_mask are THIS device's tunnel address
     * (wg-quick's [Interface] Address), NOT the peer AllowedIPs — route-all is set_default() below. */
    wc.allowed_ip      = s_wg_addr;
    wc.allowed_ip_mask = s_wg_netmask;
    wc.persistent_keepalive = s_wg_keepalive;

    if (esp_wireguard_init(&wc, &s_wg_ctx) != ESP_OK) { snprintf(s_tunnel_status, GW_STR, "init failed"); return false; }
    s_wg_inited = true;
    if (esp_wireguard_connect(&s_wg_ctx) != ESP_OK) { snprintf(s_tunnel_status, GW_STR, "connect failed"); return false; }
    esp_wireguard_set_default(&s_wg_ctx);
    snprintf(s_tunnel_status, GW_STR, "connecting");
    ESP_LOGI(TAG, "wireguard connecting to %s:%d", s_wg_ep, s_wg_port);
    return true;
}
static void wg_stop(void)
{
    if (s_wg_inited) { esp_wireguard_disconnect(&s_wg_ctx); s_wg_inited = false; }
    snprintf(s_tunnel_status, GW_STR, "off");
}
#else  /* !GW_HAVE_WIREGUARD */
static bool wg_up(void)   { return false; }
static bool wg_start(void){ wg_refresh(); snprintf(s_tunnel_status, GW_STR, "unavailable"); return false; }
static void wg_stop(void) { snprintf(s_tunnel_status, GW_STR, "off"); }
#endif

/* ============================ DNS transport fallback (req 5) ============================= *
 * FEASIBILITY: there is no maintained ESP-IDF port of an iodine/dnstt client. A working design
 * needs (a) a custom lwIP netif whose linkoutput encodes each outbound IP packet into DNS query
 * labels (base32 of a chunk) to the operator's authoritative domain, and (b) an rx task that polls
 * for downstream packets via TXT/NULL answers and injects them with netif->input. Throughput is a
 * few kbps and latency is high. This scaffold PARSES the config and reports state honestly; the
 * codec/netif is left as the operator-server-specific piece (wire format documented in the note),
 * gated behind CONFIG_NOCSIF_GW_DNST_EXPERIMENTAL. Default: report "experimental" and stay off. */
static struct { char server[96]; char key[64]; int mtu; bool loaded; } s_dnst;

static void dnst_load_conf(void)
{
    memset(&s_dnst, 0, sizeof s_dnst);
    s_dnst.mtu = 1200; s_dnst.loaded = true;
    char path[GW_PATH];
    snprintf(path, sizeof path, "%s/dnstunnel.conf", NOCSIF_GW_DNST_DIR);
    if (!nocsif_sdcard_lock(500)) return;
    FILE *f = fopen(path, "r");
    if (f) {
        while (fgets(s_bl_linebuf, sizeof s_bl_linebuf, f)) {
            char *k, *v; if (!kv_split(s_bl_linebuf, &k, &v)) continue;
            if      (!strcasecmp(k, "server")) snprintf(s_dnst.server, sizeof s_dnst.server, "%s", v);
            else if (!strcasecmp(k, "key"))    snprintf(s_dnst.key, sizeof s_dnst.key, "%s", v);
            else if (!strcasecmp(k, "mtu"))    s_dnst.mtu = atoi(v);
        }
        fclose(f);
    }
    nocsif_sdcard_unlock();

    /* On-watch edits (NVS) win over the file, so the dnst screen is the source of truth. */
    char sv[96]; nocsif_settings_get_str(K_DNSRV, sv, sizeof sv, ""); if (sv[0]) snprintf(s_dnst.server, sizeof s_dnst.server, "%s", sv);
    char kv[64]; nocsif_settings_get_str(K_DNKEY, kv, sizeof kv, ""); if (kv[0]) snprintf(s_dnst.key, sizeof s_dnst.key, "%s", kv);
    int m = nocsif_settings_get_i32(K_DNMTU, 0); if (m > 0) s_dnst.mtu = m;
}

static bool dnst_start(void)
{
    dnst_load_conf();
    if (!s_dnst.server[0] || !s_dnst.key[0]) { snprintf(s_dnst_status, GW_STR, "no config"); return false; }
#if defined(CONFIG_NOCSIF_GW_DNST_EXPERIMENTAL)
    /* Operator-specific codec + custom netif bring-up would go here (see the design note). */
    snprintf(s_dnst_status, GW_STR, "experimental");
    return false;   /* not wired for general use */
#else
    snprintf(s_dnst_status, GW_STR, "experimental");
    ESP_LOGW(TAG, "DNS transport: config OK (server=%s mtu=%d) but no codec build; see note",
             s_dnst.server, s_dnst.mtu);
    return false;
#endif
}
static void dnst_stop(void) { snprintf(s_dnst_status, GW_STR, "off"); }

/* ============================ SoftAP + NAPT (req 1) ===================================== */
static bool uplink_ready(char *ip_out, size_t len)
{
    if (nocsif_wifi_connected()) {
        snprintf(ip_out, len, "%s", nocsif_wifi_ip_str());
        return ip_out[0] != '\0';
    }
    return false;
}

/* Raise the downstream SoftAP alongside the STA (single radio -> APSTA; the AP follows the STA
 * channel automatically). Idempotent. The AP is raised for ANY downstream capability (share /
 * filter / portal), so those treatments compose regardless of what asked for the AP. */
static bool ap_up(void)
{
    if (s_ap_up) return true;
    if (safe_mode()) { snprintf(s_detail, GW_STR, "safe mode"); return false; }

    /* Single radio: we ride wifi.c's STA driver, so the radio must already be up. */
    wifi_mode_t m = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&m) != ESP_OK || (m != WIFI_MODE_STA && m != WIFI_MODE_APSTA)) {
        snprintf(s_detail, GW_STR, "wifi off"); return false;
    }
    if (s_ap_netif == NULL) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
        if (s_ap_netif == NULL) { snprintf(s_detail, GW_STR, "AP netif"); return false; }
    }
    if (esp_wifi_set_mode(WIFI_MODE_APSTA) != ESP_OK) { snprintf(s_detail, GW_STR, "APSTA"); return false; }

    wifi_config_t apc = { 0 };
    snprintf((char *)apc.ap.ssid, sizeof apc.ap.ssid, "%s", s_cfg.ssid);
    apc.ap.ssid_len = strlen((char *)apc.ap.ssid);
    apc.ap.max_connection = s_cfg.maxconn;
    apc.ap.ssid_hidden = s_cfg.hidden ? 1 : 0;
    if (strlen(s_cfg.pass) >= 8) {
        snprintf((char *)apc.ap.password, sizeof apc.ap.password, "%s", s_cfg.pass);
        apc.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        apc.ap.authmode = WIFI_AUTH_OPEN;
    }
    uint8_t pri = 0; wifi_second_chan_t sec;
    if (esp_wifi_get_channel(&pri, &sec) == ESP_OK && pri) apc.ap.channel = pri;   /* match STA */
    if (esp_wifi_set_config(WIFI_IF_AP, &apc) != ESP_OK) snprintf(s_detail, GW_STR, "AP config");

    esp_netif_ip_info_t ipi;
    if (esp_netif_get_ip_info(s_ap_netif, &ipi) == ESP_OK) {
        snprintf(s_ap_ip, sizeof s_ap_ip, IPSTR, IP2STR(&ipi.ip));
        /* Hand downstream clients a working DNS via the SoftAP DHCP: point them at the watch
         * (192.168.4.1), where our resolver answers (forwards upstream, or filters). Without this
         * the SoftAP offers no usable resolver, so clients associate but show "connected, no
         * internet" — name lookups fail even though NAPT would route raw IP traffic fine. The
         * DHCP option can only change while dhcps is stopped. */
        esp_netif_dns_info_t dns = { 0 };
        dns.ip.type = ESP_IPADDR_TYPE_V4;
        dns.ip.u_addr.ip4.addr = ipi.ip.addr;                  /* the AP's own IP (192.168.4.1) */
        esp_netif_dhcps_stop(s_ap_netif);
        esp_netif_set_dns_info(s_ap_netif, ESP_NETIF_DNS_MAIN, &dns);
        uint8_t offer_dns = 2;                                  /* OFFER_DNS (dhcps_offer_t) */
        esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER,
                               &offer_dns, sizeof offer_dns);
        esp_netif_dhcps_start(s_ap_netif);
    }
    snprintf(s_ap_ssid, sizeof s_ap_ssid, "%s", s_cfg.ssid);

    s_ap_up = true;
    s_client_gen++;
    ESP_LOGW(TAG, "gateway SoftAP up: \"%s\" ip %s; int-dma free=%u",
             s_ap_ssid, s_ap_ip, (unsigned)nocsif_int_dma_free());
    return true;
}

static void ap_down(void)
{
    if (!s_ap_up) return;
#if defined(CONFIG_LWIP_IPV4_NAPT)
    if (s_napt_on && s_ap_netif) { esp_netif_napt_disable(s_ap_netif); s_napt_on = false; }
#endif
    esp_wifi_set_mode(WIFI_MODE_STA);            /* drop the AP, keep the uplink */
    s_ap_up = false; s_napt_on = false; s_share_active = false;
    s_ap_ssid[0] = s_ap_ip[0] = '\0';
    s_client_cnt = 0; s_client_gen++;
    ESP_LOGI(TAG, "gateway SoftAP down");
}

/* Layer NAPT forwarding (the "share" capability) on the already-raised AP. Needs an uplink IP. */
static void napt_apply(bool want)
{
    if (want && !s_napt_on && s_ap_up) {
        if (!uplink_ready(s_uplink_ip, sizeof s_uplink_ip)) { s_share_active = false; return; }
#if defined(CONFIG_LWIP_IPV4_NAPT)
        esp_err_t ne = esp_netif_napt_enable(s_ap_netif);
        s_napt_on = (ne == ESP_OK);
        if (!s_napt_on) ESP_LOGE(TAG, "esp_netif_napt_enable: %s (check LWIP_IP_FORWARD/IPV4_NAPT)", esp_err_to_name(ne));
#else
        ESP_LOGE(TAG, "NAPT not built: set CONFIG_LWIP_IP_FORWARD=y + CONFIG_LWIP_IPV4_NAPT=y (sdkconfig.defaults)");
#endif
        s_share_active = s_napt_on;
        if (s_napt_on) ESP_LOGW(TAG, "gateway share ON: %s -> %s%s", s_ap_ip, s_uplink_ip,
                                (s_want_tunnel && wg_up()) ? " (wg)" : "");
    } else if (!want && s_napt_on) {
#if defined(CONFIG_LWIP_IPV4_NAPT)
        if (s_ap_netif) esp_netif_napt_disable(s_ap_netif);
#endif
        s_napt_on = false; s_share_active = false;
        ESP_LOGI(TAG, "gateway share OFF");
    }
}

static void refresh_clients(void)
{
    if (!s_ap_up) { s_client_cnt = 0; return; }
    wifi_sta_list_t list;
    int n = (esp_wifi_ap_get_sta_list(&list) == ESP_OK) ? list.num : 0;
    if (n != s_client_cnt) { s_client_cnt = n; s_client_gen++; }
}

/* ============================ reconcile ================================================= *
 * Bring ACTUAL state to match the DESIRED flags, honouring dependencies. Runs on the worker. */
static void gw_reconcile(void)
{
    if (safe_mode()) { s_available = false; publish_strings(); return; }
    if (!s_cfg.loaded) load_gateway_conf();

    /* Uplink transport: DNS fallback substitutes for STA when asked. */
    if (s_want_dnst && !s_dnst_active) s_dnst_active = dnst_start();
    if (!s_want_dnst && s_dnst_active) { dnst_stop(); s_dnst_active = false; }

    /* Downstream SoftAP: raised for ANY downstream capability, so filter/portal/share compose on
     * one AP (the spec's "whether the AP was raised by share, an advertiser, or the sign-in flow"). */
    bool need_ap = s_want_share || s_want_filter || s_want_portal || up_passthru_active();
    /* A live SSID/pass change re-raises the AP so the new config beacons (ap_down clears NAPT too;
     * ap_up + napt_apply below bring it straight back). */
    if (s_ap_dirty && s_ap_up) ap_down();
    s_ap_dirty = false;
    if (need_ap) ap_up(); else ap_down();
    /* NAPT forwarding (the "share" layer) rides on the AP; needs an uplink IP. */
    napt_apply(s_want_share);

    /* Refresh the WireGuard effective values from the selected file + overrides so the UI always
     * shows the current tunnel (endpoint / port / DNS / address), connected or not. */
    wg_refresh();

    /* WireGuard default route for the forwarded traffic (independent of share, but only
     * meaningful when something is being forwarded). */
    if (s_want_tunnel && !s_tunnel_active) s_tunnel_active = wg_start();
    if (!s_want_tunnel && s_tunnel_active) { wg_stop(); s_tunnel_active = false; }

    /* DNS resolver: run it whenever the AP forwards traffic (share) OR filters OR gates, since clients
     * are handed the watch (192.168.4.1) as their DNS. Share-only just forwards upstream; filter adds
     * the blocklist; portal adds the sign-in gate. The card is only needed for filter/portal. */
    bool need_dns = s_want_share || s_want_filter || s_want_portal || up_passthru_active();
    bool need_sd  = s_want_filter || s_want_portal;
    if (need_sd && !sd_claim() && s_want_filter) {
        snprintf(s_filter_status, GW_STR, "no card");
    }
    if (s_want_filter && !s_filter_active) { bl_load(); s_filter_active = (s_bl_size >= 0); }
    if (!s_want_filter && s_filter_active) {
        s_filter_active = false;
        if (s_bl_mutex) xSemaphoreTake(s_bl_mutex, portMAX_DELAY);
        bl_unload();
        if (s_bl_mutex) xSemaphoreGive(s_bl_mutex);
    }

    if (need_dns) { if (!dns_start()) snprintf(s_detail, GW_STR, "dns bind failed"); }
    else          { dns_stop(); }

    /* Sign-in splash (portal) — and the HTTP server that the Tier 2 uplink-portal passthrough also uses. */
    if (s_want_portal && !s_portal_active) {
        sd_claim(); portal_load_page(); signed_clear();
        s_portal_active = http_start();
        snprintf(s_portal_status, GW_STR, s_portal_active ? "waiting" : "http failed");
    } else if (up_passthru_active() && !s_httpd) {
        http_start();                        /* passthrough-only: bring the proxy httpd up (no splash) */
    }
    if (!s_want_portal && s_portal_active) { http_stop(); s_portal_active = false; snprintf(s_portal_status, GW_STR, "off"); }
    else if (!s_want_portal && !up_passthru_active() && s_httpd) { http_stop(); }   /* passthrough ended */

    /* Release the card when neither filter nor portal needs it. */
    if (!s_want_filter && !s_want_portal) sd_release();

    s_available = true;
    publish_strings();
}

/* ============================ published strings + tick ================================== */
static void publish_strings(void)
{
    if (!s_want_filter) snprintf(s_filter_status, GW_STR, "off");
    if (!s_want_tunnel) snprintf(s_tunnel_status, GW_STR, "off");
    else if (s_tunnel_active) snprintf(s_tunnel_status, GW_STR, "%s", wg_up() ? "up" : "connecting");
    if (!s_want_portal) snprintf(s_portal_status, GW_STR, "off");
    else if (s_portal_active) snprintf(s_portal_status, GW_STR, s_signed_n ? "%d signed" : "waiting", s_signed_n);
    if (!s_want_dnst) snprintf(s_dnst_status, GW_STR, "off");

    if (s_share_active) {
        snprintf(s_status, GW_STR, s_client_cnt ? "%d cli" : "share", s_client_cnt);
        snprintf(s_uplink_ip, sizeof s_uplink_ip, "%s", nocsif_wifi_ip_str());
        snprintf(s_detail, GW_STR, "%s -> %s%s", s_ap_ip[0] ? s_ap_ip : "AP",
                 s_uplink_ip[0] ? s_uplink_ip : "?", (s_want_tunnel && wg_up()) ? " (wg)" : "");
    } else if (s_want_share) {
        snprintf(s_status, GW_STR, "no uplink");
    } else if (s_filter_active || s_portal_active) {
        snprintf(s_status, GW_STR, "dns");
    } else {
        snprintf(s_status, GW_STR, "off");
        if (!s_detail[0] || !s_want_share) snprintf(s_detail, GW_STR, "off");
    }

    /* Compact menu tag. */
    if (s_share_active)       snprintf(s_tag, GW_STR, s_client_cnt ? "on · %d" : "on", s_client_cnt);
    else if (s_want_share)    snprintf(s_tag, GW_STR, "no uplink");
    else if (s_filter_active) snprintf(s_tag, GW_STR, "dns");
    else                      snprintf(s_tag, GW_STR, "off");
}

/* Sample forwarded throughput once a second. lwIP increments ip.fw per NAPT-forwarded packet (ip4.c
 * ip4_forward); the only forwarding on this device is the gateway, so the delta IS the shared traffic.
 * STAT_COUNTER is u16 (LWIP_STATS_LARGE=0), so a 16-bit delta wraps correctly for any sane per-second
 * rate. kbps assumes full-size frames — coarse, but a good live A/B signal alongside the phone test. */
static void sample_throughput(void)
{
#if LWIP_STATS && IP_STATS
    uint16_t cur = (uint16_t)lwip_stats.ip.fw;
    if (s_fwd_have) {
        uint16_t d = (uint16_t)(cur - s_fwd_last);
        s_fwd_pps  = d;
        s_fwd_kbps = (uint32_t)(((uint64_t)d * 1500u * 8u) / 1000u);
    }
    s_fwd_last = cur;
    s_fwd_have = true;
#else
    s_fwd_pps = s_fwd_kbps = 0;
#endif
}

static void gw_tick(void *arg)
{
    (void)arg;
    refresh_clients();
    sample_throughput();
    /* Late uplink: share requested but NAPT not yet on because there was no uplink -> retry when
     * one appears. And tear NAPT down if the uplink drops out from under an active share. */
    bool linked = nocsif_wifi_connected();
    if ((s_want_share && !s_napt_on && linked) || (s_napt_on && !linked)) {
        gw_cmd_t c = CMD_RECONCILE; if (s_q) xQueueSend(s_q, &c, 0);
    }
    /* Uplink captive-portal detection: on a fresh STA link, probe for a sign-in page (only when the
     * owner is travel-routing — share intended — or auto-accept is armed, so we don't probe on every
     * casual join). A manual "Sign in" from the UI calls portal_kick(true) directly. */
    if (linked && !s_up_link_last && (s_want_share || s_up_autoacc || s_up_passthru)) portal_kick(false);
    if (!linked && s_up_link_last) { s_up_state = UP_NONE; s_up_url[0] = s_up_host[0] = '\0'; s_up_gen++; }
    s_up_link_last = linked;
    publish_strings();
}

/* ============================ worker task + queue ======================================= */
static void gw_task(void *arg)
{
    (void)arg;
    for (;;) {
        gw_cmd_t c;
        if (xQueueReceive(s_q, &c, portMAX_DELAY) != pdTRUE) continue;
        switch (c) {
        case CMD_RELOAD:
            s_cfg.loaded = false;
            load_gateway_conf();
            if (s_filter_active) { bl_unload(); bl_load(); }
            if (s_portal_active) portal_load_page();
            /* fallthrough to reconcile */
        case CMD_RECONCILE:
        default:
            gw_reconcile();
            break;
        }
    }
}

static void post(gw_cmd_t c) { if (s_q) xQueueSend(s_q, &c, 0); }

/* ============================ public API =============================================== */
esp_err_t nocsif_gateway_init(void)
{
    if (s_task) return ESP_OK;
    if (safe_mode()) { s_available = false; return ESP_OK; }

    /* Load desired flags from NVS (all default off). */
    s_want_share  = nocsif_settings_get_i32(K_SHARE,  0) != 0;
    s_want_filter = nocsif_settings_get_i32(K_FILTER, 0) != 0;
    /* The tunnel is NOT restored at boot: it is a manual per-session action. Auto-connecting a VPN
     * before the network is even up is fragile, and a tunnel that faults at bring-up would otherwise
     * boot-loop (the persisted flag re-crashing every boot). Clear any stale persisted value. */
    s_want_tunnel = false;
    nocsif_settings_set_i32(K_TUNNEL, 0);
    s_want_portal = nocsif_settings_get_i32(K_PORTAL, 0) != 0;
    s_want_dnst   = nocsif_settings_get_i32(K_DNST,   0) != 0;
    /* Uplink captive-portal sign-in prefs. */
    s_up_autoacc  = nocsif_settings_get_i32(K_AUTOACC, 0) != 0;
    s_up_passthru = nocsif_settings_get_i32(K_PXY,     0) != 0;
    snprintf(s_up_detail, sizeof s_up_detail, "%s", "not checked");

    /* WireGuard selection + minor overrides, and dnst settings, from NVS (UI shows them at once). */
    nocsif_settings_get_str(K_WGFILE, s_wg_file,   sizeof s_wg_file,   "wg0.conf");
    nocsif_settings_get_str(K_WGEP,   s_wg_ov_ep,  sizeof s_wg_ov_ep,  "");
    s_wg_ov_port = nocsif_settings_get_i32(K_WGPORT, 0);
    nocsif_settings_get_str(K_WGDNS,  s_wg_ov_dns, sizeof s_wg_ov_dns, "");
    nocsif_settings_get_str(K_DNSRV,  s_dnst.server, sizeof s_dnst.server, "");
    nocsif_settings_get_str(K_DNKEY,  s_dnst.key,    sizeof s_dnst.key,    "");
    s_dnst.mtu = nocsif_settings_get_i32(K_DNMTU, 1200);

    /* SoftAP SSID / passphrase (defaults + any on-watch NVS override) so the UI shows them at once. */
    cfg_defaults();
    cfg_apply_nvs();

    snprintf(s_status, GW_STR, "off");
    snprintf(s_detail, GW_STR, "off");
    snprintf(s_tag,    GW_STR, "off");
    snprintf(s_filter_status, GW_STR, "off");
    snprintf(s_tunnel_status, GW_STR, "off");
    snprintf(s_portal_status, GW_STR, "off");
    snprintf(s_dnst_status,   GW_STR, "off");

    s_bl_mutex = xSemaphoreCreateMutex();   /* guards blocklist rebuild vs the dns_task lookups */

    s_q = xQueueCreate(GW_CMD_QLEN, sizeof(gw_cmd_t));
    if (!s_q) return ESP_ERR_NO_MEM;
    /* 6144 internal: the worker does FatFs reads (gateway.conf + the blocklist index build) + socket
     * and httpd bring-up on this stack. */
    if (xTaskCreate(gw_task, "gateway", 6144, NULL, 4, &s_task) != pdPASS) return ESP_FAIL;

    const esp_timer_create_args_t ta = { .callback = gw_tick, .name = "gwtick" };
    if (esp_timer_create(&ta, &s_tick) == ESP_OK) esp_timer_start_periodic(s_tick, 1000000);

    s_available = true;
    /* Re-apply any persisted desired flags (e.g. a saved "share on"). */
    if (s_want_share || s_want_filter || s_want_tunnel || s_want_portal || s_want_dnst) post(CMD_RECONCILE);
    ESP_LOGI(TAG, "gateway worker up");
    return ESP_OK;
}

/* Setters: persist on the CALLER task (no flash writes on the PSRAM worker), then reconcile. */
void nocsif_gateway_request_share(bool on)  { s_want_share  = on; nocsif_settings_set_i32(K_SHARE,  on); post(CMD_RECONCILE); }
void nocsif_gateway_request_filter(bool on) { s_want_filter = on; nocsif_settings_set_i32(K_FILTER, on); post(CMD_RECONCILE); }
void nocsif_gateway_request_tunnel(bool on) { s_want_tunnel = on; post(CMD_RECONCILE); }   /* session-only (not persisted; see init) */
void nocsif_gateway_request_portal(bool on) { s_want_portal = on; nocsif_settings_set_i32(K_PORTAL, on); post(CMD_RECONCILE); }
void nocsif_gateway_request_dnst(bool on)   { s_want_dnst   = on; nocsif_settings_set_i32(K_DNST,   on); post(CMD_RECONCILE); }
void nocsif_gateway_request_reload(void)    { post(CMD_RELOAD); }

/* No-persist test entry (safe from any task — only sets a volatile flag + posts to the worker). */
void nocsif_gateway_set_test(char cap, bool on)
{
    switch (cap) {
        case 's': s_want_share  = on; break;
        case 'f': s_want_filter = on; break;
        case 't': s_want_tunnel = on; break;
        case 'p': s_want_portal = on; break;
        case 'd': s_want_dnst   = on; break;
        default: return;
    }
    post(CMD_RECONCILE);
}

/* ---- WireGuard config selection + minor-field editing (persist on the caller task) --------- */
void nocsif_gateway_wg_select(const char *name)
{
    snprintf(s_wg_file, sizeof s_wg_file, "%s", (name && name[0]) ? name : "wg0.conf");
    nocsif_settings_set_str(K_WGFILE, s_wg_file);
    /* A newly-imported file carries its own endpoint/port/DNS — drop the old overrides. */
    s_wg_ov_ep[0] = s_wg_ov_dns[0] = '\0'; s_wg_ov_port = 0;
    nocsif_settings_set_str(K_WGEP, ""); nocsif_settings_set_str(K_WGDNS, ""); nocsif_settings_set_i32(K_WGPORT, 0);
    post(CMD_RECONCILE);
}
const char *nocsif_gateway_wg_selected(void)      { return s_wg_file; }
void nocsif_gateway_wg_set_endpoint(const char *host)
{
    snprintf(s_wg_ov_ep, sizeof s_wg_ov_ep, "%s", host ? host : "");
    nocsif_settings_set_str(K_WGEP, s_wg_ov_ep); post(CMD_RECONCILE);
}
void nocsif_gateway_wg_set_port(int port)
{
    s_wg_ov_port = (port > 0 && port < 65536) ? port : 0;
    nocsif_settings_set_i32(K_WGPORT, s_wg_ov_port); post(CMD_RECONCILE);
}
void nocsif_gateway_wg_set_dns(const char *dns)
{
    snprintf(s_wg_ov_dns, sizeof s_wg_ov_dns, "%s", dns ? dns : "");
    nocsif_settings_set_str(K_WGDNS, s_wg_ov_dns); post(CMD_RECONCILE);
}
const char *nocsif_gateway_wg_endpoint_host(void) { return s_wg_ep; }
int         nocsif_gateway_wg_port(void)          { return s_wg_port; }
const char *nocsif_gateway_wg_dns_str(void)       { return s_wg_dns; }
const char *nocsif_gateway_wg_address_str(void)   { return s_wg_addr; }
bool        nocsif_gateway_wg_has_config(void)    { return s_wg_conf_ok; }

/* ---- dnst editable settings (scaffold; persisted for later server work) -------------------- */
void nocsif_gateway_dnst_set_server(const char *s)
{
    snprintf(s_dnst.server, sizeof s_dnst.server, "%s", s ? s : "");
    nocsif_settings_set_str(K_DNSRV, s_dnst.server); post(CMD_RECONCILE);
}
void nocsif_gateway_dnst_set_key(const char *k)
{
    snprintf(s_dnst.key, sizeof s_dnst.key, "%s", k ? k : "");
    nocsif_settings_set_str(K_DNKEY, s_dnst.key); post(CMD_RECONCILE);
}
void nocsif_gateway_dnst_set_mtu(int mtu)
{
    s_dnst.mtu = (mtu >= 200 && mtu <= 1400) ? mtu : 1200;
    nocsif_settings_set_i32(K_DNMTU, s_dnst.mtu); post(CMD_RECONCILE);
}
const char *nocsif_gateway_dnst_server_str(void)  { return s_dnst.server; }
const char *nocsif_gateway_dnst_key_str(void)     { return s_dnst.key; }
int         nocsif_gateway_dnst_mtu(void)         { return s_dnst.mtu; }

/* ---- SoftAP name / passphrase (edit from the Travel Router screen) -------------------------- */
void nocsif_gateway_set_ap_ssid(const char *ssid)
{
    snprintf(s_cfg.ssid, sizeof s_cfg.ssid, "%s", (ssid && ssid[0]) ? ssid : "NocSif-Gateway");
    nocsif_settings_set_str(K_APSSID, s_cfg.ssid);
    s_ap_dirty = true; post(CMD_RECONCILE);
}
void nocsif_gateway_set_ap_pass(const char *pass)
{
    /* Empty = open. 1..7 chars can't form WPA2, so treat as open to avoid a silently-open "secured" AP. */
    if (pass && strlen(pass) >= 8) snprintf(s_cfg.pass, sizeof s_cfg.pass, "%s", pass);
    else                           s_cfg.pass[0] = '\0';
    nocsif_settings_set_str(K_APPASS, s_cfg.pass);
    s_ap_dirty = true; post(CMD_RECONCILE);
}
const char *nocsif_gateway_ap_ssid_cfg(void) { return s_cfg.ssid[0] ? s_cfg.ssid : "NocSif-Gateway"; }
const char *nocsif_gateway_ap_pass(void)     { return s_cfg.pass; }
bool        nocsif_gateway_ap_secured(void)  { return strlen(s_cfg.pass) >= 8; }

bool nocsif_gateway_share_wanted(void)  { return s_want_share; }
bool nocsif_gateway_filter_wanted(void) { return s_want_filter; }
bool nocsif_gateway_tunnel_wanted(void) { return s_want_tunnel; }
bool nocsif_gateway_portal_wanted(void) { return s_want_portal; }
bool nocsif_gateway_dnst_wanted(void)   { return s_want_dnst; }

bool nocsif_gateway_available(void)     { return s_available; }
bool nocsif_gateway_share_active(void)  { return s_share_active; }
bool nocsif_gateway_filter_active(void) { return s_filter_active; }
bool nocsif_gateway_tunnel_active(void) { return s_tunnel_active; }
bool nocsif_gateway_portal_active(void) { return s_portal_active; }
bool nocsif_gateway_dnst_active(void)   { return s_dnst_active; }

int         nocsif_gateway_client_count(void) { return s_client_cnt; }
uint32_t    nocsif_gateway_client_gen(void)   { return s_client_gen; }
const char *nocsif_gateway_ap_ssid(void)      { return s_ap_ssid; }
const char *nocsif_gateway_ap_ip_str(void)    { return s_ap_ip; }
const char *nocsif_gateway_uplink_ip_str(void){ return s_uplink_ip; }
const char *nocsif_gateway_status_str(void)   { return s_status; }
const char *nocsif_gateway_detail_str(void)   { return s_detail; }

bool        nocsif_gateway_share_persisted(void) { return nocsif_settings_get_i32(K_SHARE, 0) != 0; }
uint32_t    nocsif_gateway_fwd_pps(void)      { return s_fwd_pps; }
uint32_t    nocsif_gateway_fwd_kbps(void)     { return s_fwd_kbps; }

uint32_t    nocsif_gateway_dns_queries(void)   { return s_dns_q; }
uint32_t    nocsif_gateway_dns_blocked(void)   { return s_dns_blocked; }
uint32_t    nocsif_gateway_dns_forwarded(void) { return s_dns_fwd; }
uint32_t    nocsif_gateway_dns_cached(void)    { return s_dns_cached; }
int         nocsif_gateway_blocklist_size(void){ return s_bl_size; }
const char *nocsif_gateway_filter_status_str(void) { return s_filter_status; }

bool        nocsif_gateway_tunnel_up(void)          { return s_tunnel_active && wg_up(); }
const char *nocsif_gateway_tunnel_status_str(void)  { return s_tunnel_status; }
const char *nocsif_gateway_tunnel_endpoint_str(void){ return s_tunnel_ep; }

int         nocsif_gateway_signed_count(void)       { return s_signed_n; }
uint32_t    nocsif_gateway_portal_gen(void)         { return s_portal_gen; }
const char *nocsif_gateway_portal_status_str(void)  { return s_portal_status; }

const char *nocsif_gateway_dnst_status_str(void)    { return s_dnst_status; }
const char *nocsif_gateway_tag_str(void)            { return s_tag; }

/* ---- uplink captive-portal sign-in (Travel Router) ------------------------------------------ */
void nocsif_gateway_uplink_autoaccept(bool on)
{
    s_up_autoacc = on; nocsif_settings_set_i32(K_AUTOACC, on);
}
void nocsif_gateway_uplink_passthru(bool on)
{
    s_up_passthru = on; nocsif_settings_set_i32(K_PXY, on);
    post(CMD_RECONCILE);                 /* bring the AP/DNS/proxy up or down for the passthrough */
}
void nocsif_gateway_uplink_signin(void) { portal_kick(true); }   /* manual: probe + auto-accept now */

bool nocsif_gateway_uplink_autoaccept_on(void) { return s_up_autoacc; }
bool nocsif_gateway_uplink_passthru_on(void)   { return s_up_passthru; }
bool nocsif_gateway_uplink_online(void) { return s_up_state == UP_ONLINE || s_up_state == UP_SIGNED; }
bool nocsif_gateway_uplink_signin_needed(void)
{
    return s_up_state == UP_SIGNIN || s_up_state == UP_SIGNING ||
           s_up_state == UP_LOGIN  || s_up_state == UP_FAILED;
}
uint32_t    nocsif_gateway_uplink_gen(void)        { return s_up_gen; }
const char *nocsif_gateway_uplink_detail_str(void) { return s_up_detail; }
const char *nocsif_gateway_uplink_url_str(void)    { return s_up_url; }
const char *nocsif_gateway_uplink_status_str(void)
{
    switch (s_up_state) {
        case UP_CHECKING: return "checking";
        case UP_ONLINE:   return "online";
        case UP_SIGNIN:   return "sign-in required";
        case UP_SIGNING:  return "signing in";
        case UP_SIGNED:   return "signed in";
        case UP_LOGIN:    return "login needed";
        case UP_FAILED:   return "sign-in failed";
        default:          return "not checked";
    }
}
