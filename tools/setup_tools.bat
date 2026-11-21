@echo off
rem Double-click: runs setup_tools.ps1 (one-time move of the dev tools into D:\Development\Tools).
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0setup_tools.ps1"
pause
