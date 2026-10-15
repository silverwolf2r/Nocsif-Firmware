/*
 * NocSif — Travel Router traffic: per-device usage + "calls first" priority. See qos.h.
 */
#include "qos.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_private/wifi.h"          /* esp_wifi_set_tx_done_cb — frames leaving the driver */
#include "esp_netif_net_stack.h"       /* esp_netif_get_netif_impl — the lwIP netifs to hook */
#include "lwip/netif.h"
#include "lwip/pbuf.h"

static const char *TAG = "qos";

#define QOS_TOS        0xA0      /* CS5: TOS >> 5 = 5 -> TID 5 -> the video access category              */
#define QOS_RESERVE    2         /* TX buffers bulk frames leave free while priority frames are failing */
#define QOS_RESERVE_US 2000000   /* ... for this long after a priority frame found the TX pool full     */
#define QOS_ACTIVE_US  1500000   /* "priority traffic flowing" = a priority packet within this window    */
#define QOS_STALE_US   300000    /* no TX-done this long: the in-flight count is not trustworthy, reset */
#define QOS_TCP_SMALL  100       /* TCP packets this short (IP total length) are ACKs / keystrokes       */
#define QOS_TUNNEL_SMALL 300     /* tunnel-port packets this short are voice (Wi-Fi Calling, VPN'd calls) */
/* Per-device priority budget, each direction: enough for a video call plus remote-desktop typing. Beyond it a
 * device's interactive packets go out unmarked and compete as normal traffic, so one busy remote-desktop picture
 * cannot take the whole link from the other device. */
#define QOS_BUDGET_BPMS 250      /* bytes per ms = 2 Mbit/s                                               */
#define QOS_BURST      24000     /* bytes of priority a device may send at once                           */
#define QOS_MAX_CLI    8

static netif_input_fn      s_ap_in_orig;
static netif_linkoutput_fn s_ap_out_orig, s_sta_out_orig;
static volatile uint32_t   s_ap_net, s_ap_mask;          /* hotspot subnet, network order (0 = unknown) */

static uint32_t *s_up, *s_dn, *s_pk;                     /* [256] per last address octet, PSRAM         */
static int32_t  *s_tok_up, *s_tok_dn;                    /* [256] priority budget left (bytes), per direction */
static uint32_t *s_tokt_up, *s_tokt_dn;                  /* [256] last refill (ms)                       */
static volatile uint32_t s_prio_up, s_prio_dn, s_shed, s_tx_full, s_over, s_prio_fail;
static volatile int32_t  s_inflight, s_cap;
static volatile int64_t  s_prio_last_us, s_txdone_last_us, s_prio_fail_us;
static volatile bool     s_txcb, s_hooked;

typedef struct {
    nocsif_qos_client_t pub;
    uint32_t up_last, dn_last, pk_last;
    bool     have;
} cli_t;
static cli_t       *s_cli;                            /* [QOS_MAX_CLI], PSRAM (allocated with the counters) */
typedef struct {                                         /* the tick's scratch (PSRAM: it runs on the esp_timer task) */
    wifi_sta_list_t         sl;
    esp_netif_pair_mac_ip_t pairs[QOS_MAX_CLI];
    cli_t                   next[QOS_MAX_CLI];
} tick_scratch_t;
static tick_scratch_t *s_ts;
static int          s_ncli;
static int64_t      s_tick_last_us;
static portMUX_TYPE s_cli_mux = portMUX_INITIALIZER_UNLOCKED;

/* Interactive or bulk? `ip` = IPv4 header, `len` bytes of it in this segment; `up` = sent by a hotspot device. */
static bool is_prio(const uint8_t *ip, int len, bool up)
{
    if (len < 20 || (ip[0] >> 4) != 4) return false;
    const int ihl = (ip[0] & 0x0F) * 4;
    const uint16_t tot = (uint16_t)((ip[2] << 8) | ip[3]);
    const uint8_t proto = ip[9];
    if (proto == 1) return true;                                /* ping: measures the priority path */
    if (proto == 6) return tot <= QOS_TCP_SMALL;
    if (proto != 17 || len < ihl + 8 || (ip[6] & 0x1F) || ip[7]) return false;   /* UDP, first fragment */
    const uint8_t *u = ip + ihl;
    const uint16_t far = up ? (uint16_t)((u[2] << 8) | u[3]) : (uint16_t)((u[0] << 8) | u[1]);   /* server port */
    switch (far) {
    case 500: case 4500: case 1194: case 51820:                 /* tunnels: voice-sized packets only (Wi-Fi Calling) */
        return tot <= QOS_TUNNEL_SMALL;
    case 443: case 80: {                                        /* QUIC (video, web) vs a DTLS stream (remote desktop) */
        if (len < ihl + 8 + 3) return false;
        const uint8_t *d = u + 8;
        return d[0] >= 0x14 && d[0] <= 0x19 && d[1] == 0xFE && d[2] >= 0xFC;
    }
    default: return true;
    }
}

