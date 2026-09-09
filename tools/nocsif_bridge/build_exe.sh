#!/usr/bin/env sh
# Builds the macOS/Linux single-file NocSif Desktop Bridge binary with PyInstaller.
#   pip install pyinstaller pyserial esptool requests
#   sh build_exe.sh        -> dist/NocSifBridge (a .app bundle on macOS, since --windowed is set)
set -e
cd "$(dirname "$0")"
[ -f nocsif_icon.png ] || python3 gen_icon.py
python3 -m PyInstaller --noconfirm --clean --onefile --windowed --name NocSifBridge \
    --add-data "fonts:fonts" --add-data "nocsif_icon.png:." \
    --collect-all esptool --hidden-import serial.tools.list_ports \
    nocsif_bridge_app.py
echo "built dist/NocSifBridge"
