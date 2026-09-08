/*
 * NocSif — §4.15 desktop bridge: the firmware seam for the computer-side tool (tools/nocsif_bridge).
 *
 * Transport = the USB-Serial/JTAG console (COM7 / /dev/ttyACM* / /dev/cu.usbmodem*), the ONE USB
 * channel that is always alive (the TinyUSB gadget replaces it when a USB mode is picked, and esptool
 * uses the same port). The host sends one JSON object per line; the watch answers with one or more
 * lines prefixed "NB>" so they are trivially separated from the ordinary log stream:
 *
 *   host  → {"id":7,"c":"version"}
 *   watch → NB>{"id":7,"ok":true,"end":true,"version":"…",…}
 *
 * Long answers (file chunks, screenshots, the menu tree, the log tail) travel as base64 FRAGMENT lines
 * {"id":7,"d":"…"} followed by the terminating {"id":7,"ok":true,"end":true,…}; every line stays
 * under ~1 KB so a log line from another task can only land BETWEEN reply lines, never inside one.
 *
 * Commands (all replies carry the request id):
 *   ping · version · status · health · test {t:tone|nfc|lora|gnss} · state · menu · screenshot
 *   mirror {seq, full, t:[x,y,pressed]}  (live view: the changed rectangle as RLE, or "none")
 *   sd.info · sd.provision · sd.format
 *   fs.ls {p} · fs.get {p,off,len} · fs.put {p,off,final,d} · fs.rm {p} · fs.mkdir {p}
 *   ctl {a:launch|back|home|type|key|bright|vol|button|touch|cast, …}
 *   log.tail {n} · usb {mode} · reboot
 *
 * Everything runs on ONE PSRAM-stacked task; file access follows sdfs.h (jail / claim / short locks);
 * UI commands ride the companion dispatch (marshalled onto the LVGL task); nothing here touches LVGL
 * except the screenshot, which takes the port lock explicitly.
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Install the interrupt-driven USB-Serial/JTAG console driver (small internal RX/TX rings) and route
 * stdio through it, so the bridge can READ host lines at USB speed (the driverless console polls a
 * 64-byte FIFO). Logging keeps its fail-fast semantics: the console write returns at once when no host
 * is attached. Call FIRST in app_main so the rings sit under the boot reserves. Returns false (bridge
 * disabled, console unchanged) if the driver can't be installed. */
bool nocsif_bridge_console_init(void);

/* Start the bridge task (no-op if the console driver isn't installed). Call once the UI + workers are
 * up. Always started — in safe mode too, since that is exactly when a host wants to look inside. */
void nocsif_bridge_init(void);

#ifdef __cplusplus
}
#endif