/* Raise the IP precedence to CS5 (keeps ECN), with an incremental header-checksum update (RFC 1624). */
static void mark(uint8_t *ip)
{
    const uint8_t old = ip[1];
    if ((old >> 5) >= (QOS_TOS >> 5)) return;
    const uint8_t nt = (uint8_t)(QOS_TOS | (old & 0x03));
    uint32_t sum = (uint16_t)~((ip[10] << 8) | ip[11]);
    sum += (uint16_t)~((ip[0] << 8) | old);
    sum += (uint16_t)((ip[0] << 8) | nt);
    sum = (sum & 0xFFFF) + (sum >> 16);
    sum = (sum & 0xFFFF) + (sum >> 16);
    const uint16_t hc = (uint16_t)~sum;
    ip[1] = nt;
    ip[10] = (uint8_t)(hc >> 8);
    ip[11] = (uint8_t)hc;
}

static inline bool on_hotspot(uint32_t a) { return s_ap_mask && (a & s_ap_mask) == s_ap_net; }

/* Spend `bytes` of device `o`'s priority budget (token bucket). Each direction has one writer: uploads on the
 * Wi-Fi receive path, downloads on the lwIP thread. False = over budget: send it as normal traffic. */
static bool take_budget(int32_t *tok, uint32_t *tt, uint8_t o, uint32_t bytes)
{
    if (!tok) return true;
    const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    int64_t v = (int64_t)tok[o] + (int64_t)(uint32_t)(now - tt[o]) * QOS_BUDGET_BPMS;
    tt[o] = now;
    if (v > QOS_BURST) v = QOS_BURST;
    if (v < (int64_t)bytes) { tok[o] = (int32_t)v; s_over++; return false; }
    tok[o] = (int32_t)(v - (int64_t)bytes);
    return true;
}

/* From a hotspot device: count its upload, mark interactive packets before they are forwarded. */
static err_t ap_input(struct pbuf *p, struct netif *inp)
{
    if (p && p->len >= 14 + 20) {
        uint8_t *f = (uint8_t *)p->payload;
        if (f[12] == 0x08 && f[13] == 0x00) {
            uint8_t *ip = f + 14;
            uint32_t src;
            memcpy(&src, ip + 12, 4);
            if (on_hotspot(src)) {
                const uint8_t o = ((const uint8_t *)&src)[3];
                if (s_up) s_up[o] += p->tot_len;
                if (is_prio(ip, p->len - 14, true) && take_budget(s_tok_up, s_tokt_up, o, p->tot_len)) {
                    mark(ip);
                    s_prio_up++;
                    s_prio_last_us = esp_timer_get_time();
                    if (s_pk) s_pk[o]++;
                }
            }
        }
    }
    return s_ap_in_orig(p, inp);
}

/* Both transmit paths: priority frames always go to the driver. Bulk frames leave QOS_RESERVE buffers free only
 * while priority frames are actually failing for lack of one (within QOS_RESERVE_US of the last such failure) —
 * a steady call or remote desktop that gets its buffers holds nothing back. In-flight = frames accepted by the
 * driver minus TX-done callbacks. */
