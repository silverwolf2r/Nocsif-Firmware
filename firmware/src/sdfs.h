/*
 * NocSif — shared rule set for any remote view of the microSD card (phone browser and
 * desktop bridge alike), so both agree on what's reachable and how the card is accessed:
 * paths are jailed to /sd with no traversal tricks, every operation claims the card away
 * from File Share for its duration, and the FAT lock is held only around the actual I/O
 * rather than across a network send. Callers handle their own transport around these.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NOCSIF_SDFS_ROOT      "/sd"
#define NOCSIF_SDFS_PATH_MAX  256     /* max full path length including the NUL terminator */
#define NOCSIF_SDFS_NAME_MAX  96      /* max single entry name including the NUL terminator */
#define NOCSIF_SDFS_LIST_MAX  200     /* max entries in one listing snapshot before truncating */

typedef struct {
    char     name[NOCSIF_SDFS_NAME_MAX];
    uint32_t size;
    bool     is_dir;
} nocsif_sdfs_ent_t;

/* Whether `path` is a legal path within the jail — "/sd" itself only if allow_root, or
 * "/sd/<segment>/<segment>..." with no traversal or illegal characters in any segment. */
bool nocsif_sdfs_path_ok(const char *path, bool allow_root);

/* Whether `name` is a legal single new file/folder name: one segment, FAT-legal
 * characters, and not a dotfile (browsers hide those). */
bool nocsif_sdfs_name_ok(const char *name);

/* Claim the card away from File Share for the duration of an operation. Returns NULL on
 * success, or a short reason string for the client on failure. Always pair with a
 * matching nocsif_sdfs_release() call. */
const char *nocsif_sdfs_claim(void);
void        nocsif_sdfs_release(void);

/* Guess a Content-Type for `name` from its file extension. */
const char *nocsif_sdfs_mime(const char *name);

/* List a directory's entries into the caller-provided `ents` array (up to `max`),
 * claiming and locking the card internally. Directories sort first, then
 * case-insensitive by name; dotfiles are skipped. Returns NULL on success (with *n and
 * *trunc filled in), or a reason string on failure. */
const char *nocsif_sdfs_list(const char *path, nocsif_sdfs_ent_t *ents, int max, int *n, bool *trunc);

/* Report whether a card is present, and its total/free bytes (0 if the volume can't
 * currently be read, e.g. while File Share holds it). */
void nocsif_sdfs_info(bool *present, uint64_t *total, uint64_t *free_bytes);

/* Create any of the standard NocSif folders (and a README describing them) that don't
 * already exist. Returns NULL on success (with *made set to the count created), or a
 * reason string on failure. */
const char *nocsif_sdfs_provision(int *made);

/* Reformat the card, erasing everything on it. Returns NULL on success, or a reason
 * string on failure. The caller is expected to have already confirmed with the user. */
const char *nocsif_sdfs_format(void);

#ifdef __cplusplus
}
#endif
