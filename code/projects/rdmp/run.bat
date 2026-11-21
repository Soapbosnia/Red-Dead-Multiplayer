@echo off
rem Starts the game with rdmp from the build output (RDMP.exe also starts the local server, see rdmp.ini [net]).
if not exist "%~dp0build\bin\RDMP.exe" (
  echo build\bin\RDMP.exe is missing: run build.bat first.
  pause
  exit /b 1
)
start "" /d "%~dp0build\bin" "%~dp0build\bin\RDMP.exe" %*
