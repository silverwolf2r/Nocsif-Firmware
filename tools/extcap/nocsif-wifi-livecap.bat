@echo off
rem Windows shim for the NocSif live-capture extcap: Wireshark on Windows only launches .bat/.exe
rem extcaps, never a bare .py, so this just relays to the Python script. Needs Python 3 + pyserial on PATH.
python "%~dp0nocsif-wifi-livecap.py" %*
