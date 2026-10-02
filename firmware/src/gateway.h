/*
 * NocSif — network gateway (travel-router subsystem). Owner's-own-device infrastructure.
 *
 * A self-contained worker that turns the watch into the same thing a consumer travel router
 * (GL.iNet / OpenWRT) does: it re-shares its own Wi-Fi STATION uplink to downstream clients that
 * join a device-hosted SoftAP, with four independently-toggleable treatments layered on the
 * forwarded traffic. Everything is file-driven from the microSD card; nothing here is hardcoded.
 *
 * Five independent capabilities, EACH default OFF and EACH a reusable boolean the caller sets
 * (never hardcoded into one code path). The worker keeps DESIRED vs ACTUAL state and reconciles:
 *
 *   1. share   — NAT gateway. With a working STA uplink (has an IP), raise a SoftAP and
 *                NAPT-forward downstream traffic to the uplink (esp_netif_napt_enable, the IDF
 *                "wifi router / NAPT" example). WIFI_MODE_APSTA: the one radio runs STA + AP at once.
 *   2. filter  — DNS blocklist. A resolver for downstream clients checks each queried name against
 *                an SD-backed sorted blocklist (/sd/nocsif/Pihole/) and returns 0.0.0.0 / NXDOMAIN
 *                for matches, forwarding the rest upstream. Same idea as a home ad/tracker blocker.
 *   3. tunnel  — route the FORWARDED (downstream) traffic through a WireGuard tunnel configured
 *                from /sd/nocsif/wireguard/ (a WireGuard-over-lwIP port). The tunnel's UDP port is
 *                configurable (e.g. 53 / 67) for uplinks that only pass those ports.
 *   4. portal  — a local sign-in page (served by the device, chosen from /sd/nocsif/portals/) that a
 *                downstream client must load before its traffic is forwarded. Composes with (1).
 *   5. dnst    — DNS-based transport fallback: carry the uplink over DNS to an operator-run server
 *                (/sd/nocsif/dnstunnel/) for uplinks that only permit DNS resolution (the
 *                iodine / dnstt technique). See the FEASIBILITY note in gateway.c — this is the one
 *                capability that has no drop-in ESP-IDF port; it is scaffolded + reported honestly.
 *
 * ARCHITECTURE (mirrors wifi.c / nfc.cpp / ducky.c):
 *   - A dedicated worker task owns every blocking / network action. LVGL callbacks only *request*
 *     (post a flag + a reconcile), so the UI never stalls on the radio or on SD I/O.
 *   - All getters return cached, module-owned scalars / strings (no radio, no SD), so they are safe
 *     to read from the LVGL task (Control-Center rows, the Gateway screen, live tags).
 *   - Gated on nocsif_reliability_safe_mode(): after a boot loop the worker stays idle and
 *     nocsif_gateway_available() is false.
 *
 * SINGLE-RADIO / SINGLE-AP INTEGRATION CONTRAINT (see the integration note): the ESP32-S3 has one
 * 2.4 GHz radio and one default SoftAP netif. The gateway OWNS the SoftAP while "share" is active,
 * so it is mutually exclusive with wifi.c's own SoftAP-based features (software AP, captive portal,
 * companion web remote, promiscuous monitor). The gateway does NOT init esp_wifi itself — it rides
 * the STA driver wifi.c already brought up, and requires nocsif_wifi_connected() before sharing.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* SD config roots (created on first use). One dir per capability, per the spec. */
#define NOCSIF_GW_DIR          "/sd/nocsif/gateway"    /* gateway.conf: SoftAP + master options   */
#define NOCSIF_GW_PIHOLE_DIR   "/sd/nocsif/Pihole"     /* any *.txt lists (any format) merged; allowlist.txt = exceptions */
#define NOCSIF_GW_WG_DIR       "/sd/nocsif/wireguard"  /* *.conf (wg-quick INI); a lone one auto-selects */
#define NOCSIF_GW_PORTAL_DIR   "/sd/nocsif/portals"    /* *.html sign-in pages                     */
#define NOCSIF_GW_DNST_DIR     "/sd/nocsif/dnstunnel"  /* dnstunnel.conf (server / key / mtu)      */

/* Create the idle worker task. Idempotent; LVGL-task-safe (lazy trigger on first Gateway-screen
 * entry). In reliability safe mode this is a no-op and nocsif_gateway_available() stays false.
 * Does NOT touch the radio or the card — that happens on the first request. */
esp_err_t nocsif_gateway_init(void);

/* ---- independent capability toggles (each default OFF) ------------------------------------- *
 * Each setter is non-blocking (posts to the worker), persists the desired flag to NVS on the
 * CALLER's task (never the PSRAM worker — no flash writes there), and triggers a reconcile. The
 * toggles are orthogonal: e.g. filter can run without share (DNS-only), tunnel + portal compose
 * with share, dnst substitutes for the STA uplink. Turning a flag off tears down only that layer. */
