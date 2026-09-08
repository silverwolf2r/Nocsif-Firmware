r"""
NocSif Desktop Bridge — protocol client (PLAN §4.15).

Talks to the watch's USB-Serial/JTAG console (COM7 / /dev/ttyACM* / /dev/cu.usbmodem*), the one USB
channel that is always alive. One JSON object per line goes in; the watch answers with lines prefixed
"NB>" (see firmware/src/bridge.h). Long answers arrive as base64 fragment lines {"id":N,"d":"…"},
listings as {"id":N,"e":{…}} entry lines, health as {"id":N,"chk":{…}} lines, always terminated by
{"id":N,"ok":…,"end":true,…}. Everything that is not an NB> line is the ordinary log stream and is
handed to an optional callback (the app's live log tail).

Only pyserial is needed here; esptool / requests are used by flasher.py and updater.py.
"""
import base64
import json
import os
import struct
import threading
import time
import zlib

import serial                      # pyserial
from serial.tools import list_ports

ESP32S3_USJ = (0x303A, 0x1001)     # Espressif USB-Serial/JTAG (the ESP32-S3 native port)
PREFIX = "NB>"
CHUNK = 8192          # fs.get payload per request (the watch paces its replies; any size is safe)
PUT_CHUNK = 2400      # fs.put payload per request: ~3.3 KB line, inside the watch's 4 KB console RX ring
                      # (the driver's ISR drops bytes if the ring fills); a lost chunk is retried
PUT_RETRIES = 3


class BridgeError(Exception):
    pass


def find_ports():
    """All serial ports, ESP32-S3 USB-Serial/JTAG ports first. Returns [(device, description)]."""
    found = []
    for p in list_ports.comports():
        score = 0
        if (p.vid, p.pid) == ESP32S3_USJ:
            score = 2
        elif p.vid == ESP32S3_USJ[0]:
            score = 1
        found.append((score, p.device, p.description or ""))
    found.sort(key=lambda t: (-t[0], t[1]))
    return [(d, desc) for _, d, desc in found]


def default_port():
    ports = find_ports()
    return ports[0][0] if ports else None


