# Builds rdmp: finds Visual Studio (any edition with the C++ x64 tools), sets up the x64 compiler environment and runs
# CMake + Ninja from Visual Studio. Output: build\bin. Full log: build\build.log.
#
#   build.bat              configure (first time) and build everything
#   build.bat clean        delete build\ (keeps nothing) and build from scratch
#   build.bat RDMPServer   build only the named target(s)
param([Parameter(ValueFromRemainingArguments = $true)][string[]]$Targets)

$ErrorActionPreference = 'Stop'
$root  = $PSScriptRoot
$build = Join-Path $root 'build'
$log   = Join-Path $build 'build.log'

if ($Targets -and $Targets[0] -eq 'clean') {
    if (Test-Path $build) { Remove-Item -Recurse -Force $build }
    $Targets = $Targets | Select-Object -Skip 1
}
New-Item -ItemType Directory -Force $build | Out-Null
$logWriter = New-Object IO.StreamWriter($log, $false, (New-Object Text.UTF8Encoding($false)))
$logWriter.AutoFlush = $true
$logWriter.WriteLine("rdmp build $(Get-Date -Format s)")

function Say($text) { Write-Host $text; $logWriter.WriteLine($text) }
function Fail($text) { Say "BUILD FAILED: $text"; $logWriter.Close(); exit 1 }

# --- Visual Studio ---------------------------------------------------------------------------------------------------
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { Fail 'vswhere.exe not found: install Visual Studio (or Build Tools) with "Desktop development with C++".' }
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { Fail 'No Visual Studio with the C++ x64 tools found.' }
Say "Visual Studio: $vs"

$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
$env:PATH = "$(Split-Path $vswhere);$env:PATH"   # vcvars looks vswhere up on PATH
cmd /c "`"$vcvars`" >nul && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process') }
}
$vsCMake = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake'
$env:PATH = "$vsCMake\CMake\bin;$vsCMake\Ninja;$env:PATH"
$cmake = Get-Command cmake -ErrorAction SilentlyContinue
if (-not $cmake) { Fail 'cmake not found (Visual Studio component "C++ CMake tools for Windows").' }

# --- Framework SDK cache -------------------------------------------------------------------------------------------
# FW_SDK_CACHE_DIR holds the Framework SDK and its vcpkg checkout (downloaded there on first configure if missing).
if (-not $env:FW_SDK_CACHE_DIR) { $env:FW_SDK_CACHE_DIR = [Environment]::GetEnvironmentVariable('FW_SDK_CACHE_DIR', 'User') }
# Default: Tools\MafiaHub next to the folder that holds this repository (Development\Repositories\<repo> ->
# Development\Tools\MafiaHub).
if (-not $env:FW_SDK_CACHE_DIR) {
    $repoTop = (& git -C $root rev-parse --show-toplevel 2>$null)
    if (-not $repoTop) { $repoTop = $root }
    $env:FW_SDK_CACHE_DIR = [IO.Path]::GetFullPath((Join-Path $repoTop '..\..\Tools\MafiaHub'))
}
Say "Framework SDK cache: $env:FW_SDK_CACHE_DIR"

# --- configure + build ---------------------------------------------------------------------------------------------
function Run([string[]]$cmdArgs) {
    Say "> cmake $($cmdArgs -join ' ')"
    # Windows PowerShell turns native stderr lines into errors; they are ordinary output here.
    $ErrorActionPreference = 'Continue'
    & cmake @cmdArgs 2>&1 | ForEach-Object { Say "$_" }
    $code = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    return $code
}

if (-not (Test-Path (Join-Path $build 'build.ninja'))) {
    if ((Run @('-S', $root, '-B', $build, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release')) -ne 0) { Fail 'configure (see build\build.log)' }
}
$buildArgs = @('--build', $build)
if ($Targets) { $buildArgs += @('--target') + $Targets }
$buildArgs += @('--', '-k', '0')   # keep going: one run reports every compile error
if ((Run $buildArgs) -ne 0) { Fail 'compile (see build\build.log)' }

Say "BUILD OK -> $build\bin"
$logWriter.Close()
