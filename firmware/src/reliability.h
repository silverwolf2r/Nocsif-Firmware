/*
 * NocSif — public API for crash detection and recovery.
 *
 * Makes freezes and crash loops observable and self-recovering instead of silent:
 *  - a UI-liveness watchdog catches a wedged renderer that the stock idle-task watchdog
 *    wouldn't notice, and reboots with the LVGL task named in the backtrace;
 *  - after a crash, the saved core dump is decoded into a short record (task, PC,
 *    backtrace, build hash) and kept in NVS and the log for the next boot to read;
 *  - repeated crash-class boots that never reach a healthy dwell trip a safe-mode flag
 *    so the caller can skip risky subsystems for that boot.
 *
 * Every entry point here is safe to call before the UI or settings store exist.
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Call first in app_main, before any heavy init. Classifies why this boot happened,
 * records a crash if one occurred, and updates the boot-loop guard. Never fails hard —
 * on any internal error it just logs and leaves safe mode off. */
void nocsif_reliability_boot_check(void);

/* True if the boot-loop guard has tripped (too many crash-class boots in a row without a
 * healthy dwell between them). The caller should skip risky or heavy subsystems this
 * boot. Only meaningful after boot_check(). */
bool nocsif_reliability_safe_mode(void);

/* Fold THIS boot into the safe-mode (minimal-init) path on demand, without touching the persisted
 * crash streak — so after a reboot safe mode is recomputed from the streak alone and a one-off reason
 * never strands the watch. Used by the charge-first boot (main.c): a critically low battery on USB
 * forces the minimal init so the boot's load burst doesn't out-draw the charger and brown-out-loop.
 * After this, nocsif_reliability_safe_mode() returns true for the rest of this boot. */
void nocsif_reliability_force_safe_mode(void);

/* Arm the UI-liveness watchdog. Call AFTER nocsif_ui_init() (needs the LVGL port up). Subscribes the
 * LVGL task to the Task-WDT via a repeating LVGL timer that pets it; if the LVGL task wedges (blocked
 * in the flush wait, or spinning in glyph render) the timer stops firing and the WDT reboots with the
 * LVGL task named in the backtrace. No-op in safe mode or if called twice. */
void nocsif_reliability_ui_liveness_arm(void);

/* Temporarily stop (or resume) watching the LVGL task, for a deliberate operation that
 * legitimately blocks it longer than the watchdog would tolerate — e.g. a flash write
 * that disables the cache the LVGL pet-timer needs. Must always be paired with a resume
 * call, or the LVGL task stays unwatched until reboot. No-op before arming. */
void nocsif_reliability_ui_liveness_suspend(bool suspend);

/* Call once the firmware has reached a stable, working state (e.g. from the main
 * heartbeat). After a short delay this clears the crash streak, so only rapid repeated
 * crashes that never get this far can trip safe mode. Cheap and idempotent. */
void nocsif_reliability_mark_healthy(void);

/* A one-line description of the most recent crash, or "" if none is recorded. Persists
 * across clean reboots. Valid after boot_check(). */
const char *nocsif_reliability_last_crash_str(void);

/* Erase the stored last-crash record (NVS + in-RAM), so a stale crash from an old build stops showing.
 * The core-dump image is already erased at boot-fold time; this clears the persisted summary string.
 * Wired to System > Diagnostics > "Clear crash record". */
void nocsif_reliability_clear_crash(void);

/* Human-readable reset reason for THIS boot ("power-on", "panic", "task-wdt", "brownout", ...). */
const char *nocsif_reliability_reset_reason_str(void);

#ifdef __cplusplus
}
#endif
