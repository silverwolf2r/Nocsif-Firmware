#!/usr/bin/env python3
r"""
NocSif — publish a firmware image to the PUBLIC repo (PLAN §4.10 GitHub firmware pull).

The watch pulls updates from the public mirror `silverwolf2r/Nocsif-Firmware`:

    https://raw.githubusercontent.com/<owner>/<repo>/main/nocsif/firmware/manifest.json
    https://raw.githubusercontent.com/<owner>/<repo>/main/nocsif/firmware/firmware.bin

(the same `nocsif/firmware/` folder shape the watch uses on its microSD). This script is the ONE way
those two files change:

    python tools/publish_firmware.py --public-dir D:\path\to\Nocsif-Firmware [--notes "..."] [--mirror] [--no-push]

  - reads the built image (default firmware/.pio/build/nocsif-twatch-ultra/firmware.bin — build first),
  - reads the version FROM THE IMAGE (esp_app_desc.version at offset 48 of an ESP app image — the string
    the watch compares against its own; ESP-IDF stamps it from `git describe` at CMake-configure time, so
    reading it back is the only way the manifest can never disagree with the binary), refusing a dirty tree,
  - writes nocsif/firmware/manifest.json {version, build, size, sha256, notes, file} + copies the image,
  - with --mirror also refreshes the public SOURCE snapshot (every tracked file of this private repo,
    minus the private-only bits below) so the public repo stays a faithful copy of `main`,
  - commits in the public clone and pushes (unless --no-push).

Privacy: the mirror copies TRACKED files only (secrets never were), skips .github/ workflow secrets by
construction (none exist), and refuses to run on a dirty private tree so an untracked scratch file can
never ride along. Review `git status` in the public clone before the first push.
"""
import argparse
import datetime as dt
import hashlib
import json
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PRIVATE_ROOT = os.path.abspath(os.path.join(HERE, ".."))
DEFAULT_BIN = os.path.join(PRIVATE_ROOT, "firmware", ".pio", "build", "nocsif-twatch-ultra", "firmware.bin")
PUBLIC_FW_DIR = os.path.join("nocsif", "firmware")
MIRROR_SKIP_PREFIXES = ()                                # nothing private today; keep the hook
MIRROR_SKIP_DIRS = (".git",)
# The public README carries a mirror banner right under the title; the mirror copies the private README
# verbatim, so the banner is re-inserted after every refresh (idempotent — keyed on its first line).
README_BANNER = [
    "",
    "> **Public mirror.** This repository is a snapshot of the NocSif firmware `main` branch, refreshed by",
    "> `tools/publish_firmware.py --mirror` on each release. The watch's **Update (OTA)** screen pulls its",
    "> firmware from here: `nocsif/firmware/manifest.json` + `nocsif/firmware/firmware.bin` (the same",
    "> `nocsif/firmware/` folder the watch keeps on its microSD). Development happens in the private repo;",
    "> issues and PRs here are read but not merged directly.",
]


def inject_readme_banner(public_dir):
    path = os.path.join(public_dir, "README.md")
    if not os.path.exists(path):
        return
    lines = open(path, encoding="utf-8").read().split("\n")
    if any(l.startswith("> **Public mirror.**") for l in lines):
        return
    # after the first "# " heading (the title), else at the top
    at = next((i + 1 for i, l in enumerate(lines) if l.startswith("# ")), 0)
    lines[at:at] = README_BANNER
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(lines))


