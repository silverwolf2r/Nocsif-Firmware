#!/usr/bin/env sh
# Runs INSIDE the Ubuntu 22.04 builder (see Dockerfile); invoked by ../build_appimage.sh.
# cwd on entry is the mounted tools/nocsif_bridge checkout.
set -e
cd "$(dirname "$0")/.."

APP=NocSifBridge
OUT=dist/NocSifBridge-linux-x86_64.AppImage
WORK=build/appimage
APPDIR="$WORK/AppDir"

rm -rf "$APPDIR" "dist/$APP"
[ -f nocsif_icon.png ] || python3 gen_icon.py

# Same inputs as build_exe.sh, but --onedir (faster start, no double-unpack) and no UPX. Data-file
# sources are absolute because --add-data resolves them relative to --specpath, not the cwd.
pyinstaller --noconfirm --clean --onedir --name "$APP" --noupx \
    --distpath dist --workpath "$WORK/pyi" --specpath "$WORK" \
    --add-data "$PWD/fonts:fonts" --add-data "$PWD/nocsif_icon.png:." \
    --collect-all esptool --hidden-import serial.tools.list_ports \
    --hidden-import PIL._tkinter_finder \
    nocsif_bridge_app.py

# Assemble the AppDir (the minimum appimagetool needs).
mkdir -p "$APPDIR/usr/lib/nocsif-bridge"
cp -a "dist/$APP/." "$APPDIR/usr/lib/nocsif-bridge/"
cp nocsif_icon.png "$APPDIR/nocsif-bridge.png"
cp nocsif_icon.png "$APPDIR/.DirIcon"
cp appimage/nocsif-bridge.desktop "$APPDIR/nocsif-bridge.desktop"
cp appimage/AppRun "$APPDIR/AppRun"
chmod +x "$APPDIR/AppRun"

# Pack with the pinned static runtime, so users need no libfuse2 installed.
ARCH=x86_64 appimagetool --runtime-file /opt/runtime-x86_64 "$APPDIR" "$OUT"
chmod +x "$OUT"
