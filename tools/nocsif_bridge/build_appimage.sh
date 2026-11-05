#!/usr/bin/env sh
# Builds dist/NocSifBridge-linux-x86_64.AppImage inside an Ubuntu 22.04 container, so it runs on any
# glibc >= 2.35 distro (Ubuntu 22.04+, Debian 12+, Fedora 36+). Needs Docker. x86_64 only.
#
#   ./build_appimage.sh
#
# The Windows .exe is built with release_app.ps1; the macOS/Linux source binary with build_exe.sh.
set -e
cd "$(dirname "$0")"
IMAGE=nocsif-bridge-appimage-builder
docker build -t "$IMAGE" appimage
docker run --rm -e HOME=/tmp --user "$(id -u):$(id -g)" -v "$PWD":/src -w /src "$IMAGE" \
    sh appimage/build-in-container.sh
echo
echo "built dist/NocSifBridge-linux-x86_64.AppImage"
echo "publish: gh release upload app-v<version> dist/NocSifBridge-linux-x86_64.AppImage --repo silverwolf2r/Nocsif-Firmware"
