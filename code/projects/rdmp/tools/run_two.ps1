# Starts two copies of the game on this PC (sync testing): copy 1 (also starts the local server), then copy 2
# (RDMP.exe --instance 2) once copy 1 is up. Stops running copies first.
#   powershell -File tools\run_two.ps1 [-Delay 20]
param([int]$Delay = 20)
$bin = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\build\bin'))
Get-Process RDR -ErrorAction SilentlyContinue | Stop-Process -Force
Get-Process RDR -ErrorAction SilentlyContinue | Wait-Process -Timeout 15 -ErrorAction SilentlyContinue
Start-Process (Join-Path $bin 'RDMP.exe') -WorkingDirectory $bin
Start-Sleep $Delay
Start-Process (Join-Path $bin 'RDMP.exe') -ArgumentList '--instance', '2' -WorkingDirectory $bin
"started two copies from $bin"
