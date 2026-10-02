/* nettools.h — on-LAN network tooling for the network the watch is joined to (WiFi sector #6).
 *
 * A small, self-contained companion to wifi.c: pure lwIP-socket tooling that operates on the LAN the
 * station link is on. It touches NO radio state (it neither scans nor transmits 802.11), so it does not
 * ride the wifi.c CMD queue; it runs its own single PSRAM-stacked worker and only READS the STA link's
 * IPv4 config (esp_netif) to know which /24 to work. Every capability requires a live STA link
 * (nocsif_wifi_connected()); with no link the requests are refused and the state reads NT_ERR_NOLINK.
 *
 * Capabilities (all authorized-testing, on the operator's own LAN):
 *   - Host discovery: an ICMP echo sweep of the local /24, then per-up-host enrichment — the ARP-cache
 *     MAC (lwIP etharp), an OUI->vendor guess, and a best-effort NetBIOS (NBNS) name.
 *   - TCP connect-scan + banner on a chosen host: a bounded non-blocking socket pool, short timeouts,
 *     the first banner bytes of each open port.
 *   - Targeted ICMP ping of one host.
 *   - The UI layers "nmap"/"masscan" PRESETS on top: curated port sets / a sweep, driving the SAME two
 *     primitives. They are NOT the real tools (the on-watch copy says so) — just familiar option names.
 *
 * All getters are snapshot copies, safe on the LVGL task; every request is non-blocking (posted to the
 * worker). Results live in PSRAM, guarded by a mutex, with a generation counter the UI polls for change.
 * The socket pool + result caps are bounded (RAM + the lwIP socket budget). Optional log to
 * /sd/nocsif/wifi/nettools-NNN.txt reuses the same SD claim/lock the PCAP path uses. */
#ifndef NOCSIF_NETTOOLS_H
#define NOCSIF_NETTOOLS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bounds (also the UI's array sizes). */
#define NOCSIF_NT_HOST_MAX   64    /* up-hosts remembered from a sweep                       */
#define NOCSIF_NT_PORT_MAX   64    /* open ports remembered from a scan                      */
#define NOCSIF_NT_PORTS_IN   128   /* max ports a single scan request may probe (preset cap) */
#define NOCSIF_NT_BANNER     64    /* banner bytes kept per open port (NUL-terminated)       */
#define NOCSIF_NT_NAME       32    /* NBNS host name buffer                                  */
#define NOCSIF_NT_VENDOR     24    /* OUI vendor buffer                                      */
#define NOCSIF_NT_SVC        16    /* likely-service name per open port                      */
#define NOCSIF_NT_DNS_MAX    24    /* records kept from a DNS query                          */
#define NOCSIF_NT_DNS_REC    96    /* one formatted DNS record line                          */
#define NOCSIF_NT_HOP_MAX    30    /* traceroute hops                                        */
#define NOCSIF_NT_DISC_MAX   48    /* service-discovery result lines                         */
#define NOCSIF_NT_DISC_REC   96    /* one formatted discovery line                           */
#define NOCSIF_NT_NC_RX      4096  /* netcat transcript ring (bytes)                         */
#define NOCSIF_NT_REP_MAX    48    /* HTTP-recon / packet-crafter report lines               */
#define NOCSIF_NT_REP_REC    128   /* one report line (headers can be long)                  */

/* Worker/job state (published; read on the LVGL task). */
typedef enum {
    NT_IDLE = 0,        /* no job has run yet / last job cleared         */
    NT_SWEEP_RUN,       /* ping-sweeping the local /24                   */
    NT_SCAN_RUN,        /* connect-scanning a host                       */
    NT_PING_RUN,        /* pinging a host                                */
    NT_DONE,            /* last job finished; results are ready          */
    NT_CANCELLED,       /* last job was cancelled                        */
    NT_ERR_NOLINK,      /* refused: no STA link                          */
    NT_ERR,             /* refused/failed: busy, socket error, etc.      */
} nocsif_nt_state_t;

/* One discovered host (a snapshot copy for the UI). */
typedef struct {
    uint32_t ip;                     /* host-order IPv4                                   */
    int16_t  rtt_ms;                 /* echo RTT in ms, -1 = up via ARP but no echo reply */
    uint8_t  mac[6];                 /* ARP-cache MAC (valid iff have_mac)                */
    bool     have_mac;
    char     name[NOCSIF_NT_NAME];   /* NBNS name, "" if none                             */
    char     vendor[NOCSIF_NT_VENDOR]; /* OUI vendor, "" if unknown                        */
} nocsif_nt_host_t;

