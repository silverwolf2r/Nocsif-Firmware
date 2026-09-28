r"""
NocSif Desktop Bridge — protocol client (PLAN §4.15).

Talks to the watch's USB-Serial/JTAG console (COM7 / /dev/ttyACM* / /dev/cu.usbmodem*), the one USB
channel that is always alive. One JSON object per line goes in; the watch answers with lines prefixed
"NB>" (see firmware/src/bridge.h). Long answers arrive as base64 fragment lines {"id":N,"d":"…"},
listings as {"id":N,"e":{…}} entry lines, health as {"id":N,"chk":{…}} lines, always terminated by
{"id":N,"ok":…,"end":true,…}. Everything that is not an NB> line is the ordinary log stream and is
handed to an optional callback (the app's live log tail).

ONE reader thread owns the port's input: it splits lines, routes reply lines to the request waiting on
that id and log lines to the callback. Commands are still issued one at a time (the watch's bridge is a
single task), but nothing else ever blocks a command — the old design let the log tail hold the command
lock for its whole 0.5 s budget, which is what made the live view stutter.

Only pyserial is needed here; esptool / requests are used by flasher.py and updater.py.
"""
import base64
import json
import os
import queue
import struct
import threading
import time
import zlib

import serial                      # pyserial
from serial.tools import list_ports

