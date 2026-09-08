r"""
NocSif Desktop Bridge — the published-firmware channel (PLAN §4.10 / §4.15).

The public mirror `silverwolf2r/Nocsif-Firmware` carries, under nocsif/firmware/:
    manifest.json   {version, build, file, size, sha256, notes, source, parts:[{file, offset, size, sha256}]}
    firmware.bin    the app (offset 0x20000)
    bootloader.bin  partitions.bin  ota_data_initial.bin   (the provisioning parts, offsets in `parts`)

`tools/publish_firmware.py` writes them; this module reads them over HTTPS (fine on a computer — the
watch's own pull lives in firmware/src/ota.c). Downloads are verified against the manifest's sha256.
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
    """Stream one published file to dest; verify size / sha256 when the manifest gives them."""
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
    """Download the app (+ the provisioning parts when the manifest lists them).
    Returns {name: {"path":…, "offset":int}} — always includes firmware.bin; ota_data_initial.bin,
    bootloader.bin, partitions.bin when published."""
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


def latest_app_release(repo=DEFAULT_REPO, timeout=10):
    """The newest desktop-app release on the public repo (tags `app-vX.Y.Z`), or None. Returns
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
    """'same' | 'behind' | 'differs' — versions are `git describe` strings, not ordered numbers, so
    only equality is certain; anything else is 'differs' unless the running one is a -dirty build."""
    if not running or not published:
        return "unknown"
    if running == published:
        return "same"
    return "differs"
