/*
 * NocSif — saved-network file store ("Networks folder").
 *
 * A per-network file under /sd/nocsif/Networks/, holding everything the watch knows about a network
 * so it can be re-created / emulated later and paired with a captive-portal page. The store is the
 * common sink for "clone this AP", "save this probe's SSID", and the manual import path: the files
 * are plain key=value text, so a network can also be dropped in from a phone/laptop over USB File
 * Share and it just appears here.
 *
 * The SSID is the identity: an upsert MERGES onto the existing file for that SSID (a re-seen network
 * refreshes its record rather than spawning duplicates). One SSID can live in many physical places,
 * but the *file* is per-SSID (the geo-store keeps BSSID→place separately).
 *
 * All I/O claims the card away from File Share and holds the short FAT lock, exactly like sdfs.c, so
 * these calls are safe to make from the LVGL task (small text files, mirrors Notes / the portal
 * picker). Getters that read the directory do one claim+lock pass.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NOCSIF_NET_DIR         "/sd/nocsif/Networks"
#define NOCSIF_NET_SSID_MAX    33     /* SSID incl. NUL                                             */
#define NOCSIF_NET_NAME_MAX    64     /* file basename incl. NUL (e.g. "MyNet.net")                */
#define NOCSIF_NET_PORTAL_MAX  64     /* associated portal filename under /sd/nocsif/wifi/portals   */
#define NOCSIF_NET_VENDOR_MAX  24
#define NOCSIF_NET_HOST_MAX    33
#define NOCSIF_NET_PSK_MAX     64
#define NOCSIF_NET_NOTE_MAX    48
#define NOCSIF_NET_IE_MAX      460    /* raw beacon IE bytes as hex (up to ~220 B -> 440 hex chars) */
#define NOCSIF_NET_LIST_MAX    64     /* files returned by one nocsif_net_list()                    */

/* One stored network profile. An empty string / a 0 in an optional field means "unknown/unset". */
typedef struct {
    char     ssid[NOCSIF_NET_SSID_MAX];
    uint8_t  bssid[6];
    bool     has_bssid;
    uint8_t  channel;                      /* 0 = unknown                                          */
    uint8_t  security;                     /* nocsif_wifi_sec_t                                    */
    bool     has_security;
    bool     hidden;
    char     vendor[NOCSIF_NET_VENDOR_MAX];
    char     hostname[NOCSIF_NET_HOST_MAX];
    char     psk[NOCSIF_NET_PSK_MAX];      /* optional (own gear / emulation)                      */
    char     portal[NOCSIF_NET_PORTAL_MAX];/* associated portal file ("" = none)                  */
    bool     forgotten;                    /* hidden from the Saved-Networks list (still emulatable) */
    int32_t  lat_ud, lon_ud;               /* micro-degrees; 0 = unknown                           */
    int8_t   rssi;                         /* last seen dBm (0 = unknown)                          */
    uint32_t seen;                         /* times observed / merged                              */
    char     note[NOCSIF_NET_NOTE_MAX];
    char     ie_hex[NOCSIF_NET_IE_MAX];    /* raw beacon IEs as hex, for emulation ("" = none)     */
} nocsif_net_t;

/* Zero a record and set the "unset" defaults (call before filling one for an upsert). */
void nocsif_net_clear(nocsif_net_t *n);

/* Map an SSID to its file basename (FAT-sanitized + ".net"). A hidden/empty SSID maps to a
 * BSSID-derived name if `bssid` is non-NULL, else "_hidden.net". */
void nocsif_net_ssid_to_name(const char *ssid, const uint8_t *bssid, char *out, size_t len);

/* Snapshot the stored files' basenames (sorted). Returns the count written into `names`
 * (<= max); one claim+lock pass. `names` is a caller array of [max][NOCSIF_NET_NAME_MAX]. */
int nocsif_net_list(char (*names)[NOCSIF_NET_NAME_MAX], int max);

/* Load a stored network by its file basename (from nocsif_net_list). false if missing / unparseable. */
bool nocsif_net_load(const char *name, nocsif_net_t *out);

/* Load the stored network for an SSID (its derived file). false if none. */
bool nocsif_net_load_ssid(const char *ssid, nocsif_net_t *out);

/* True if a file exists for this SSID. */
bool nocsif_net_exists(const char *ssid);

/* Upsert by SSID: if a file for `n->ssid` exists, overlay only the MEANINGFUL fields of `n` onto it
 * (non-empty strings, non-zero channel/lat/lon/rssi, has_bssid, has_security, non-empty ie/note) and
 * bump `seen`; else create a new file. Creates NOCSIF_NET_DIR on demand. Returns NULL on success,
 * else a short human reason (for a toast). `n->ssid` must be non-empty. */
const char *nocsif_net_upsert(const nocsif_net_t *n);

/* Set / clear the associated portal for an SSID (pass "" or NULL to clear). Creates the record if
 * it does not exist yet. Returns NULL on success, else a reason. */
const char *nocsif_net_set_portal(const char *ssid, const char *portal);

/* Set / clear the "forgotten" flag for an SSID — hidden from the Saved-Networks list but kept in the
 * folder (still emulatable). Reconnecting clears it. Creates the record if missing. NULL on success. */
const char *nocsif_net_set_forgotten(const char *ssid, bool forgotten);

/* Write /sd/nocsif/Networks/_README.txt (the file-format cheatsheet for hand-editing / importing) if
 * it is not already present. No-op on failure. */
void nocsif_net_ensure_readme(void);

/* Delete a stored network by file basename. Returns NULL on success, else a reason. */
const char *nocsif_net_delete(const char *name);

#ifdef __cplusplus
}
#endif
