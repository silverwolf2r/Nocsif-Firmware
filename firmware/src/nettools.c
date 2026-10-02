/* nettools.c — on-LAN network tooling (WiFi sector #6). See nettools.h for the contract.
 *
 * Model: one PSRAM-stacked worker drains a short job queue (sweep / scan / ping / log). Jobs run to
 * completion with a cancel flag checked in every bounded loop. Results live in PSRAM behind a mutex;
 * the LVGL task reads snapshot copies and polls a generation counter. NO radio I/O — this is pure
 * lwIP sockets over the existing STA link, so it never touches the wifi.c worker or the 802.11 state.
 *
 * ICMP uses one raw socket (fire-and-collect for the sweep; sequential for a targeted ping) rather than
 * 254 esp_ping sessions — far lighter and fully cancellable. Host enrichment reads the lwIP ARP cache
 * (best-effort MAC), maps the OUI to a vendor, and does a NBNS node-status query for the host name. The
 * TCP connect-scan runs a small non-blocking socket pool (bounded by the lwIP socket budget) with short
 * timeouts and an optional first-bytes banner grab. */

#include "nettools.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   /* strncasecmp — SSDP header match */
#include <ctype.h>
#include <fcntl.h>
#include <errno.h>
#include <stdarg.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/idf_additions.h"   /* xTaskCreateWithCaps / vTaskDeleteWithCaps — PSRAM worker stack */

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_netif.h"

#include "lwip/sockets.h"
#include "lwip/etharp.h"              /* etharp_get_entry — best-effort ARP-cache MAC */
#include "lwip/netdb.h"              /* getaddrinfo — HTTP recon name resolution */
#include "lwip/raw.h"                /* raw_pcb + RAW_FLAGS_HDRINCL — crafter source-IP (HDRINCL) send */
#include "lwip/tcpip.h"              /* tcpip_callback — run the raw send on the tcpip thread (no core lock) */
#include "mdns.h"                     /* mDNS / DNS-SD browse (service discovery) */
/* mbedTLS 3.x marks the x509_crt fields we read (subject/issuer/validity/pk/SANs) private; this lets
 * the cert peek read them directly. Must precede the mbedtls includes. */
#define MBEDTLS_ALLOW_PRIVATE_ACCESS
#include "mbedtls/ssl.h"             /* TLS peek (zgrab-style cert grab, no verification) */
#include "mbedtls/net_sockets.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/x509_crt.h"

#include "wifi.h"                     /* nocsif_wifi_connected */
#include "reliability.h"              /* nocsif_reliability_safe_mode */
#include "sdcard.h"                   /* nocsif_sdcard_lock/unlock */
#include "usb_gadget.h"               /* nocsif_usb_gadget_claim_sd */

static const char *TAG = "nettools";

#ifndef IPPROTO_ICMP
#define IPPROTO_ICMP 1
#endif

/* ---- tunables (bounded for RAM + the lwIP socket budget + a sane on-watch wall-clock) ---- */
#define NT_STACK          16384       /* PSRAM worker stack (headroom for the TLS handshake in http_tls) */
#define NT_QLEN           3           /* pending jobs                                          */
#define NT_ICMP_PAYLOAD   32          /* echo data bytes                                       */
#define NT_SWEEP_BATCH    24          /* echo requests fired between settle passes              */
#define NT_SWEEP_MAX      254         /* host cap per sweep (a /24 minus net + broadcast)      */
#define NT_SWEEP_SETTLE   180         /* ms per batch: drain ICMP + let this batch's ARP resolve */
#define NT_SWEEP_COLLECT  900         /* ms to collect replies after the last request          */
#define NT_PING_TIMEOUT   700         /* ms per targeted-ping echo                             */
#define NT_SCAN_POOL      5           /* concurrent non-blocking connects (socket budget)      */
#define NT_SCAN_CTIMEOUT  700         /* ms per connect attempt                                */
#define NT_BANNER_TIMEOUT 500         /* ms to wait for a banner                               */
#define NT_NBNS_TIMEOUT   300         /* ms to wait for a NBNS node-status reply               */

/* ---- published state (mutex-guarded) ---- */
static SemaphoreHandle_t s_mtx;
static TaskHandle_t      s_task;
static QueueHandle_t     s_q;
static volatile bool     s_busy;
static volatile bool     s_cancel;
static volatile int      s_progress;                 /* 0..100 for the running job */
static nocsif_nt_state_t s_state = NT_IDLE;
static char              s_status[80] = "idle";
static char              s_log_status[40] = "";
static char              s_log_path[96] = "";
static uint32_t          s_gen;
static bool              s_last_was_scan;

static nocsif_nt_host_t *s_hosts;                    /* PSRAM: NOCSIF_NT_HOST_MAX */
static int               s_host_cnt;
static nocsif_nt_port_t *s_ports;                    /* PSRAM: NOCSIF_NT_PORT_MAX */
static int               s_port_cnt;
static uint32_t          s_scan_target;
static nocsif_nt_fp_t    s_fp;                        /* last deep-dive fingerprint      */
static bool              s_have_fp;
static char            (*s_dns)[NOCSIF_NT_DNS_REC];   /* PSRAM: NOCSIF_NT_DNS_MAX record lines */
static int               s_dns_cnt;
static nocsif_nt_hop_t   s_hops[NOCSIF_NT_HOP_MAX];   /* traceroute hops                 */
static int               s_hop_cnt;
static char            (*s_disc)[NOCSIF_NT_DISC_REC]; /* PSRAM: service-discovery lines  */
static int               s_disc_cnt;
static char            (*s_rep)[NOCSIF_NT_REP_REC];   /* PSRAM: HTTP-recon / crafter report lines */
static int               s_rep_cnt;

/* netcat session (own task, own state). */
static TaskHandle_t          s_nc_task;
static volatile bool         s_nc_stop;
static volatile nocsif_nt_nc_state_t s_nc_state;
static uint32_t              s_nc_ip;
static uint16_t              s_nc_port;
static bool                  s_nc_udp;
static char                  s_nc_status[48];
static char                 *s_nc_rx;               /* PSRAM transcript ring */
static int                   s_nc_rx_len;
static uint32_t              s_nc_rx_gen;
static char                  s_nc_tx[540];          /* pending line to send */
static volatile int          s_nc_tx_len;           /* >0 = a line is pending */

#define NT_LOCK()   xSemaphoreTake(s_mtx, portMAX_DELAY)
#define NT_UNLOCK() xSemaphoreGive(s_mtx)

static void set_status(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    if (s_mtx) NT_LOCK();
    vsnprintf(s_status, sizeof s_status, fmt, ap);   /* single LVGL-task caller before init */
    if (s_mtx) NT_UNLOCK();
    va_end(ap);
}

/* ================================ small helpers ==================================== */

bool nocsif_nettools_parse_ip(const char *s, uint32_t *out)
{
    if (!s || !out) return false;
    unsigned a, b, c, d; char extra;
    if (sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra) != 4) return false;
    if (a > 255 || b > 255 || c > 255 || d > 255) return false;
    *out = (a << 24) | (b << 16) | (c << 8) | d;
    return true;
}

void nocsif_nettools_ip_str(uint32_t ip, char *out, size_t len)
{
    if (!out || !len) return;
    snprintf(out, len, "%u.%u.%u.%u",
             (unsigned)((ip >> 24) & 0xFF), (unsigned)((ip >> 16) & 0xFF),
             (unsigned)((ip >> 8) & 0xFF), (unsigned)(ip & 0xFF));
}

/* A compact, best-effort OUI->vendor table (24-bit prefix). A subset of common home/enterprise
 * vendors — enough to label most LAN devices; "" for anything unlisted. */
typedef struct { uint32_t oui; const char *name; } oui_t;
static const oui_t k_oui[] = {
    { 0x001C14, "VMware" },   { 0x005056, "VMware" },   { 0x000C29, "VMware" },
    { 0x080027, "VirtualBox" },
    { 0xB827EB, "Raspberry Pi" }, { 0xDCA632, "Raspberry Pi" }, { 0xE45F01, "Raspberry Pi" },
    { 0x2462AB, "Espressif" }, { 0x246F28, "Espressif" }, { 0x3C6105, "Espressif" },
    { 0x7CDFA1, "Espressif" }, { 0x84F703, "Espressif" }, { 0xA0764E, "Espressif" },
    { 0xF008D1, "Espressif" },
    { 0x001A11, "Google" },   { 0x3C5AB4, "Google" },   { 0xF4F5D8, "Google" },
    { 0x001451, "Apple" },    { 0x0C74C2, "Apple" },    { 0x3C0754, "Apple" },
    { 0xA4C361, "Apple" },    { 0xF0DBF8, "Apple" },    { 0xACBC32, "Apple" },
    { 0x0017C8, "Samsung" },  { 0x5CF8A1, "Samsung" },  { 0x8425DB, "Samsung" },
    { 0x00248C, "Asustek" },  { 0x2C56DC, "Asustek" },
    { 0x00095B, "Netgear" },  { 0x9C3DCF, "Netgear" },  { 0x2CB05D, "Netgear" },
    { 0x000FB5, "Netgear" },
    { 0x001018, "Broadcom" },
    { 0xC0C1C0, "Cisco" },    { 0x00000C, "Cisco" },    { 0x0025BC, "Cisco/Apple" },
    { 0x001DD8, "Microsoft" },{ 0x0017FA, "Microsoft" },{ 0x7C1E52, "Microsoft" },
    { 0x001167, "TP-Link" },  { 0x50C7BF, "TP-Link" },  { 0x6466B3, "TP-Link" },
    { 0xEC086B, "TP-Link" },
    { 0x0022F4, "Ubiquiti" }, { 0x24A43C, "Ubiquiti" }, { 0x788A20, "Ubiquiti" },
    { 0xFCECDA, "Ubiquiti" },
    { 0x0018E7, "Amazon" },   { 0x44650D, "Amazon" },   { 0xFC65DE, "Amazon" },
    { 0x001788, "Philips Hue" },
    { 0xB0BE76, "TP-Link" },  { 0x1C3BF3, "Amazon" },
    { 0x001132, "Synology" }, { 0x0011D8, "Asustek" },
    { 0x0004F2, "Polycom" },  { 0x000E58, "Sonos" },    { 0x347E5C, "Sonos" },
    { 0x000D4B, "Roku" },     { 0xB83E59, "GE" },       { 0x18B430, "Nest" },
};

const char *nocsif_nettools_oui_vendor(const uint8_t mac[6])
{
    if (!mac) return "";
    uint32_t oui = ((uint32_t)mac[0] << 16) | ((uint32_t)mac[1] << 8) | mac[2];
    for (size_t i = 0; i < sizeof k_oui / sizeof k_oui[0]; i++) {
        if (k_oui[i].oui == oui) return k_oui[i].name;
    }
    /* Locally-administered address (bit 1 of the first octet)? Almost always a randomized/private MAC. */
    if (mac[0] & 0x02) return "private";
    return "";
}

/* Port -> likely-service name (a compact well-known + registered subset). "" if not in the table. */
typedef struct { uint16_t port; const char *svc; } svc_t;
static const svc_t k_svc[] = {
    {20,"ftp-data"},{21,"ftp"},{22,"ssh"},{23,"telnet"},{25,"smtp"},{37,"time"},{53,"dns"},
    {67,"dhcp"},{69,"tftp"},{79,"finger"},{80,"http"},{88,"kerberos"},{110,"pop3"},{111,"rpcbind"},
    {113,"ident"},{119,"nntp"},{123,"ntp"},{135,"msrpc"},{137,"netbios-ns"},{139,"netbios-ssn"},
    {143,"imap"},{161,"snmp"},{179,"bgp"},{389,"ldap"},{427,"slp"},{443,"https"},{445,"smb"},
    {465,"smtps"},{500,"isakmp"},{515,"printer"},{548,"afp"},{554,"rtsp"},{587,"submission"},
    {623,"ipmi"},{631,"ipp"},{636,"ldaps"},{873,"rsync"},{902,"vmware"},{989,"ftps-data"},
    {990,"ftps"},{993,"imaps"},{995,"pop3s"},{1080,"socks"},{1194,"openvpn"},{1433,"mssql"},
    {1521,"oracle"},{1723,"pptp"},{1883,"mqtt"},{1900,"ssdp"},{2049,"nfs"},{2082,"cpanel"},
    {2181,"zookeeper"},{2375,"docker"},{2376,"docker-tls"},{3000,"dev/grafana"},{3128,"squid"},
    {3306,"mysql"},{3389,"rdp"},{3690,"svn"},{4443,"https-alt"},{4567,"tram"},{5000,"upnp/uPnP"},
    {5060,"sip"},{5222,"xmpp"},{5353,"mdns"},{5432,"postgres"},{5555,"adb"},{5601,"kibana"},
    {5672,"amqp"},{5900,"vnc"},{5985,"winrm"},{5986,"winrm-tls"},{6379,"redis"},{6443,"kube-api"},
    {7001,"weblogic"},{8000,"http-alt"},{8006,"proxmox"},{8008,"http-alt"},{8009,"ajp"},
    {8080,"http-proxy"},{8081,"http-alt"},{8086,"influxdb"},{8088,"http-alt"},{8123,"homeassist"},
    {8443,"https-alt"},{8883,"mqtt-tls"},{8888,"http-alt"},{9000,"http-alt"},{9042,"cassandra"},
    {9090,"prometheus"},{9100,"jetdirect"},{9200,"elasticsearch"},{9300,"elastic-node"},
    {10000,"webmin"},{11211,"memcached"},{27017,"mongodb"},{32400,"plex"},{49152,"upnp"},
    {51820,"wireguard"},{62078,"apple-sync"},
};
static const char *nt_svc_name(uint16_t port)
{
    for (size_t i = 0; i < sizeof k_svc / sizeof k_svc[0]; i++)
        if (k_svc[i].port == port) return k_svc[i].svc;
    return "";
}

