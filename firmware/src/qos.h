/*
 * NocSif — Travel Router traffic: per-device usage on the hotspot + "calls first" priority (no caps).
 *
 * Usage: byte counters on the hotspot interface keyed by each device's address, turned into per-second
 * rates and session totals once a second (the router dashboard, bridge `gw clients`).
 *
 * Calls first: interactive packets are marked CS5 in the IP header. The Wi-Fi driver's classifier
 * (ieee80211_classify) picks the TX queue from TOS >> 5, so they go out on the video access category,
 * ahead of bulk traffic, on both hops the watch transmits (up to the router, down to the device).
 * Interactive = UDP (remote-desktop streams, call media) except QUIC on 443 / 80, voice-sized packets on
 * tunnel ports (Wi-Fi Calling), ping and ACK-sized TCP; each device may mark up to ~2 Mbit/s per direction,
 * beyond that its packets compete as normal traffic. Nothing is capped. Only while priority frames are failing
 * to get a TX buffer do bulk frames leave QOS_RESERVE buffers free.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_netif.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t  mac[6];
    uint32_t ip;                 /* network order; 0 = no DHCP lease seen yet          */
    int8_t   rssi;
    uint32_t up_kbps, dn_kbps;   /* last second                                         */
    uint64_t up_bytes, dn_bytes; /* since the device joined (this boot)                 */
    uint32_t prio_pps;           /* priority packets/s, both directions                 */
} nocsif_qos_client_t;

typedef struct {
    uint32_t prio_up, prio_dn;   /* packets marked since boot                           */
    uint32_t shed;               /* bulk frames held back so priority frames found a TX buffer */
    uint32_t tx_full;            /* the driver's TX pool was full (ERR_MEM)             */
    uint32_t over;               /* interactive packets sent unmarked: the device was over its priority budget */
    uint32_t prio_fail;          /* priority frames that found the TX pool full (these switch the reserve on) */
    int32_t  inflight, cap;      /* frames with the driver now; learned pool size (0 = never full) */
    bool     hooked, txcb, prio_active;
} nocsif_qos_stats_t;

/* Once a second from the gateway tick: installs the hotspot / station hooks (idempotent, re-checked so a
 * driver restart cannot drop them), registers the TX-done counter, and refreshes the per-device table. */
void nocsif_qos_tick(esp_netif_t *ap, bool sharing);

/* Snapshot of the devices on the hotspot (any task). Returns the count copied. */
int  nocsif_qos_clients(nocsif_qos_client_t *out, int max);
void nocsif_qos_stats(nocsif_qos_stats_t *out);

#ifdef __cplusplus
}
#endif
