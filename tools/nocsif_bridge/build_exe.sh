#!/usr/bin/env sh
# NocSif Desktop Bridge — macOS / Linux standalone build (PyInstaller).
#   pip install pyinstaller pyserial esptool requests
#   sh build_exe.sh        -> dist/NocSifBridge (a .app bundle on macOS with --windowed)
set -e
cd "$(dirname "$0")"
python3 -m PyInstaller --noconfirm --clean --onefile --windowed --name NocSifBridge \
    --collect-all esptool --hidden-import serial.tools.list_ports \
    nocsif_bridge_app.py
echo "built dist/NocSifBridge"
