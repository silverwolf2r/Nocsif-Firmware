/*
 * NocSif — shared microSD file-access rules implementation. See sdfs.h.
 */
#include "sdfs.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"      /* esp_vfs_fat_info — FAT totals */

#include "sdcard.h"           /* card FAT lock and raw card handle */
#include "usb_gadget.h"       /* claim/release the card away from USB-MSC, and card format */

static const char *TAG = "sdfs";

bool nocsif_sdfs_path_ok(const char *p, bool allow_root)
{
    size_t n = strlen(p);
    if (n >= NOCSIF_SDFS_PATH_MAX) return false;
    if (strncmp(p, NOCSIF_SDFS_ROOT, 3) != 0) return false;
    if (p[3] == '\0') return allow_root;
    if (p[3] != '/') return false;
    const char *s = p + 4;
    for (;;) {
        const char *e = strchr(s, '/');
        size_t seg = e ? (size_t)(e - s) : strlen(s);
        if (seg == 0) return false;                                            /* empty segment, e.g. "//" */
        if (s[0] == '.' && (seg == 1 || (seg == 2 && s[1] == '.'))) return false;
        for (size_t i = 0; i < seg; i++) {
            if ((unsigned char)s[i] < 0x20 || s[i] == '\\') return false;
        }
        if (!e) return true;
        s = e + 1;
    }
}

bool nocsif_sdfs_name_ok(const char *n)
{
    size_t len = strlen(n);
    if (len == 0 || len >= NOCSIF_SDFS_NAME_MAX || n[0] == '.') return false;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)n[i];
        if (c < 0x20 || strchr("/\\:*?\"<>|", (int)c)) return false;
    }
    return true;
}

const char *nocsif_sdfs_claim(void)
{
    esp_err_t ce = nocsif_usb_gadget_claim_sd(2000);
    if (ce == ESP_OK) return NULL;
    if (ce == ESP_ERR_INVALID_STATE && nocsif_usb_gadget_mode() == NOCSIF_USB_MODE_MSC) {
        return "File Share has the card";
    }
    return "microSD unavailable";
}

void nocsif_sdfs_release(void)
{
    nocsif_usb_gadget_release_sd();
}

const char *nocsif_sdfs_mime(const char *name)
{
    const char *ext = strrchr(name, '.');
    if (!ext) return "application/octet-stream";
    ext++;
    if (!strcasecmp(ext, "pcap"))                                    return "application/vnd.tcpdump.pcap";
    if (!strcasecmp(ext, "gpx"))                                     return "application/gpx+xml";
    if (!strcasecmp(ext, "json"))                                    return "application/json";
    if (!strcasecmp(ext, "txt") || !strcasecmp(ext, "log") ||
        !strcasecmp(ext, "md")  || !strcasecmp(ext, "csv"))          return "text/plain";
    if (!strcasecmp(ext, "html"))                                    return "text/html";
    if (!strcasecmp(ext, "wav"))                                     return "audio/wav";
    if (!strcasecmp(ext, "mp3"))                                     return "audio/mpeg";
    if (!strcasecmp(ext, "png"))                                     return "image/png";
    if (!strcasecmp(ext, "jpg") || !strcasecmp(ext, "jpeg"))         return "image/jpeg";
    return "application/octet-stream";
}

static int ent_cmp(const void *a, const void *b)
{
    const nocsif_sdfs_ent_t *x = (const nocsif_sdfs_ent_t *)a, *y = (const nocsif_sdfs_ent_t *)b;
    if (x->is_dir != y->is_dir) return x->is_dir ? -1 : 1;
    return strcasecmp(x->name, y->name);
}

const char *nocsif_sdfs_list(const char *path, nocsif_sdfs_ent_t *ents, int max, int *n_out, bool *trunc)
{
    *n_out = 0;
    *trunc = false;
    const char *why = nocsif_sdfs_claim();
    if (why) return why;
    int  n = 0;
    bool opened = false;
    if (nocsif_sdcard_lock(1500)) {
        DIR *d = opendir(path);
        if (d) {
            opened = true;
            struct dirent *ent;
            char full[NOCSIF_SDFS_PATH_MAX + NOCSIF_SDFS_NAME_MAX + 2];
            while ((ent = readdir(d)) != NULL) {
                if (ent->d_name[0] == '.') continue;                       /* skip dotfiles, "." and ".." */
                if (strlen(ent->d_name) >= NOCSIF_SDFS_NAME_MAX) continue;
                if (n >= max) { *trunc = true; break; }
                nocsif_sdfs_ent_t *e = &ents[n];
                snprintf(e->name, sizeof e->name, "%s", ent->d_name);
                e->is_dir = (ent->d_type == DT_DIR);
                e->size   = 0;
                struct stat st;
                snprintf(full, sizeof full, "%s/%s", path, e->name);
                if (stat(full, &st) == 0) {                                /* trust this over d_type */
                    e->is_dir = S_ISDIR(st.st_mode);
                    if (!e->is_dir) e->size = (uint32_t)st.st_size;
                }
                n++;
            }
            closedir(d);
        }
        nocsif_sdcard_unlock();
    }
    nocsif_sdfs_release();
    if (!opened) return "folder unavailable";
    qsort(ents, n, sizeof ents[0], ent_cmp);
    *n_out = n;
    return NULL;
}