/* The built-in "top ports" set a deep-dive scans (curated common services). */
static const uint16_t k_deep_ports[] = {
    21,22,23,25,53,80,110,135,139,143,161,443,445,515,554,631,993,995,1723,1883,3306,3389,
    5000,5060,5353,5900,5985,8000,8080,8443,8888,9100,32400,49152,62078,
};

/* Standard 16-bit ones-complement checksum. */
static uint16_t nt_cksum(const void *data, int len)
{
    const uint8_t *p = data;
    uint32_t sum = 0;
    while (len > 1) { sum += (uint16_t)((p[0] << 8) | p[1]); p += 2; len -= 2; }
    if (len == 1) sum += (uint16_t)(p[0] << 8);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)(~sum & 0xFFFF);
}

/* Read the STA link's IPv4 config (host-order). false if not connected / no netif. */
bool nocsif_nettools_link(uint32_t *ip, uint32_t *mask, uint32_t *gw)
{
    if (!nocsif_wifi_connected()) return false;
    esp_netif_t *nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!nif) return false;
    esp_netif_ip_info_t info;
    if (esp_netif_get_ip_info(nif, &info) != ESP_OK) return false;
    if (info.ip.addr == 0) return false;
    if (ip)   *ip   = ntohl(info.ip.addr);
    if (mask) *mask = ntohl(info.netmask.addr);
    if (gw)   *gw   = ntohl(info.gw.addr);
    return true;
}

/* ================================ ICMP (raw socket) ================================ */

/* Open a non-blocking raw ICMP socket, or -1. */
static int icmp_open(void)
{
    int s = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (s < 0) return -1;
    int fl = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, fl | O_NONBLOCK);
    return s;
}

/* Build an ICMP echo request into `buf` (returns its length). */
static int icmp_build(uint8_t *buf, uint16_t id, uint16_t seq)
{
    memset(buf, 0, 8 + NT_ICMP_PAYLOAD);
    buf[0] = 8;                       /* type: echo request */
    buf[1] = 0;                       /* code               */
    buf[4] = id >> 8;  buf[5] = id & 0xFF;
    buf[6] = seq >> 8; buf[7] = seq & 0xFF;
    for (int i = 0; i < NT_ICMP_PAYLOAD; i++) buf[8 + i] = (uint8_t)('a' + (i & 0x1F));
    uint16_t ck = nt_cksum(buf, 8 + NT_ICMP_PAYLOAD);
    buf[2] = ck >> 8;  buf[3] = ck & 0xFF;
    return 8 + NT_ICMP_PAYLOAD;
}

/* Parse a received raw-ICMP datagram; on an echo reply with matching id, return the source host-order
 * IP in *src and true. */
static bool icmp_parse_reply(const uint8_t *pkt, int n, uint16_t id, uint32_t *src)
{
    if (n < 20) return false;                 /* min IPv4 header */
    int ihl = (pkt[0] & 0x0F) * 4;
    if (ihl < 20 || n < ihl + 8) return false;
    const uint8_t *ic = pkt + ihl;
    if (ic[0] != 0) return false;             /* not an echo reply */
    uint16_t rid = (ic[4] << 8) | ic[5];
    if (rid != id) return false;
    *src = ((uint32_t)pkt[12] << 24) | ((uint32_t)pkt[13] << 16) |
           ((uint32_t)pkt[14] << 8) | pkt[15];
    return true;
}

/* One blocking-with-timeout echo to `ip`; returns RTT ms, or -1 on timeout/error. When it returns >=0
 * and `ttl_out` is non-NULL, *ttl_out gets the reply's observed IP TTL (for OS fingerprinting). */
static int icmp_ping_once(int s, uint32_t ip, uint16_t id, uint16_t seq, int timeout_ms, int *ttl_out)
{
    uint8_t out[8 + NT_ICMP_PAYLOAD];
    int len = icmp_build(out, id, seq);
    struct sockaddr_in dst = { 0 };
    dst.sin_family = AF_INET;
    dst.sin_addr.s_addr = htonl(ip);
    int64_t t0 = esp_timer_get_time();
    if (sendto(s, out, len, 0, (struct sockaddr *)&dst, sizeof dst) < 0) return -1;
    while (1) {
        int64_t left = timeout_ms * 1000 - (esp_timer_get_time() - t0);
        if (left <= 0) return -1;
        fd_set rf; FD_ZERO(&rf); FD_SET(s, &rf);
        struct timeval tv = { .tv_sec = left / 1000000, .tv_usec = left % 1000000 };
        if (select(s + 1, &rf, NULL, NULL, &tv) <= 0) return -1;
        uint8_t in[128]; uint32_t src = 0;
        int n = recv(s, in, sizeof in, 0);
        if (n <= 0) return -1;
        if (icmp_parse_reply(in, n, id, &src) && src == ip) {
            if (ttl_out && n >= 9) *ttl_out = in[8];   /* IP header TTL byte */
            return (int)((esp_timer_get_time() - t0) / 1000);
        }
    }
}

/* ================================ NBNS name query ================================== */

/* Query the NetBIOS node status of `ip` (host-order) and copy the first UNIQUE workstation name into
 * `out`. Best-effort — silence is normal on non-Windows hosts. */
static void nbns_name(uint32_t ip, char *out, size_t olen)
{
    out[0] = '\0';
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) return;
    struct timeval tv = { .tv_sec = 0, .tv_usec = NT_NBNS_TIMEOUT * 1000 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    /* Node-status request for the wildcard name "*": header + encoded "*" + NBSTAT(0x21)/IN(0x01). */
    static const char enc[] = "CKAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";  /* "*" first-level encoded */
    uint8_t q[50]; int p = 0;
    q[p++] = 0x00; q[p++] = 0x00;   /* transaction id */
    q[p++] = 0x00; q[p++] = 0x00;   /* flags          */
    q[p++] = 0x00; q[p++] = 0x01;   /* QDCOUNT = 1    */
    q[p++] = 0x00; q[p++] = 0x00;   /* ANCOUNT        */
    q[p++] = 0x00; q[p++] = 0x00;   /* NSCOUNT        */
    q[p++] = 0x00; q[p++] = 0x00;   /* ARCOUNT        */
    q[p++] = 0x20;                  /* name length 32 */
    memcpy(&q[p], enc, 32); p += 32;
    q[p++] = 0x00;                  /* name terminator */
    q[p++] = 0x00; q[p++] = 0x21;   /* QTYPE = NBSTAT  */
    q[p++] = 0x00; q[p++] = 0x01;   /* QCLASS = IN     */

    struct sockaddr_in dst = { 0 };
    dst.sin_family = AF_INET;
    dst.sin_port = htons(137);
    dst.sin_addr.s_addr = htonl(ip);
    if (sendto(s, q, p, 0, (struct sockaddr *)&dst, sizeof dst) < 0) { close(s); return; }

    uint8_t r[512];
    int n = recv(s, r, sizeof r, 0);
    close(s);
    if (n < 57) return;                       /* header(12)+name(34)+type/class/ttl/rdlen(10)+count(1) */

    /* Skip the header (12) + the echoed question name (1 len byte + 32 + 1 terminator = 34) + TYPE(2)
     * + CLASS(2) + TTL(4) + RDLENGTH(2), landing on the name count. */
    int off = 12 + 34 + 2 + 2 + 4 + 2;
    if (off >= n) return;
    int count = r[off++];
    for (int i = 0; i < count; i++) {
        if (off + 18 > n) break;
        const uint8_t *name = &r[off];        /* 15 chars + 1 suffix */
        uint8_t suffix = r[off + 15];
        uint16_t flags = (r[off + 16] << 8) | r[off + 17];
        off += 18;
        bool group = flags & 0x8000;
        if (!group && suffix == 0x00) {       /* UNIQUE workstation name */
            int w = 0;
            for (int j = 0; j < 15 && w + 1 < (int)olen; j++) {
                char c = (char)name[j];
                if (c == ' ' || c == '\0') break;
                out[w++] = (c >= 32 && c < 127) ? c : '?';
            }
            out[w] = '\0';
            if (w > 0) return;
        }
    }
}

/* ================================ ARP-cache enrichment ============================= */

static void host_add(uint32_t ip, int rtt_ms);   /* defined below; used by the ARP fold */

/* Fold the lwIP ARP table into the host list. The /24 sweep sends an echo to every address, and lwIP
 * ARP-resolves each on-link target before it can send — so by the end the ARP table holds every host
 * that answered ARP, INCLUDING the many LAN devices (Windows boxes, phones dozing in power-save, IoT)
 * that silently drop ICMP echo. So we ADD each ARP neighbour on our own /24 as an up-host (rtt -1 =
 * "up via ARP, no echo reply") and fill MAC + vendor on all. This is what takes a real home network
 * from "2 answered ping" to "every device that's actually on the wire". Needs CONFIG_LWIP_ARP_TABLE_SIZE
 * >= the host count (we set 64) so the .1->.254 walk doesn't LRU-evict earlier neighbours. Worker task. */
static void arp_scan_hosts(uint32_t net, uint32_t mask)
{
    uint32_t bcast = net | ~mask;
    /* etharp_get_entry() returns 0 for BOTH an empty slot and an out-of-range index, so we cannot
     * break on the first 0 — we iterate a fixed span well past the ARP table and skip empties. */
    for (size_t i = 0; i < 64; i++) {
        ip4_addr_t *ipa = NULL; struct netif *nif = NULL; struct eth_addr *eth = NULL;
        if (!etharp_get_entry(i, &ipa, &nif, &eth)) continue;   /* empty slot / past the table */
        if (!ipa || !eth) continue;
        uint32_t hip = ntohl(ipa->addr);
        if ((hip & mask) != net) continue;                      /* our STA /24 only (skip the SoftAP subnet etc.) */
        if (hip == net || hip == bcast) continue;               /* skip network + broadcast */
        host_add(hip, -1);                                      /* add if new (rtt -1 = ARP-only); dedups by IP, keeps a better RTT */
        NT_LOCK();
        for (int h = 0; h < s_host_cnt; h++) {
            if (s_hosts[h].ip == hip && !s_hosts[h].have_mac) {
                memcpy(s_hosts[h].mac, eth->addr, 6);
                s_hosts[h].have_mac = true;
                snprintf(s_hosts[h].vendor, sizeof s_hosts[h].vendor, "%s", nocsif_nettools_oui_vendor(eth->addr));
                break;
            }
        }
        NT_UNLOCK();
    }
}

/* ================================ jobs ============================================= */

typedef struct {
    uint8_t  kind;                       /* 1 sweep·2 scan·3 ping·4 log·5 deep-dive·6 dns·7 traceroute·8 discover·9 http·10 craft */
    uint32_t ip;                         /* target, or (dns) resolver (0 = system) */
    int      count;                      /* ping/craft count, or (dns) qtype */
    bool     banners;                    /* scan: grab banners; http: use TLS */
    int      nports;                     /* scan: port count; craft: mode (0 ICMP · 1 TCP · 2 UDP) */
    uint16_t ports[NOCSIF_NT_PORTS_IN];
    char     host[128];                  /* dns name / http host / craft UDP payload */
    uint16_t port;                       /* http / craft destination port */
    uint8_t  flags;                      /* craft: TCP flag byte, or ICMP type */
    uint32_t src_ip;                     /* craft: spoofed source IP (0 = auto / real link IP) */
} nt_job_t;

static void host_add(uint32_t ip, int rtt_ms)
{
    NT_LOCK();
    for (int i = 0; i < s_host_cnt; i++) {
        if (s_hosts[i].ip == ip) {                       /* already known — keep the best RTT */
            if (rtt_ms >= 0 && (s_hosts[i].rtt_ms < 0 || rtt_ms < s_hosts[i].rtt_ms))
                s_hosts[i].rtt_ms = (int16_t)rtt_ms;
            NT_UNLOCK();
            return;
        }
    }
    if (s_host_cnt < NOCSIF_NT_HOST_MAX) {
        nocsif_nt_host_t *h = &s_hosts[s_host_cnt++];
        memset(h, 0, sizeof *h);
        h->ip = ip;
        h->rtt_ms = (int16_t)(rtt_ms >= 0 ? rtt_ms : -1);
    }
    NT_UNLOCK();
}

