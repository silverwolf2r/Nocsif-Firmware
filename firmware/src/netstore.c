/*
 * NocSif — saved-network file store (see netstore.h).
 *
 * Plain key=value text files under /sd/nocsif/Networks/, one per SSID. Small files, read/written
 * under the same claim(File-Share)+FAT-lock discipline as sdfs.c, so the UI can call these directly.
 */
#include "netstore.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "esp_log.h"

#include "sdfs.h"        /* nocsif_sdfs_claim / nocsif_sdfs_release (File-Share ownership)   */
#include "sdcard.h"      /* nocsif_sdcard_lock / nocsif_sdcard_unlock (short FAT lock)       */
#include "wifi.h"        /* nocsif_wifi_sec_t + nocsif_wifi_sec_str (security <-> label)     */

static const char *TAG = "netstore";

/* ---- claim + lock, mirroring sdfs.c --------------------------------------------------- */
static const char *net_lock(void)
{
    const char *why = nocsif_sdfs_claim();
    if (why) return why;
    if (!nocsif_sdcard_lock(1500)) {
        nocsif_sdfs_release();
        return "card busy";
    }
    return NULL;
}
static void net_unlock(void)
{
    nocsif_sdcard_unlock();
    nocsif_sdfs_release();
}

static void net_mkdir_p(void)   /* call under the lock */
{
    mkdir("/sd/nocsif", 0777);
    mkdir(NOCSIF_NET_DIR, 0777);
}

/* ---- helpers ------------------------------------------------------------------------- */
void nocsif_net_clear(nocsif_net_t *n)
{
    if (!n) return;
    memset(n, 0, sizeof *n);
}

void nocsif_net_ssid_to_name(const char *ssid, const uint8_t *bssid, char *out, size_t len)
{
    if (!out || len == 0) return;
    if (!ssid || ssid[0] == '\0') {
        if (bssid) {
            snprintf(out, len, "%02x%02x%02x%02x%02x%02x.net",
                     bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);
        } else {
            snprintf(out, len, "_hidden.net");
        }
        return;
    }
    /* FAT-sanitize: keep alnum . _ - and space; anything else -> '_'. Leading '.' -> '_'. */
    char base[NOCSIF_NET_NAME_MAX];
    size_t j = 0;
    for (size_t i = 0; ssid[i] && j < sizeof base - 5; i++) {
        unsigned char c = (unsigned char)ssid[i];
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-' || c == ' ';
        base[j++] = ok ? (char)c : '_';
    }
    base[j] = '\0';
    if (base[0] == '.' || base[0] == '\0') base[0] = '_';
    snprintf(out, len, "%s.net", base);
}

/* security label <-> value. Writing uses nocsif_wifi_sec_str; parsing maps the same words back. */
static uint8_t net_sec_parse(const char *s)
{
    if (!strcasecmp(s, "open"))   return NOCSIF_WIFI_SEC_OPEN;
    if (!strcasecmp(s, "WEP"))    return NOCSIF_WIFI_SEC_WEP;
    if (!strcasecmp(s, "WPA"))    return NOCSIF_WIFI_SEC_WPA;
    if (!strcasecmp(s, "WPA2"))   return NOCSIF_WIFI_SEC_WPA2;
    if (!strcasecmp(s, "WPA3"))   return NOCSIF_WIFI_SEC_WPA3;
    if (!strcasecmp(s, "WPA2-E")) return NOCSIF_WIFI_SEC_WPA2E;
    return NOCSIF_WIFI_SEC_OPEN;
}