class Bridge:
    """A connection to one watch. Thread-safe per command (one command at a time)."""

    def __init__(self, port, log_cb=None, timeout=10.0):
        self.port = port
        self.log_cb = log_cb
        self.timeout = timeout
        self._lock = threading.Lock()
        self._id = 0
        self._ser = serial.Serial()
        self._ser.port = port
        self._ser.baudrate = 115200
        self._ser.timeout = 0.2
        self._ser.dtr = False          # the native USB-Serial/JTAG toggles reset on a DTR/RTS pulse —
        self._ser.rts = False          # keep both low BEFORE open so connecting never reboots the watch
        self._ser.open()
        self._buf = b""

    def close(self):
        try:
            self._ser.close()
        except Exception:
            pass

    # ---- line I/O ------------------------------------------------------------------------------
    def _readline(self, deadline):
        """One raw line (bytes, no newline) or None at the deadline."""
        while True:
            i = self._buf.find(b"\n")
            if i >= 0:
                line, self._buf = self._buf[:i], self._buf[i + 1:]
                return line.rstrip(b"\r")
            if time.time() >= deadline:
                return None
            # read(1) blocks (up to the port timeout) for the first byte, then take whatever else is
            # waiting — read(4096) would sit out the whole timeout for a 60-byte reply.
            chunk = self._ser.read(1)
            if chunk:
                waiting = self._ser.in_waiting
                if waiting:
                    chunk += self._ser.read(waiting)
                self._buf += chunk

    def pump_log(self, seconds=0.0):
        """Drain pending log lines to the callback (the live tail while no command is in flight).
        Skips silently when a command holds the port, so it never eats a reply line."""
        if not self._lock.acquire(timeout=seconds):
            return
        try:
            deadline = time.time() + seconds
            while True:
                line = self._readline(deadline)
                if line is None:
                    return
                at = line.rfind(PREFIX.encode())
                if at >= 0:                           # a stray reply from a timed-out command
                    if at > 0:
                        self._dispatch_other(line[:at])
                    continue
                self._dispatch_other(line)
        finally:
            self._lock.release()

    def _dispatch_other(self, line):
        if self.log_cb:
            try:
                self.log_cb(line.decode("utf-8", "replace"))
            except Exception:
                pass

    def request(self, cmd, timeout=None, **args):
        """Send one command; return (final_dict, fragments_bytes, entries, checks).
        "id" and "c" are the envelope — a command argument must never use those keys (the launch target
        is "app" for exactly that reason)."""
        if "id" in args or "c" in args:
            raise BridgeError("argument name %s collides with the envelope" % ("id" if "id" in args else "c"))
        with self._lock:
            self._id += 1
            rid = self._id
            req = dict(args)
            req["id"] = rid
            req["c"] = cmd
            self._ser.write((json.dumps(req, separators=(",", ":")) + "\n").encode("utf-8"))
            self._ser.flush()
            deadline = time.time() + (timeout or self.timeout)
            frags, entries, checks = [], [], []
            while True:
                line = self._readline(deadline)
                if line is None:
                    raise BridgeError("timeout waiting for %s" % cmd)
                # The watch's log goes into the same console char by char, so a log line can wrap
                # around a reply: "I (12) nocsif: heaNB>{...}" — the reply starts at the LAST marker,
                # never at column 0 by guarantee. Anything before the marker is log text.
                at = line.rfind(PREFIX.encode())
                if at < 0:
                    self._dispatch_other(line)
                    continue
                if at > 0:
                    self._dispatch_other(line[:at])
                try:
                    msg = json.loads(line[at + len(PREFIX):].decode("utf-8", "replace"))
                except ValueError:
                    continue
                if msg.get("id") != rid:
                    continue                          # a stale reply from an earlier, timed-out command
                if "d" in msg:
                    frags.append(base64.b64decode(msg["d"]))
                elif "e" in msg:
                    entries.append(msg["e"])
                elif "chk" in msg:
                    checks.append(msg["chk"])
                if msg.get("end"):
                    if not msg.get("ok", False):
                        raise BridgeError(msg.get("err", "failed"))
                    return msg, b"".join(frags), entries, checks

    # ---- commands ------------------------------------------------------------------------------
    def ping(self):
        return self.request("ping", timeout=3.0)[0]

    def version(self):
        return self.request("version")[0]

    def status(self):
        return self.request("status")[0]

    def health(self):
        final, _, _, checks = self.request("health", timeout=20.0)
        return final, checks

    def test(self, which):
        return self.request("test", timeout=15.0, t=which)[0]

    def state(self):
        return self.request("state")[0].get("state", {})

    def menu(self):
        _, blob, _, _ = self.request("menu")
        return json.loads(blob.decode("utf-8", "replace") or "{}")

    def screenshot(self):
        """Returns (width, height, rgb565le_bytes)."""
        final, blob, _, _ = self.request("screenshot", timeout=30.0)
        return final["w"], final["h"], blob

    def mirror_poll(self, seq, full=False, touch=None):
        """One live-view poll. Returns the final dict (none:true when nothing changed, else
        seq/x/y/w/h/raw) and the decoded RGB565-LE rectangle bytes (b'' when none). `touch` = (x, y,
        pressed) in watch pixels rides the same round trip."""
        args = {"seq": seq, "full": 1 if full else 0}
        if touch is not None:
            args["t"] = [int(touch[0]), int(touch[1]), int(touch[2])]
        final, blob, _, _ = self.request("mirror", timeout=6.0, **args)
        if final.get("none"):
            return final, b""
        raw = rle565_decode(blob, int(final["raw"]))
        return final, raw

    def log_tail(self, n=2048):
        _, blob, _, _ = self.request("log.tail", n=n)
        return blob.decode("utf-8", "replace")

    def sd_info(self):
        return self.request("sd.info")[0]

    def sd_provision(self):
        return self.request("sd.provision", timeout=20.0)[0]

    def sd_format(self):
        return self.request("sd.format", timeout=180.0, confirm=1)[0]

    def ls(self, path="/sd"):
        final, _, entries, _ = self.request("fs.ls", timeout=20.0, p=path)
        return final, entries

    def rm(self, path):
        return self.request("fs.rm", p=path)[0]

    def mkdir(self, path):
        return self.request("fs.mkdir", p=path)[0]

    def get(self, remote, local, progress=None):
        """Download remote → local file. progress(done, total) optional. A chunk that arrives with
        fewer bytes than the watch says it sent (a lost fragment line) is re-requested."""
        off = 0
        total = None
        with open(local, "wb") as fh:
            while True:
                final, blob, _, _ = self.request("fs.get", timeout=30.0, p=remote, off=off, len=CHUNK)
                total = final.get("size", total)
                sent = int(final.get("n", len(blob)))
                if len(blob) != sent:
                    continue                          # incomplete reply — ask for the same offset again
                fh.write(blob)
                off += len(blob)
                if progress:
                    progress(off, total or off)
                if len(blob) < CHUNK or (total is not None and off >= total):
                    break
        return off

    def put(self, local, remote, progress=None, _restarts=1):
        """Upload local file → remote path (via <remote>.part, renamed when complete). A chunk whose
        reply was lost is retried (the watch treats an already-written chunk as success); if the
        watch dropped the session meanwhile ("offset mismatch") the upload restarts once from 0."""
        size = os.path.getsize(local)
        off = 0
        with open(local, "rb") as fh:
            while True:
                data = fh.read(PUT_CHUNK)
                final = (off + len(data)) >= size
                last_err = None
                for attempt in range(PUT_RETRIES):
                    try:
                        self.request("fs.put", timeout=6.0, p=remote, off=off, final=1 if final else 0,
                                     d=base64.b64encode(data).decode("ascii"))
                        last_err = None
                        break
                    except BridgeError as e:
                        last_err = e
                        if "offset mismatch" in str(e):
                            if _restarts > 0:
                                return self.put(local, remote, progress, _restarts - 1)
                            raise
                        if "bad path" in str(e) or "bad file name" in str(e):
                            raise
                if last_err:
                    raise last_err
                off += len(data)
                if progress:
                    progress(off, size)
                if final:
                    break
        return off

    def ctl(self, action, **args):
        return self.request("ctl", a=action, **args)[0]

    def usb(self, mode):
        return self.request("usb", mode=mode)[0]

    def reboot(self):
        return self.request("reboot", timeout=3.0)[0]