static void job_sweep(void)
{
    uint32_t ip = 0, mask = 0, gw = 0;
    if (!nocsif_nettools_link(&ip, &mask, &gw)) {
        s_state = NT_ERR_NOLINK; set_status("no network link"); return;
    }
    NT_LOCK();
    s_host_cnt = 0; s_last_was_scan = false; s_gen++;
    NT_UNLOCK();

    uint32_t net   = ip & mask;
    uint32_t bcast = net | ~mask;
    uint32_t first = net + 1;
    uint32_t last  = (bcast > 0) ? bcast - 1 : net;
    uint32_t nhosts = (last >= first) ? (last - first + 1) : 0;
    if (nhosts > NT_SWEEP_MAX) nhosts = NT_SWEEP_MAX;

    s_state = NT_SWEEP_RUN; s_progress = 0;
    set_status("sweeping %u host(s)", (unsigned)nhosts);

    /* We already know we are up. Seed ourselves (RTT 0) and the gateway. */
    host_add(ip, 0);

    int s = icmp_open();
    if (s < 0) {                          /* raw ICMP unavailable — still list what ARP knows */
        set_status("icmp unavailable; arp only");
        arp_scan_hosts(net, mask);
        s_state = NT_DONE; s_progress = 100;
        NT_LOCK(); s_gen++; NT_UNLOCK();
        return;
    }
    uint16_t id = (uint16_t)(esp_timer_get_time() & 0xFFFF) | 1;
    int64_t *send_ts = calloc(nhosts ? nhosts : 1, sizeof(int64_t));

    /* Fire echo requests in batches, draining replies between batches. */
    uint32_t sent = 0;
    while (sent < nhosts && !s_cancel) {
        for (int b = 0; b < NT_SWEEP_BATCH && sent < nhosts; b++, sent++) {
            uint32_t target = first + sent;
            uint8_t out[8 + NT_ICMP_PAYLOAD];
            int len = icmp_build(out, id, (uint16_t)sent);
            struct sockaddr_in dst = { 0 };
            dst.sin_family = AF_INET;
            dst.sin_addr.s_addr = htonl(target);
            if (send_ts) send_ts[sent] = esp_timer_get_time();
            sendto(s, out, len, 0, (struct sockaddr *)&dst, sizeof dst);
        }
        s_progress = (int)(sent * 90 / (nhosts ? nhosts : 1));   /* send phase = 0..90% */
        /* Hold a window open between batches to drain ICMP replies AND let this batch's ARP settle.
         * lwIP resolves each on-link MAC (~40-100 ms on WiFi) before the echo can leave, and only about
         * ARP_TABLE_SIZE resolutions can be pending at once — so if we fire the next batch before this
         * one's ARP has resolved, later hosts' ARP requests get dropped and those devices are never
         * seen. Keep the window open for the whole span (don't break out on an idle select) so the
         * pending table drains to near-zero before the next volley. */
        int64_t drain_end = esp_timer_get_time() + NT_SWEEP_SETTLE * 1000;
        while (esp_timer_get_time() < drain_end && !s_cancel) {
            fd_set rf; FD_ZERO(&rf); FD_SET(s, &rf);
            struct timeval tv = { .tv_sec = 0, .tv_usec = 15 * 1000 };
            int r = select(s + 1, &rf, NULL, NULL, &tv);
            if (r < 0) break;
            if (r == 0) continue;                     /* no ICMP right now — keep waiting for ARP */
            uint8_t in[128]; uint32_t src = 0;
            int n = recv(s, in, sizeof in, 0);
            if (n <= 0) continue;
            if (icmp_parse_reply(in, n, id, &src)) {
                int rtt = -1;
                if (send_ts && src >= first && src < first + nhosts)
                    rtt = (int)((esp_timer_get_time() - send_ts[src - first]) / 1000);
                host_add(src, rtt);
            }
        }
        /* Fold the ARP table into our own host list every batch. Hosts resolve as we ping them, and
         * lwIP's ARP table (default 10 entries) LRU-evicts earlier neighbours as the .1->.254 walk
         * proceeds — capturing per-batch keeps every device even if the ARP_TABLE_SIZE bump doesn't
         * take, since s_hosts (64) is never evicted once a host lands in it. */
        arp_scan_hosts(net, mask);
    }

    /* Final collect window for stragglers. */
    int64_t end = esp_timer_get_time() + NT_SWEEP_COLLECT * 1000;
    while (esp_timer_get_time() < end && !s_cancel) {
        fd_set rf; FD_ZERO(&rf); FD_SET(s, &rf);
        struct timeval tv = { .tv_sec = 0, .tv_usec = 50 * 1000 };
        if (select(s + 1, &rf, NULL, NULL, &tv) <= 0) continue;
        uint8_t in[128]; uint32_t src = 0;
        int n = recv(s, in, sizeof in, 0);
        if (n <= 0) continue;
        if (icmp_parse_reply(in, n, id, &src)) {
            int rtt = -1;
            if (send_ts && src >= first && src < first + nhosts)
                rtt = (int)((esp_timer_get_time() - send_ts[src - first]) / 1000);
            host_add(src, rtt);
        }
    }
    free(send_ts);
    close(s);

    /* ARP settle: after the pings stop, keep folding the ARP table for a couple of seconds. Hosts that
     * were dozing in WiFi power-save answer ARP a beat late, and their entries land now. Poll rather
     * than one read so a late neighbour is captured the moment it resolves. */
    s_progress = 92;
    set_status("resolving hosts");
    int64_t arp_end = esp_timer_get_time() + 2200 * 1000;
    while (esp_timer_get_time() < arp_end && !s_cancel) {
        arp_scan_hosts(net, mask);
        vTaskDelay(pdMS_TO_TICKS(150));
    }

    s_progress = 95;
    set_status("resolving names");
    arp_scan_hosts(net, mask);
    /* NBNS name each up host (bounded by NOCSIF_NT_HOST_MAX + the short per-host timeout). */
    int hc; NT_LOCK(); hc = s_host_cnt; NT_UNLOCK();
    for (int i = 0; i < hc && !s_cancel; i++) {
        uint32_t hip; NT_LOCK(); hip = s_hosts[i].ip; NT_UNLOCK();
        char nm[NOCSIF_NT_NAME]; nbns_name(hip, nm, sizeof nm);
        if (nm[0]) { NT_LOCK(); snprintf(s_hosts[i].name, sizeof s_hosts[i].name, "%s", nm); NT_UNLOCK(); }
    }

    s_progress = 100;
    s_state = s_cancel ? NT_CANCELLED : NT_DONE;
    NT_LOCK(); s_gen++; NT_UNLOCK();
    set_status(s_cancel ? "cancelled — %d host(s)" : "%d host(s) up", s_host_cnt);
}

