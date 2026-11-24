# Starts two copies of the game on this PC (sync testing): the local server and copy 1 joined to it, then copy 2
# (RDMP.exe --instance 2) once copy 1 is up. Stops running copies first.
#   powershell -File tools\run_two.ps1 [-Delay 20]
param([int]$Delay = 20)
$bin = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\build\bin'))
Get-Process RDR -ErrorAction SilentlyContinue | Stop-Process -Force
Get-Process RDR -ErrorAction SilentlyContinue | Wait-Process -Timeout 15 -ErrorAction SilentlyContinue
# Started directly (CreateProcess), not through the shell: Start-Process goes through ShellExecute, which stops on
# Windows' "publisher could not be verified" prompt for an unsigned build.
function Start-Rdmp([string]$arguments) {
    $info = New-Object Diagnostics.ProcessStartInfo (Join-Path $bin 'RDMP.exe'), $arguments
    $info.WorkingDirectory = $bin
    $info.UseShellExecute = $false
    [Diagnostics.Process]::Start($info) | Out-Null
}
Start-Rdmp '--server --connect 127.0.0.1:27015'
Start-Sleep $Delay
Start-Rdmp '--instance 2 --connect 127.0.0.1:27015'
"started two copies from $bin"
