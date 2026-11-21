# Dump rip-relative LEA/MOV instructions in a VA range of a PE64 and resolve their targets.
# String targets are printed as text. Usage: RipDump.ps1 -Exe <path> -Start 0x140018800 -End 0x140018900
param([string]$Exe, [string]$Start, [string]$End)
$b = [IO.File]::ReadAllBytes($Exe)
$e = [BitConverter]::ToInt32($b, 0x3c); $n = [BitConverter]::ToUInt16($b, $e + 6); $opt = [BitConverter]::ToUInt16($b, $e + 20)
$ib = [BitConverter]::ToInt64($b, $e + 24 + 24); $s = $e + 24 + $opt
$secs = for ($i = 0; $i -lt $n; $i++) { $o = $s + $i * 40; [pscustomobject]@{ va = [BitConverter]::ToUInt32($b, $o + 12); vs = [BitConverter]::ToUInt32($b, $o + 8); raw = [BitConverter]::ToUInt32($b, $o + 20); rs = [BitConverter]::ToUInt32($b, $o + 16) } }
function Off([long]$va) { $r = $va - $ib; foreach ($x in $secs) { if ($r -ge $x.va -and $r -lt $x.va + $x.rs) { return [long]($x.raw + $r - $x.va) } }; return -1 }
function Str([long]$va) { $o = Off $va; if ($o -lt 0) { return $null }; $sb = New-Object Text.StringBuilder; for ($k = 0; $k -lt 120; $k++) { $c = $b[$o + $k]; if ($c -eq 0) { break }; if ($c -lt 0x20 -or $c -gt 0x7e) { return $null }; [void]$sb.Append([char]$c) }; if ($sb.Length -ge 2) { return $sb.ToString() } else { return $null } }
$sv = [Convert]::ToInt64(($Start -replace '^0x',''), 16); $ev = [Convert]::ToInt64(($End -replace '^0x',''), 16)
for ($va = $sv; $va -lt $ev; $va++) {
  $o = Off $va; if ($o -lt 0) { continue }
  $p0 = $b[$o]; $p1 = $b[$o + 1]; $p2 = $b[$o + 2]
  if (($p0 -eq 0x48 -or $p0 -eq 0x4C) -and ($p1 -eq 0x8D -or $p1 -eq 0x89 -or $p1 -eq 0x8B) -and (($p2 -band 0xC7) -eq 0x05)) {
    $disp = [BitConverter]::ToInt32($b, [int]$o + 3); $tgt = $va + 7 + $disp
    $op = @{ 0x8D = 'lea'; 0x89 = 'mov [t],r'; 0x8B = 'mov r,[t]' }[[int]$p1]
    $str = Str $tgt
    "{0:x}  {1,-10} -> {2:x}  {3}" -f $va, $op, $tgt, ($(if ($str) { '"' + $str + '"' } else { '' }))
  }
}