/* Try to elicit + capture a one-line banner from an already-connected socket. */
static void grab_banner(int fd, uint16_t port, char *out, size_t olen)
{
    out[0] = '\0';
    struct timeval tv = { .tv_sec = 0, .tv_usec = NT_BANNER_TIMEOUT * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    /* Nudge server-quiet protocols. */
    if (port == 80 || port == 8080 || port == 8000 || port == 8888 || port == 8081) {
        const char *req = "HEAD / HTTP/1.0\r\n\r\n";
        send(fd, req, strlen(req), 0);
    }
    char buf[128];
    int n = recv(fd, buf, sizeof buf - 1, 0);
    if (n <= 0) return;
    buf[n] = '\0';
    int w = 0;
    for (int i = 0; i < n && w + 1 < (int)olen; i++) {
        char c = buf[i];
        if (c == '\r' || c == '\n') break;        /* first line only */
        out[w++] = (c >= 32 && c < 127) ? c : '.';
    }
    out[w] = '\0';
}

static void port_add(uint16_t port, const char *banner)
{
    NT_LOCK();
    if (s_port_cnt < NOCSIF_NT_PORT_MAX) {
        nocsif_nt_port_t *p = &s_ports[s_port_cnt++];
        p->port = port;
        snprintf(p->service, sizeof p->service, "%s", nt_svc_name(port));
        snprintf(p->banner, sizeof p->banner, "%s", banner ? banner : "");
    }
    s_gen++;
    NT_UNLOCK();
}

/* True if the last scan found `port` open (used by the device-type heuristic). */
static bool scan_has_port(uint16_t port)
{
    for (int i = 0; i < s_port_cnt; i++) if (s_ports[i].port == port) return true;
    return false;
}

/* Connect-scan `ports` on `ip`, filling s_ports (reset first). Updates s_progress. The non-blocking
 * pool is bounded by NT_SCAN_POOL to stay within the lwIP socket budget. Shared by scan + deep-dive. */
static void scan_ports(uint32_t ip, const uint16_t *ports, int nports, bool banners)
{
    NT_LOCK(); s_port_cnt = 0; s_gen++; NT_UNLOCK();

    typedef struct { int fd; uint16_t port; int64_t deadline; bool used; } slot_t;
    slot_t pool[NT_SCAN_POOL];
    for (int i = 0; i < NT_SCAN_POOL; i++) pool[i].used = false;

    int next = 0, done = 0, live = 0;
    while ((next < nports || live > 0) && !s_cancel) {
        for (int i = 0; i < NT_SCAN_POOL && next < nports; i++) {
            if (pool[i].used) continue;
            int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (fd < 0) break;                    /* socket budget hit — reap first */
            int fl = fcntl(fd, F_GETFL, 0);
            fcntl(fd, F_SETFL, fl | O_NONBLOCK);
            struct sockaddr_in dst = { 0 };
            dst.sin_family = AF_INET;
            dst.sin_port = htons(ports[next]);
            dst.sin_addr.s_addr = htonl(ip);
            int rc = connect(fd, (struct sockaddr *)&dst, sizeof dst);
            if (rc == 0) {                        /* immediate connect (rare) */
                char b[NOCSIF_NT_BANNER] = "";
                if (banners) grab_banner(fd, ports[next], b, sizeof b);
                port_add(ports[next], b);
                close(fd); done++; next++;
                continue;
            }
            if (errno != EINPROGRESS) { close(fd); done++; next++; continue; }
            pool[i].fd = fd; pool[i].port = ports[next];
            pool[i].deadline = esp_timer_get_time() + NT_SCAN_CTIMEOUT * 1000;
            pool[i].used = true; live++; next++;
        }
        fd_set wf; FD_ZERO(&wf); int maxfd = -1;
        for (int i = 0; i < NT_SCAN_POOL; i++)
            if (pool[i].used) { FD_SET(pool[i].fd, &wf); if (pool[i].fd > maxfd) maxfd = pool[i].fd; }
        if (maxfd >= 0) {
            struct timeval tv = { .tv_sec = 0, .tv_usec = 60 * 1000 };
            select(maxfd + 1, NULL, &wf, NULL, &tv);
        }
        int64_t now = esp_timer_get_time();
        for (int i = 0; i < NT_SCAN_POOL; i++) {
            if (!pool[i].used) continue;
            bool settled = FD_ISSET(pool[i].fd, &wf);
            bool expired = now >= pool[i].deadline;
            if (!settled && !expired) continue;
            if (settled) {
                int err = 0; socklen_t el = sizeof err;
                getsockopt(pool[i].fd, SOL_SOCKET, SO_ERROR, &err, &el);
                if (err == 0) {                    /* port open */
                    char b[NOCSIF_NT_BANNER] = "";
                    if (banners) grab_banner(pool[i].fd, pool[i].port, b, sizeof b);
                    port_add(pool[i].port, b);
                }
            }
            close(pool[i].fd);
            pool[i].used = false; live--; done++;
        }
        s_progress = (int)(done * 100 / (nports ? nports : 1));
    }
    for (int i = 0; i < NT_SCAN_POOL; i++) if (pool[i].used) close(pool[i].fd);
}

static void job_scan(const nt_job_t *j)
{
    if (!nocsif_wifi_connected()) { s_state = NT_ERR_NOLINK; set_status("no network link"); return; }
    NT_LOCK(); s_scan_target = j->ip; s_last_was_scan = true; s_have_fp = false; s_gen++; NT_UNLOCK();
    char ips[16]; nocsif_nettools_ip_str(j->ip, ips, sizeof ips);
    s_state = NT_SCAN_RUN; s_progress = 0;
    set_status("scanning %s", ips);
    scan_ports(j->ip, j->ports, j->nports, j->banners);
    s_progress = 100;
    s_state = s_cancel ? NT_CANCELLED : NT_DONE;
    NT_LOCK(); s_gen++; NT_UNLOCK();
    set_status(s_cancel ? "cancelled — %d open" : "%d port(s) open", s_port_cnt);
}

/* ---- device fingerprint (deep-dive) ---- */
static void os_from_ttl(int ttl, char *out, size_t len)
{
    if (ttl <= 0)       snprintf(out, len, "%s", "");
    else if (ttl > 128) snprintf(out, len, "network");     /* 255 initial (routers / printers) */
    else if (ttl > 64)  snprintf(out, len, "Windows");     /* 128 initial                      */
    else                snprintf(out, len, "Linux/Unix");  /* 64 initial (Linux/Android/iOS)   */
}

/* Best-effort device class from the open-port profile + the OUI vendor. */
static void devtype_guess(char *out, size_t len, const char *vendor)
{
    const char *v = vendor ? vendor : "";
    if (scan_has_port(9100) || scan_has_port(515) || scan_has_port(631)) { snprintf(out,len,"printer"); return; }
    if (scan_has_port(554)) { snprintf(out,len,"camera / DVR"); return; }
    if (scan_has_port(32400) || strstr(v,"Plex")) { snprintf(out,len,"media server"); return; }
    if (scan_has_port(2049) || strstr(v,"Synology") || (scan_has_port(445) && scan_has_port(5000))) { snprintf(out,len,"NAS"); return; }
    if (scan_has_port(62078) || strcmp(v,"Apple")==0) { snprintf(out,len,"Apple device"); return; }
    if (scan_has_port(3389) || scan_has_port(5985)) { snprintf(out,len,"Windows PC"); return; }
    if (scan_has_port(1883) || scan_has_port(8883)) { snprintf(out,len,"IoT / MQTT"); return; }
    if ((scan_has_port(80)||scan_has_port(443)) && scan_has_port(53) &&
        (strstr(v,"Ubiquiti")||strstr(v,"Netgear")||strstr(v,"TP-Link")||strstr(v,"Asustek")||strstr(v,"Cisco"))) {
        snprintf(out,len,"router / AP"); return;
    }
    if (scan_has_port(445) || scan_has_port(139)) { snprintf(out,len,"Windows / SMB"); return; }
    if (scan_has_port(22)) { snprintf(out,len,"Linux host"); return; }
    if (scan_has_port(80) || scan_has_port(443)) { snprintf(out,len,"web host"); return; }
    snprintf(out,len,"%s","");
}

static void job_deepdive(const nt_job_t *j)
{
    if (!nocsif_wifi_connected()) { s_state = NT_ERR_NOLINK; set_status("no network link"); return; }
    NT_LOCK(); s_scan_target = j->ip; s_last_was_scan = true; s_have_fp = false; s_gen++; NT_UNLOCK();
    char ips[16]; nocsif_nettools_ip_str(j->ip, ips, sizeof ips);
    s_state = NT_SCAN_RUN; s_progress = 0;
    set_status("probing %s", ips);

    nocsif_nt_fp_t fp; memset(&fp, 0, sizeof fp);
    fp.ip = j->ip; fp.ttl = -1; fp.rtt_ms = -1;

    /* 1) One echo for RTT + TTL (OS fingerprint). */
    int s = icmp_open();
    if (s >= 0) {
        uint16_t id = (uint16_t)(esp_timer_get_time() & 0xFFFF) | 1;
        int ttl = -1;
        int rtt = icmp_ping_once(s, j->ip, id, 1, NT_PING_TIMEOUT, &ttl);
        if (rtt >= 0) { fp.rtt_ms = (int16_t)rtt; fp.ttl = (int16_t)ttl; }
        close(s);
    }

    /* 2) Connect-scan the built-in top-ports set. */
    scan_ports(j->ip, k_deep_ports, (int)(sizeof k_deep_ports / sizeof k_deep_ports[0]), true);
    fp.open_ports = s_port_cnt;

    /* 3) Identity: ARP-cache MAC + OUI vendor + NBNS name for this one host. */
    for (size_t i = 0; i < 64; i++) {
        ip4_addr_t *ipa = NULL; struct netif *nif = NULL; struct eth_addr *eth = NULL;
        if (!etharp_get_entry(i, &ipa, &nif, &eth)) continue;
        if (ipa && eth && ntohl(ipa->addr) == j->ip) {
            memcpy(fp.mac, eth->addr, 6); fp.have_mac = true;
            snprintf(fp.vendor, sizeof fp.vendor, "%s", nocsif_nettools_oui_vendor(eth->addr));
            break;
        }
    }
    if (!s_cancel) nbns_name(j->ip, fp.name, sizeof fp.name);

    /* 4) Guesses. */
    os_from_ttl(fp.ttl, fp.os_guess, sizeof fp.os_guess);
    devtype_guess(fp.dev_type, sizeof fp.dev_type, fp.vendor);

    NT_LOCK(); s_fp = fp; s_have_fp = true; s_gen++; NT_UNLOCK();
    s_progress = 100;
    s_state = s_cancel ? NT_CANCELLED : NT_DONE;
    if (s_cancel) set_status("cancelled — %d open", s_port_cnt);
    else set_status("%s: %d open, %s", ips, s_port_cnt, fp.dev_type[0] ? fp.dev_type : "host");
}

static void job_ping(const nt_job_t *j)
{
    if (!nocsif_wifi_connected()) { s_state = NT_ERR_NOLINK; set_status("no network link"); return; }
    char ips[16]; nocsif_nettools_ip_str(j->ip, ips, sizeof ips);
    int cnt = j->count; if (cnt < 1) cnt = 1; if (cnt > 8) cnt = 8;
    s_state = NT_PING_RUN; s_progress = 0;
    set_status("pinging %s", ips);

    int s = icmp_open();
    if (s < 0) { s_state = NT_ERR; set_status("icmp unavailable"); return; }
    uint16_t id = (uint16_t)(esp_timer_get_time() & 0xFFFF) | 1;
    int ok = 0, best = 1 << 30, worst = 0, sum = 0;
    for (int i = 0; i < cnt && !s_cancel; i++) {
        int rtt = icmp_ping_once(s, j->ip, id, (uint16_t)i, NT_PING_TIMEOUT, NULL);
        if (rtt >= 0) { ok++; sum += rtt; if (rtt < best) best = rtt; if (rtt > worst) worst = rtt; }
        s_progress = (i + 1) * 100 / cnt;
        if (i + 1 < cnt) vTaskDelay(pdMS_TO_TICKS(200));
    }
    close(s);
    s_state = s_cancel ? NT_CANCELLED : NT_DONE;
    if (ok > 0) set_status("%s: %d/%d, %d/%d/%d ms", ips, ok, cnt, best, sum / ok, worst);
    else        set_status("%s: 0/%d — no reply", ips, cnt);
}

/* ================================ DNS ============================================= */

static void dns_add(const char *fmt, ...)
{
    if (!s_dns) return;
    NT_LOCK();
    if (s_dns_cnt < NOCSIF_NT_DNS_MAX) {
        va_list ap; va_start(ap, fmt);
        vsnprintf(s_dns[s_dns_cnt], NOCSIF_NT_DNS_REC, fmt, ap);
        va_end(ap);
        s_dns_cnt++;
    }
    s_gen++;
    NT_UNLOCK();
}

/* Encode "a.b.c" into DNS wire format at dst; returns bytes written (incl. the 0 terminator). */
static int dns_encode(uint8_t *dst, const char *name)
{
    int w = 0;
    const char *p = name;
    while (*p) {
        const char *dot = strchr(p, '.');
        int seg = dot ? (int)(dot - p) : (int)strlen(p);
        if (seg > 63) seg = 63;
        dst[w++] = (uint8_t)seg;
        memcpy(&dst[w], p, seg); w += seg;
        if (!dot) break;
        p = dot + 1;
    }
    dst[w++] = 0;
    return w;
}

/* Read a (possibly compressed) name at `off`; write the dotted form to `out`; return the offset just
 * past the name at THIS position (after the first pointer, or after the terminating 0). */
static int dns_read_name(const uint8_t *m, int mlen, int off, char *out, size_t olen)
{
    int w = 0, jumped = 0, ret = -1, safety = 0;
    while (off >= 0 && off < mlen && safety++ < 128) {
        uint8_t len = m[off];
        if ((len & 0xC0) == 0xC0) {
            if (off + 1 >= mlen) break;
            int ptr = ((len & 0x3F) << 8) | m[off + 1];
            if (!jumped) ret = off + 2;
            off = ptr; jumped = 1; continue;
        }
        if (len == 0) { if (!jumped) ret = off + 1; break; }
        off++;
        for (int i = 0; i < len && off < mlen; i++, off++)
            if (w + 2 < (int)olen) out[w++] = (m[off] >= 32 && m[off] < 127) ? (char)m[off] : '?';
        if (w + 1 < (int)olen) out[w++] = '.';
    }
    if (w > 0 && out[w - 1] == '.') w--;
    out[w] = '\0';
    return ret < 0 ? off : ret;
}

static void job_dns(const nt_job_t *j)
{
    if (!nocsif_wifi_connected()) { s_state = NT_ERR_NOLINK; set_status("no network link"); return; }
    NT_LOCK(); s_dns_cnt = 0; s_gen++; NT_UNLOCK();
    s_state = NT_SCAN_RUN; s_progress = 0;
    set_status("resolving %s", j->host);

    uint32_t server = j->ip;
    if (server == 0) {
        esp_netif_t *nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        esp_netif_dns_info_t di;
        if (nif && esp_netif_get_dns_info(nif, ESP_NETIF_DNS_MAIN, &di) == ESP_OK)
            server = ntohl(di.ip.u_addr.ip4.addr);
    }
    if (server == 0) { s_state = NT_ERR; set_status("no resolver"); return; }

    int qtype = j->count ? j->count : NT_DNS_A;

    /* PTR: accept a plain a.b.c.d and reverse it into <d.c.b.a>.in-addr.arpa. */
    char qname[160];
    uint32_t pip;
    if (qtype == NT_DNS_PTR && nocsif_nettools_parse_ip(j->host, &pip))
        snprintf(qname, sizeof qname, "%u.%u.%u.%u.in-addr.arpa",
                 (unsigned)(pip & 0xFF), (unsigned)((pip >> 8) & 0xFF),
                 (unsigned)((pip >> 16) & 0xFF), (unsigned)((pip >> 24) & 0xFF));
    else
        snprintf(qname, sizeof qname, "%s", j->host);

    uint8_t q[300]; int p = 0;
    uint16_t id = (uint16_t)(esp_timer_get_time() & 0xFFFF) | 1;
    q[p++] = id >> 8; q[p++] = id & 0xFF;
    q[p++] = 0x01; q[p++] = 0x00;     /* flags: RD */
    q[p++] = 0x00; q[p++] = 0x01;     /* QDCOUNT 1 */
    q[p++] = 0x00; q[p++] = 0x00;     /* AN */
    q[p++] = 0x00; q[p++] = 0x00;     /* NS */
    q[p++] = 0x00; q[p++] = 0x00;     /* AR */
    p += dns_encode(&q[p], qname);
    q[p++] = (qtype >> 8) & 0xFF; q[p++] = qtype & 0xFF;
    q[p++] = 0x00; q[p++] = 0x01;     /* IN */

    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) { s_state = NT_ERR; set_status("socket error"); return; }
    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    struct sockaddr_in dst = { 0 };
    dst.sin_family = AF_INET; dst.sin_port = htons(53); dst.sin_addr.s_addr = htonl(server);
    if (sendto(s, q, p, 0, (struct sockaddr *)&dst, sizeof dst) < 0) {
        close(s); s_state = NT_ERR; set_status("send failed"); return;
    }

    uint8_t r[600];
    int n = recv(s, r, sizeof r, 0);
    close(s);
    if (n < 12) { s_state = NT_DONE; set_status("no response"); NT_LOCK(); s_gen++; NT_UNLOCK(); return; }

    int ancount = (r[6] << 8) | r[7];
    int rcode = r[3] & 0x0F;
    if (rcode == 3) { s_state = NT_DONE; set_status("NXDOMAIN"); NT_LOCK(); s_gen++; NT_UNLOCK(); return; }
    if (rcode != 0) { s_state = NT_DONE; set_status("dns rcode %d", rcode); NT_LOCK(); s_gen++; NT_UNLOCK(); return; }

    int off = 12;
    char tmp[128];
    off = dns_read_name(r, n, off, tmp, sizeof tmp);
    off += 4;   /* QTYPE + QCLASS */

    for (int a = 0; a < ancount && off + 10 < n && !s_cancel; a++) {
        char nm[128];
        off = dns_read_name(r, n, off, nm, sizeof nm);
        if (off + 10 > n) break;
        int type  = (r[off] << 8) | r[off + 1];
        int rdlen = (r[off + 8] << 8) | r[off + 9];
        int rd = off + 10;
        off = rd + rdlen;
        if (rd + rdlen > n) break;
        char val[128];
        switch (type) {
        case NT_DNS_A:
            if (rdlen == 4) { snprintf(val, sizeof val, "%u.%u.%u.%u", r[rd], r[rd+1], r[rd+2], r[rd+3]); dns_add("A     %s", val); }
            break;
        case NT_DNS_AAAA:
            if (rdlen == 16) {
                int w = 0;
                for (int k = 0; k < 16; k += 2) w += snprintf(val + w, sizeof val - w, "%s%02x%02x", k ? ":" : "", r[rd+k], r[rd+k+1]);
                dns_add("AAAA  %s", val);
            }
            break;
        case NT_DNS_CNAME: dns_read_name(r, n, rd, val, sizeof val); dns_add("CNAME %s", val); break;
        case NT_DNS_NS:    dns_read_name(r, n, rd, val, sizeof val); dns_add("NS    %s", val); break;
        case NT_DNS_PTR:   dns_read_name(r, n, rd, val, sizeof val); dns_add("PTR   %s", val); break;
        case NT_DNS_MX:
            if (rdlen >= 3) { int pref = (r[rd] << 8) | r[rd+1]; dns_read_name(r, n, rd+2, val, sizeof val); dns_add("MX    %d %s", pref, val); }
            break;
        case NT_DNS_TXT:
            if (rdlen >= 1) {
                int l = r[rd]; if (l > (int)sizeof val - 1) l = sizeof val - 1;
                if (rd + 1 + l <= n) { memcpy(val, &r[rd+1], l); val[l] = '\0';
                    for (int k = 0; k < l; k++) if (val[k] < 32 || val[k] > 126) val[k] = '.';
                    dns_add("TXT   %s", val); }
            }
            break;
        case NT_DNS_SOA: {
            char m2[96]; int o2 = dns_read_name(r, n, rd, val, sizeof val); dns_read_name(r, n, o2, m2, sizeof m2);
            dns_add("SOA   %s %s", val, m2);
            break; }
        case NT_DNS_SRV:
            if (rdlen >= 7) {
                int pr = (r[rd] << 8) | r[rd+1], wt = (r[rd+2] << 8) | r[rd+3], po = (r[rd+4] << 8) | r[rd+5];
                dns_read_name(r, n, rd+6, val, sizeof val); dns_add("SRV   %d %d %d %s", pr, wt, po, val);
            }
            break;
        default:
            dns_add("type%d (%d B)", type, rdlen);
            break;
        }
    }

    s_progress = 100;
    s_state = s_cancel ? NT_CANCELLED : NT_DONE;
    NT_LOCK(); s_gen++; NT_UNLOCK();
    set_status("%d record(s)", s_dns_cnt);
}

/* ================================ traceroute ====================================== */

static void hop_add(uint8_t hop, uint32_t ip, int rtt, bool reached)
{
    NT_LOCK();
    if (s_hop_cnt < NOCSIF_NT_HOP_MAX) {
        nocsif_nt_hop_t *h = &s_hops[s_hop_cnt++];
        h->hop = hop; h->ip = ip; h->rtt_ms = (int16_t)rtt; h->reached = reached;
    }
    s_gen++;
    NT_UNLOCK();
}