void nocsif_gateway_request_share(bool on);
void nocsif_gateway_request_filter(bool on);
void nocsif_gateway_request_tunnel(bool on);
void nocsif_gateway_request_portal(bool on);
void nocsif_gateway_request_dnst(bool on);

/* Re-read every SD config file and re-apply to whatever is currently active. Non-blocking. */
void nocsif_gateway_request_reload(void);

/* Test/bridge entry: set a capability's DESIRED flag and reconcile WITHOUT persisting to NVS, so it
 * is safe to call from ANY task (including the PSRAM-stacked desktop bridge, which must not touch
 * flash). `cap`: 's'=share 'f'=filter 't'=tunnel 'p'=portal 'd'=dnst; other values are ignored. The
 * change does not survive a reboot (unlike the request_* setters). For on-device testing. */
void nocsif_gateway_set_test(char cap, bool on);

/* ---- SoftAP name + password (edit from the Travel Router screen) --------------------------- *
 * The hotspot SSID / passphrase, editable on-watch and NVS-persisted (they override gateway.conf).
 * A passphrase of 8+ chars enables WPA2; empty (or shorter) means an OPEN AP. Applied on the next
 * bring-up, and re-applied live if the hotspot is already up. LVGL-task-safe (persist on caller). */
void        nocsif_gateway_set_ap_ssid(const char *ssid);
void        nocsif_gateway_set_ap_pass(const char *pass);
const char *nocsif_gateway_ap_ssid_cfg(void);   /* the configured SSID (default "NocSif-Gateway")   */
const char *nocsif_gateway_ap_pass(void);        /* the configured passphrase ("" = open) — seeds the editor */
bool        nocsif_gateway_ap_secured(void);     /* true when a WPA2 passphrase (>=8 chars) is set   */

/* ---- WireGuard config: import + view + edit minor fields (requirement 3) ------------------- *
 * The keys come only from the SD file (too long to type on-watch). The endpoint host / port / DNS
 * can be overridden on-watch and are persisted (NVS). All setters/getters are LVGL-task-safe (the
 * setter persists on the caller task and posts a worker reconcile; the getters read cached values). */
void        nocsif_gateway_wg_select(const char *name);   /* pick a .conf under /sd/nocsif/wireguard/ (import) */
const char *nocsif_gateway_wg_selected(void);             /* current filename (default "wg0.conf")             */
void        nocsif_gateway_wg_set_endpoint(const char *host);
void        nocsif_gateway_wg_set_port(int port);
void        nocsif_gateway_wg_set_dns(const char *dns);
const char *nocsif_gateway_wg_endpoint_host(void);        /* effective (file or override), "" if none          */
int         nocsif_gateway_wg_port(void);
const char *nocsif_gateway_wg_dns_str(void);
const char *nocsif_gateway_wg_address_str(void);          /* this device's tunnel address (from the file)      */
bool        nocsif_gateway_wg_has_config(void);           /* keys + endpoint + address all present              */

/* ---- DNS transport (dnst) editable settings (requirement 5, scaffold) ---------------------- *
 * Persisted for later server work; tunnelling is not implemented (Enable reports "experimental"). */
void        nocsif_gateway_dnst_set_server(const char *server);
void        nocsif_gateway_dnst_set_key(const char *key);
void        nocsif_gateway_dnst_set_mtu(int mtu);
const char *nocsif_gateway_dnst_server_str(void);
const char *nocsif_gateway_dnst_key_str(void);
int         nocsif_gateway_dnst_mtu(void);

/* ---- desired state (what the caller last asked for; NVS-persisted, RAM-cached) ------------- */
bool nocsif_gateway_share_wanted(void);
bool nocsif_gateway_filter_wanted(void);
bool nocsif_gateway_tunnel_wanted(void);
bool nocsif_gateway_portal_wanted(void);
bool nocsif_gateway_dnst_wanted(void);

/* ---- live published state (no radio / SD I/O; safe on the LVGL task) ----------------------- */

/* False in safe mode / before init; true once the worker exists. */
bool nocsif_gateway_available(void);

/* Per-capability ACTUAL state (may lag the desired flag while a reconcile brings it up, or stay
 * false when a precondition is missing — e.g. share with no STA uplink, tunnel with no wg0.conf). */
bool nocsif_gateway_share_active(void);
bool nocsif_gateway_filter_active(void);
bool nocsif_gateway_tunnel_active(void);
bool nocsif_gateway_portal_active(void);
bool nocsif_gateway_dnst_active(void);

/* SoftAP facts. ssid / ip are "" until the AP is up. client_count mirrors the driver association
 * table + DHCP leases; gen bumps on a membership change so a list screen rebuilds only when needed. */
