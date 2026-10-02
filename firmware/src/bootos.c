/*
 * NocSif — Bootable OS (USB). See bootos.h.
 *
 * Serves an OS image FILE on the shared microSD to the host as a READ-ONLY bootable disk.
 * The image stays a plain file in NOCSIF_BOOTOS_DIR; usb_gadget presents its bytes via the
 * vendored esp_tinyusb callback medium (tinyusb_msc_new_storage_callback), so the card is
 * never repartitioned and nothing is written back.
 *
 * The read callback (nocsif_bootos_read) runs on the TinyUSB task. It reads the image with
 * POSIX I/O on the app-mounted /sd (the File-Share MSC storage stays APP-owned while serving,
 * so /sd remains mounted for us). Every read is serialised with the rest of the app via
 * nocsif_sdcard_lock. In serve mode the watch is otherwise idle, so the card is uncontended.
 *
 * Phase 0 note: POSIX off_t is 32-bit on the ESP32-S3, so an image must be < 2 GiB (Tails
 * ~1.5 GB is fine). Larger images will need the raw-sector extent path planned for Phase 1.
 */
#include "bootos.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

#include "esp_log.h"

#include "sdcard.h"       /* nocsif_sdcard_card / _lock / _unlock */
#include "usb_gadget.h"   /* nocsif_usb_gadget_bootos_request / _request_mode */

static const char *TAG = "bootos";

#define BOOTOS_SECTOR_SIZE   512u
#define BOOTOS_MIN_BYTES     (1024u * 1024u)     /* reject a file too small to be a disk image */
#define BOOTOS_LOCK_MS       3000

static int      s_fd = -1;                /* open image fd while serving, else -1 */
static uint32_t s_total_sectors;
static volatile bool s_active;
static char     s_path[256];

/* Host SCSI READ(10) → bytes from the image file. Runs on the TinyUSB task. */
static esp_err_t nocsif_bootos_read(void *ctx, uint32_t lba, uint32_t offset, size_t size, void *dest)
{
    (void)ctx;
    if (s_fd < 0 || dest == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint64_t byte_off = (uint64_t)lba * BOOTOS_SECTOR_SIZE + offset;
    if (!nocsif_sdcard_lock(BOOTOS_LOCK_MS)) {
        ESP_LOGE(TAG, "read: /sd busy (lba=%u)", (unsigned)lba);
        return ESP_FAIL;
    }
    esp_err_t ret = ESP_OK;
    if (lseek(s_fd, (off_t)byte_off, SEEK_SET) != (off_t)byte_off) {
        ret = ESP_FAIL;
    } else {
        uint8_t *p = (uint8_t *)dest;
        size_t left = size;
        while (left > 0) {
            ssize_t n = read(s_fd, p, left);
            if (n < 0) { ret = ESP_FAIL; break; }
            if (n == 0) { memset(p, 0, left); break; }   /* past EOF: zero-fill (defensive) */
            p += n;
            left -= (size_t)n;
        }
    }
    nocsif_sdcard_unlock();
    return ret;
}

esp_err_t nocsif_bootos_init(void)
{
    if (nocsif_sdcard_card() == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!nocsif_sdcard_lock(BOOTOS_LOCK_MS)) {
        return ESP_ERR_TIMEOUT;
    }
    mkdir("/sd/nocsif", 0777);          /* ignore EEXIST */
    mkdir(NOCSIF_BOOTOS_DIR, 0777);
    nocsif_sdcard_unlock();
    return ESP_OK;
}

bool nocsif_bootos_find_first(char *buf, size_t buflen)
{
    if (buf == NULL || buflen == 0) {
        return false;
    }
    if (!nocsif_sdcard_lock(BOOTOS_LOCK_MS)) {
        return false;
    }
    bool found = false;
    DIR *d = opendir(NOCSIF_BOOTOS_DIR);
    if (d != NULL) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            const char *dot = strrchr(e->d_name, '.');
            if (dot != NULL && (strcasecmp(dot, ".iso") == 0 || strcasecmp(dot, ".img") == 0)) {
                snprintf(buf, buflen, "%s/%s", NOCSIF_BOOTOS_DIR, e->d_name);
                found = true;
                break;
            }
        }
        closedir(d);
    }
    nocsif_sdcard_unlock();
    return found;
}

const char *nocsif_bootos_kind_str(nocsif_bootos_kind_t kind)
{
    switch (kind) {
    case NOCSIF_BOOTOS_BOOTABLE: return "bootable";
    case NOCSIF_BOOTOS_CD_ONLY:  return "CD-only ISO";
    default:                     return "unknown";
    }
}

