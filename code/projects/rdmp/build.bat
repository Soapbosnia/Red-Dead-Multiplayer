@echo off
rem Builds rdmp (see build.ps1). Double-click it, or: build.bat [clean] [target...]
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*
set RC=%ERRORLEVEL%
if "%RDMP_NO_PAUSE%"=="" pause
exit /b %RC%
