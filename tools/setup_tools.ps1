# One-time setup for this PC: moves the RE/dev tools out of the old project folders into D:\Development\Tools
# and points the build at the MafiaHub Framework SDK there. Safe to run twice (moves only what is still in the
# old place). Writes setup_tools.log next to this script.
#
#   Ghidra 12.2, JDK 25          rdo.pre-rdrmp\_tools\...            -> Tools\ghidra_12.2_PUBLIC, Tools\jdk-25.0.5+8
#   x64dbg, rizin, Magic-RDR     rdo.pre-rdrmp\_tools\...            -> Tools\x64dbg, Tools\rizin, Tools\Magic-RDR
#   Ghidra project (RDR.exe)     rdo.pre-rdrmp\_tools\ghproj         -> Tools\ghidra-projects\rdr
#   Framework SDK 44.1.1         _incoming\framework-sdk-44.1.1\sdk  -> Tools\MafiaHub\Framework-44.1.1-windows
#   vcpkg checkout (+downloads)  D:\fwb\vcpkg                        -> Tools\MafiaHub\vcpkg
#   MTA reference gamemode       _incoming\ug\unethical-gangsters-main -> Tools\reference\unethical-gangsters
#
# FW_SDK_CACHE_DIR (user environment) = Tools\MafiaHub: the Framework's own bootstrap looks there for the SDK and
# its vcpkg checkout, so the repository holds no machine paths.

$ErrorActionPreference = 'Continue'
$log = Join-Path $PSScriptRoot 'setup_tools.log'
Start-Transcript -Path $log -Force | Out-Null

$dev   = 'D:\Development'
$tools = Join-Path $dev 'Tools'
$old   = Join-Path $dev 'Repositories\rdo.pre-rdrmp\_tools'
$inc   = Join-Path $dev 'Repositories\_incoming'

function Move-Tool([string]$from, [string]$to) {
    if (-not (Test-Path -LiteralPath $from)) {
        if (Test-Path -LiteralPath $to) { Write-Host "ok      $to (already moved)" }
        else { Write-Host "MISSING $from" }
        return
    }
    if (Test-Path -LiteralPath $to) { Write-Host "SKIP    $to exists, left $from in place"; return }
    $parent = Split-Path -Parent $to
    if (-not (Test-Path -LiteralPath $parent)) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }
    try {
        Move-Item -LiteralPath $from -Destination $to -ErrorAction Stop
        Write-Host "moved   $from -> $to"
    } catch {
        Write-Host "FAILED  $from -> $to : $($_.Exception.Message)"
    }
}

Move-Tool "$old\ghidra\ghidra_12.2_PUBLIC"                      "$tools\ghidra_12.2_PUBLIC"
Move-Tool "$old\jdk25\jdk-25.0.5+8"                             "$tools\jdk-25.0.5+8"
Move-Tool "$old\x64dbg"                                         "$tools\x64dbg"
Move-Tool "$old\rizin\rizin-win-installer-vs2019_static-64"     "$tools\rizin"
Move-Tool "$old\cw\magicrdr\Magic-RDR-master"                   "$tools\Magic-RDR"
Move-Tool "$old\ghproj"                                         "$tools\ghidra-projects\rdr"
Move-Tool "$inc\framework-sdk-44.1.1\sdk\Framework-44.1.1-windows" "$tools\MafiaHub\Framework-44.1.1-windows"
Move-Tool "D:\fwb\vcpkg"                                        "$tools\MafiaHub\vcpkg"
Move-Tool "$inc\ug\unethical-gangsters-main"                    "$tools\reference\unethical-gangsters"

# The Framework bootstrap trusts an extracted SDK only with this stamp (the archive's SHA-256, FrameworkPin.cmake).
$sdk = "$tools\MafiaHub\Framework-44.1.1-windows"
if (Test-Path -LiteralPath $sdk) {
    Set-Content -LiteralPath "$sdk\.fw-extracted" -Value '56ed287b3e54e0d83f2922ed3a2933cdf14986abe778de499dd68ef80b049a08' -Encoding ascii
    Write-Host "stamped $sdk\.fw-extracted"
}
[Environment]::SetEnvironmentVariable('FW_SDK_CACHE_DIR', "$tools\MafiaHub", 'User')
Write-Host "set     FW_SDK_CACHE_DIR=$tools\MafiaHub (user environment)"

# Ghidra finds its JDK without a global JAVA_HOME.
$launch = "$tools\ghidra_12.2_PUBLIC\support\launch.properties"
if (Test-Path -LiteralPath $launch) {
    $text = Get-Content -LiteralPath $launch -Raw
    $line = "JAVA_HOME_OVERRIDE=$tools\jdk-25.0.5+8"
    if ($text -notmatch [regex]::Escape($line)) {
        $text = [regex]::Replace($text, '(?m)^JAVA_HOME_OVERRIDE=.*$', '')
        Set-Content -LiteralPath $launch -Value ($text.TrimEnd() + "`r`n" + $line + "`r`n") -Encoding ascii
        Write-Host "set     $line in launch.properties"
    }
}

Write-Host ''
Write-Host 'Done. Close this window.'
Stop-Transcript | Out-Null