/* One open port (a snapshot copy for the UI). */
typedef struct {
    uint16_t port;                       /* TCP port that accepted a connection            */
    char     service[NOCSIF_NT_SVC];     /* likely service from the built-in table ("" unknown) */
    char     banner[NOCSIF_NT_BANNER];   /* first printable banner bytes, "" if silent     */
} nocsif_nt_port_t;

/* Device fingerprint produced by a deep-dive scan (a snapshot copy). */
typedef struct {
    uint32_t ip;                         /* host-order IPv4                                 */
    uint8_t  mac[6];
    bool     have_mac;
    char     name[NOCSIF_NT_NAME];       /* NBNS / mDNS name, "" if none                    */
    char     vendor[NOCSIF_NT_VENDOR];   /* OUI vendor, "" if unknown                       */
    int16_t  ttl;                        /* observed IP TTL from the echo reply (-1 unknown)*/
    int16_t  rtt_ms;                     /* echo RTT ms, -1 if no reply                     */
    int      open_ports;                 /* count of open ports found                       */
    char     os_guess[16];               /* "Linux/Unix" / "Windows" / "network" / ""       */
    char     dev_type[20];               /* "router" / "printer" / "NAS" / "camera" / …      */
} nocsif_nt_fp_t;

/* One traceroute hop (a snapshot copy for the UI). */
typedef struct {
    uint8_t  hop;                        /* TTL / hop number (1-based)                      */
    uint32_t ip;                         /* responder host-order IPv4, 0 if no reply         */
    int16_t  rtt_ms;                     /* RTT ms, -1 if the hop timed out                 */
    bool     reached;                    /* this hop is the final target                     */
} nocsif_nt_hop_t;

/* DNS record TYPE values (a subset). */
enum {
    NT_DNS_A = 1, NT_DNS_NS = 2, NT_DNS_CNAME = 5, NT_DNS_SOA = 6,
    NT_DNS_PTR = 12, NT_DNS_MX = 15, NT_DNS_TXT = 16, NT_DNS_AAAA = 28, NT_DNS_SRV = 33,
};

/* Lazy init: create the worker + PSRAM result buffers on first use. Idempotent; a no-op in reliability
 * safe mode (returns ESP_OK but stays inert). Safe to call from the LVGL task. */
esp_err_t nocsif_nettools_init(void);

/* ---- job requests (non-blocking; posted to the worker; refused if !connected or already busy) ---- */

/* ICMP-sweep the local /24, then enrich each up host (ARP MAC + OUI vendor + NBNS name). */
void nocsif_nettools_request_sweep(void);

/* TCP connect-scan `ports` (up to NOCSIF_NT_PORTS_IN) on host `ip` (host-order). `banners` grabs the
 * first bytes of each open port. `ports` is copied into the worker; the caller keeps ownership. */
void nocsif_nettools_request_scan(uint32_t ip, const uint16_t *ports, int nports, bool banners);

/* ICMP-ping host `ip` (host-order) `count` times (clamped 1..8). Results in the status string. */
void nocsif_nettools_request_ping(uint32_t ip, int count);

/* Deep-dive one host: ping (for RTT/TTL), connect-scan the built-in top-ports set, name each open port
 * from the service table + grab banners, ARP/NBNS-enrich, and compute a device fingerprint. Results in
 * the port getters (with `service` filled) plus nocsif_nettools_fp(). */
void nocsif_nettools_request_deepdive(uint32_t ip);

/* DNS query: `name` (host or, for PTR, an IPv4 in a.b.c.d form — reversed for you), `qtype` a NT_DNS_*
 * TYPE, `server` the resolver host-order (0 = the STA link's own DNS). Records via the dns getters. */
void nocsif_nettools_request_dns(const char *name, int qtype, uint32_t server);

/* Traceroute to `ip` (host-order) via ICMP echo with increasing TTL. Hops via the hop getters. */
void nocsif_nettools_request_traceroute(uint32_t ip);

/* Service discovery: SSDP/UPnP M-SEARCH + an mDNS/DNS-SD browse of common service types. Results via
 * the disc getters. Runs on the job worker (kind 8). */
void nocsif_nettools_request_discover(void);

/* HTTP + TLS recon: request line/methods/headers, and for `tls` a zgrab-style certificate peek (subject,
 * issuer, validity, key, SANs, negotiated version + cipher — the cert is grabbed WITHOUT verification).
 * `host` may be a dotted IPv4 or a name (resolved). Results via the report getters (kind 9). */
void nocsif_nettools_request_http(const char *host, uint16_t port, bool tls);