static void job_trace(const nt_job_t *j)
{
    if (!nocsif_wifi_connected()) { s_state = NT_ERR_NOLINK; set_status("no network link"); return; }
    NT_LOCK(); s_hop_cnt = 0; s_gen++; NT_UNLOCK();
    char ips[16]; nocsif_nettools_ip_str(j->ip, ips, sizeof ips);
    s_state = NT_SCAN_RUN; s_progress = 0;
    set_status("tracing %s", ips);

    int s = icmp_open();
    if (s < 0) { s_state = NT_ERR; set_status("icmp unavailable"); return; }
    uint16_t id = (uint16_t)(esp_timer_get_time() & 0xFFFF) | 1;

    bool reached = false;
    for (int hop = 1; hop <= NOCSIF_NT_HOP_MAX && !reached && !s_cancel; hop++) {
        int ttl = hop;
        setsockopt(s, IPPROTO_IP, IP_TTL, &ttl, sizeof ttl);

        uint32_t hop_ip = 0; int hop_rtt = -1;
        for (int attempt = 0; attempt < 2 && hop_ip == 0 && !s_cancel; attempt++) {
            uint8_t out[8 + NT_ICMP_PAYLOAD];
            int len = icmp_build(out, id, (uint16_t)hop);
            struct sockaddr_in dst = { 0 };
            dst.sin_family = AF_INET; dst.sin_addr.s_addr = htonl(j->ip);
            int64_t t0 = esp_timer_get_time();
            sendto(s, out, len, 0, (struct sockaddr *)&dst, sizeof dst);
            while (!s_cancel) {
                int64_t left = 1200 * 1000 - (esp_timer_get_time() - t0);
                if (left <= 0) break;
                fd_set rf; FD_ZERO(&rf); FD_SET(s, &rf);
                struct timeval tv = { .tv_sec = left / 1000000, .tv_usec = left % 1000000 };
                if (select(s + 1, &rf, NULL, NULL, &tv) <= 0) break;
                uint8_t in[256]; int n = recv(s, in, sizeof in, 0);
                if (n < 20) continue;
                int ihl = (in[0] & 0x0F) * 4;
                if (ihl < 20 || n < ihl + 8) continue;
                uint8_t type = in[ihl];
                uint32_t src = ((uint32_t)in[12] << 24) | ((uint32_t)in[13] << 16) | ((uint32_t)in[14] << 8) | in[15];
                if (type == 11) {                    /* time-exceeded — an intermediate hop */
                    hop_ip = src; hop_rtt = (int)((esp_timer_get_time() - t0) / 1000); break;
                }
                if (type == 0 && src == j->ip) {     /* echo reply — reached the target */
                    hop_ip = src; hop_rtt = (int)((esp_timer_get_time() - t0) / 1000); reached = true; break;
                }
            }
        }
        hop_add((uint8_t)hop, hop_ip, hop_rtt, reached);
        s_progress = hop * 100 / NOCSIF_NT_HOP_MAX;
    }
    close(s);
    s_progress = 100;
    s_state = s_cancel ? NT_CANCELLED : NT_DONE;
    set_status(reached ? "reached in %d hop(s)" : "stopped at %d hop(s)", s_hop_cnt);
}

/* ================================ service discovery =============================== */

static void disc_add(const char *fmt, ...)
{
    if (!s_disc) return;
    NT_LOCK();
    if (s_disc_cnt < NOCSIF_NT_DISC_MAX) {
        va_list ap; va_start(ap, fmt);
        vsnprintf(s_disc[s_disc_cnt], NOCSIF_NT_DISC_REC, fmt, ap);
        va_end(ap);
        s_disc_cnt++;
    }
    s_gen++;
    NT_UNLOCK();
}

/* Copy the value of an SSDP/HTTP header line ("SERVER:", "ST:") into out (best-effort, case-insensitive). */
static void ssdp_header(const char *resp, const char *key, char *out, size_t olen)
{
    out[0] = '\0';
    const char *p = resp;
    size_t klen = strlen(key);
    while (*p) {
        if (strncasecmp(p, key, klen) == 0) {
            p += klen;
            while (*p == ' ' || *p == '\t') p++;
            int w = 0;
            while (*p && *p != '\r' && *p != '\n' && w + 1 < (int)olen)
                out[w++] = (*p >= 32 && *p < 127) ? *p : '.', p++;
            out[w] = '\0';
            return;
        }
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }
}

static void discover_ssdp(void)
{
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) return;
    struct timeval tv = { .tv_sec = 0, .tv_usec = 400 * 1000 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    static const char msearch[] =
        "M-SEARCH * HTTP/1.1\r\n"
        "HOST: 239.255.255.250:1900\r\n"
        "MAN: \"ssdp:discover\"\r\n"
        "MX: 2\r\n"
        "ST: ssdp:all\r\n\r\n";
    struct sockaddr_in dst = { 0 };
    dst.sin_family = AF_INET;
    dst.sin_port = htons(1900);
    dst.sin_addr.s_addr = htonl(0xEFFFFFFAu);   /* 239.255.255.250 */
    sendto(s, msearch, sizeof msearch - 1, 0, (struct sockaddr *)&dst, sizeof dst);

    uint32_t seen[NOCSIF_NT_DISC_MAX]; int nseen = 0;
    int64_t end = esp_timer_get_time() + 2500 * 1000;
    while (esp_timer_get_time() < end && !s_cancel) {
        char buf[512]; struct sockaddr_in from; socklen_t fl = sizeof from;
        int n = recvfrom(s, buf, sizeof buf - 1, 0, (struct sockaddr *)&from, &fl);
        if (n <= 0) continue;
        buf[n] = '\0';
        uint32_t src = ntohl(from.sin_addr.s_addr);
        bool dup = false;
        for (int i = 0; i < nseen; i++) if (seen[i] == src) { dup = true; break; }
        if (dup) continue;
        if (nseen < NOCSIF_NT_DISC_MAX) seen[nseen++] = src;
        char server[80]; ssdp_header(buf, "SERVER:", server, sizeof server);
        if (!server[0]) ssdp_header(buf, "ST:", server, sizeof server);
        char ips[16]; nocsif_nettools_ip_str(src, ips, sizeof ips);
        disc_add("UPnP  %s  %s", ips, server);
    }
    close(s);
}

static void discover_mdns(void)
{
    static const struct { const char *svc; const char *proto; } types[] = {
        {"_http","_tcp"}, {"_https","_tcp"}, {"_ssh","_tcp"}, {"_sftp-ssh","_tcp"},
        {"_smb","_tcp"}, {"_afpovertcp","_tcp"}, {"_ipp","_tcp"}, {"_printer","_tcp"},
        {"_pdl-datastream","_tcp"}, {"_airplay","_tcp"}, {"_raop","_tcp"}, {"_googlecast","_tcp"},
        {"_spotify-connect","_tcp"}, {"_hap","_tcp"}, {"_workstation","_tcp"}, {"_device-info","_tcp"},
    };
    esp_err_t e = mdns_init();
    bool i_own = (e == ESP_OK);   /* only free what we initialised (companion may own the responder) */

    for (size_t t = 0; t < sizeof types / sizeof types[0] && !s_cancel; t++) {
        mdns_result_t *res = NULL;
        if (mdns_query_ptr(types[t].svc, types[t].proto, 300, 6, &res) != ESP_OK || !res) continue;
        for (mdns_result_t *r = res; r; r = r->next) {
            char ipp[24]; uint32_t hip = 0;
            for (mdns_ip_addr_t *ad = r->addr; ad; ad = ad->next)
                if (ad->addr.type == ESP_IPADDR_TYPE_V4) { hip = ntohl(ad->addr.u_addr.ip4.addr); break; }
            if (hip) { char ips[16]; nocsif_nettools_ip_str(hip, ips, sizeof ips); snprintf(ipp, sizeof ipp, "%s:%u", ips, r->port); }
            else       snprintf(ipp, sizeof ipp, ":%u", r->port);
            disc_add("mDNS  %s  %s  %s", types[t].svc,
                     r->instance_name ? r->instance_name : (r->hostname ? r->hostname : "?"), ipp);
        }
        mdns_query_results_free(res);
    }
    if (i_own) mdns_free();
}

static void job_discover(void)
{
    if (!nocsif_wifi_connected()) { s_state = NT_ERR_NOLINK; set_status("no network link"); return; }
    NT_LOCK(); s_disc_cnt = 0; s_gen++; NT_UNLOCK();
    s_state = NT_SCAN_RUN; s_progress = 0;
    set_status("SSDP discover…");
    discover_ssdp();
    s_progress = 50;
    if (!s_cancel) { set_status("mDNS discover…"); discover_mdns(); }
    s_progress = 100;
    s_state = s_cancel ? NT_CANCELLED : NT_DONE;
    NT_LOCK(); s_gen++; NT_UNLOCK();
    set_status("%d service(s)", s_disc_cnt);
}

/* ================================ report lines (shared) =========================== */

static void rep_reset(void) { NT_LOCK(); s_rep_cnt = 0; s_gen++; NT_UNLOCK(); }
static void rep_add(const char *fmt, ...)
{
    if (!s_rep) return;
    NT_LOCK();
    if (s_rep_cnt < NOCSIF_NT_REP_MAX) {
        va_list ap; va_start(ap, fmt);
        vsnprintf(s_rep[s_rep_cnt], NOCSIF_NT_REP_REC, fmt, ap);
        va_end(ap);
        s_rep_cnt++;
    }
    s_gen++;
    NT_UNLOCK();
}

/* ================================ HTTP + TLS recon ================================ */

/* Add the status line + a curated set of response headers from a raw HTTP header block. */
static void http_report(const char *resp)
{
    char line[NOCSIF_NT_REP_REC]; int i = 0;
    while (resp[i] && resp[i] != '\r' && resp[i] != '\n' && i < (int)sizeof line - 1) { line[i] = resp[i]; i++; }
    line[i] = '\0';
    if (line[0]) rep_add("%s", line);
    static const char *keys[] = {
        "Server:", "X-Powered-By:", "Content-Type:", "Content-Length:", "Location:",
        "Allow:", "WWW-Authenticate:", "Strict-Transport-Security:", "X-Frame-Options:", "Set-Cookie:",
    };
    for (size_t k = 0; k < sizeof keys / sizeof keys[0]; k++) {
        char v[100]; ssdp_header(resp, keys[k], v, sizeof v);
        if (v[0]) rep_add("%s %s", keys[k], v);
    }
}

typedef int (*nt_recv_fn)(void *ctx, uint8_t *buf, int len);
static void http_read_headers(nt_recv_fn rd, void *ctx, char *out, int cap)
{
    int w = 0;
    int64_t end = esp_timer_get_time() + 4000 * 1000;
    while (w < cap - 1 && esp_timer_get_time() < end && !s_cancel) {
        uint8_t c;
        int n = rd(ctx, &c, 1);
        if (n <= 0) break;
        out[w++] = (char)c;
        if (w >= 4 && out[w-1] == '\n' && out[w-2] == '\r' && out[w-3] == '\n' && out[w-4] == '\r') break;
    }
    out[w] = '\0';
}
static int plain_recv(void *ctx, uint8_t *buf, int len) { return recv(*(int *)ctx, buf, len, 0); }
static int tls_recv(void *ctx, uint8_t *buf, int len)   { return mbedtls_ssl_read((mbedtls_ssl_context *)ctx, buf, len); }

static void http_plain(const char *host, uint16_t port)
{
    char ports[8]; snprintf(ports, sizeof ports, "%u", port);
    struct addrinfo hints; memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *ai = NULL;
    if (getaddrinfo(host, ports, &hints, &ai) != 0 || !ai) { rep_add("resolve failed"); return; }
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) { freeaddrinfo(ai); rep_add("socket error"); return; }
    struct timeval tv = { .tv_sec = 4, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    int rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
    freeaddrinfo(ai);
    if (rc != 0) { close(fd); rep_add("connect failed"); return; }

    char req[256];
    int n = snprintf(req, sizeof req, "HEAD / HTTP/1.1\r\nHost: %s\r\nUser-Agent: NocSif\r\nConnection: keep-alive\r\n\r\n", host);
    send(fd, req, n, 0);
    char buf[2048]; http_read_headers(plain_recv, &fd, buf, sizeof buf);
    http_report(buf);
    n = snprintf(req, sizeof req, "OPTIONS / HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", host);
    if (send(fd, req, n, 0) > 0) {
        char b2[1024]; http_read_headers(plain_recv, &fd, b2, sizeof b2);
        char allow[100]; ssdp_header(b2, "Allow:", allow, sizeof allow);
        if (allow[0]) rep_add("methods %s", allow);
    }
    close(fd);
}

