r"""
NocSif Desktop Bridge — esptool wrappers (PLAN §4.15).

Every flash/erase goes through `python -m esptool --chip esp32s3 --port <port> --no-stub …`, the
project's known-good path (the ESP32-S3 native USB-Serial/JTAG port; the stub loader is avoided on
purpose — see docs/RESUME.md flash recipe). Output lines stream to a callback so the app can show
progress. Nothing here talks to the running firmware: esptool resets the chip into the ROM loader.

Flash layout (firmware/partitions.csv):
    0x0       bootloader.bin          0x9000   nvs (settings / creds / bonds — 0x6000)
    0x8000    partitions.bin          0xf000   otadata (0x2000)     0x11000  phy_init (0x1000)
    0x20000   ota_0 (app, 4 MB)       0x420000 ota_1 (4 MB)         0x820000 storage (LittleFS)
    0xEE0000  coredump                0xF20000 logs
"""
import os
import subprocess
import sys
import time

CHIP = "esp32s3"
APP_OFFSET = 0x20000
OTADATA_OFFSET = 0xF000

# Everything except the bootloader / partition table / nvs / phy_init: both app slots in one span,
# the LittleFS store, the coredump and the log ring.
WIPE_KEEP_NVS_REGIONS = [
    (0xF000, 0x2000),          # otadata  -> clean boot into ota_0 after the reflash
    (0x20000, 0x800000),       # ota_0 + ota_1
    (0x820000, 0x6C0000),      # storage
    (0xEE0000, 0x40000),       # coredump
    (0xF20000, 0xE0000),       # logs
]


def _python():
    return sys.executable or "python"


def run_esptool(args, port, line_cb=None, stub=False, capture=None, before=None, after=None):
    """Run one esptool invocation; stream lines to line_cb; return the exit code. `stub=False` is the
    project's proven write path (--no-stub); the stub loader is used for the backup reads (chunked —
    see backup_full) where the ROM path would take a quarter of an hour. `capture`, a list, collects
    every output line for parsing. before/after = esptool's --before / --after (e.g. "no-reset" to stay
    in the loader between chunked calls)."""
    cmd = [_python(), "-m", "esptool", "--chip", CHIP, "--port", port] + ([] if stub else ["--no-stub"])
    if before:
        cmd += ["--before", before]
    if after:
        cmd += ["--after", after]
    cmd += list(args)
    if line_cb:
        line_cb("$ " + " ".join(cmd[2:]))
    env = dict(os.environ)
    env["PYTHONIOENCODING"] = "utf-8"
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                         encoding="utf-8", errors="replace", env=env, bufsize=1)
    for line in p.stdout:
        line = line.rstrip("\r\n")
        if capture is not None:
            capture.append(line)
        if line_cb:
            line_cb(line)
    return p.wait()


# ---- any-watch flows (stock LilyGo / blank boards) ----------------------------------------------
FLASH_TOTAL = 0x1000000                 # 16 MB — every T-Watch Ultra
ARDUINO_APP_OFFSET = 0x10000            # LilyGo's Arduino layout: app0 at 0x10000
NOCSIF_APP_OFFSET = APP_OFFSET          # ota_0 at 0x20000


def probe_rom(port, line_cb=None):
    """Talk to the ROM loader only: {chip, revision, mac, flash_size, flash_id} or None if nothing
    answers. Works on ANY firmware (or none) — the chip is reset into the loader by esptool."""
    out = []
    rc = run_esptool(["flash-id"], port, line_cb, capture=out)
    if rc != 0:
        return None
    info = {"chip": None, "revision": None, "mac": None, "flash_size": None, "flash_id": None}
    for line in out:
        s = line.strip()
        if s.startswith("Chip is") or s.startswith("Chip type:"):
            info["chip"] = s.split(":", 1)[-1].strip() if ":" in s else s[8:].strip()
        elif s.startswith("Chip type:"):
            info["chip"] = s.split(":", 1)[1].strip()
        elif s.startswith("MAC:"):
            info["mac"] = s.split(":", 1)[1].strip()
        elif "flash size" in s.lower():
            info["flash_size"] = s.split(":")[-1].strip()
        elif s.startswith("Manufacturer:") or s.startswith("Device:"):
            info["flash_id"] = ((info["flash_id"] or "") + " " + s).strip()
        elif "revision" in s.lower() and info["revision"] is None:
            info["revision"] = s.split(":")[-1].strip() if ":" in s else s
    if info["chip"] and "revision" in info["chip"]:           # "ESP32-S3 (QFN56) (revision v0.2)"
        info["revision"] = info["chip"].split("revision", 1)[1].strip(" )")
        info["chip"] = info["chip"].split("(revision", 1)[0].strip()
    return info


