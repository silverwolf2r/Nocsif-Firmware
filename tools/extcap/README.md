# NocSif WiFi live capture (Wireshark extcap)

Stream 802.11 frames the watch captures in monitor mode straight into Wireshark over
USB-CDC — a live capture, no SD-card round-trip.

The watch emits a standard little-endian PCAP stream (radiotap link-type 127). This extcap
opens the watch's serial port, resynchronises to the PCAP magic, and forwards the bytes to
Wireshark's capture fifo. Because the on-wire format is a real PCAP stream, the host side is
a thin pump; the watch does the capture.

## Prerequisites

- Wireshark 3.x/4.x
- Python 3 with **pyserial**: `pip install pyserial`

## Install

1. Find Wireshark's extcap folder: **Help ▸ About Wireshark ▸ Folders ▸ "Personal Extcap path"**
   (e.g. `%APPDATA%\Wireshark\extcap` on Windows, `~/.config/wireshark/extcap` on Linux,
   `~/.local/lib/wireshark/extcap` or `~/.config/wireshark/extcap` on macOS).
2. Copy `nocsif-wifi-livecap.py` into that folder.
   - **Windows:** also copy `nocsif-wifi-livecap.bat` next to it (Wireshark runs `.bat`, not `.py`).
   - **macOS/Linux:** `chmod +x nocsif-wifi-livecap.py`.
3. In Wireshark, refresh the interface list (or restart). **"NocSif WiFi live capture"** appears.

## Use

1. On the watch: **WiFi ▸ PCAP Capture ▸ "Stream to USB (live)"**. The watch auto-switches to
   the CDC USB mode and shows **"waiting for host"** until Wireshark connects.
2. In Wireshark, open the interface's options (gear icon), pick the watch's **serial port**,
   and start the capture. The watch flips to **"streaming to USB"** and frames arrive live.
3. Use the on-watch Monitor screen to hop or lock a channel while capturing.

Stop from either side: stop the Wireshark capture, or tap **"Stop streaming"** on the watch.

## Notes & limits

- **One sink at a time.** The live stream and *Record to SD* share the single capture writer —
  start one or the other, not both. The screen greys the row that isn't in use.
- **Throughput.** USB-CDC full-speed carries roughly ~1 MB/s. On a busy channel the on-watch
  ring absorbs bursts and drops the overflow; the **dropped** counter on the PCAP screen shows it.
  Lock a single channel (Monitor screen) for the most complete capture of one network.
- **Snap length.** Frames are captured up to the watch's snaplen (400 bytes) — enough for
  management/EAPOL analysis; large data payloads are truncated (radiotap `orig_len` preserved).
- **Reconnect.** Closing Wireshark drops DTR; the watch returns to "waiting for host". Re-starting
  the capture makes the watch emit a fresh PCAP header, so the new session is always well-framed.
- **Line rate.** The *Baud* option is cosmetic — USB-CDC ignores it. Any value works.

## Troubleshooting

- **Interface missing:** confirm the file is in the *Personal Extcap path* and (Windows) the `.bat`
  is present. From a shell, `python nocsif-wifi-livecap.py --extcap-interfaces` should print an
  `interface {value=nocsif-wifi}...` line.
- **"pyserial not installed":** `pip install pyserial` into the same Python the `.bat`/shebang runs.
- **No packets / port busy:** close other serial monitors holding the port. Make sure the watch
  shows "streaming" (it needs "Stream to USB (live)" armed).
- **Port not listed:** the port dropdown is populated when the options dialog opens. Plug the
  watch in (and arm "Stream to USB (live)") *before* opening the interface options, or close and
  reopen the options dialog to re-enumerate.

Authorized testing only — capture on networks and devices you own or are explicitly cleared to test.
