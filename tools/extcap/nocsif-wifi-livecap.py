#!/usr/bin/env python3
"""NocSif WiFi live-capture extcap for Wireshark.

Streams 802.11 frames captured by a NocSif watch (WiFi monitor mode) over its USB-CDC
serial port straight into Wireshark as a live capture interface — no SD card round-trip.

The watch emits a standard little-endian PCAP stream (radiotap link-type 127): a 24-byte
global header once, then one radiotap+frame record per captured frame. This script opens
the serial port (which asserts DTR, so the watch (re)starts the stream with a fresh global
header), resynchronises to the PCAP magic, and forwards the byte stream to the capture fifo.

Setup (once):
  1. Install pyserial:            pip install pyserial
  2. Find Wireshark's extcap dir: Wireshark > Help > About > Folders > "Personal Extcap path"
  3. Copy this file there. On macOS/Linux: chmod +x nocsif-wifi-livecap.py
     On Windows: also copy nocsif-wifi-livecap.bat next to it (Wireshark runs .bat, not .py).
  4. In Wireshark, hit the refresh on the capture-interface list. "NocSif WiFi live capture"
     appears; pick the watch's serial port in its gear/options, then start.

On the watch: WiFi > PCAP Capture > "Stream to USB (live)". The watch auto-switches to the
CDC USB mode and waits ("no host") until Wireshark opens the port, then streams ("stream").

Authorized testing only — capture on networks/devices you own or are cleared to test.
"""
import argparse
import sys

INTERFACE    = "nocsif-wifi"
DISPLAY      = "NocSif WiFi live capture"
DLT_RADIOTAP = 127
PCAP_MAGIC   = b"\xd4\xc3\xb2\xa1"   # little-endian PCAP file magic, as it appears on the wire


def eprint(*a):
    print(*a, file=sys.stderr)


def list_serial_ports():
    """(device, description) for each serial port, best-effort (empty if pyserial is absent)."""
    try:
        from serial.tools import list_ports
        return [(p.device, (p.description or p.device)) for p in list_ports.comports()]
    except Exception:
        return []


def extcap_interfaces():
    print("extcap {version=1.0}{display=NocSif}")
    print("interface {value=%s}{display=%s}" % (INTERFACE, DISPLAY))


def extcap_dlts():
    print("dlt {number=%d}{name=IEEE802_11_RADIOTAP}"
          "{display=802.11 plus radiotap header}" % DLT_RADIOTAP)


def extcap_config():
    print("arg {number=0}{call=--port}{display=Serial port}"
          "{tooltip=The watch's USB-CDC serial port}{type=selector}{required=true}")
    for dev, desc in list_serial_ports():
        print("value {arg=0}{value=%s}{display=%s (%s)}" % (dev, dev, desc))
    print("arg {number=1}{call=--baud}{display=Baud (ignored by USB-CDC)}"
          "{tooltip=USB-CDC ignores the line rate; any value works}{type=unsigned}{default=921600}")


def capture(port, baud, fifo):
    try:
        import serial
    except Exception:
        eprint("nocsif extcap: pyserial not installed — run: pip install pyserial")
        return 1
    try:
        ser = serial.Serial(port, baudrate=baud, timeout=1)
    except Exception as ex:
        eprint("nocsif extcap: cannot open %s: %s" % (port, ex))
        return 1

    # Opening the port asserts DTR, which the watch reads as "host present" and (re)starts the
    # stream from a fresh global header. We still resync to the PCAP magic so a mid-stream connect
    # (or leftover bytes) can never hand Wireshark a truncated first record.
    try:
        with open(fifo, "wb") as out:
            synced = False
            window = b""
            while True:
                chunk = ser.read(4096)
                if not chunk:
                    continue
                if synced:
                    out.write(chunk)
                    out.flush()
                    continue
                window += chunk
                idx = window.find(PCAP_MAGIC)
                if idx >= 0:
                    out.write(window[idx:])   # forward from the global header onward
                    out.flush()
                    synced = True
                    window = b""
                elif len(window) > 3:
                    window = window[-3:]      # keep a 3-byte tail so a split magic still matches
    except (BrokenPipeError, KeyboardInterrupt):
        pass                                   # Wireshark stopped the capture — normal exit
    except Exception as ex:
        eprint("nocsif extcap: capture error: %s" % ex)
        return 1
    finally:
        try:
            ser.close()
        except Exception:
            pass
    return 0


def main():
    ap = argparse.ArgumentParser(description=DISPLAY, add_help=False)
    ap.add_argument("--extcap-interfaces", action="store_true")
    ap.add_argument("--extcap-dlts", action="store_true")
    ap.add_argument("--extcap-config", action="store_true")
    ap.add_argument("--extcap-version")
    ap.add_argument("--extcap-interface")
    ap.add_argument("--capture", action="store_true")
    ap.add_argument("--fifo")
    ap.add_argument("--port")
    ap.add_argument("--baud", type=int, default=921600)
    # Wireshark may also pass --extcap-control-in/-out and others; ignore the unknowns.
    args, _ = ap.parse_known_args()

    if args.extcap_interfaces:
        extcap_interfaces()
        return 0

    # All remaining modes are per-interface; ignore requests aimed at another extcap.
    if args.extcap_interface not in (None, INTERFACE):
        return 0

    if args.extcap_dlts:
        extcap_dlts()
        return 0
    if args.extcap_config:
        extcap_config()
        return 0
    if args.capture:
        if not args.fifo or not args.port:
            eprint("nocsif extcap: --capture needs --fifo and --port")
            return 1
        return capture(args.port, args.baud, args.fifo)

    # No recognised action: print the interface list (harmless default).
    extcap_interfaces()
    return 0


if __name__ == "__main__":
    sys.exit(main())
