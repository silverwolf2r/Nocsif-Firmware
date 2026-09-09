# Builds the Windows NocSif Desktop Bridge exe and, optionally, publishes it as a GitHub Release on
# the public mirror repo (the same one the watch pulls its firmware updates from):
#
#   powershell -File release_app.ps1            # build only (dist\NocSifBridge.exe)
#   powershell -File release_app.ps1 -Publish   # build + `gh release create app-v<APP_VERSION>` with the exe
#
# APP_VERSION is read straight out of nocsif_bridge_app.py; the app itself polls the releases list for
# a newer `app-v*` tag and surfaces an "update available" link. The macOS/Linux binaries are built
# separately with build_exe.sh and attached to the same release via gh release upload.
param([switch]$Publish, [string]$Repo = "silverwolf2r/Nocsif-Firmware")
$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot
$ver = (Select-String -Path nocsif_bridge_app.py -Pattern '^APP_VERSION\s*=\s*"([^"]+)"').Matches[0].Groups[1].Value
if (-not $ver) { throw "APP_VERSION not found in nocsif_bridge_app.py" }
Write-Host "building NocSif Desktop Bridge v$ver"
if (-not (Test-Path nocsif.ico)) { python gen_icon.py }
python -m PyInstaller --noconfirm --clean --onefile --windowed --name NocSifBridge `
    --icon nocsif.ico --add-data "fonts;fonts" --add-data "nocsif.ico;." --add-data "nocsif_icon.png;." `
    --collect-all esptool --hidden-import serial.tools.list_ports `
    nocsif_bridge_app.py
$exe = Join-Path $PSScriptRoot "dist\NocSifBridge.exe"
if (-not (Test-Path $exe)) { throw "build produced no exe" }
Write-Host ("built {0} ({1:N1} MB)" -f $exe, ((Get-Item $exe).Length / 1MB))
if ($Publish) {
    $tag = "app-v$ver"
    $notes = @"
NocSif Desktop Bridge v$ver — the computer-side companion for the NocSif watch (Windows build).

Plug the watch in over USB-C: flash / update / provision a watch, run the hardware-defect check, manage the
microSD, drive the watch from the computer with a live view of its screen. See tools/nocsif_bridge/README.md.

macOS / Linux: run from source (python nocsif_bridge_app.py) or build with build_exe.sh.
"@
    $notesFile = Join-Path $env:TEMP "nocsif_app_release_notes.md"
    Set-Content -Path $notesFile -Value $notes -Encoding utf8
    # Copy to a descriptive filename before uploading: gh names the release asset after the file's own
    # basename (a `file#label` only sets a display label, the download URL still points at the real
    # filename), so this keeps the stable .../releases/latest/download/NocSifBridge-windows-x64.exe link.
    $named = Join-Path $PSScriptRoot "dist\NocSifBridge-windows-x64.exe"
    Copy-Item $exe $named -Force
    gh release create $tag "$named" --repo $Repo --title "NocSif Desktop Bridge v$ver" --notes-file $notesFile
    Write-Host "published https://github.com/$Repo/releases/tag/$tag"
}