# ---- helpers ------------------------------------------------------------------------------------
def rle565_decode(data, raw_len):
    """PackBits over 16-bit pixels (firmware rle565_encode): 0x80..0xFF = repeat (n & 0x7F) + 2 pixels,
    0x00..0x7F = n + 1 literal pixels."""
    out = bytearray()
    i, n = 0, len(data)
    while i < n and len(out) < raw_len:
        c = data[i]
        i += 1
        if c & 0x80:
            out += data[i:i + 2] * ((c & 0x7F) + 2)
            i += 2
        else:
            k = (c + 1) * 2
            out += data[i:i + k]
            i += k
    return bytes(out[:raw_len])


_LUT565 = None


def rgb565_to_rgb888(data):
    """RGB565-LE bytes → RGB888 bytes via a one-time 65536-entry table (fast enough for a full frame
    in pure Python — ~25 ms — and instant for partial rectangles)."""
    global _LUT565
    if _LUT565 is None:
        _LUT565 = [bytes((((p >> 11) & 31) << 3 | ((p >> 11) & 31) >> 2,
                          ((p >> 5) & 63) << 2 | ((p >> 5) & 63) >> 4,
                          (p & 31) << 3 | (p & 31) >> 2)) for p in range(65536)]
    lut = _LUT565
    px = struct.unpack("<%dH" % (len(data) // 2), data[: (len(data) // 2) * 2])
    return b"".join([lut[p] for p in px])


def ppm_from_rgb565(width, height, data):
    """A binary PPM (Tk decodes it natively) from an RGB565-LE rectangle."""
    return b"P6 %d %d 255\n" % (width, height) + rgb565_to_rgb888(data)


def rgb565_to_png(width, height, data):
    """A minimal PNG encoder (stdlib only) for the screenshot frames."""
    rows = []
    for y in range(height):
        row = bytearray([0])                      # filter type 0
        base = y * width * 2
        for x in range(width):
            px = data[base + x * 2] | (data[base + x * 2 + 1] << 8)
            r, g, b = (px >> 11) & 31, (px >> 5) & 63, px & 31
            row += bytes(((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)))
        rows.append(bytes(row))

    def chunk(tag, payload):
        return (struct.pack(">I", len(payload)) + tag + payload +
                struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF))

    ihdr = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) +
            chunk(b"IDAT", zlib.compress(b"".join(rows), 6)) + chunk(b"IEND", b""))


def human_size(n):
    n = float(n or 0)
    for unit in ("B", "KB", "MB", "GB"):
        if n < 1024 or unit == "GB":
            return ("%d %s" if unit == "B" else "%.1f %s") % (n, unit)
        n /= 1024.0
    return "%.1f GB" % n
