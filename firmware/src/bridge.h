/*
 * NocSif §4.15 desktop bridge — the firmware side of the computer-side tool (tools/nocsif_bridge).
 *
 * Transport is the USB-Serial/JTAG console (COM7 / /dev/ttyACM* / /dev/cu.usbmodem*) — the one USB
 * channel that's always alive, since a picked USB gadget mode replaces it and esptool shares the
 * same port. The host sends one JSON object per line; the watch replies with one or more lines
 * prefixed "NB>" so they're easy to pick out of the ordinary log stream:
 *
 *   host  → {"id":7,"c":"version"}
 *   watch → NB>{"id":7,"ok":true,"end":true,"version":"…",…}
 *
 * Long answers (file chunks, screenshots, the menu tree, log tails) travel as base64 fragment lines
 * {"id":7,"d":"…"} followed by a terminating {"id":7,"ok":true,"end":true,…}; each line stays under
 * ~1 KB so a log line from another task can only ever land between two reply lines, never inside one.
 *
 * Commands (every reply carries back the request id):
 *   ping · version · status · health · test {t:tone|nfc|lora|gnss} · state · menu · screenshot
 *   mirror {seq, full, t:[x,y,pressed]}  (live view: the changed rectangle as RLE, or "none")
 *   sd.info · sd.provision · sd.format
 *   fs.ls {p} · fs.get {p,off,len} · fs.put {p,off,final,d} · fs.rm {p} · fs.mkdir {p}
 *   ctl {a:launch|back|home|type|key|bright|vol|button|touch|cast, …}
 *   log.tail {n} · usb {mode} · reboot
 *
 * Everything runs on a single PSRAM-stacked task; file access goes through sdfs.h (path jailing,
 * claiming, short locks); UI commands ride the companion dispatch onto the LVGL task; nothing here
 * touches LVGL directly except the screenshot, which takes the port lock itself.
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Installs the interrupt-driven USB-Serial/JTAG console driver and routes stdio through it, so the
 * bridge can read host lines at USB speed (the polling console can only manage a 64-byte FIFO).
 * Logging keeps failing fast when nothing is attached. Call first in app_main so its ring buffers
 * are allocated before the other boot reserves. Returns false (bridge stays disabled, console left
 * as-is) if the driver can't be installed. */
bool nocsif_bridge_console_init(void);

/* Starts the bridge task (a no-op if the console driver never installed). Call once the UI and
 * workers are up. Always started, even in safe mode, since that's exactly when a host most wants
 * to inspect the watch. */
void nocsif_bridge_init(void);

#ifdef __cplusplus
}
#endif