int         nocsif_gateway_client_count(void);
uint32_t    nocsif_gateway_client_gen(void);
const char *nocsif_gateway_ap_ssid(void);
const char *nocsif_gateway_ap_ip_str(void);

/* The uplink IPv4 the gateway is NAPTing to ("" if no uplink). Normally the STA IP; the WireGuard
 * address when tunnelling; the dnst virtual address when the DNS transport carries the uplink. */
const char *nocsif_gateway_uplink_ip_str(void);

/* Compact status tag ("off" / "share" / "N cli" / "no uplink" / "err") + a one-line detail. */
const char *nocsif_gateway_status_str(void);
const char *nocsif_gateway_detail_str(void);

/* Was "share" left persisted on across boots? main.c reads this at boot (before the gateway worker is up)
 * to arm the WiFi throughput profile for the session's single esp_wifi_init. Safe from any task (NVS read). */
bool        nocsif_gateway_share_persisted(void);

/* ---- uplink captive-portal sign-in (the watch's STA behind a hotel/cafe portal) -------------- *
 * Detected on a fresh STA link when travel-routing. autoaccept (opt-in) clicks a simple Terms/Continue
 * page; passthru (opt-in, Tier 2) relays the venue portal to a downstream phone so the user completes a
 * login/code sign-in through the watch. signin() forces a probe + accept attempt now (the UI button). */
void        nocsif_gateway_uplink_autoaccept(bool on);
void        nocsif_gateway_uplink_passthru(bool on);
void        nocsif_gateway_uplink_signin(void);
bool        nocsif_gateway_uplink_autoaccept_on(void);
bool        nocsif_gateway_uplink_passthru_on(void);
bool        nocsif_gateway_uplink_online(void);          /* uplink reachable (no portal / signed in)     */
bool        nocsif_gateway_uplink_signin_needed(void);   /* a portal is blocking the uplink              */
uint32_t    nocsif_gateway_uplink_gen(void);             /* bumps on state change (UI refresh gate)      */
const char *nocsif_gateway_uplink_status_str(void);      /* "online" / "sign-in required" / ...          */
const char *nocsif_gateway_uplink_detail_str(void);      /* one-line detail                              */
const char *nocsif_gateway_uplink_url_str(void);         /* detected portal URL ("" if none)             */

/* Live forwarded-traffic throughput through NAPT, sampled once a second from lwIP's ip.fw counter (needs
 * CONFIG_LWIP_STATS). pps = forwarded packets/s (both directions); kbps is a rough estimate assuming
 * full-size frames (pps * 1500 B * 8) — a live A/B signal, the phone speed test is the ground truth. */
uint32_t    nocsif_gateway_fwd_pps(void);
uint32_t    nocsif_gateway_fwd_kbps(void);

/* ---- DNS filter (requirement 2) live counters + status ------------------------------------ */
uint32_t    nocsif_gateway_dns_queries(void);     /* queries seen from downstream clients      */
uint32_t    nocsif_gateway_dns_blocked(void);     /* answered as blocked (0.0.0.0 / NXDOMAIN)  */
uint32_t    nocsif_gateway_dns_forwarded(void);   /* forwarded to the upstream resolver         */
uint32_t    nocsif_gateway_dns_cached(void);      /* answered from the local TTL cache (no round-trip) */
int         nocsif_gateway_blocklist_size(void);  /* indexed domain count, or -1 if not loaded */
const char *nocsif_gateway_filter_status_str(void); /* "off"/"loading"/"N domains"/"no list"/… */

/* ---- WireGuard tunnel (requirement 3) live state ------------------------------------------ */
bool        nocsif_gateway_tunnel_up(void);          /* a handshake has completed (peer is up)  */
const char *nocsif_gateway_tunnel_status_str(void);  /* "off"/"connecting"/"up"/"no config"/…   */
const char *nocsif_gateway_tunnel_endpoint_str(void);/* "host:port" from wg0.conf, or ""        */

/* ---- sign-in portal (requirement 4) live state -------------------------------------------- */
int         nocsif_gateway_signed_count(void);       /* downstream clients that have signed in  */
uint32_t    nocsif_gateway_portal_gen(void);         /* bumps on a hit / a sign-in (UI refresh) */
const char *nocsif_gateway_portal_status_str(void);  /* "off"/"waiting"/"N signed"/"no card"/…  */

/* ---- DNS transport fallback (requirement 5) live state ------------------------------------ */
const char *nocsif_gateway_dnst_status_str(void);    /* "off"/"experimental"/"no config"/…      */

/* Compact live tag for a System / Connectivity "Gateway" menu row. Module-owned. */
const char *nocsif_gateway_tag_str(void);

#ifdef __cplusplus
}
#endif