ESP32S3_USJ = (0x303A, 0x1001)     # Espressif USB-Serial/JTAG (the ESP32-S3 native port)
PREFIX = b"NB>"
CHUNK = 8192          # fs.get payload per request (the watch paces its replies; any size is safe)
PUT_CHUNK = 2400      # fs.put payload per request: ~3.3 KB line, inside the watch's 4 KB console RX ring
                      # (the driver's ISR drops bytes if the ring fills); a lost chunk is retried
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
    """A connection to one watch. Thread-safe: any thread may issue a command; they run one at a time."""

    def __init__(self, port, log_cb=None, timeout=10.0):
        self.port = port
        self.log_cb = log_cb
        self.timeout = timeout
        self._cmd_lock = threading.Lock()      # one command in flight at a time
        self._pend_lock = threading.Lock()
        self._pending = {}                     # request id -> Queue of reply dicts
        self._id = 0
        self._alive = True
        self._err = None
        self._ser = serial.Serial()
        self._ser.port = port
        self._ser.baudrate = 115200
        self._ser.timeout = 0.1
        self._ser.dtr = False          # the native USB-Serial/JTAG toggles reset on a DTR/RTS pulse —
        self._ser.rts = False          # keep both low BEFORE open so connecting never reboots the watch
        self._ser.open()
        self._reader = threading.Thread(target=self._read_loop, name="nbridge-reader", daemon=True)
        self._reader.start()

    @property
    def alive(self):
        return self._alive

    def close(self):
        self._alive = False
        try:
            self._ser.close()
        except Exception:
            pass

    # ---- the reader thread -----------------------------------------------------------------------
    BIN_MAGIC = b"\xa5\x5aNB"
    BIN_HDR = 7                                # magic(4) + chunk index(1) + length(2, LE)

    def _read_loop(self):
        """Text mode: split lines, route NB> replies by id, everything else is log. A reply line that
        announces a binary blob ({"id":N,"bin":TOTAL}) switches to binary mode until TOTAL bytes have
        arrived in magic-stamped chunks (bytes between chunks are log text). A stalled blob (2 s without
        bytes) drops back to text mode; the waiting command then times out on its own."""
        buf = b""
        binm = None                            # {"id", "left", "last"} while a blob is streaming
        while self._alive:
            try:
                chunk = self._ser.read(1)
                if chunk:
                    waiting = self._ser.in_waiting
                    if waiting:
                        chunk += self._ser.read(waiting)
            except Exception as e:                # the port went away (unplugged / esptool took it)
                self._err = e
                break
            if not chunk:
                if binm and time.time() - binm["last"] > 2.0:
                    binm = None
                continue
            buf += chunk
            while True:
                if binm is None:
                    i = buf.find(b"\n")
                    if i < 0:
                        break
                    line, buf = buf[:i].rstrip(b"\r"), buf[i + 1:]
                    msg = self._dispatch(line)
                    if msg and "bin" in msg:
                        binm = {"id": msg.get("id"), "left": int(msg["bin"]), "last": time.time()}
                        if binm["left"] <= 0:
                            binm = None
                    continue
                m = buf.find(self.BIN_MAGIC)
                if m < 0:
                    if len(buf) > 3:               # a magic could straddle reads: keep its possible start
                        self._log(buf[:-3]); buf = buf[-3:]
                    break
                if m > 0:
                    self._log(buf[:m]); buf = buf[m:]
                if len(buf) < self.BIN_HDR:
                    break
                n = buf[5] | (buf[6] << 8)
                if len(buf) < self.BIN_HDR + n:
                    break
                payload, buf = buf[self.BIN_HDR:self.BIN_HDR + n], buf[self.BIN_HDR + n:]
                with self._pend_lock:
                    q = self._pending.get(binm["id"])
                if q is not None:
                    q.put({"id": binm["id"], "b": payload})
                binm["left"] -= n
                binm["last"] = time.time()
                if binm["left"] <= 0:
                    binm = None
        self._alive = False
        with self._pend_lock:                     # wake every waiter so it fails fast, not on its timeout
            for q in self._pending.values():
                q.put(None)

    def _dispatch(self, line):
        """Route one text line; returns the parsed reply dict (or None for a log line)."""
        # The watch's log goes into the same console char by char, so a log line can wrap around a
        # reply: "I (12) nocsif: heaNB>{...}" — the reply starts at the LAST marker, never at column 0
        # by guarantee. Anything before the marker is log text.
        at = line.rfind(PREFIX)
        if at < 0:
            self._log(line)
            return None
        if at > 0:
            self._log(line[:at])
        try:
            msg = json.loads(line[at + len(PREFIX):].decode("utf-8", "replace"))
        except ValueError:
            return None
        with self._pend_lock:
            q = self._pending.get(msg.get("id"))
        if q is not None:
            q.put(msg)                            # a stale id (a timed-out command) is simply dropped
        return msg

    def _log(self, line):
        if self.log_cb:
            try:
                self.log_cb(line.decode("utf-8", "replace"))
            except Exception:
                pass

    def pump_log(self, seconds=0.0):
        """Kept for callers that used to drive the log tail: the reader thread now delivers log lines
        on its own. Sleeps for up to `seconds` and raises once the port is gone, so a tailing loop still
        notices an unplug."""
        if not self._alive:
            raise BridgeError("port closed: %s" % (self._err or "closed"))
        time.sleep(min(seconds, 0.25) if seconds > 0 else 0)
        if not self._alive:
            raise BridgeError("port closed: %s" % (self._err or "closed"))

    # ---- commands ------------------------------------------------------------------------------
    def request(self, cmd, timeout=None, **args):
        """Sends one command and returns (final_dict, fragments_bytes, entries, checks).
        "id" and "c" are reserved for the request envelope, so a command argument must never reuse
        either name — this is exactly why the launch target argument is called "app" instead of "id"."""
        if "id" in args or "c" in args:
            raise BridgeError("argument name %s collides with the envelope" % ("id" if "id" in args else "c"))
        if not self._alive:
            raise BridgeError("port closed: %s" % (self._err or "closed"))
        with self._cmd_lock:
            self._id += 1
            rid = self._id
            q = queue.Queue()
            with self._pend_lock:
                self._pending[rid] = q
            try:
                req = dict(args)
                req["id"] = rid
                req["c"] = cmd
                req["bin"] = 1                    # blobs raw (proto 2); an older firmware ignores it
                try:
                    self._ser.write((json.dumps(req, separators=(",", ":")) + "\n").encode("utf-8"))
                    self._ser.flush()
                except Exception as e:
                    raise BridgeError("write failed: %s" % e)
                deadline = time.time() + (timeout or self.timeout)
                frags, entries, checks = [], [], []
                while True:
                    remaining = deadline - time.time()
                    if remaining <= 0:
                        raise BridgeError("timeout waiting for %s" % cmd)
                    try:
                        msg = q.get(timeout=remaining)
                    except queue.Empty:
                        raise BridgeError("timeout waiting for %s" % cmd)
                    if msg is None:
                        raise BridgeError("port closed: %s" % (self._err or "closed"))
                    if "d" in msg:
                        frags.append(base64.b64decode(msg["d"]))
                    elif "b" in msg:
                        frags.append(msg["b"])   # a raw binary chunk (see _read_loop)
                    elif "e" in msg:
                        entries.append(msg["e"])
                    elif "chk" in msg:
                        checks.append(msg["chk"])
                    if msg.get("end"):
                        if not msg.get("ok", False):
                            raise BridgeError(msg.get("err", "failed"))
                        return msg, b"".join(frags), entries, checks
            finally:
                with self._pend_lock:
                    self._pending.pop(rid, None)

    def ping(self):
        return self.request("ping", timeout=3.0)[0]

    def version(self):
        return self.request("version")[0]

    def status(self):
        return self.request("status")[0]

    def health(self):
        final, _, _, checks = self.request("health", timeout=20.0)
        return final, checks

    def test(self, which, **args):
        """Self-tests: 'tone' (optional hz=, ms=, vol= — a speaker sine, e.g. a tuner reference),
        'nfc', 'lora', 'lora-reset' (stuck-radio recovery), 'gnss'."""
        return self.request("test", timeout=15.0, t=which, **args)[0]

    def state(self):
        return self.request("state")[0].get("state", {})

    def menu(self):
        _, blob, _, _ = self.request("menu")
        return json.loads(blob.decode("utf-8", "replace") or "{}")

    def screenshot(self):
        """Returns (width, height, rgb565le_bytes)."""
        final, blob, _, _ = self.request("screenshot", timeout=30.0)
        return final["w"], final["h"], blob

    def mirror_poll(self, seq, full=False, touch=None, scale=1, touches=None, delta=False):
        """One live-view poll. Returns the final dict and the decoded RGB565-LE rectangle bytes (b''
        when there is no rectangle). The final dict is one of: a rectangle (seq/x/y/w/h/scale/raw),
        none:true (nothing changed), or busy:true (the UI could not be locked — a requested full frame
        was NOT delivered, ask again). Every reply carries asleep / blank / focused. `touch` = one
        (x, y, pressed) in watch pixels, or `touches` = a list of them (a whole swipe), rides the same
        round trip. scale 1 = the panel's own 410×502 pixels; 2 = half resolution for a slow link.
        `delta` = the caller keeps every rectangle it receives and can apply XOR deltas: when the reply
        says delta:true the bytes are (new XOR previous) for that rectangle, not pixels."""
        args = {"seq": seq, "full": 1 if full else 0, "scale": 2 if scale == 2 else 1}
        if delta:
            args["delta"] = 1
        pts = list(touches or [])
        if touch is not None:
            pts.append(touch)
        if len(pts) == 1:
            args["t"] = [int(pts[0][0]), int(pts[0][1]), int(pts[0][2])]
        elif pts:
            args["t"] = [[int(p[0]), int(p[1]), int(p[2])] for p in pts]
        final, blob, _, _ = self.request("mirror", timeout=6.0, **args)
        if final.get("none") or final.get("busy"):
            return final, b""
        raw = rle565_decode(blob, int(final["raw"]))
        return final, raw

    def log_tail(self, n=2048):
        _, blob, _, _ = self.request("log.tail", n=n)
        return blob.decode("utf-8", "replace")

    def sd_info(self):
        return self.request("sd.info")[0]

    def sd_format(self):
        return self.request("sd.format", timeout=180.0, confirm=1)[0]

    def sd_rescan(self):
        """Re-probe a reseated / previously-absent microSD without rebooting the watch (rail power-cycle +
        card re-init + File-Share remount). Returns present/total/free like sd_info."""
        return self.request("sd.rescan", timeout=20.0)[0]

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
                        if "bad path" in str(e) or "bad file name" in str(e) or "port closed" in str(e):
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