/* Packet crafter (hping-style): send crafted probes to `ip` (host-order). mode 0 = ICMP · 1 = TCP ·
 * 2 = UDP. `port` is the TCP/UDP destination; `tcp_flags` a raw TCP flag byte (SYN=0x02, ACK=0x10, …);
 * `count` probes (clamped); `payload` optional text. Results via the report getters (kind 10). */
void nocsif_nettools_request_craft(uint32_t ip, uint32_t src_ip, int mode, uint16_t port, uint8_t tcp_flags, int count, const char *payload);

/* ---- netcat (raw TCP/UDP console) — its OWN session task, independent of the job worker ---- */
typedef enum {
    NC_IDLE = 0, NC_CONNECTING, NC_CONNECTED, NC_CLOSED, NC_ERR,
} nocsif_nt_nc_state_t;

/* Open a session to `ip`:`port` (host-order ip). `udp` picks UDP vs TCP. Refused if a session is already
 * open or there is no link. Non-blocking (spawns the session task). */
void nocsif_nettools_nc_open(uint32_t ip, uint16_t port, bool udp);

/* Queue `text` to send (a CRLF is appended); it is also echoed into the transcript prefixed "> ". */
void nocsif_nettools_nc_send(const char *text);

/* Close the session (the task drains + self-deletes). */
void nocsif_nettools_nc_close(void);

nocsif_nt_nc_state_t nocsif_nettools_nc_state(void);
const char *nocsif_nettools_nc_status(void);         /* one-line status                         */
int  nocsif_nettools_nc_rx(char *out, size_t len);   /* copy the transcript tail; returns length */
uint32_t nocsif_nettools_nc_gen(void);               /* bumps on new transcript data             */

/* Ask the running job to stop at the next bounded step. */
void nocsif_nettools_request_cancel(void);

/* Dump the current result set (sweep hosts or the last scan) to /sd/nocsif/wifi/nettools-NNN.txt. */
void nocsif_nettools_request_log(void);

/* ---- published state (LVGL-safe) ---- */
bool               nocsif_nettools_busy(void);
nocsif_nt_state_t  nocsif_nettools_state(void);
const char        *nocsif_nettools_status_str(void);  /* one-line status for the screen header */
uint32_t           nocsif_nettools_gen(void);          /* bumps whenever a result set changes   */
int                nocsif_nettools_progress(void);     /* 0..100 for the current job, else 0     */

/* Host-discovery results (from the last sweep). */
int  nocsif_nettools_host_count(void);
bool nocsif_nettools_host_get(int idx, nocsif_nt_host_t *out);

/* Port-scan results (from the last scan / deep-dive). `scan_target` is the host (host-order, 0 if none). */
uint32_t nocsif_nettools_scan_target(void);
int      nocsif_nettools_port_count(void);
bool     nocsif_nettools_port_get(int idx, nocsif_nt_port_t *out);

/* The last deep-dive's device fingerprint. false until a deep-dive has run. */
bool nocsif_nettools_fp(nocsif_nt_fp_t *out);

/* DNS query results (formatted record lines, newest query only). */
int  nocsif_nettools_dns_count(void);
bool nocsif_nettools_dns_get(int idx, char *out, size_t len);

/* Traceroute results (hops, in order). */
int  nocsif_nettools_hop_count(void);
bool nocsif_nettools_hop_get(int idx, nocsif_nt_hop_t *out);

/* Service-discovery results (formatted lines, newest browse only). */
int  nocsif_nettools_disc_count(void);
bool nocsif_nettools_disc_get(int idx, char *out, size_t len);

/* Report lines — shared by HTTP recon + the packet crafter (newest run only). */
int  nocsif_nettools_rep_count(void);
bool nocsif_nettools_rep_get(int idx, char *out, size_t len);

/* The local STA link the tools operate on (host-order). false if not connected. */
bool nocsif_nettools_link(uint32_t *ip, uint32_t *mask, uint32_t *gw);

/* Log status ("" / "writing…" / "saved N" / "no microSD card" / "busy" / "error"). */
const char *nocsif_nettools_log_status_str(void);
const char *nocsif_nettools_log_path(void);   /* "" until a log is written */

/* ---- small helpers (pure; safe anywhere) ---- */
bool nocsif_nettools_parse_ip(const char *s, uint32_t *out);  /* "a.b.c.d" -> host-order (false if bad) */
void nocsif_nettools_ip_str(uint32_t ip, char *out, size_t len);
const char *nocsif_nettools_oui_vendor(const uint8_t mac[6]); /* built-in OUI table, "" if unknown */

#ifdef __cplusplus
}
#endif
#endif /* NOCSIF_NETTOOLS_H */
