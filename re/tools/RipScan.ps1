# Scan a PE64's .text for RIP-relative references (disp32 with no trailing immediate) to target VAs.
# Usage: RipScan.ps1 -Exe <path> -Targets 0x141cae6c0,0x141cae620
param([string]$Exe, [string[]]$Targets)
Add-Type -TypeDefinition @"
using System; using System.Collections.Generic; using System.IO;
public static class RipScan {
  public static List<string> Scan(string path, long[] targets) {
    byte[] b = File.ReadAllBytes(path);
    int e = BitConverter.ToInt32(b, 0x3c);
    int n = BitConverter.ToUInt16(b, e + 6);
    int opt = BitConverter.ToUInt16(b, e + 20);
    long imageBase = BitConverter.ToInt64(b, e + 24 + 24);
    int s = e + 24 + opt;
    var res = new List<string>();
    var set = new HashSet<long>(targets);
    for (int i = 0; i < n; i++) {
      int o = s + i * 40;
      uint ch = BitConverter.ToUInt32(b, o + 36);
      if ((ch & 0x20) == 0) continue; // code sections only
      uint va = BitConverter.ToUInt32(b, o + 12);
      uint rawSize = BitConverter.ToUInt32(b, o + 16);
      uint raw = BitConverter.ToUInt32(b, o + 20);
      for (uint p = 0; p + 4 <= rawSize; p++) {
        int disp = BitConverter.ToInt32(b, (int)(raw + p));
        long next = imageBase + va + p + 4;
        long tgt = next + disp;
        if (set.Contains(tgt)) {
          // show 3 bytes before disp so the opcode can be checked (e.g. 48 8D 05 = lea rax)
          string pre = BitConverter.ToString(b, (int)(raw + p - 3), 3);
          res.Add(string.Format("{0:x}\t{1:x}\t{2}", tgt, imageBase + va + p - 3, pre));
        }
      }
    }
    return res;
  }
}
"@
$t = $Targets | ForEach-Object { [Convert]::ToInt64(($_ -replace '^0x',''), 16) }
[RipScan]::Scan($Exe, [long[]]$t) | ForEach-Object { $_ }