static bool net_parse_bssid(const char *s, uint8_t out[6])
{
    unsigned v[6];
    if (sscanf(s, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return false;
    for (int i = 0; i < 6; i++) out[i] = (uint8_t)v[i];
    return true;
}

/* Copy the value part of a "key=value" line, trimming trailing CR/LF, into dst. */
static void net_copy_val(char *dst, size_t dstlen, const char *val)
{
    snprintf(dst, dstlen, "%s", val);
    size_t n = strlen(dst);
    while (n && (dst[n - 1] == '\r' || dst[n - 1] == '\n' || dst[n - 1] == ' ')) dst[--n] = '\0';
}

/* ---- file read / write (both under the lock, path pre-built) -------------------------- */
static bool net_read(const char *path, nocsif_net_t *out)
{
    FILE *f = fopen(path, "r");
    if (!f) return false;
    nocsif_net_clear(out);
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        const char *key = line;
        const char *val = eq + 1;
        if      (!strcmp(key, "ssid"))     net_copy_val(out->ssid, sizeof out->ssid, val);
        else if (!strcmp(key, "bssid"))    { out->has_bssid = net_parse_bssid(val, out->bssid); }
        else if (!strcmp(key, "channel"))  out->channel = (uint8_t)atoi(val);
        else if (!strcmp(key, "security")) { char t[16]; net_copy_val(t, sizeof t, val);
                                             out->security = net_sec_parse(t); out->has_security = true; }
        else if (!strcmp(key, "hidden"))   out->hidden = (atoi(val) != 0);
        else if (!strcmp(key, "vendor"))   net_copy_val(out->vendor, sizeof out->vendor, val);
        else if (!strcmp(key, "hostname")) net_copy_val(out->hostname, sizeof out->hostname, val);
        else if (!strcmp(key, "psk"))      net_copy_val(out->psk, sizeof out->psk, val);
        else if (!strcmp(key, "portal"))   net_copy_val(out->portal, sizeof out->portal, val);
        else if (!strcmp(key, "lat"))      out->lat_ud = (int32_t)atoi(val);
        else if (!strcmp(key, "lon"))      out->lon_ud = (int32_t)atoi(val);
        else if (!strcmp(key, "rssi"))     out->rssi = (int8_t)atoi(val);
        else if (!strcmp(key, "seen"))     out->seen = (uint32_t)strtoul(val, NULL, 10);
        else if (!strcmp(key, "note"))     net_copy_val(out->note, sizeof out->note, val);
        else if (!strcmp(key, "ie"))       net_copy_val(out->ie_hex, sizeof out->ie_hex, val);
        else if (!strcmp(key, "forgotten")) out->forgotten = (atoi(val) != 0);
    }
    fclose(f);
    return out->ssid[0] != '\0';
}

static bool net_write(const char *path, const nocsif_net_t *n)
{
    FILE *f = fopen(path, "w");
    if (!f) return false;
    fprintf(f, "ssid=%s\n", n->ssid);
    if (n->has_bssid)
        fprintf(f, "bssid=%02x:%02x:%02x:%02x:%02x:%02x\n",
                n->bssid[0], n->bssid[1], n->bssid[2], n->bssid[3], n->bssid[4], n->bssid[5]);
    if (n->channel)      fprintf(f, "channel=%u\n", (unsigned)n->channel);
    if (n->has_security) fprintf(f, "security=%s\n", nocsif_wifi_sec_str(n->security));
    if (n->hidden)       fprintf(f, "hidden=1\n");
    if (n->vendor[0])    fprintf(f, "vendor=%s\n", n->vendor);
    if (n->hostname[0])  fprintf(f, "hostname=%s\n", n->hostname);
    if (n->psk[0])       fprintf(f, "psk=%s\n", n->psk);
    if (n->portal[0])    fprintf(f, "portal=%s\n", n->portal);
    if (n->forgotten)    fprintf(f, "forgotten=1\n");
    if (n->lat_ud)       fprintf(f, "lat=%d\n", (int)n->lat_ud);
    if (n->lon_ud)       fprintf(f, "lon=%d\n", (int)n->lon_ud);
    if (n->rssi)         fprintf(f, "rssi=%d\n", (int)n->rssi);
    fprintf(f, "seen=%u\n", (unsigned)n->seen);
    if (n->note[0])      fprintf(f, "note=%s\n", n->note);
    if (n->ie_hex[0])    fprintf(f, "ie=%s\n", n->ie_hex);
    fclose(f);
    return true;
}

/* ---- public API ---------------------------------------------------------------------- */
static int net_name_cmp(const void *a, const void *b)
{
    return strcasecmp((const char *)a, (const char *)b);
}

int nocsif_net_list(char (*names)[NOCSIF_NET_NAME_MAX], int max)
{
    if (!names || max <= 0) return 0;
    if (net_lock()) return 0;
    int n = 0;
    DIR *d = opendir(NOCSIF_NET_DIR);
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (ent->d_name[0] == '.') continue;
            if (ent->d_type == DT_DIR) continue;
            size_t len = strlen(ent->d_name);
            if (len < 5 || strcasecmp(ent->d_name + len - 4, ".net") != 0) continue;
            if (len >= NOCSIF_NET_NAME_MAX) continue;
            if (n >= max) break;
            snprintf(names[n], NOCSIF_NET_NAME_MAX, "%s", ent->d_name);
            n++;
        }
        closedir(d);
    }
    net_unlock();
    if (n > 1) qsort(names, n, NOCSIF_NET_NAME_MAX, net_name_cmp);
    return n;
}