def run(cmd, cwd, check=True):
    p = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, shell=(os.name == "nt"))
    if check and p.returncode != 0:
        sys.exit("command failed: %s\n%s%s" % (" ".join(cmd), p.stdout, p.stderr))
    return p.stdout.strip()


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def image_version(path):
    """esp_app_desc_t sits at image offset 32 (24 B image header + 8 B first segment header):
    magic_word u32 (0xABCD5432), secure_version u32, reserv1[2] u32, version[32], project_name[32], ..."""
    with open(path, "rb") as fh:
        hdr = fh.read(32 + 16 + 32)
    if len(hdr) < 80 or hdr[0] != 0xE9:
        sys.exit("%s is not an ESP app image (bad magic)" % path)
    if int.from_bytes(hdr[32:36], "little") != 0xABCD5432:
        sys.exit("%s carries no esp_app_desc (bad descriptor magic)" % path)
    return hdr[48:80].split(b"\0", 1)[0].decode("utf-8", "replace")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--public-dir", required=True, help="local clone of the public repo")
    ap.add_argument("--bin", default=DEFAULT_BIN, help="firmware.bin to publish")
    ap.add_argument("--notes", default="", help="one-line release note for the manifest")
    ap.add_argument("--mirror", action="store_true", help="also refresh the public source snapshot")
    ap.add_argument("--no-push", action="store_true", help="commit but do not push")
    ap.add_argument("--allow-dirty", action="store_true", help="publish from a dirty private tree (NOT for releases)")
    a = ap.parse_args()

    if not os.path.isfile(a.bin):
        sys.exit("no image at %s — build first" % a.bin)
    if not os.path.isdir(os.path.join(a.public_dir, ".git")):
        sys.exit("%s is not a git clone" % a.public_dir)

    dirty = run(["git", "status", "--porcelain"], PRIVATE_ROOT)
    if dirty and not a.allow_dirty:
        sys.exit("private tree is dirty — commit or stash first (or --allow-dirty for a test):\n" + dirty)
    version = image_version(a.bin)                      # what the watch will report for this image
    sha = run(["git", "rev-parse", "--short", "HEAD"], PRIVATE_ROOT)
    if not version:
        sys.exit("the image carries an empty version string")
    if version.endswith("-dirty") and not a.allow_dirty:
        sys.exit("the image was built from a dirty tree (version %s) — rebuild from a commit" % version)
    size = os.path.getsize(a.bin)
    digest = sha256_of(a.bin)
    manifest = {
        "version": version,
        "build": dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "file": "firmware.bin",
        "size": size,
        "sha256": digest,
        "notes": a.notes,
        "source": sha,
    }

    fw_dir = os.path.join(a.public_dir, PUBLIC_FW_DIR)
    os.makedirs(fw_dir, exist_ok=True)
    shutil.copyfile(a.bin, os.path.join(fw_dir, "firmware.bin"))
    with open(os.path.join(fw_dir, "manifest.json"), "w", encoding="utf-8", newline="\n") as fh:
        json.dump(manifest, fh, indent=2)
        fh.write("\n")
    print("manifest:", json.dumps(manifest))

    if a.mirror:
        tracked = run(["git", "ls-files"], PRIVATE_ROOT).splitlines()
        copied = 0
        for rel in tracked:
            if rel.startswith(MIRROR_SKIP_PREFIXES) or rel.split("/")[0] in MIRROR_SKIP_DIRS:
                continue
            src = os.path.join(PRIVATE_ROOT, rel)
            dst = os.path.join(a.public_dir, rel)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copyfile(src, dst)
            copied += 1
        inject_readme_banner(a.public_dir)
        print("mirrored %d tracked files (snapshot of %s)" % (copied, sha))

    # The private .gitignore excludes *.bin; the public repo must carry the image.
    gi = os.path.join(a.public_dir, ".gitignore")
    rule = "!nocsif/firmware/firmware.bin"
    lines = open(gi, encoding="utf-8").read().splitlines() if os.path.exists(gi) else []
    if rule not in lines:
        lines += ["", "# The published firmware image (PLAN §4.10) — the one .bin that IS committed", rule]
        with open(gi, "w", encoding="utf-8", newline="\n") as fh:
            fh.write("\n".join(lines) + "\n")

    run(["git", "add", "-A"], a.public_dir)
    if not run(["git", "status", "--porcelain"], a.public_dir):
        print("nothing changed in the public repo")
        return
    msg = "firmware %s (%d bytes, sha256 %s…)%s" % (version, size, digest[:12],
                                                     " + source mirror of %s" % sha if a.mirror else "")
    run(["git", "commit", "-q", "-m", msg], a.public_dir)
    print("committed:", msg)
    if not a.no_push:
        run(["git", "push"], a.public_dir)
        print("pushed")


if __name__ == "__main__":
    main()
