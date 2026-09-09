# Builds the Windows single-file NocSif Desktop Bridge executable with PyInstaller.
#   pip install pyinstaller pyserial esptool requests
#   powershell -File build_exe.ps1        -> dist\NocSifBridge.exe
$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot
python -m PyInstaller --noconfirm --clean --onefile --windowed --name NocSifBridge `
    --collect-all esptool --hidden-import serial.tools.list_ports `
    nocsif_bridge_app.py
Write-Host "built dist\NocSifBridge.exe"