bool nocsif_net_load(const char *name, nocsif_net_t *out)
{
    if (!name || !out) return false;
    char path[NOCSIF_SDFS_PATH_MAX];
    snprintf(path, sizeof path, "%s/%s", NOCSIF_NET_DIR, name);
    if (net_lock()) return false;
    bool ok = net_read(path, out);
    net_unlock();
    return ok;
}

bool nocsif_net_load_ssid(const char *ssid, nocsif_net_t *out)
{
    if (!ssid || !out) return false;
    char name[NOCSIF_NET_NAME_MAX];
    nocsif_net_ssid_to_name(ssid, NULL, name, sizeof name);
    return nocsif_net_load(name, out);
}

bool nocsif_net_exists(const char *ssid)
{
    nocsif_net_t tmp;
    return nocsif_net_load_ssid(ssid, &tmp);
}

/* Overlay only meaningful fields of `src` onto `dst`. */
static void net_overlay(nocsif_net_t *dst, const nocsif_net_t *src)
{
    if (src->ssid[0])     snprintf(dst->ssid, sizeof dst->ssid, "%s", src->ssid);
    if (src->has_bssid)   { memcpy(dst->bssid, src->bssid, 6); dst->has_bssid = true; }
    if (src->channel)     dst->channel = src->channel;
    if (src->has_security){ dst->security = src->security; dst->has_security = true; }
    dst->hidden = src->hidden || dst->hidden;
    if (src->vendor[0])   snprintf(dst->vendor, sizeof dst->vendor, "%s", src->vendor);
    if (src->hostname[0]) snprintf(dst->hostname, sizeof dst->hostname, "%s", src->hostname);
    if (src->psk[0])      snprintf(dst->psk, sizeof dst->psk, "%s", src->psk);
    if (src->portal[0])   snprintf(dst->portal, sizeof dst->portal, "%s", src->portal);
    if (src->lat_ud)      dst->lat_ud = src->lat_ud;
    if (src->lon_ud)      dst->lon_ud = src->lon_ud;
    if (src->rssi)        dst->rssi = src->rssi;
    if (src->note[0])     snprintf(dst->note, sizeof dst->note, "%s", src->note);
    if (src->ie_hex[0])   snprintf(dst->ie_hex, sizeof dst->ie_hex, "%s", src->ie_hex);
}

const char *nocsif_net_upsert(const nocsif_net_t *n)
{
    if (!n || n->ssid[0] == '\0') return "no SSID";
    char name[NOCSIF_NET_NAME_MAX], path[NOCSIF_SDFS_PATH_MAX];
    nocsif_net_ssid_to_name(n->ssid, n->has_bssid ? n->bssid : NULL, name, sizeof name);
    snprintf(path, sizeof path, "%s/%s", NOCSIF_NET_DIR, name);

    const char *why = net_lock();
    if (why) return why;
    net_mkdir_p();

    nocsif_net_t merged;
    if (!net_read(path, &merged)) {              /* new file */
        nocsif_net_clear(&merged);
    }
    net_overlay(&merged, n);
    merged.seen += 1;
    bool ok = net_write(path, &merged);
    net_unlock();
    if (!ok) { ESP_LOGW(TAG, "upsert write failed: %s", path); return "write failed"; }
    ESP_LOGI(TAG, "upsert %s (seen=%u)", name, (unsigned)merged.seen);
    return NULL;
}

