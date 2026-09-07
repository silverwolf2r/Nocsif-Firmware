/*
 * NocSif — reliability hardening (Phase A).
 *
 * Turns silent freezes and crash loops into observable, self-recovering events:
 *  - Task-WDT PANIC (sdkconfig) reboots on a hang instead of freezing forever; this module adds a
 *    UI-liveness watchdog so a BLOCKED render wedge (the DMA-hang class, which does NOT starve the
 *    idle task the stock WDT watches) also trips it.
 *  - On the next boot after a crash, the ESP-IDF core dump (to the 'coredump' flash partition) is
 *    read back into a compact record (task / PC / backtrace PCs / app-ELF sha) and stashed to NVS +
 *    the serial log, then erased so the next crash can write.
 *  - A boot-loop guard counts crash-class boots that don't survive a short healthy dwell; past a
 *    threshold it reports safe mode so the caller skips the risky/heavy subsystems for one boot.
 *
 * All entry points are safe to call before the UI and the settings store are up (this module brings
 * up its own small NVS handle). LVGL work happens only inside nocsif_reliability_ui_liveness_arm().
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Call FIRST in app_main, before any heavy init. Classifies the reset reason, folds a stored core
 * dump into the last-crash record (NVS + serial), and runs the boot-loop guard. Non-fatal
 * throughout: on any internal failure it logs and leaves safe mode off. */
void nocsif_reliability_boot_check(void);

/* True when the boot-loop guard tripped this boot (>= threshold crash-class boots without a healthy
 * dwell in between). The caller should bring up only a minimal, serviceable environment this boot
 * (skip USB / macro playback / other risky-or-heavy subsystems). Valid after boot_check(). */
bool nocsif_reliability_safe_mode(void);

/* Arm the UI-liveness watchdog. Call AFTER nocsif_ui_init() (needs the LVGL port up). Subscribes the
 * LVGL task to the Task-WDT via a repeating LVGL timer that pets it; if the LVGL task wedges (blocked
 * in the flush wait, or spinning in glyph render) the timer stops firing and the WDT reboots with the
 * LVGL task named in the backtrace. No-op in safe mode or if called twice. */
void nocsif_reliability_ui_liveness_arm(void);

/* Pause / resume the UI-liveness watchdog's watch on the LVGL task. Call around a long, DELIBERATE
 * operation that legitimately blocks the LVGL task longer than the WDT timeout — e.g. an OTA flash
 * erase/write, which disables the cache the LVGL task's pet-timer runs from, so the task cannot pet
 * even though nothing is wrong. Pause unsubscribes the LVGL task from the Task-WDT; resume re-adds it.
 * No-op until the watchdog is armed. MUST be balanced (resume when done), or the LVGL task stays
 * unwatched until the next reboot. */
void nocsif_reliability_ui_liveness_suspend(bool suspend);

/* Signal that the firmware reached a stable, serviceable state. After a short healthy dwell this
 * clears the crash streak, so only *rapid repeated* crashes (that never reach this point) trip safe
 * mode. Idempotent + cheap; call it once uptime crosses the dwell (e.g. from the heartbeat loop). */
void nocsif_reliability_mark_healthy(void);

/* The most recent crash record as a compact one-line string (persisted across clean reboots), or ""
 * if none has ever been recorded. Points at internal storage; valid after boot_check(). For the
 * System > Diagnostics screen (A2). */
const char *nocsif_reliability_last_crash_str(void);

/* Human-readable reset reason for THIS boot ("power-on", "panic", "task-wdt", "brownout", ...). */
const char *nocsif_reliability_reset_reason_str(void);

#ifdef __cplusplus
}
#endif
