@echo off
setlocal
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0ensure_utf8_bom.ps1"
exit /b %errorlevel%
