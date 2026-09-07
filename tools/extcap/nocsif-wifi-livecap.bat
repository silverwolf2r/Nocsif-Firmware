@echo off
rem Windows launcher for the NocSif live-capture extcap. Wireshark on Windows executes .bat/.exe
rem extcaps (not bare .py), so this hands off to Python. Requires Python 3 + pyserial on PATH.
python "%~dp0nocsif-wifi-livecap.py" %*