static err_t tx_common(netif_linkoutput_fn orig, struct netif *nif, struct pbuf *p, bool ap)
{
    bool prio = true;                                           /* ARP / EAPOL / anything non-IPv4: never held */
    if (p->len >= 14 + 20) {
        uint8_t *f = (uint8_t *)p->payload;
        if (f[12] == 0x08 && f[13] == 0x00) {
            uint8_t *ip = f + 14;
            if (ap) {
                uint32_t dst;
                memcpy(&dst, ip + 16, 4);
                if (on_hotspot(dst)) {
                    const uint8_t o = ((const uint8_t *)&dst)[3];
                    if (s_dn) s_dn[o] += p->tot_len;
                    if (is_prio(ip, p->len - 14, false) && take_budget(s_tok_dn, s_tokt_dn, o, p->tot_len)) {
                        mark(ip);
                        s_prio_dn++;
                        s_prio_last_us = esp_timer_get_time();
                        if (s_pk) s_pk[o]++;
                    }
                }
            }
            prio = (ip[1] >> 5) >= (QOS_TOS >> 5);
        }
    }
    if (s_txcb) {
        const int64_t now = esp_timer_get_time();
        if (s_inflight > 0 && now - s_txdone_last_us > QOS_STALE_US) s_inflight = 0;
        if (!prio && s_cap >= 4 && now - s_prio_fail_us < QOS_RESERVE_US && s_inflight >= s_cap - QOS_RESERVE) {
            s_shed++;
            return ERR_MEM;                                     /* as a full pool would: TCP / QUIC back off */
        }
    }
    err_t e = orig(nif, p);
    if (e == ERR_MEM) {
        s_tx_full++;
        if (prio) { s_prio_fail++; s_prio_fail_us = esp_timer_get_time(); }
    }
    if (s_txcb) {
        if (e == ERR_OK) {
            __atomic_add_fetch(&s_inflight, 1, __ATOMIC_RELAXED);
        } else if (e == ERR_MEM) {
            const int32_t c = s_inflight;
            if (c > 0) s_cap = s_cap ? (s_cap * 3 + c) / 4 : c;
        }
    }
    return e;
}
static err_t ap_output(struct netif *nif, struct pbuf *p)  { return tx_common(s_ap_out_orig, nif, p, true); }
static err_t sta_output(struct netif *nif, struct pbuf *p) { return tx_common(s_sta_out_orig, nif, p, false); }

static void IRAM_ATTR tx_done_cb(uint8_t ifidx, uint8_t *data, uint16_t *len, bool ok)
{
    (void)ifidx; (void)data; (void)len; (void)ok;
    s_txdone_last_us = esp_timer_get_time();
    if (__atomic_sub_fetch(&s_inflight, 1, __ATOMIC_RELAXED) < 0) __atomic_store_n(&s_inflight, 0, __ATOMIC_RELAXED);
}

static void hook(esp_netif_t *ap)
{
    struct netif *a = (struct netif *)esp_netif_get_netif_impl(ap);
    esp_netif_t *sh = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    struct netif *s = sh ? (struct netif *)esp_netif_get_netif_impl(sh) : NULL;
    if (a) {
        esp_netif_ip_info_t ii;
        if (esp_netif_get_ip_info(ap, &ii) == ESP_OK && ii.netmask.addr) {
            s_ap_net = ii.ip.addr & ii.netmask.addr;
            s_ap_mask = ii.netmask.addr;
        }
        if (a->input && a->input != ap_input) {
            s_ap_in_orig = a->input; __sync_synchronize(); a->input = ap_input;
        }
        if (a->linkoutput && a->linkoutput != ap_output) {
            s_ap_out_orig = a->linkoutput; __sync_synchronize(); a->linkoutput = ap_output;
        }
    }
    if (s && s->linkoutput && s->linkoutput != sta_output) {
        s_sta_out_orig = s->linkoutput; __sync_synchronize(); s->linkoutput = sta_output;
    }
    const bool now_hooked = a && a->input == ap_input && a->linkoutput == ap_output && s && s->linkoutput == sta_output;
    if (now_hooked && !s_hooked) ESP_LOGI(TAG, "hotspot usage counters + calls-first priority on");
    s_hooked = now_hooked;
    if (!s_txcb && esp_wifi_set_tx_done_cb(tx_done_cb) == ESP_OK) s_txcb = true;
}

