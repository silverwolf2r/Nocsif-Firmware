r"""
NocSif Desktop Bridge — the published-firmware channel (PLAN §4.10 / §4.15).

The public mirror `silverwolf2r/Nocsif-Firmware` carries, under nocsif/firmware/:
    manifest.json   {version, build, file, size, sha256, notes, source, parts:[{file, offset, size, sha256}]}
    firmware.bin    the app (offset 0x20000)
    bootloader.bin  partitions.bin  ota_data_initial.bin   (the provisioning parts, offsets in `parts`)

`tools/publish_firmware.py` is what writes them; this module is what reads them back over HTTPS (fine
from a desktop app — the watch's own OTA pull logic lives separately, in firmware/src/ota.c). Every
download is checked against the manifest's sha256.
"""
import hashlib
import json
import os

import requests

DEFAULT_REPO = "silverwolf2r/Nocsif-Firmware"
RAW_BASE = "https://raw.githubusercontent.com/%s/main/nocsif/firmware/"


def raw_url(repo, name):
    return (RAW_BASE % repo) + name


def fetch_manifest(repo=DEFAULT_REPO, timeout=15):
    r = requests.get(raw_url(repo, "manifest.json"), timeout=timeout, headers={"Cache-Control": "no-cache"})
    r.raise_for_status()
    return r.json()


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def download(repo, name, dest, expect_sha=None, expect_size=None, progress=None, timeout=60):
    """Streams one published file down to dest, checking size and/or sha256 when the manifest provides them."""
    r = requests.get(raw_url(repo, name), stream=True, timeout=timeout)
    r.raise_for_status()
    total = int(r.headers.get("Content-Length") or expect_size or 0)
    done = 0
    with open(dest, "wb") as fh:
        for chunk in r.iter_content(64 * 1024):
            fh.write(chunk)
            done += len(chunk)
            if progress:
                progress(done, total)
    if expect_size is not None and os.path.getsize(dest) != int(expect_size):
        raise IOError("%s: size %d != published %s" % (name, os.path.getsize(dest), expect_size))
    if expect_sha and sha256_of(dest) != expect_sha:
        raise IOError("%s: sha256 mismatch" % name)
    return dest


def fetch_release(repo, manifest, dest_dir, progress=None, want_parts=True):
    """Downloads the app image, plus the provisioning parts too if the manifest lists them and
    want_parts is set. Returns {name: {"path":…, "offset":int}}; firmware.bin is always present, and
    ota_data_initial.bin / bootloader.bin / partitions.bin are included when they were published."""
    os.makedirs(dest_dir, exist_ok=True)
    out = {}
    parts = manifest.get("parts") or [{"file": manifest.get("file", "firmware.bin"), "offset": "0x20000",
                                       "size": manifest.get("size"), "sha256": manifest.get("sha256")}]
    for p in parts:
        name = p["file"]
        if not want_parts and name != "firmware.bin":
            continue
        dest = os.path.join(dest_dir, name)
        download(repo, name, dest, p.get("sha256"), p.get("size"),
                 progress=(lambda d, t, n=name: progress(n, d, t)) if progress else None)
        out[name] = {"path": dest, "offset": int(str(p.get("offset", "0x20000")), 16)}
    return out


LILYGO_REPO = "Xinyuan-LilyGO/LilyGoLib"
LILYGO_VARIANTS = ("sx1262", "sx1280")     # the two T-Watch Ultra LoRa radio variants (915 MHz / 2.4 GHz)


def lilygo_factory_images(timeout=15):
    """Looks up LilyGo's own merged factory images for the T-Watch Ultra, the newest one per radio
    variant, straight from their LilyGoLib repo's firmware/ folder: {variant: {"name", "size", "url",
    "date"}}. Each file is a complete 16 MB flash image meant to be written at offset 0x0."""
    r = requests.get("https://api.github.com/repos/%s/contents/firmware" % LILYGO_REPO, timeout=timeout,
                     headers={"Accept": "application/vnd.github+json"})
    r.raise_for_status()
    out = {}
    for entry in r.json():
        name = entry.get("name", "")
        if not name.startswith("factory.watch.ultra.") or not name.endswith(".bin"):
            continue
        parts = name.split(".")                 # filename shape: factory.watch.ultra.<variant>.<date>.bin
        if len(parts) < 6:
            continue
        variant, date = parts[3], parts[4]
        if variant not in LILYGO_VARIANTS:
            continue
        cur = out.get(variant)
        if cur is None or date > cur["date"]:
            out[variant] = {"name": name, "size": entry.get("size"), "url": entry.get("download_url"), "date": date}
    return out


def download_url(url, dest, expect_size=None, progress=None, timeout=120):
    """Streams an arbitrary URL to dest with an optional size check (LilyGo's images don't carry a checksum)."""
    r = requests.get(url, stream=True, timeout=timeout)
    r.raise_for_status()
    total = int(r.headers.get("Content-Length") or expect_size or 0)
    done = 0
    with open(dest, "wb") as fh:
        for chunk in r.iter_content(256 * 1024):
            fh.write(chunk)
            done += len(chunk)
            if progress:
                progress(done, total)
    if expect_size is not None and os.path.getsize(dest) != int(expect_size):
        raise IOError("%s: size %d != listed %s" % (os.path.basename(dest), os.path.getsize(dest), expect_size))
    return dest


def latest_app_release(repo=DEFAULT_REPO, timeout=10):
    """Finds the newest desktop-app release on the public repo (tagged `app-vX.Y.Z`), or None. Returns
    {"version": "X.Y.Z", "url": html_url, "assets": {name: download_url}}."""
    r = requests.get("https://api.github.com/repos/%s/releases" % repo, timeout=timeout,
                     headers={"Accept": "application/vnd.github+json"})
    if r.status_code != 200:
        return None
    best = None
    for rel in r.json():
        tag = rel.get("tag_name", "")
        if not tag.startswith("app-v") or rel.get("draft"):
            continue
        ver = tag[5:]
        if best is None or version_tuple(ver) > version_tuple(best["version"]):
            best = {"version": ver, "url": rel.get("html_url"),
                    "assets": {a["name"]: a["browser_download_url"] for a in rel.get("assets", [])}}
    return best


def version_tuple(v):
    out = []
    for part in str(v).split("."):
        num = "".join(ch for ch in part if ch.isdigit())
        out.append(int(num) if num else 0)
    return tuple(out)


def compare_versions(running, published):
    """Returns 'same' | 'behind' | 'differs' — version strings come from `git describe` and aren't a
    strict ordering, so only exact equality can be trusted; any mismatch is reported as 'differs'."""
    if not running or not published:
        return "unknown"
    if running == published:
        return "same"
    return "differs"
