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

/* Start watching the LVGL task for wedges. Call after the LVGL port is up. A repeating
 * timer pets the task watchdog on the LVGL task's behalf; if LVGL stops running the
 * timer stops firing and the watchdog reboots with LVGL named in the backtrace. No-op
 * in safe mode or if already armed. */
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

/* Human-readable reason this boot happened ("power-on", "panic", "task-wdt", ...). */
const char *nocsif_reliability_reset_reason_str(void);

#ifdef __cplusplus
}
#endif