static void http_tls(const char *host, uint16_t port)
{
    char ports[8]; snprintf(ports, sizeof ports, "%u", port);
    mbedtls_net_context net; mbedtls_ssl_context ssl; mbedtls_ssl_config conf;
    mbedtls_ctr_drbg_context ctr; mbedtls_entropy_context ent;
    mbedtls_net_init(&net); mbedtls_ssl_init(&ssl); mbedtls_ssl_config_init(&conf);
    mbedtls_ctr_drbg_init(&ctr); mbedtls_entropy_init(&ent);

    do {
        if (mbedtls_ctr_drbg_seed(&ctr, mbedtls_entropy_func, &ent, (const unsigned char *)"nocsif", 6) != 0) { rep_add("rng init failed"); break; }
        if (mbedtls_net_connect(&net, host, ports, MBEDTLS_NET_PROTO_TCP) != 0) { rep_add("connect failed"); break; }
        if (mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT) != 0) { rep_add("tls cfg failed"); break; }
        mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_NONE);   /* grab ANY cert, valid or not */
        mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &ctr);
        mbedtls_ssl_conf_read_timeout(&conf, 4000);
        if (mbedtls_ssl_setup(&ssl, &conf) != 0) { rep_add("tls setup failed"); break; }
        mbedtls_ssl_set_hostname(&ssl, host);
        mbedtls_ssl_set_bio(&ssl, &net, mbedtls_net_send, mbedtls_net_recv, mbedtls_net_recv_timeout);

        int hs;
        while ((hs = mbedtls_ssl_handshake(&ssl)) != 0)
            if (hs != MBEDTLS_ERR_SSL_WANT_READ && hs != MBEDTLS_ERR_SSL_WANT_WRITE) break;
        if (hs != 0) { rep_add("handshake failed (-0x%04x)", -hs); break; }

        rep_add("TLS %s", mbedtls_ssl_get_version(&ssl));
        const char *cs = mbedtls_ssl_get_ciphersuite(&ssl);
        if (cs) rep_add("cipher %s", cs);

        const mbedtls_x509_crt *crt = mbedtls_ssl_get_peer_cert(&ssl);
        if (crt) {
            char dn[160];
            if (mbedtls_x509_dn_gets(dn, sizeof dn, &crt->subject) > 0) rep_add("subject %s", dn);
            if (mbedtls_x509_dn_gets(dn, sizeof dn, &crt->issuer) > 0)  rep_add("issuer %s", dn);
            rep_add("valid %04d-%02d-%02d .. %04d-%02d-%02d",
                    crt->valid_from.year, crt->valid_from.mon, crt->valid_from.day,
                    crt->valid_to.year,   crt->valid_to.mon,   crt->valid_to.day);
            rep_add("key %s %d-bit", mbedtls_pk_get_name(&crt->pk), (int)mbedtls_pk_get_bitlen(&crt->pk));
            const mbedtls_x509_sequence *san = &crt->subject_alt_names;
            int shown = 0;
            for (; san && shown < 5; san = san->next) {
                if (san->buf.p && san->buf.len) {
                    char s[80]; int l = (int)san->buf.len; if (l > (int)sizeof s - 1) l = sizeof s - 1;
                    memcpy(s, san->buf.p, l); s[l] = '\0';
                    for (int i = 0; i < l; i++) if (s[i] < 32 || s[i] > 126) s[i] = '.';
                    rep_add("SAN %s", s); shown++;
                }
            }
        } else {
            rep_add("(no peer certificate)");
        }

        char req[256];
        int n = snprintf(req, sizeof req, "HEAD / HTTP/1.1\r\nHost: %s\r\nUser-Agent: NocSif\r\nConnection: close\r\n\r\n", host);
        mbedtls_ssl_write(&ssl, (const unsigned char *)req, n);
        char buf[2048]; http_read_headers(tls_recv, &ssl, buf, sizeof buf);
        http_report(buf);
        mbedtls_ssl_close_notify(&ssl);
    } while (0);

    mbedtls_ssl_free(&ssl); mbedtls_ssl_config_free(&conf);
    mbedtls_ctr_drbg_free(&ctr); mbedtls_entropy_free(&ent); mbedtls_net_free(&net);
}

static void job_http(const nt_job_t *j)
{
    if (!nocsif_wifi_connected()) { s_state = NT_ERR_NOLINK; set_status("no network link"); return; }
    rep_reset();
    s_state = NT_SCAN_RUN; s_progress = 0;
    set_status(j->banners ? "https %s:%u" : "http %s:%u", j->host, j->port);
    if (j->banners) http_tls(j->host, j->port);
    else            http_plain(j->host, j->port);
    s_progress = 100;
    s_state = s_cancel ? NT_CANCELLED : NT_DONE;
    NT_LOCK(); s_gen++; NT_UNLOCK();
    set_status("%d line(s)", s_rep_cnt);
}

/* ================================ packet crafter (hping-style) ==================== */

/* L4 checksum over the IPv4 pseudo-header + the L4 segment (all host-order args; proto 6 TCP / 17 UDP). */
static uint16_t l4_cksum(uint32_t src, uint32_t dst, uint8_t proto, const uint8_t *l4, int len)
{
    uint32_t sum = 0;
    sum += (src >> 16) & 0xFFFF; sum += src & 0xFFFF;
    sum += (dst >> 16) & 0xFFFF; sum += dst & 0xFFFF;
    sum += proto;
    sum += (uint32_t)len;
    for (int i = 0; i + 1 < len; i += 2) sum += (l4[i] << 8) | l4[i + 1];
    if (len & 1) sum += l4[len - 1] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)(~sum & 0xFFFF);
}
static uint16_t tcp_cksum(uint32_t src, uint32_t dst, const uint8_t *tcp, int len)
{
    return l4_cksum(src, dst, 6, tcp, len);
}

/* ---- source-IP spoofing (HDRINCL) — we build the whole IP header so its source can be ANY address.
 * lwIP's socket API has no IP_HDRINCL, and this build has no TCPIP core locking, so the raw-pcb send
 * runs on the tcpip thread via tcpip_callback. Fire-and-forget: replies go to the chosen source, not
 * us. Authorized testing on your own network. ---- */
/* Context handed to the tcpip thread; the callback OWNS it (frees the pbuf + itself), so the crafter
 * task never waits on it — no stack-lifetime race if the callback runs after we return. */
typedef struct { struct pbuf *p; uint32_t dst; uint8_t proto; } craft_tx_t;

static void craft_tx_cb(void *arg)
{
    craft_tx_t *t = (craft_tx_t *)arg;                 /* runs on the tcpip thread */
    struct raw_pcb *pcb = raw_new(t->proto);
    if (pcb) {
        raw_setflags(pcb, RAW_FLAGS_HDRINCL);
        ip_addr_t d; IP_ADDR4(&d, (t->dst >> 24) & 0xFF, (t->dst >> 16) & 0xFF, (t->dst >> 8) & 0xFF, t->dst & 0xFF);
        raw_sendto(pcb, t->p, &d);
        raw_remove(pcb);
    }
    pbuf_free(t->p);
    free(t);
}

/* Build an IPv4 header for (src -> dst, proto, l4[l4len]) and queue the whole frame to the raw HDRINCL
 * output on the tcpip thread. Fire-and-forget; returns true once queued. */
static bool craft_send_hdrincl(uint32_t src, uint32_t dst, uint8_t proto, const uint8_t *l4, int l4len)
{
    int total = 20 + l4len;
    struct pbuf *p = pbuf_alloc(PBUF_IP, total, PBUF_RAM);
    if (!p) return false;
    uint8_t *ip = (uint8_t *)p->payload;
    memset(ip, 0, 20);
    ip[0] = 0x45;                                  /* version 4, IHL 5 */
    ip[2] = (uint8_t)(total >> 8); ip[3] = (uint8_t)total;
    uint16_t ipid = (uint16_t)(esp_timer_get_time() & 0xFFFF);
    ip[4] = ipid >> 8; ip[5] = ipid & 0xFF;
    ip[8] = 64;                                    /* TTL */
    ip[9] = proto;
    ip[12] = src >> 24; ip[13] = src >> 16; ip[14] = src >> 8; ip[15] = src;
    ip[16] = dst >> 24; ip[17] = dst >> 16; ip[18] = dst >> 8; ip[19] = dst;
    uint16_t ick = nt_cksum(ip, 20); ip[10] = ick >> 8; ip[11] = ick & 0xFF;
    memcpy(ip + 20, l4, l4len);

    craft_tx_t *t = (craft_tx_t *)malloc(sizeof *t);
    if (!t) { pbuf_free(p); return false; }
    t->p = p; t->dst = dst; t->proto = proto;
    if (tcpip_callback(craft_tx_cb, t) != ERR_OK) { free(t); pbuf_free(p); return false; }
    return true;                                   /* queued (fire-and-forget) */
}

/* Craft `count` packets FROM a chosen (spoofed) source, one per mode. Fire-and-forget. */
static void craft_spoof(uint32_t src, uint32_t dst, int mode, uint16_t port, uint8_t flags, const char *payload, int count)
{
    char ss[16], ds[16];
    nocsif_nettools_ip_str(src, ss, sizeof ss);
    nocsif_nettools_ip_str(dst, ds, sizeof ds);
    for (int i = 0; i < count && !s_cancel; i++) {
        bool ok = false;
        if (mode == 1) {                            /* TCP */
            uint16_t sport = (uint16_t)(40000 + (esp_timer_get_time() + i * 131) % 20000);
            uint32_t seq = (uint32_t)esp_timer_get_time() + i * 7919u;
            uint8_t tcp[20]; memset(tcp, 0, sizeof tcp);
            tcp[0] = sport >> 8; tcp[1] = sport & 0xFF; tcp[2] = port >> 8; tcp[3] = port & 0xFF;
            tcp[4] = seq >> 24; tcp[5] = seq >> 16; tcp[6] = seq >> 8; tcp[7] = seq;
            tcp[12] = 0x50; tcp[13] = flags; tcp[14] = 0x20;
            uint16_t ck = l4_cksum(src, dst, 6, tcp, sizeof tcp); tcp[16] = ck >> 8; tcp[17] = ck & 0xFF;
            ok = craft_send_hdrincl(src, dst, 6, tcp, sizeof tcp);
        } else if (mode == 2) {                     /* UDP */
            const char *pl = (payload && payload[0]) ? payload : "nocsif\r\n";
            int pn = (int)strlen(pl);
            uint8_t udp[8 + 96]; if (pn > 96) pn = 96;
            int ulen = 8 + pn;
            udp[0] = 40000 >> 8; udp[1] = 40000 & 0xFF; udp[2] = port >> 8; udp[3] = port & 0xFF;
            udp[4] = ulen >> 8; udp[5] = ulen & 0xFF; udp[6] = 0; udp[7] = 0;
            memcpy(udp + 8, pl, pn);
            uint16_t ck = l4_cksum(src, dst, 17, udp, ulen); if (!ck) ck = 0xFFFF;
            udp[6] = ck >> 8; udp[7] = ck & 0xFF;
            ok = craft_send_hdrincl(src, dst, 17, udp, ulen);
        } else {                                    /* ICMP */
            uint8_t icmp[8 + NT_ICMP_PAYLOAD]; memset(icmp, 0, sizeof icmp);
            icmp[0] = flags ? flags : 8; icmp[1] = 0;
            icmp[4] = 0x13; icmp[5] = 0x37; icmp[7] = (uint8_t)i;
            for (int k = 0; k < NT_ICMP_PAYLOAD; k++) icmp[8 + k] = (uint8_t)('a' + (k & 31));
            uint16_t ck = nt_cksum(icmp, sizeof icmp); icmp[2] = ck >> 8; icmp[3] = ck & 0xFF;
            ok = craft_send_hdrincl(src, dst, 1, icmp, sizeof icmp);
        }
        rep_add(ok ? "#%d sent  %s -> %s" : "#%d send failed  %s -> %s", i + 1, ss, ds);
        if (i + 1 < count) vTaskDelay(pdMS_TO_TICKS(200));
    }
    rep_add("spoofed source %s - replies go there, not here", ss);
}

static void craft_icmp(uint32_t ip, uint8_t type, uint8_t code, int count)
{
    int s = icmp_open();
    if (s < 0) { rep_add("icmp unavailable"); return; }
    uint16_t id = (uint16_t)(esp_timer_get_time() & 0xFFFF) | 1;
    for (int i = 0; i < count && !s_cancel; i++) {
        uint8_t out[8 + NT_ICMP_PAYLOAD]; memset(out, 0, sizeof out);
        out[0] = type; out[1] = code;
        out[4] = id >> 8; out[5] = id & 0xFF; out[7] = (uint8_t)i;
        for (int k = 0; k < NT_ICMP_PAYLOAD; k++) out[8 + k] = (uint8_t)('a' + (k & 31));
        uint16_t ck = nt_cksum(out, sizeof out); out[2] = ck >> 8; out[3] = ck & 0xFF;
        struct sockaddr_in dst = { 0 }; dst.sin_family = AF_INET; dst.sin_addr.s_addr = htonl(ip);
        int64_t t0 = esp_timer_get_time();
        sendto(s, out, sizeof out, 0, (struct sockaddr *)&dst, sizeof dst);
        bool got = false;
        while (esp_timer_get_time() - t0 < 1000 * 1000 && !s_cancel) {
            fd_set rf; FD_ZERO(&rf); FD_SET(s, &rf);
            struct timeval tv = { .tv_sec = 0, .tv_usec = 100 * 1000 };
            if (select(s + 1, &rf, NULL, NULL, &tv) <= 0) break;
            uint8_t in[128]; int n = recv(s, in, sizeof in, 0);
            if (n < 20) continue;
            int ihl = (in[0] & 0x0F) * 4; if (n < ihl + 2) continue;
            uint32_t srcip = ((uint32_t)in[12] << 24) | ((uint32_t)in[13] << 16) | ((uint32_t)in[14] << 8) | in[15];
            if (srcip == ip) { rep_add("#%d reply type %d code %d  %d ms", i + 1, in[ihl], in[ihl + 1], (int)((esp_timer_get_time() - t0) / 1000)); got = true; break; }
        }
        if (!got) rep_add("#%d no reply", i + 1);
        if (i + 1 < count) vTaskDelay(pdMS_TO_TICKS(200));
    }
    close(s);
}