def read_flash(port, offset, size, dest, line_cb=None, stub=True, before=None, after=None):
    """Dump `size` bytes from `offset` to `dest` (the stub loader; the ROM path is ~20 KB/s)."""
    return run_esptool(["read-flash", "0x%x" % offset, "0x%x" % size, dest], port, line_cb, stub=stub, before=before, after=after)


def read_app_desc(port, app_offset, line_cb=None):
    """The esp_app_desc of whatever image sits at app_offset (project name / version / build), or None.
    Reads 0x120 bytes: image header (24) + segment header (8) + the descriptor (256)."""
    import tempfile
    tmp = os.path.join(tempfile.gettempdir(), "nocsif_appdesc_%x.bin" % app_offset)
    if read_flash(port, app_offset, 0x120, tmp, line_cb, stub=False) != 0:
        return None
    try:
        with open(tmp, "rb") as fh:
            hdr = fh.read(0x120)
    finally:
        try:
            os.remove(tmp)
        except OSError:
            pass
    if len(hdr) < 0x80 or hdr[0] != 0xE9 or int.from_bytes(hdr[32:36], "little") != 0xABCD5432:
        return None
    def s(a, n):
        return hdr[a:a + n].split(b"\0", 1)[0].decode("utf-8", "replace")
    return {"version": s(48, 32), "project": s(80, 32), "time": s(112, 16), "date": s(128, 16), "idf": s(144, 32)}


def identify(port, line_cb=None):
    """What is on this board, from the ROM side: {"rom": probe, "nocsif": desc|None, "arduino": desc|None}.
    NocSif keeps its app at 0x20000, LilyGo's Arduino builds at 0x10000 — both are looked at."""
    rom = probe_rom(port, line_cb)
    if rom is None:
        return None
    return {"rom": rom,
            "nocsif": read_app_desc(port, NOCSIF_APP_OFFSET, line_cb),
            "arduino": read_app_desc(port, ARDUINO_APP_OFFSET, line_cb)}


BACKUP_CHUNK = 0x40000        # 256 KB per esptool call
BACKUP_SUBCHUNK = 0x10000     # 64 KB pieces when a chunk's stub read fails


def _read_piece(port, off, size, part, clean, last, stub):
    """One esptool read into `part`; True when the file is there with the right size. `clean` = enter
    the loader with a reset (the first call, and after any failure — a failed stub read leaves the
    loader wedged); otherwise chain with --before no-reset. The last piece hard-resets the chip back
    into its firmware."""
    rc = read_flash(port, off, size, part, None, stub=stub, before=None if clean else "no-reset",
                    after="hard-reset" if last else "no-reset")
    return rc == 0 and os.path.isfile(part) and os.path.getsize(part) == size


