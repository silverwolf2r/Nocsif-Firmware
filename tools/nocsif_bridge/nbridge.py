r"""
NocSif Desktop Bridge — protocol client (PLAN §4.15).

Talks over the watch's USB-Serial/JTAG console (COM7 / /dev/ttyACM* / /dev/cu.usbmodem*), which is the
one USB channel that's always available. Each request is one JSON object per line in; the watch answers
with lines prefixed "NB>" (see firmware/src/bridge.h). Long replies arrive split into base64 fragment
lines {"id":N,"d":"…"}, directory listings as {"id":N,"e":{…}} entry lines, health results as
{"id":N,"chk":{…}} lines, and every reply is closed by a {"id":N,"ok":…,"end":true,…} line. Anything
that isn't an NB> line is ordinary log output and is handed to an optional callback (used for the app's
live log tail).

Only pyserial is required here; esptool and requests are used by flasher.py and updater.py instead.
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

ESP32S3_USJ = (0x303A, 0x1001)     # USB vid/pid for Espressif's native USB-Serial/JTAG on the ESP32-S3
PREFIX = "NB>"
CHUNK = 8192          # bytes requested per fs.get call (the watch paces its own replies, so any size works)
PUT_CHUNK = 2400      # bytes sent per fs.put call: ~3.3 KB line, kept under the watch's 4 KB console RX ring
                      # buffer (its ISR drops bytes once that ring fills); a dropped chunk gets retried
PUT_RETRIES = 3


class BridgeError(Exception):
    pass


def find_ports():
    """Lists all serial ports, with the ESP32-S3's USB-Serial/JTAG ports sorted first. Returns [(device, description)]."""
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
    """Represents a live connection to one watch. Safe for one command in flight at a time."""

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
        self._ser.dtr = False          # a DTR/RTS pulse resets the native USB-Serial/JTAG port, so both
        self._ser.rts = False          # are forced low BEFORE open() so simply connecting never reboots the watch
        self._ser.open()
        self._buf = b""

    def close(self):
        try:
            self._ser.close()
        except Exception:
            pass

    # ---- line I/O ------------------------------------------------------------------------------
    def _readline(self, deadline):
        """Returns one raw line (bytes, newline stripped), or None once the deadline passes."""
        while True:
            i = self._buf.find(b"\n")
            if i >= 0:
                line, self._buf = self._buf[:i], self._buf[i + 1:]
                return line.rstrip(b"\r")
            if time.time() >= deadline:
                return None
            # read(1) blocks up to the port timeout waiting for the first byte, then grabs whatever
            # else is already buffered — a plain read(4096) would sit out the full timeout for a reply
            # that's only 60 bytes long.
            chunk = self._ser.read(1)
            if chunk:
                waiting = self._ser.in_waiting
                if waiting:
                    chunk += self._ser.read(waiting)
                self._buf += chunk

    def pump_log(self, seconds=0.0):
        """Drains any pending log lines out to the callback — this is the live tail running while no
        command is currently in flight. Does nothing (silently) if a command already holds the lock,
        so it can never steal a reply line out from under it."""
        if not self._lock.acquire(timeout=seconds):
            return
        try:
            deadline = time.time() + seconds
            while True:
                line = self._readline(deadline)
                if line is None:
                    return
                at = line.rfind(PREFIX.encode())
                if at >= 0:                           # a leftover reply from a request that already timed out
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
        """Sends one command and returns (final_dict, fragments_bytes, entries, checks).
        "id" and "c" are reserved for the request envelope, so a command argument must never reuse
        either name — this is exactly why the launch target argument is called "app" instead of "id"."""
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
                # The watch's log text shares the same console byte-for-byte, so a log line can wrap
                # right around a reply — e.g. "I (12) nocsif: heaNB>{...}" — so the reply always starts
                # at the LAST marker on the line, never guaranteed to be at column 0. Whatever comes
                # before that marker is ordinary log text.
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
                    continue                          # a stale reply left over from an earlier timed-out command
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

    def mirror_poll(self, seq, full=False, touch=None, scale=1):
        """Runs one poll of the live-view protocol. Returns the final dict (none:true if nothing
        changed, else seq/x/y/w/h/scale/raw) plus the decoded RGB565-LE bytes for that rectangle
        (b'' when none changed). `touch` = (x, y, pressed) in watch-panel pixels piggybacks on the same
        round trip. scale 1 requests the panel's native 410×502 resolution; scale 2 halves it (a
        quarter of the data) for a slow link."""
        args = {"seq": seq, "full": 1 if full else 0, "scale": 2 if scale == 2 else 1}
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

    def walk(self, path="/sd", progress=None):
        """Recursively lists every file under `path`: returns ([dirs], [files]) where files is
        [(remote_path, size)]. Dotfiles are already hidden by the watch's own listing; the Windows
        'System Volume Information' folder is skipped explicitly."""
        dirs, files = [], []
        todo = [path]
        while todo:
            d = todo.pop(0)
            final, ents = self.ls(d)
            for e in ents:
                full = d.rstrip("/") + "/" + e["n"]
                if e["d"]:
                    if e["n"] == "System Volume Information":
                        continue
                    dirs.append(full)
                    todo.append(full)
                else:
                    files.append((full, int(e["s"])))
            if progress:
                progress(len(dirs), len(files))
        return dirs, files

    def rm(self, path):
        return self.request("fs.rm", p=path)[0]

    def mkdir(self, path):
        return self.request("fs.mkdir", p=path)[0]

    def get(self, remote, local, progress=None):
        """Downloads remote -> local file. progress(done, total) is optional. If a chunk arrives
        shorter than what the watch says it sent (i.e. a fragment line got lost), the same offset is
        simply requested again."""
        off = 0
        total = None
        with open(local, "wb") as fh:
            while True:
                final, blob, _, _ = self.request("fs.get", timeout=30.0, p=remote, off=off, len=CHUNK)
                total = final.get("size", total)
                sent = int(final.get("n", len(blob)))
                if len(blob) != sent:
                    continue                          # partial reply received — re-request the same offset
                fh.write(blob)
                off += len(blob)
                if progress:
                    progress(off, total or off)
                if len(blob) < CHUNK or (total is not None and off >= total):
                    break
        return off

    def put(self, local, remote, progress=None, _restarts=1):
        """Uploads local -> remote path (via a <remote>.part staging name, renamed once complete). A
        chunk whose acknowledgement got lost is simply retried (the watch treats re-sending an
        already-written chunk as success); if the watch drops the whole upload session in the meantime
        (reported as "offset mismatch"), the upload restarts once from byte 0."""
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
    """Decodes PackBits-style RLE over 16-bit pixels, matching the firmware's rle565_encode: bytes
    0x80..0xFF mean "repeat the next 2-byte pixel (n & 0x7F) + 2 times", 0x00..0x7F mean "n + 1
    literal pixels follow"."""
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
    """Converts RGB565-LE bytes to RGB888 using a lazily-built 65536-entry lookup table — fast enough
    for a whole frame in pure Python (~25 ms) and effectively instant for small partial rectangles."""
    global _LUT565
    if _LUT565 is None:
        _LUT565 = [bytes((((p >> 11) & 31) << 3 | ((p >> 11) & 31) >> 2,
                          ((p >> 5) & 63) << 2 | ((p >> 5) & 63) >> 4,
                          (p & 31) << 3 | (p & 31) >> 2)) for p in range(65536)]
    lut = _LUT565
    px = struct.unpack("<%dH" % (len(data) // 2), data[: (len(data) // 2) * 2])
    return b"".join([lut[p] for p in px])


def ppm_from_rgb565(width, height, data):
    """Builds a binary PPM image (which Tk can decode natively) from an RGB565-LE rectangle."""
    return b"P6 %d %d 255\n" % (width, height) + rgb565_to_rgb888(data)


def rgb565_to_png(width, height, data):
    """A minimal PNG encoder (standard library only), used to save screenshot frames to disk."""
    rows = []
    for y in range(height):
        row = bytearray([0])                      # PNG filter type 0 (none)
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
