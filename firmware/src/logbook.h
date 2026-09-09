/*
 * NocSif — persistent rolling log (Phase A / A2).
 *
 * Mirrors the ESP_LOG stream to the 'logs' flash partition so recent
 * activity plus the crash records survive a reboot and are readable
 * untethered (System > Diagnostics). A sector-based, sequence-numbered
 * ring: log lines accumulate in a small RAM buffer and a low-priority task
 * commits full-ish buffers to the next flash sector; on boot the ring head
 * is found by scanning sector headers for the highest sequence number. Raw
 * partition access (no filesystem), so an unclean reboot can at worst lose
 * the last un-committed RAM buffer, never corrupt the ring.
 */
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Finds the 'logs' partition, scans for the ring head, installs the
 * ESP_LOG tee, and starts the flush task. Call early in app_main (right
 * after the banner) so the crash record + boot logs are captured. Non-fatal:
 * if the partition is missing it logs and disables itself (console logging
 * is unaffected). */
void nocsif_logbook_init(void);

/* Appends one line explicitly (a trailing newline is ensured). Thread-safe; safe before init (no-op). */
void nocsif_logbook_append(const char *line);

/* Forces the current RAM buffer to flash now (e.g. before a deliberate reboot). Thread-safe. */
void nocsif_logbook_flush(void);

/* Copies the most recent log bytes (newest flushed sector + the
 * un-committed RAM buffer) into out, NUL terminated. Returns the number of
 * bytes written (excluding the NUL). For the Diagnostics screen. */
size_t nocsif_logbook_read_tail(char *out, size_t out_sz);

/* Erases the entire ring (Diagnostics "clear"). Thread-safe. */
void nocsif_logbook_clear(void);

#ifdef __cplusplus
}
#endif
