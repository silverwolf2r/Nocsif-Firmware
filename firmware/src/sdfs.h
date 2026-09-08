/*
 * NocSif — shared /sd file-service rules (§4.8a P4 companion browser + §4.15 desktop bridge).
 *
 * ONE set of rules for every remote view of the microSD, so the phone page and the desktop tool can
 * never disagree about what is reachable or how the card is touched:
 *   - the /sd JAIL: absolute paths under /sd, no "." / ".." / empty segments, no control chars or
 *     backslashes, length-capped; a too-long path is REJECTED, never truncated onto another name;
 *   - card OWNERSHIP: every operation CLAIMS the card away from File Share for its duration
 *     (nocsif_usb_gadget_claim_sd — refused, with the reason, while a host has the drive);
 *   - the FAT LOCK (nocsif_sdcard_lock) is held only around the actual reads/writes — never across a
 *     socket send or a console write — so the rest of the watch keeps its short card accesses.
 * Callers do their own transport (HTTP chunks / console lines) around these primitives.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NOCSIF_SDFS_ROOT      "/sd"
#define NOCSIF_SDFS_PATH_MAX  256     /* a full path incl. NUL (matches the on-watch Files screen)  */
#define NOCSIF_SDFS_NAME_MAX  96      /* one entry name incl. NUL (matches the on-watch Files screen) */
#define NOCSIF_SDFS_LIST_MAX  200     /* entries per listing snapshot before "truncated"              */

typedef struct {
    char     name[NOCSIF_SDFS_NAME_MAX];
    uint32_t size;
    bool     is_dir;
} nocsif_sdfs_ent_t;

/* The jail: "/sd" (only if allow_root) or "/sd/<seg>/<seg>..." with sane segments. */
bool nocsif_sdfs_path_ok(const char *path, bool allow_root);

/* A NEW file/folder name: one segment, FAT-legal, not a dotfile (the browsers hide dotfiles). */
bool nocsif_sdfs_name_ok(const char *name);

/* Own the card for an operation (away from File Share). NULL = ok, else a short reason for the
 * client ("File Share has the card" / "microSD unavailable"). Pair with nocsif_sdfs_release(). */
const char *nocsif_sdfs_claim(void);
void        nocsif_sdfs_release(void);

/* Content type by extension (download headers). */
const char *nocsif_sdfs_mime(const char *name);

/* Snapshot a directory into ents (caller-provided, max entries) — claim + lock inside; dirs first,
 * case-insensitive by name, dotfiles hidden, stat() authoritative over d_type. NULL = ok (n / trunc
 * filled), else the reason ("folder unavailable", or a claim reason). */
const char *nocsif_sdfs_list(const char *path, nocsif_sdfs_ent_t *ents, int max, int *n, bool *trunc);

/* Card facts: present (raw card initialised) + FAT totals in bytes (0 when the volume can't be
 * read, e.g. File Share has it). */
void nocsif_sdfs_info(bool *present, uint64_t *total, uint64_t *free_bytes);

/* Create the canonical NocSif folder set (+ a README.txt describing it) — only what is missing.
 * NULL = ok (*made = folders/files created), else the reason. */
const char *nocsif_sdfs_provision(int *made);

/* Reformat the card (FAT, fresh). EVERYTHING on the card is lost. NULL = ok, else the reason.
 * Takes the lock for the whole operation; the caller is expected to have confirmed with the user. */
const char *nocsif_sdfs_format(void);

#ifdef __cplusplus
}
#endif