static void craft_tcp(uint32_t ip, uint16_t port, uint8_t flags, int count)
{
    uint32_t src_ip = 0; nocsif_nettools_link(&src_ip, NULL, NULL);
    int s = socket(AF_INET, SOCK_RAW, IPPROTO_TCP);
    if (s < 0) { rep_add("raw TCP unavailable on this build"); return; }
    int fl = fcntl(s, F_GETFL, 0); fcntl(s, F_SETFL, fl | O_NONBLOCK);
    for (int i = 0; i < count && !s_cancel; i++) {
        uint16_t sport = (uint16_t)(40000 + (esp_timer_get_time() + i * 131) % 20000);
        uint32_t seq = (uint32_t)esp_timer_get_time() + i * 7919u;
        uint8_t tcp[20]; memset(tcp, 0, sizeof tcp);
        tcp[0] = sport >> 8; tcp[1] = sport & 0xFF; tcp[2] = port >> 8; tcp[3] = port & 0xFF;
        tcp[4] = seq >> 24; tcp[5] = seq >> 16; tcp[6] = seq >> 8; tcp[7] = seq;
        tcp[12] = 0x50;              /* data offset 5 words */
        tcp[13] = flags;
        tcp[14] = 0x20; tcp[15] = 0x00; /* window 8192 */
        uint16_t ck = tcp_cksum(src_ip, ip, tcp, sizeof tcp); tcp[16] = ck >> 8; tcp[17] = ck & 0xFF;
        struct sockaddr_in dst = { 0 }; dst.sin_family = AF_INET; dst.sin_addr.s_addr = htonl(ip); dst.sin_port = htons(port);
        int64_t t0 = esp_timer_get_time();
        if (sendto(s, tcp, sizeof tcp, 0, (struct sockaddr *)&dst, sizeof dst) < 0) { rep_add("#%d send failed (raw TX blocked)", i + 1); break; }
        bool got = false;
        while (esp_timer_get_time() - t0 < 1200 * 1000 && !s_cancel) {
            fd_set rf; FD_ZERO(&rf); FD_SET(s, &rf);
            struct timeval tv = { .tv_sec = 0, .tv_usec = 100 * 1000 };
            if (select(s + 1, &rf, NULL, NULL, &tv) <= 0) break;
            uint8_t in[128]; int n = recv(s, in, sizeof in, 0);
            if (n < 20) continue;
            int ihl = (in[0] & 0x0F) * 4; if (n < ihl + 20) continue;
            uint32_t srcip = ((uint32_t)in[12] << 24) | ((uint32_t)in[13] << 16) | ((uint32_t)in[14] << 8) | in[15];
            const uint8_t *t = in + ihl;
            uint16_t dp = (t[2] << 8) | t[3];
            if (srcip != ip || dp != sport) continue;
            uint8_t rflags = t[13];
            int ms = (int)((esp_timer_get_time() - t0) / 1000);
            if (rflags & 0x04)             rep_add("#%d port %u  RST (closed)  %d ms", i + 1, port, ms);
            else if ((rflags & 0x12) == 0x12) rep_add("#%d port %u  SYN-ACK (open)  %d ms", i + 1, port, ms);
            else                           rep_add("#%d port %u  flags 0x%02x  %d ms", i + 1, port, rflags, ms);
            got = true; break;
        }
        if (!got) rep_add("#%d port %u  no response (filtered?)", i + 1, port);
        if (i + 1 < count) vTaskDelay(pdMS_TO_TICKS(200));
    }
    close(s);
}

static void craft_udp(uint32_t ip, uint16_t port, const char *payload, int count)
{
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) { rep_add("socket error"); return; }
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    struct sockaddr_in dst = { 0 }; dst.sin_family = AF_INET; dst.sin_addr.s_addr = htonl(ip); dst.sin_port = htons(port);
    const char *p = (payload && payload[0]) ? payload : "nocsif\r\n";
    for (int i = 0; i < count && !s_cancel; i++) {
        int64_t t0 = esp_timer_get_time();
        sendto(s, p, strlen(p), 0, (struct sockaddr *)&dst, sizeof dst);
        uint8_t in[256]; int n = recv(s, in, sizeof in, 0);
        if (n > 0) rep_add("#%d %d-byte reply  %d ms", i + 1, n, (int)((esp_timer_get_time() - t0) / 1000));
        else       rep_add("#%d no reply", i + 1);
        if (i + 1 < count) vTaskDelay(pdMS_TO_TICKS(200));
    }
    close(s);
}

static void job_craft(const nt_job_t *j)
{
    if (!nocsif_wifi_connected()) { s_state = NT_ERR_NOLINK; set_status("no network link"); return; }
    rep_reset();
    s_state = NT_SCAN_RUN; s_progress = 0;
    char ips[16]; nocsif_nettools_ip_str(j->ip, ips, sizeof ips);
    int count = j->count; if (count < 1) count = 1; if (count > 16) count = 16;
    int mode = j->nports;
    /* A spoofed source (custom src_ip that isn't our own link IP) goes out via HDRINCL — fire-and-forget,
     * since replies land on the spoofed source. Auto (src_ip 0) keeps the socket path + reply detection. */
    uint32_t link_ip = 0; nocsif_nettools_link(&link_ip, NULL, NULL);
    if (j->src_ip && j->src_ip != link_ip) {
        const char *mn = mode == 1 ? "TCP" : mode == 2 ? "UDP" : "ICMP";
        set_status("%s %s (spoofed src)", mn, ips);
        craft_spoof(j->src_ip, j->ip, mode, j->port, j->flags, j->host, count);
    }
    else if (mode == 1) { set_status("TCP %s:%u", ips, j->port); craft_tcp(j->ip, j->port, j->flags, count); }
    else if (mode == 2) { set_status("UDP %s:%u", ips, j->port); craft_udp(j->ip, j->port, j->host, count); }
    else                { set_status("ICMP %s", ips);            craft_icmp(j->ip, j->flags ? j->flags : 8, 0, count); }
    s_progress = 100;
    s_state = s_cancel ? NT_CANCELLED : NT_DONE;
    NT_LOCK(); s_gen++; NT_UNLOCK();
    set_status("%d line(s)", s_rep_cnt);
}

/* ================================ netcat (own session task) ======================= */

/* Append one char to the transcript ring (caller holds NT_LOCK). Drops the oldest 1 KB when full. */
static void nc_putc(char c)
{
    if (s_nc_rx_len >= NOCSIF_NT_NC_RX) {
        int drop = 1024;
        memmove(s_nc_rx, s_nc_rx + drop, s_nc_rx_len - drop);
        s_nc_rx_len -= drop;
    }
    s_nc_rx[s_nc_rx_len++] = c;
}

static void nc_append(const char *data, int len, bool sent)
{
    if (!s_nc_rx || len < 0) return;
    NT_LOCK();
    if (sent) { nc_putc('>'); nc_putc(' '); }
    for (int i = 0; i < len; i++) {
        char c = data[i];
        if (c != '\n' && c != '\t' && (c < 32 || c > 126)) c = '.';
        nc_putc(c);
    }
    if (sent) nc_putc('\n');
    s_nc_rx_gen++;
    NT_UNLOCK();
}

static void nc_task(void *arg)
{
    (void)arg;
    char ips[16]; nocsif_nettools_ip_str(s_nc_ip, ips, sizeof ips);
    s_nc_state = NC_CONNECTING;
    snprintf(s_nc_status, sizeof s_nc_status, "connecting %s:%u", ips, s_nc_port);

    int fd = socket(AF_INET, s_nc_udp ? SOCK_DGRAM : SOCK_STREAM, s_nc_udp ? IPPROTO_UDP : IPPROTO_TCP);
    if (fd < 0) { s_nc_state = NC_ERR; snprintf(s_nc_status, sizeof s_nc_status, "socket error"); s_nc_task = NULL; vTaskDeleteWithCaps(NULL); return; }
    struct sockaddr_in dst = { 0 };
    dst.sin_family = AF_INET; dst.sin_port = htons(s_nc_port); dst.sin_addr.s_addr = htonl(s_nc_ip);

    if (!s_nc_udp) {
        int fl = fcntl(fd, F_GETFL, 0); fcntl(fd, F_SETFL, fl | O_NONBLOCK);
        int rc = connect(fd, (struct sockaddr *)&dst, sizeof dst);
        if (rc != 0 && errno == EINPROGRESS) {
            fd_set wf; FD_ZERO(&wf); FD_SET(fd, &wf);
            struct timeval tv = { .tv_sec = 4, .tv_usec = 0 };
            if (select(fd + 1, NULL, &wf, NULL, &tv) > 0) {
                int err = 0; socklen_t el = sizeof err;
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
                rc = (err == 0) ? 0 : -1;
            } else rc = -1;
        }
        if (rc != 0) { close(fd); s_nc_state = NC_ERR; snprintf(s_nc_status, sizeof s_nc_status, "connect failed"); s_nc_task = NULL; vTaskDeleteWithCaps(NULL); return; }
    } else {
        connect(fd, (struct sockaddr *)&dst, sizeof dst);   /* set the default UDP peer */
        int fl = fcntl(fd, F_GETFL, 0); fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    }

    s_nc_state = NC_CONNECTED;
    snprintf(s_nc_status, sizeof s_nc_status, "connected %s:%u", ips, s_nc_port);

    while (!s_nc_stop) {
        char txbuf[540]; int txn = 0;
        NT_LOCK();
        if (s_nc_tx_len > 0) { txn = s_nc_tx_len; if (txn > (int)sizeof txbuf) txn = sizeof txbuf; memcpy(txbuf, s_nc_tx, txn); s_nc_tx_len = 0; }
        NT_UNLOCK();
        if (txn > 0) send(fd, txbuf, txn, 0);

        fd_set rf; FD_ZERO(&rf); FD_SET(fd, &rf);
        struct timeval tv = { .tv_sec = 0, .tv_usec = 150 * 1000 };
        int r = select(fd + 1, &rf, NULL, NULL, &tv);
        if (r > 0 && FD_ISSET(fd, &rf)) {
            char buf[512];
            int n = recv(fd, buf, sizeof buf, 0);
            if (n > 0) nc_append(buf, n, false);
            else if (n == 0 && !s_nc_udp) { s_nc_state = NC_CLOSED; snprintf(s_nc_status, sizeof s_nc_status, "peer closed"); break; }
            else if (n < 0 && errno != EWOULDBLOCK && errno != EAGAIN) break;
        }
    }
    close(fd);
    if (s_nc_state == NC_CONNECTED) { s_nc_state = NC_CLOSED; snprintf(s_nc_status, sizeof s_nc_status, "closed"); }
    s_nc_task = NULL;
    vTaskDeleteWithCaps(NULL);
}

static void job_log(void)
{
    int was_scan; NT_LOCK(); was_scan = s_last_was_scan; NT_UNLOCK();
    esp_err_t ce = nocsif_usb_gadget_claim_sd(2000);
    if (ce != ESP_OK) { snprintf(s_log_status, sizeof s_log_status, "%s",
                        ce == ESP_ERR_INVALID_STATE ? "busy (File Share)" : "no microSD card"); return; }
    if (!nocsif_sdcard_lock(3000)) { snprintf(s_log_status, sizeof s_log_status, "error"); return; }
    mkdir("/sd/nocsif", 0777);
    mkdir("/sd/nocsif/wifi", 0777);
    int idx = 0; char path[96];
    for (idx = 0; idx < 1000; idx++) {
        snprintf(path, sizeof path, "/sd/nocsif/wifi/nettools-%03d.txt", idx);
        struct stat st; if (stat(path, &st) != 0) break;
    }
    FILE *f = fopen(path, "w");
    if (!f) { nocsif_sdcard_unlock(); snprintf(s_log_status, sizeof s_log_status, "error"); return; }

    uint32_t ip = 0, mask = 0, gw = 0; nocsif_nettools_link(&ip, &mask, &gw);
    char a[16], b[16], c[16];
    nocsif_nettools_ip_str(ip, a, sizeof a); nocsif_nettools_ip_str(mask, b, sizeof b);
    nocsif_nettools_ip_str(gw, c, sizeof c);
    fprintf(f, "NocSif Network Tools\nlink %s  mask %s  gw %s\n\n", a, b, c);

    int written = 0;
    NT_LOCK();
    if (was_scan) {
        char t[16]; nocsif_nettools_ip_str(s_scan_target, t, sizeof t);
        fprintf(f, "TCP connect-scan of %s — %d open port(s)\n", t, s_port_cnt);
        if (s_have_fp)
            fprintf(f, "  device: %s  os: %s  vendor: %s  name: %s\n",
                    s_fp.dev_type[0] ? s_fp.dev_type : "?", s_fp.os_guess[0] ? s_fp.os_guess : "?",
                    s_fp.vendor[0] ? s_fp.vendor : "?", s_fp.name[0] ? s_fp.name : "?");
        for (int i = 0; i < s_port_cnt; i++) {
            fprintf(f, "  %-5u  %-12s  %s\n", s_ports[i].port, s_ports[i].service, s_ports[i].banner);
            written++;
        }
    } else {
        fprintf(f, "Host discovery — %d host(s) up\n", s_host_cnt);
        for (int i = 0; i < s_host_cnt; i++) {
            char hs[16]; nocsif_nettools_ip_str(s_hosts[i].ip, hs, sizeof hs);
            fprintf(f, "  %-15s  ", hs);
            if (s_hosts[i].rtt_ms >= 0) fprintf(f, "%3d ms  ", s_hosts[i].rtt_ms); else fprintf(f, "  arp   ");
            if (s_hosts[i].have_mac)
                fprintf(f, "%02x:%02x:%02x:%02x:%02x:%02x  ",
                        s_hosts[i].mac[0], s_hosts[i].mac[1], s_hosts[i].mac[2],
                        s_hosts[i].mac[3], s_hosts[i].mac[4], s_hosts[i].mac[5]);
            else fprintf(f, "%-17s  ", "");
            fprintf(f, "%-12s %s\n", s_hosts[i].vendor, s_hosts[i].name);
            written++;
        }
    }
    NT_UNLOCK();
    fflush(f); fclose(f);
    nocsif_sdcard_unlock();
    snprintf(s_log_path, sizeof s_log_path, "%s", path);
    snprintf(s_log_status, sizeof s_log_status, "saved %d", written);
    ESP_LOGI(TAG, "logged %d row(s) -> %s", written, path);
}