nocsif_bootos_kind_t nocsif_bootos_probe(const char *path)
{
    if (path == NULL || path[0] == '\0') {
        return NOCSIF_BOOTOS_UNKNOWN;
    }
    if (!nocsif_sdcard_lock(BOOTOS_LOCK_MS)) {
        return NOCSIF_BOOTOS_UNKNOWN;
    }
    nocsif_bootos_kind_t kind = NOCSIF_BOOTOS_UNKNOWN;
    int fd = open(path, O_RDONLY);
    if (fd >= 0) {
        uint8_t mbr[512];
        bool mbr_sig = false, has_part = false, gpt = false, iso = false;
        if (read(fd, mbr, sizeof mbr) == (ssize_t)sizeof mbr) {
            mbr_sig = (mbr[510] == 0x55 && mbr[511] == 0xAA);
            for (int i = 0; i < 4; i++) {           /* any MBR partition entry with a non-zero type */
                if (mbr[446 + 16 * i + 4] != 0) { has_part = true; break; }
            }
        }
        uint8_t sig[8];
        if (lseek(fd, 512, SEEK_SET) == 512 && read(fd, sig, 8) == 8) {
            gpt = (memcmp(sig, "EFI PART", 8) == 0);     /* GPT header at LBA 1 */
        }
        uint8_t cd[5];
        if (lseek(fd, 0x8001, SEEK_SET) == (off_t)0x8001 && read(fd, cd, 5) == 5) {
            iso = (memcmp(cd, "CD001", 5) == 0);         /* ISO9660 primary volume descriptor */
        }
        close(fd);
        if (mbr_sig && (has_part || gpt)) {
            kind = NOCSIF_BOOTOS_BOOTABLE;               /* whole-disk img or isohybrid iso */
        } else if (iso) {
            kind = NOCSIF_BOOTOS_CD_ONLY;                /* pure CD image — won't boot served as a disk */
        }
    }
    nocsif_sdcard_unlock();
    return kind;
}

int nocsif_bootos_enumerate(nocsif_bootos_entry_t *out, int max)
{
    if (out == NULL || max <= 0) {
        return 0;
    }
    /* List names + sizes under the lock, then probe each AFTER releasing it (probe takes the lock
     * itself — the /sd lock is a plain, non-recursive mutex). */
    int n = 0;
    if (!nocsif_sdcard_lock(BOOTOS_LOCK_MS)) {
        return 0;
    }
    DIR *d = opendir(NOCSIF_BOOTOS_DIR);
    if (d != NULL) {
        struct dirent *e;
        while (n < max && (e = readdir(d)) != NULL) {
            const char *dot = strrchr(e->d_name, '.');
            if (dot == NULL || (strcasecmp(dot, ".iso") != 0 && strcasecmp(dot, ".img") != 0)) {
                continue;
            }
            snprintf(out[n].name, sizeof out[n].name, "%s", e->d_name);
            char full[320];
            snprintf(full, sizeof full, "%s/%s", NOCSIF_BOOTOS_DIR, e->d_name);
            struct stat st;
            out[n].size = (stat(full, &st) == 0) ? (uint64_t)st.st_size : 0;
            out[n].kind = NOCSIF_BOOTOS_UNKNOWN;
            n++;
        }
        closedir(d);
    }
    nocsif_sdcard_unlock();

    for (int i = 0; i < n; i++) {
        char full[320];
        snprintf(full, sizeof full, "%s/%s", NOCSIF_BOOTOS_DIR, out[i].name);
        out[i].kind = nocsif_bootos_probe(full);
    }
    return n;
}

esp_err_t nocsif_bootos_serve(const char *path)
{
    if (s_active) {
        return ESP_ERR_INVALID_STATE;
    }
    if (nocsif_sdcard_card() == NULL) {
        ESP_LOGE(TAG, "serve: no microSD");
        return ESP_ERR_INVALID_STATE;
    }
    if (path == NULL || path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    if (!nocsif_sdcard_lock(BOOTOS_LOCK_MS)) {
        return ESP_ERR_TIMEOUT;
    }
    int fd = open(path, O_RDONLY);
    off_t sz = -1;
    if (fd >= 0) {
        sz = lseek(fd, 0, SEEK_END);
        lseek(fd, 0, SEEK_SET);
    }
    nocsif_sdcard_unlock();

    if (fd < 0) {
        ESP_LOGE(TAG, "serve: open %s failed", path);
        return ESP_ERR_NOT_FOUND;
    }
    if (sz < (off_t)BOOTOS_MIN_BYTES) {
        ESP_LOGE(TAG, "serve: %s too small (%lld bytes) to be a disk image", path, (long long)sz);
        close(fd);
        return ESP_ERR_INVALID_SIZE;
    }

    s_fd = fd;
    s_total_sectors = (uint32_t)(sz / BOOTOS_SECTOR_SIZE);
    snprintf(s_path, sizeof s_path, "%s", path);
    s_active = true;
    ESP_LOGW(TAG, "serving %s as a READ-ONLY USB disk: %u sectors (%.1f MB) — plug into a computer",
             path, (unsigned)s_total_sectors, (double)sz / (1024.0 * 1024.0));

    nocsif_usb_gadget_bootos_request(nocsif_bootos_read, NULL, s_total_sectors, BOOTOS_SECTOR_SIZE);
    return ESP_OK;
}

void nocsif_bootos_stop(void)
{
    if (!s_active && s_fd < 0) {
        return;
    }
    s_active = false;                                  /* read_cb starts refusing */
    nocsif_usb_gadget_request_mode(NOCSIF_USB_MODE_DETACHED);   /* worker drops the disk */
    int fd = s_fd;
    s_fd = -1;
    if (fd >= 0) {
        close(fd);
    }
    s_path[0] = '\0';
    ESP_LOGI(TAG, "stopped serving");
}

bool nocsif_bootos_active(void)
{
    return s_active;
}

const char *nocsif_bootos_current(void)
{
    return s_path;
}