const char *nocsif_net_set_portal(const char *ssid, const char *portal)
{
    if (!ssid || ssid[0] == '\0') return "no SSID";
    char name[NOCSIF_NET_NAME_MAX], path[NOCSIF_SDFS_PATH_MAX];
    nocsif_net_ssid_to_name(ssid, NULL, name, sizeof name);
    snprintf(path, sizeof path, "%s/%s", NOCSIF_NET_DIR, name);

    const char *why = net_lock();
    if (why) return why;
    net_mkdir_p();
    nocsif_net_t rec;
    if (!net_read(path, &rec)) {
        nocsif_net_clear(&rec);
        snprintf(rec.ssid, sizeof rec.ssid, "%s", ssid);
    }
    snprintf(rec.portal, sizeof rec.portal, "%s", portal ? portal : "");
    bool ok = net_write(path, &rec);
    net_unlock();
    return ok ? NULL : "write failed";
}

const char *nocsif_net_set_forgotten(const char *ssid, bool forgotten)
{
    if (!ssid || ssid[0] == '\0') return "no SSID";
    char name[NOCSIF_NET_NAME_MAX], path[NOCSIF_SDFS_PATH_MAX];
    nocsif_net_ssid_to_name(ssid, NULL, name, sizeof name);
    snprintf(path, sizeof path, "%s/%s", NOCSIF_NET_DIR, name);

    const char *why = net_lock();
    if (why) return why;
    net_mkdir_p();
    nocsif_net_t rec;
    if (!net_read(path, &rec)) {
        nocsif_net_clear(&rec);
        snprintf(rec.ssid, sizeof rec.ssid, "%s", ssid);
    }
    rec.forgotten = forgotten;
    bool ok = net_write(path, &rec);
    net_unlock();
    return ok ? NULL : "write failed";
}

void nocsif_net_ensure_readme(void)
{
    if (net_lock()) return;
    net_mkdir_p();
    char path[NOCSIF_SDFS_PATH_MAX];
    snprintf(path, sizeof path, "%s/_README.txt", NOCSIF_NET_DIR);
    struct stat st;
    if (stat(path, &st) != 0) {                 /* only write it once */
        FILE *f = fopen(path, "w");
        if (f) {
            fputs(
                "NocSif saved networks\r\n"
                "=====================\r\n"
                "One .net file per network (key=value, one per line). Drop files here to import;\r\n"
                "a network you can connect to (has psk, or security=open) shows under WiFi Connect >\r\n"
                "Saved Networks, and any file can be broadcast from SSID TX > Captive Portal > Emulate.\r\n"
                "\r\n"
                "Keys (only ssid is required):\r\n"
                "  ssid=MyNetwork        the network name\r\n"
                "  psk=hunter2           passphrase (omit / leave blank for an open network)\r\n"
                "  security=WPA2         open | WEP | WPA | WPA2 | WPA3   (optional)\r\n"
                "  bssid=aa:bb:cc:dd:ee:ff   router MAC        (optional)\r\n"
                "  channel=6             1-13                 (optional)\r\n"
                "  hidden=1              hidden SSID          (optional)\r\n"
                "  portal=mynet.html     a page in ../wifi/portals to serve when emulating (optional)\r\n"
                "  forgotten=1           hide from Saved Networks (set by Forget; cleared on reconnect)\r\n"
                "\r\n"
                "Minimal connectable example:\r\n"
                "  ssid=HomeWiFi\r\n"
                "  psk=correcthorsebatterystaple\r\n",
                f);
            fclose(f);
        }
    }
    net_unlock();
}

const char *nocsif_net_delete(const char *name)
{
    if (!name || name[0] == '\0') return "no name";
    char path[NOCSIF_SDFS_PATH_MAX];
    snprintf(path, sizeof path, "%s/%s", NOCSIF_NET_DIR, name);
    const char *why = net_lock();
    if (why) return why;
    int rc = remove(path);
    net_unlock();
    return rc == 0 ? NULL : "delete failed";
}