/* ================================ worker ========================================== */

static void nt_task(void *arg)
{
    (void)arg;
    nt_job_t j;
    while (1) {
        if (xQueueReceive(s_q, &j, portMAX_DELAY) != pdTRUE) continue;
        s_busy = true; s_cancel = false;
        switch (j.kind) {
            case 1: job_sweep();     break;
            case 2: job_scan(&j);    break;
            case 3: job_ping(&j);    break;
            case 4: job_log();       break;
            case 5: job_deepdive(&j); break;
            case 6: job_dns(&j);     break;
            case 7: job_trace(&j);   break;
            case 8: job_discover();  break;
            case 9: job_http(&j);    break;
            case 10: job_craft(&j);  break;
            default: break;
        }
        s_busy = false;
    }
}

/* ================================ public API ====================================== */

esp_err_t nocsif_nettools_init(void)
{
    if (s_task) return ESP_OK;
    if (nocsif_reliability_safe_mode()) return ESP_OK;   /* stay inert in safe mode */
    if (!s_mtx) s_mtx = xSemaphoreCreateMutex();
    if (!s_mtx) return ESP_ERR_NO_MEM;
    if (!s_hosts) s_hosts = heap_caps_calloc(NOCSIF_NT_HOST_MAX, sizeof *s_hosts, MALLOC_CAP_SPIRAM);
    if (!s_ports) s_ports = heap_caps_calloc(NOCSIF_NT_PORT_MAX, sizeof *s_ports, MALLOC_CAP_SPIRAM);
    if (!s_dns)   s_dns   = heap_caps_calloc(NOCSIF_NT_DNS_MAX, NOCSIF_NT_DNS_REC, MALLOC_CAP_SPIRAM);
    if (!s_disc)  s_disc  = heap_caps_calloc(NOCSIF_NT_DISC_MAX, NOCSIF_NT_DISC_REC, MALLOC_CAP_SPIRAM);
    if (!s_rep)   s_rep   = heap_caps_calloc(NOCSIF_NT_REP_MAX, NOCSIF_NT_REP_REC, MALLOC_CAP_SPIRAM);
    if (!s_hosts || !s_ports || !s_dns || !s_disc || !s_rep) return ESP_ERR_NO_MEM;
    if (!s_q) s_q = xQueueCreate(NT_QLEN, sizeof(nt_job_t));
    if (!s_q) return ESP_ERR_NO_MEM;
    if (xTaskCreateWithCaps(nt_task, "nettools", NT_STACK, NULL, 3, &s_task, MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "worker create failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "ready");
    return ESP_OK;
}

static bool post(const nt_job_t *j)
{
    if (nocsif_nettools_init() != ESP_OK || !s_q) return false;
    if (s_busy) { set_status("busy"); return false; }
    return xQueueSend(s_q, j, 0) == pdTRUE;
}

void nocsif_nettools_request_sweep(void)
{
    if (!nocsif_wifi_connected()) { s_state = NT_ERR_NOLINK; set_status("connect to a network first"); return; }
    nt_job_t j = { .kind = 1 };
    post(&j);
}

void nocsif_nettools_request_scan(uint32_t ip, const uint16_t *ports, int nports, bool banners)
{
    if (!nocsif_wifi_connected()) { s_state = NT_ERR_NOLINK; set_status("connect to a network first"); return; }
    if (!ports || nports <= 0) return;
    if (nports > NOCSIF_NT_PORTS_IN) nports = NOCSIF_NT_PORTS_IN;
    nt_job_t j = { .kind = 2, .ip = ip, .banners = banners, .nports = nports };
    memcpy(j.ports, ports, nports * sizeof(uint16_t));
    post(&j);
}

void nocsif_nettools_request_ping(uint32_t ip, int count)
{
    if (!nocsif_wifi_connected()) { s_state = NT_ERR_NOLINK; set_status("connect to a network first"); return; }
    nt_job_t j = { .kind = 3, .ip = ip, .count = count };
    post(&j);
}

void nocsif_nettools_request_deepdive(uint32_t ip)
{
    if (!nocsif_wifi_connected()) { s_state = NT_ERR_NOLINK; set_status("connect to a network first"); return; }
    nt_job_t j = { .kind = 5, .ip = ip };
    post(&j);
}

void nocsif_nettools_request_dns(const char *name, int qtype, uint32_t server)
{
    if (!nocsif_wifi_connected()) { s_state = NT_ERR_NOLINK; set_status("connect to a network first"); return; }
    if (!name || !name[0]) return;
    nt_job_t j = { .kind = 6, .ip = server, .count = qtype };
    snprintf(j.host, sizeof j.host, "%s", name);
    post(&j);
}

void nocsif_nettools_request_traceroute(uint32_t ip)
{
    if (!nocsif_wifi_connected()) { s_state = NT_ERR_NOLINK; set_status("connect to a network first"); return; }
    nt_job_t j = { .kind = 7, .ip = ip };
    post(&j);
}

void nocsif_nettools_request_discover(void)
{
    if (!nocsif_wifi_connected()) { s_state = NT_ERR_NOLINK; set_status("connect to a network first"); return; }
    nt_job_t j = { .kind = 8 };
    post(&j);
}

void nocsif_nettools_request_http(const char *host, uint16_t port, bool tls)
{
    if (!nocsif_wifi_connected()) { s_state = NT_ERR_NOLINK; set_status("connect to a network first"); return; }
    if (!host || !host[0]) return;
    nt_job_t j = { .kind = 9, .port = port, .banners = tls };
    snprintf(j.host, sizeof j.host, "%s", host);
    post(&j);
}

void nocsif_nettools_request_craft(uint32_t ip, uint32_t src_ip, int mode, uint16_t port, uint8_t tcp_flags, int count, const char *payload)
{
    if (!nocsif_wifi_connected()) { s_state = NT_ERR_NOLINK; set_status("connect to a network first"); return; }
    nt_job_t j = { .kind = 10, .ip = ip, .src_ip = src_ip, .nports = mode, .port = port, .flags = tcp_flags, .count = count };
    if (payload) snprintf(j.host, sizeof j.host, "%s", payload);
    post(&j);
}

/* ---- netcat session (own task) ---- */
void nocsif_nettools_nc_open(uint32_t ip, uint16_t port, bool udp)
{
    if (nocsif_nettools_init() != ESP_OK) return;
    if (!nocsif_wifi_connected()) { s_nc_state = NC_ERR; snprintf(s_nc_status, sizeof s_nc_status, "no network link"); return; }
    if (s_nc_task) return;                                  /* a session is already open */
    if (!s_nc_rx) { s_nc_rx = heap_caps_malloc(NOCSIF_NT_NC_RX, MALLOC_CAP_SPIRAM); if (!s_nc_rx) return; }
    NT_LOCK(); s_nc_rx_len = 0; s_nc_rx_gen++; NT_UNLOCK();
    s_nc_ip = ip; s_nc_port = port; s_nc_udp = udp;
    s_nc_stop = false; s_nc_tx_len = 0; s_nc_state = NC_CONNECTING;
    if (xTaskCreateWithCaps(nc_task, "nettools-nc", 5120, NULL, 3, &s_nc_task, MALLOC_CAP_SPIRAM) != pdPASS) {
        s_nc_state = NC_ERR; snprintf(s_nc_status, sizeof s_nc_status, "task error");
    }
}

void nocsif_nettools_nc_send(const char *text)
{
    if (!text || s_nc_state != NC_CONNECTED) return;
    char line[540];
    int n = snprintf(line, sizeof line, "%s\r\n", text);
    if (n < 0) return;
    if (n > (int)sizeof line) n = sizeof line;
    bool ok;
    NT_LOCK();
    ok = (s_nc_tx_len == 0);
    if (ok) { memcpy(s_nc_tx, line, n); s_nc_tx_len = n; }
    NT_UNLOCK();
    if (ok) nc_append(text, (int)strlen(text), true);
}

void nocsif_nettools_nc_close(void) { s_nc_stop = true; }

nocsif_nt_nc_state_t nocsif_nettools_nc_state(void) { return s_nc_state; }
const char *nocsif_nettools_nc_status(void) { return s_nc_status[0] ? s_nc_status : "idle"; }
uint32_t nocsif_nettools_nc_gen(void) { return s_nc_rx_gen; }

int nocsif_nettools_nc_rx(char *out, size_t len)
{
    if (!out || !len) return 0;
    out[0] = '\0';
    if (!s_nc_rx || !s_mtx) return 0;
    int copied;
    NT_LOCK();
    int start = 0, avail = s_nc_rx_len;
    if (avail > (int)len - 1) { start = avail - ((int)len - 1); avail = (int)len - 1; }
    memcpy(out, s_nc_rx + start, avail);
    out[avail] = '\0';
    copied = avail;
    NT_UNLOCK();
    return copied;
}

void nocsif_nettools_request_log(void)
{
    snprintf(s_log_status, sizeof s_log_status, "writing…");
    nt_job_t j = { .kind = 4 };
    if (!post(&j)) snprintf(s_log_status, sizeof s_log_status, "busy");
}

void nocsif_nettools_request_cancel(void) { s_cancel = true; }

bool               nocsif_nettools_busy(void)  { return s_busy; }
nocsif_nt_state_t  nocsif_nettools_state(void) { return s_state; }
int                nocsif_nettools_progress(void) { return s_progress; }
uint32_t           nocsif_nettools_gen(void)   { return s_gen; }

const char *nocsif_nettools_status_str(void)
{
    static char ret[80];
    if (!s_mtx) return s_status[0] ? s_status : "idle";
    NT_LOCK(); snprintf(ret, sizeof ret, "%s", s_status); NT_UNLOCK();
    return ret;
}

const char *nocsif_nettools_log_status_str(void) { return s_log_status; }
const char *nocsif_nettools_log_path(void)       { return s_log_path; }

int nocsif_nettools_host_count(void)
{
    if (!s_mtx) return 0;
    int n; NT_LOCK(); n = s_host_cnt; NT_UNLOCK(); return n;
}

bool nocsif_nettools_host_get(int idx, nocsif_nt_host_t *out)
{
    if (!s_mtx || !out) return false;
    bool ok = false;
    NT_LOCK();
    if (idx >= 0 && idx < s_host_cnt) { *out = s_hosts[idx]; ok = true; }
    NT_UNLOCK();
    return ok;
}

uint32_t nocsif_nettools_scan_target(void) { return s_scan_target; }

int nocsif_nettools_port_count(void)
{
    if (!s_mtx) return 0;
    int n; NT_LOCK(); n = s_port_cnt; NT_UNLOCK(); return n;
}

bool nocsif_nettools_port_get(int idx, nocsif_nt_port_t *out)
{
    if (!s_mtx || !out) return false;
    bool ok = false;
    NT_LOCK();
    if (idx >= 0 && idx < s_port_cnt) { *out = s_ports[idx]; ok = true; }
    NT_UNLOCK();
    return ok;
}

bool nocsif_nettools_fp(nocsif_nt_fp_t *out)
{
    if (!s_mtx || !out) return false;
    bool ok = false;
    NT_LOCK();
    if (s_have_fp) { *out = s_fp; ok = true; }
    NT_UNLOCK();
    return ok;
}

int nocsif_nettools_dns_count(void)
{
    if (!s_mtx) return 0;
    int n; NT_LOCK(); n = s_dns_cnt; NT_UNLOCK(); return n;
}

bool nocsif_nettools_dns_get(int idx, char *out, size_t len)
{
    if (!s_mtx || !out || !s_dns) return false;
    bool ok = false;
    NT_LOCK();
    if (idx >= 0 && idx < s_dns_cnt) { snprintf(out, len, "%s", s_dns[idx]); ok = true; }
    NT_UNLOCK();
    return ok;
}

int nocsif_nettools_hop_count(void)
{
    if (!s_mtx) return 0;
    int n; NT_LOCK(); n = s_hop_cnt; NT_UNLOCK(); return n;
}

bool nocsif_nettools_hop_get(int idx, nocsif_nt_hop_t *out)
{
    if (!s_mtx || !out) return false;
    bool ok = false;
    NT_LOCK();
    if (idx >= 0 && idx < s_hop_cnt) { *out = s_hops[idx]; ok = true; }
    NT_UNLOCK();
    return ok;
}

int nocsif_nettools_disc_count(void)
{
    if (!s_mtx) return 0;
    int n; NT_LOCK(); n = s_disc_cnt; NT_UNLOCK(); return n;
}

bool nocsif_nettools_disc_get(int idx, char *out, size_t len)
{
    if (!s_mtx || !out || !s_disc) return false;
    bool ok = false;
    NT_LOCK();
    if (idx >= 0 && idx < s_disc_cnt) { snprintf(out, len, "%s", s_disc[idx]); ok = true; }
    NT_UNLOCK();
    return ok;
}

int nocsif_nettools_rep_count(void)
{
    if (!s_mtx) return 0;
    int n; NT_LOCK(); n = s_rep_cnt; NT_UNLOCK(); return n;
}

bool nocsif_nettools_rep_get(int idx, char *out, size_t len)
{
    if (!s_mtx || !out || !s_rep) return false;
    bool ok = false;
    NT_LOCK();
    if (idx >= 0 && idx < s_rep_cnt) { snprintf(out, len, "%s", s_rep[idx]); ok = true; }
    NT_UNLOCK();
    return ok;
}