void nocsif_sdfs_info(bool *present, uint64_t *total, uint64_t *free_bytes)
{
    *present = (nocsif_sdcard_card() != NULL);
    *total = *free_bytes = 0;
    if (!*present) return;
    if (nocsif_sdfs_claim()) return;                        /* couldn't claim it: report present but no totals */
    if (nocsif_sdcard_lock(1500)) {
        uint64_t t = 0, f = 0;
        if (esp_vfs_fat_info(NOCSIF_SDFS_ROOT, &t, &f) == ESP_OK) { *total = t; *free_bytes = f; }
        nocsif_sdcard_unlock();
    }
    nocsif_sdfs_release();
}

/* The standard folder layout the firmware writes into, listed in nesting order so
 * parents are created before their children. */
static const char *const k_dirs[] = {
    "/sd/nocsif",
    "/sd/nocsif/firmware",       /* update image + manifest */
    "/sd/nocsif/carts",          /* audio carts, one folder per set */
    "/sd/nocsif/wifi",           /* captures, handshakes, portal logs */
    "/sd/nocsif/wifi/portals",   /* captive-portal pages */
    "/sd/nocsif/ble",            /* advertisement captures */
    "/sd/nocsif/notes",          /* notes */
    "/sd/nocsif/voice",          /* voice memos */
    "/sd/nocsif/tracks",         /* GPX tracks */
    "/sd/nocsif/wardrive",       /* wardrive logs */
    "/sd/ducky",                 /* DuckyScript macros */
};

static const char k_readme[] =
    "NocSif microSD layout\n"
    "\n"
    "  nocsif/firmware   firmware.bin (+ manifest.json) for System > Update\n"
    "  nocsif/carts      audio carts (.wav / .mp3), one folder per cart set\n"
    "  nocsif/wifi       captures (.pcap), handshakes (.hc22000), portal logs\n"
    "  nocsif/wifi/portals  captive-portal pages\n"
    "  nocsif/ble        advert captures (.pcap)\n"
    "  nocsif/notes      Notes\n"
    "  nocsif/voice      voice memos (.wav)\n"
    "  nocsif/tracks     GPX tracks\n"
    "  nocsif/wardrive   wardrive .csv\n"
    "  ducky             DuckyScript macros (.txt)\n"
    "\n"
    "Created by the NocSif desktop bridge. Safe to add your own folders.\n";

const char *nocsif_sdfs_provision(int *made)
{
    *made = 0;
    const char *why = nocsif_sdfs_claim();
    if (why) return why;
    const char *err = NULL;
    if (nocsif_sdcard_lock(3000)) {
        for (size_t i = 0; i < sizeof k_dirs / sizeof k_dirs[0]; i++) {
            struct stat st;
            if (stat(k_dirs[i], &st) == 0) continue;
            if (mkdir(k_dirs[i], 0777) == 0) (*made)++;
            else { err = "cannot create folders (card read-only or full?)"; break; }
        }
        if (!err) {
            struct stat st;
            if (stat("/sd/nocsif/README.txt", &st) != 0) {
                FILE *f = fopen("/sd/nocsif/README.txt", "wb");
                if (f) {
                    fwrite(k_readme, 1, sizeof k_readme - 1, f);
                    fclose(f);
                    (*made)++;
                }
            }
        }
        nocsif_sdcard_unlock();
    } else {
        err = "card busy";
    }
    nocsif_sdfs_release();
    ESP_LOGI(TAG, "provision: %d created%s%s", *made, err ? " — " : "", err ? err : "");
    return err;
}

const char *nocsif_sdfs_format(void)
{
    if (nocsif_sdcard_card() == NULL) return "no microSD card";
    const char *why = nocsif_sdfs_claim();
    if (why) return why;
    const char *err = NULL;
    if (nocsif_sdcard_lock(5000)) {
        err = nocsif_usb_gadget_sd_format();          /* handles the mount, mkfs, and remount itself */
        nocsif_sdcard_unlock();
    } else {
        err = "card busy";
    }
    nocsif_sdfs_release();
    ESP_LOGW(TAG, "format: %s", err ? err : "done");
    return err;
}
