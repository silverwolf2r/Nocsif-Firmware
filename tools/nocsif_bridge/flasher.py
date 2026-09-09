r"""
NocSif Desktop Bridge — esptool wrappers (PLAN §4.15).

Every flash/erase operation goes through `python -m esptool --chip esp32s3 --port <port> --no-stub …`,
the project's proven-working path (the ESP32-S3 native USB-Serial/JTAG port; the stub loader is skipped
on purpose — see docs/RESUME.md flash recipe). Each esptool invocation streams its output lines to a
callback so the UI can show progress. None of this talks to the running firmware — esptool resets the
chip straight into its ROM loader.

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

# Every region wiped by "keep settings" except the bootloader, partition table, nvs and phy_init:
# both OTA app slots as one span, the LittleFS store, the coredump area and the log ring buffer.
WIPE_KEEP_NVS_REGIONS = [
    (0xF000, 0x2000),          # otadata -> resets to a clean boot into ota_0 after the reflash
    (0x20000, 0x800000),       # ota_0 + ota_1
    (0x820000, 0x6C0000),      # storage
    (0xEE0000, 0x40000),       # coredump
    (0xF20000, 0xE0000),       # logs
]


def _python():
    return sys.executable or "python"


def run_esptool(args, port, line_cb=None, stub=False, capture=None, before=None, after=None):
    """Runs one esptool subcommand and streams its output to line_cb, returning esptool's exit code.
    `stub=False` (the default) is the project's known-good write path (--no-stub); the stub loader is
    reserved for chunked backup reads (see backup_full), where the ROM path alone would take about a
    quarter of an hour. `capture`, if given a list, collects every printed line for later parsing.
    before/after map to esptool's own --before/--after flags (e.g. "no-reset" to stay in the loader
    between chained calls)."""
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
FLASH_TOTAL = 0x1000000                 # 16 MB total flash on every T-Watch Ultra
ARDUINO_APP_OFFSET = 0x10000            # where LilyGo's Arduino build places app0
NOCSIF_APP_OFFSET = APP_OFFSET          # NocSif's ota_0 slot, at 0x20000


def probe_rom(port, line_cb=None):
    """Talks only to the ROM bootloader and returns {chip, revision, mac, flash_size, flash_id}, or
    None if nothing answers. Works regardless of what firmware (if any) is installed, since esptool
    resets the chip into the ROM loader itself."""
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
    if info["chip"] and "revision" in info["chip"]:           # e.g. "ESP32-S3 (QFN56) (revision v0.2)"
        info["revision"] = info["chip"].split("revision", 1)[1].strip(" )")
        info["chip"] = info["chip"].split("(revision", 1)[0].strip()
    return info


def read_flash(port, offset, size, dest, line_cb=None, stub=True, before=None, after=None):
    """Reads `size` bytes starting at `offset` into `dest` file; stub=True uses the fast stub loader
    (the plain ROM path only manages ~20 KB/s)."""
    return run_esptool(["read-flash", "0x%x" % offset, "0x%x" % size, dest], port, line_cb, stub=stub, before=before, after=after)


def read_app_desc(port, app_offset, line_cb=None):
    """Reads back the esp_app_desc (project name / version / build info) of whatever image sits at
    app_offset, or None if there isn't a valid one there. Pulls 0x120 bytes: 24 B image header,
    8 B segment header, 256 B descriptor."""
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
    """Figures out what's on a board purely from the ROM side: {"rom": probe, "nocsif": desc|None,
    "arduino": desc|None}. Checks both possible app offsets, since NocSif's app lives at 0x20000 while
    LilyGo's stock Arduino build lives at 0x10000."""
    rom = probe_rom(port, line_cb)
    if rom is None:
        return None
    return {"rom": rom,
            "nocsif": read_app_desc(port, NOCSIF_APP_OFFSET, line_cb),
            "arduino": read_app_desc(port, ARDUINO_APP_OFFSET, line_cb)}