def backup_full(port, dest, line_cb=None, progress=None, chunk=BACKUP_CHUNK):
    """The whole 16 MB flash → dest, in chunks. Over the native USB-Serial/JTAG port the stub loader's
    streaming read dies ("Packet content transfer stopped") on some regions — deterministically per
    region on this host, about one chunk in four — while the ROM loader's read is steady but ~18 KB/s.
    So: 256 KB chunks through the stub (~95 KB/s, chained in the loader with no-reset); a chunk whose
    stub read fails is re-read as 64 KB pieces, each with one stub try and then the ROM read, so the
    slow path covers only what actually needs it. Returns 0 on a complete, size-verified image, else 1.
    progress(done_bytes, total) is optional."""
    part = dest + ".part"
    done = 0
    clean = True                                 # enter the loader with a reset: the first call, and after
    try:                                         # any failure (a failed stub read leaves the loader wedged);
        with open(dest, "wb") as out:            # chain with --before no-reset only after a success
            for off in range(0, FLASH_TOTAL, chunk):
                size = min(chunk, FLASH_TOTAL - off)
                last = off + size >= FLASH_TOTAL
                t0 = time.time()
                if _read_piece(port, off, size, part, clean, last, stub=True):
                    clean = False
                    with open(part, "rb") as fh:
                        out.write(fh.read())
                    if line_cb:
                        line_cb("chunk 0x%06x ok in %.1f s" % (off, time.time() - t0))
                else:
                    clean = True
                    if line_cb:
                        line_cb("chunk 0x%06x: stub read failed — 64 KB pieces" % off)
                    for sub in range(off, off + size, BACKUP_SUBCHUNK):
                        ssize = min(BACKUP_SUBCHUNK, off + size - sub)
                        slast = sub + ssize >= FLASH_TOTAL
                        t1 = time.time()
                        if _read_piece(port, sub, ssize, part, clean, slast, stub=True):
                            clean = False
                            how = "stub"
                        else:
                            clean = True
                            if not _read_piece(port, sub, ssize, part, True, slast, stub=False):
                                if line_cb:
                                    line_cb("piece 0x%06x: giving up" % sub)
                                return 1
                            how = "ROM"
                        with open(part, "rb") as fh:
                            out.write(fh.read())
                        if line_cb:
                            line_cb("  piece 0x%06x ok (%s, %.1f s)" % (sub, how, time.time() - t1))
                done += size
                if line_cb:
                    line_cb("backup %5.1f%%  (0x%06x)" % (100.0 * done / FLASH_TOTAL, off + size))
                if progress:
                    progress(done, FLASH_TOTAL)
    finally:
        try:
            os.remove(part)
        except OSError:
            pass
    return 0 if image_is_full_flash(dest) else 1


def write_image_at(port, offset, path, line_cb=None, stub=False):
    """One image at one offset. Default = the ROM loader (--no-stub), the path every NocSif flash has
    used: ~65 KB/s, so a 16 MB backup / factory image takes ~4-5 min but arrives whole. The stub's
    compressed write is faster when it works, but the same port flakiness that breaks its reads makes
    it a retry-only option here (stub=True)."""
    args = ["write-flash"] + (["-z"] if stub else []) + ["0x%x" % offset, path]
    return run_esptool(args, port, line_cb, stub=stub)


def image_is_full_flash(path):
    return os.path.isfile(path) and os.path.getsize(path) == FLASH_TOTAL


def flash_parts(port, parts, line_cb=None):
    """parts = [(offset_int, path)]. One write-flash with every part (verified by esptool's hash check)."""
    args = ["write-flash"]
    for off, path in sorted(parts):
        args += ["0x%x" % off, path]
    return run_esptool(args, port, line_cb)


def flash_app(port, firmware_bin, ota_data_bin, line_cb=None):
    """The everyday update: app -> ota_0 + a fresh otadata (settings in nvs untouched)."""
    return flash_parts(port, [(APP_OFFSET, firmware_bin), (OTADATA_OFFSET, ota_data_bin)], line_cb)


def erase_flash(port, line_cb=None):
    """FULL wipe — everything, settings included."""
    return run_esptool(["erase-flash"], port, line_cb)


def erase_regions(port, regions, line_cb=None):
    """Erase each (offset, size) region in turn. Non-zero on the first failure."""
    for off, size in regions:
        rc = run_esptool(["erase-region", "0x%x" % off, "0x%x" % size], port, line_cb)
        if rc != 0:
            return rc
    return 0


def chip_id(port, line_cb=None):
    """Probe the ROM loader (proves the port + a board that at least runs the ROM)."""
    return run_esptool(["chip-id"], port, line_cb)


def describe_parts(parts):
    return ", ".join("0x%x %s" % (off, os.path.basename(p)) for off, p in sorted(parts))
