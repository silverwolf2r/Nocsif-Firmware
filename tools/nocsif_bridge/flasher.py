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


def run_esptool(args, port, line_cb=None):
    """Run one esptool invocation; stream lines to line_cb; return the exit code."""
    cmd = [_python(), "-m", "esptool", "--chip", CHIP, "--port", port, "--no-stub"] + list(args)
    if line_cb:
        line_cb("$ " + " ".join(cmd[2:]))
    env = dict(os.environ)
    env["PYTHONIOENCODING"] = "utf-8"
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                         encoding="utf-8", errors="replace", env=env, bufsize=1)
    for line in p.stdout:
        if line_cb:
            line_cb(line.rstrip("\r\n"))
    return p.wait()


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
