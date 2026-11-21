# Ghidra headless helper: runs one script from re\ghidra_scripts against RDR.exe in the shared Ghidra project.
#   re\gh.ps1 DecompileAt.java 0x14010DC10
#   re\gh.ps1 DecompileAt.java "0x14010DC10 0x140103BA0"
# Tools are looked up in RDMP_TOOLS (default: ..\..\Tools beside this repository, i.e. D:\Development\Tools).
# The full log goes to re\out\<script>.log; only the script's own output is printed.
param([Parameter(Mandatory = $true)][string]$Script, [string]$Addrs = '')
$repo  = Split-Path $PSScriptRoot -Parent
$tools = if ($env:RDMP_TOOLS) { $env:RDMP_TOOLS } else { [IO.Path]::GetFullPath((Join-Path $repo '..\..\Tools')) }
$ghidra = Get-ChildItem $tools -Directory -Filter 'ghidra_*_PUBLIC' | Sort-Object Name | Select-Object -Last 1
if (-not $ghidra) { throw "No ghidra_*_PUBLIC folder in $tools" }
$jdk = Get-ChildItem $tools -Directory -Filter 'jdk-*' | Sort-Object Name | Select-Object -Last 1
if ($jdk) { $env:JAVA_HOME = $jdk.FullName }
$project = Join-Path $tools 'ghidra-projects\rdr'
$gpr = Get-ChildItem $project -Filter '*.gpr' | Select-Object -First 1
if (-not $gpr) { throw "No Ghidra project (*.gpr) in $project" }

# PowerShell may hand numeric literals over as decimal strings; the scripts want hex.
$hexArgs = @()
foreach ($a in ($Addrs -split '[,;\s]+' | Where-Object { $_ })) {
    if ($a -match '^\d+$') { $hexArgs += ('0x{0:X}' -f [uint64]$a) } else { $hexArgs += $a }
}
$logDir = Join-Path $PSScriptRoot 'out'
New-Item -ItemType Directory -Force $logDir | Out-Null
$log = Join-Path $logDir (($Script -replace '\.(java|py)$', '') + '.log')
& (Join-Path $ghidra.FullName 'support\analyzeHeadless.bat') $project $gpr.BaseName `
    -process RDR.exe -noanalysis -scriptPath (Join-Path $PSScriptRoot 'ghidra_scripts') `
    -postScript $Script @hexArgs 2>&1 | Set-Content -Path $log -Encoding utf8
$text = Get-Content $log
$start = 0
for ($i = 0; $i -lt $text.Count; $i++) {
    if ($text[$i] -match '^===== |^NO FUNCTION|^(void|int|uint|undefined|longlong|char|bool|long|byte) ') { $start = $i; break }
}
$text[$start..($text.Count - 1)]