void nocsif_qos_tick(esp_netif_t *ap, bool sharing)
{
    if (!ap || !sharing) {
        portENTER_CRITICAL(&s_cli_mux); s_ncli = 0; portEXIT_CRITICAL(&s_cli_mux);
        return;
    }
    if (!s_up) {
        s_up = heap_caps_calloc(256, sizeof(uint32_t), MALLOC_CAP_SPIRAM);
        s_dn = heap_caps_calloc(256, sizeof(uint32_t), MALLOC_CAP_SPIRAM);
        s_pk = heap_caps_calloc(256, sizeof(uint32_t), MALLOC_CAP_SPIRAM);
        s_ts = heap_caps_calloc(1, sizeof(tick_scratch_t), MALLOC_CAP_SPIRAM);
        s_tokt_up = heap_caps_calloc(256, sizeof(uint32_t), MALLOC_CAP_SPIRAM);
        s_tokt_dn = heap_caps_calloc(256, sizeof(uint32_t), MALLOC_CAP_SPIRAM);
        int32_t *tu = heap_caps_calloc(256, sizeof(int32_t), MALLOC_CAP_SPIRAM);
        int32_t *td = heap_caps_calloc(256, sizeof(int32_t), MALLOC_CAP_SPIRAM);
        if (s_tokt_up && s_tokt_dn) { s_tok_up = tu; s_tok_dn = td; }   /* budgets on only with their clocks */
        cli_t *c = heap_caps_calloc(QOS_MAX_CLI, sizeof(cli_t), MALLOC_CAP_SPIRAM);
        portENTER_CRITICAL(&s_cli_mux); s_cli = c; portEXIT_CRITICAL(&s_cli_mux);
    }
    if (!s_up || !s_dn || !s_pk || !s_ts || !s_cli) return;
    hook(ap);

    const int64_t now = esp_timer_get_time();
    const int64_t dt = s_tick_last_us ? now - s_tick_last_us : 1000000;
    s_tick_last_us = now;

    wifi_sta_list_t *slp = &s_ts->sl;
    esp_netif_pair_mac_ip_t *pairs = s_ts->pairs;
    cli_t *next = s_ts->next;
    if (esp_wifi_ap_get_sta_list(slp) != ESP_OK) slp->num = 0;
    const int n = slp->num < QOS_MAX_CLI ? slp->num : QOS_MAX_CLI;
    for (int i = 0; i < n; i++) { memcpy(pairs[i].mac, slp->sta[i].mac, 6); pairs[i].ip.addr = 0; }
    if (n > 0) esp_netif_dhcps_get_clients_by_mac(ap, n, pairs);
    memset(next, 0, sizeof(cli_t) * QOS_MAX_CLI);
    for (int i = 0; i < n; i++) {
        cli_t *c = &next[i];
        for (int j = 0; j < s_ncli; j++) {                      /* carry totals for a device already listed */
            if (memcmp(s_cli[j].pub.mac, slp->sta[i].mac, 6) == 0) { *c = s_cli[j]; break; }
        }
        memcpy(c->pub.mac, slp->sta[i].mac, 6);
        c->pub.rssi = slp->sta[i].rssi;
        const uint32_t ip = pairs[i].ip.addr;
        if (ip && on_hotspot(ip)) {
            const uint8_t o = ((const uint8_t *)&ip)[3];
            const uint32_t up = s_up[o], dn = s_dn[o], pk = s_pk[o];
            if (c->have && c->pub.ip == ip && dt > 0) {
                const uint32_t du = up - c->up_last, dd = dn - c->dn_last, dp = pk - c->pk_last;
                c->pub.up_bytes += du;
                c->pub.dn_bytes += dd;
                c->pub.up_kbps  = (uint32_t)((uint64_t)du * 8000ull / (uint64_t)dt);
                c->pub.dn_kbps  = (uint32_t)((uint64_t)dd * 8000ull / (uint64_t)dt);
                c->pub.prio_pps = (uint32_t)((uint64_t)dp * 1000000ull / (uint64_t)dt);
            }
            c->pub.ip = ip;
            c->up_last = up; c->dn_last = dn; c->pk_last = pk;
            c->have = true;
        }
    }
    portENTER_CRITICAL(&s_cli_mux);
    memcpy(s_cli, next, sizeof(cli_t) * QOS_MAX_CLI);
    s_ncli = n;
    portEXIT_CRITICAL(&s_cli_mux);
}

int nocsif_qos_clients(nocsif_qos_client_t *out, int max)
{
    if (!out || max <= 0) return 0;
    portENTER_CRITICAL(&s_cli_mux);
    const int n = !s_cli ? 0 : s_ncli < max ? s_ncli : max;
    for (int i = 0; i < n; i++) out[i] = s_cli[i].pub;
    portEXIT_CRITICAL(&s_cli_mux);
    return n;
}

void nocsif_qos_stats(nocsif_qos_stats_t *o)
{
    if (!o) return;
    o->prio_up = s_prio_up; o->prio_dn = s_prio_dn; o->shed = s_shed; o->tx_full = s_tx_full;
    o->over = s_over; o->prio_fail = s_prio_fail;
    o->inflight = s_inflight; o->cap = s_cap;
    o->hooked = s_hooked; o->txcb = s_txcb;
    o->prio_active = esp_timer_get_time() - s_prio_last_us < QOS_ACTIVE_US;
}