BACKUP_CHUNK = 0x40000        # read 256 KB per esptool call during a full backup
BACKUP_SUBCHUNK = 0x10000     # fall back to 64 KB pieces when a chunk's stub read fails


def _read_piece(port, off, size, part, clean, last, stub):
    """Reads one piece into `part`, returning True once the file exists with the expected size.
    `clean` requests a hard reset into the loader (needed on the first call, and again after any
    failure, since a broken stub read leaves the loader in a bad state); otherwise the call chains
    onto the previous one with --before no-reset. `last` hard-resets the chip back into its firmware
    once the read finishes."""
    rc = read_flash(port, off, size, part, None, stub=stub, before=None if clean else "no-reset",
                    after="hard-reset" if last else "no-reset")
    return rc == 0 and os.path.isfile(part) and os.path.getsize(part) == size


def backup_full(port, dest, line_cb=None, progress=None, chunk=BACKUP_CHUNK):
    """Copies the entire 16 MB flash to dest, chunk by chunk. Over the native USB-Serial/JTAG port the
    stub loader's streaming read is unreliable ("Packet content transfer stopped") on some regions —
    roughly one chunk in four fails, in a way that repeats for the same region on this host — while the
    plain ROM read is slow (~18 KB/s) but never drops. So: read in 256 KB chunks through the stub loader
    (~95 KB/s, chained with no-reset between calls); whenever a chunk's stub read fails, retry it as
    64 KB pieces, each attempted once via the stub and then falling back to the ROM read, so the slow
    path only covers the parts that actually need it. Returns 0 for a complete, size-verified image, 1
    otherwise. progress(done_bytes, total), if given, is called after every chunk."""
    part = dest + ".part"
    done = 0
    clean = True                                 # do a hard reset into the loader: needed on the first
    try:                                         # call, and again after any failure — after a success,
        with open(dest, "wb") as out:            # subsequent calls chain with --before no-reset instead
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
    """Writes one image at one flash offset. Defaults to the ROM loader (--no-stub) — the write path
    every NocSif flash has used: ~65 KB/s, so a 16 MB backup/factory image takes ~4-5 minutes but always
    arrives intact. The stub loader's compressed write is faster when it cooperates, but the same port
    flakiness that breaks its reads applies here too, so it's offered only as an opt-in retry (stub=True)."""
    args = ["write-flash"] + (["-z"] if stub else []) + ["0x%x" % offset, path]
    return run_esptool(args, port, line_cb, stub=stub)


def image_is_full_flash(path):
    return os.path.isfile(path) and os.path.getsize(path) == FLASH_TOTAL


def flash_parts(port, parts, line_cb=None):
    """parts = [(offset_int, path)]. Writes every part in a single esptool call, verified by esptool's own hash check."""
    args = ["write-flash"]
    for off, path in sorted(parts):
        args += ["0x%x" % off, path]
    return run_esptool(args, port, line_cb)


def flash_app(port, firmware_bin, ota_data_bin, line_cb=None):
    """The routine update path: writes the app to ota_0 and a fresh otadata (nvs settings untouched)."""
    return flash_parts(port, [(APP_OFFSET, firmware_bin), (OTADATA_OFFSET, ota_data_bin)], line_cb)


def erase_flash(port, line_cb=None):
    """FULL wipe — clears everything, settings included."""
    return run_esptool(["erase-flash"], port, line_cb)


def erase_regions(port, regions, line_cb=None):
    """Erases each (offset, size) region one at a time, stopping and returning the error code on the first failure."""
    for off, size in regions:
        rc = run_esptool(["erase-region", "0x%x" % off, "0x%x" % size], port, line_cb)
        if rc != 0:
            return rc
    return 0


def chip_id(port, line_cb=None):
    """Pings the ROM loader — proves the port is real and the board runs the ROM bootloader at least."""
    return run_esptool(["chip-id"], port, line_cb)


def describe_parts(parts):
    return ", ".join("0x%x %s" % (off, os.path.basename(p)) for off, p in sorted(parts))
