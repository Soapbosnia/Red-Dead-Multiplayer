# Find MSVC x64 vtables in a PE64: a qword pointing into .rdata (the RTTI Complete Object Locator)
# immediately followed by >= 1 qwords pointing into executable code.
# Output TSV: vtable_va  col_va  slot_count  slot_targets(comma-separated)
# Usage: VtScan.ps1 -Exe <path> -Out <file.tsv>
param([string]$Exe, [string]$Out)
Add-Type -TypeDefinition @"
using System; using System.Collections.Generic; using System.IO; using System.Text;
public static class VtScan {
  public static int Scan(string path, string outPath) {
    byte[] b = File.ReadAllBytes(path);
    int e = BitConverter.ToInt32(b, 0x3c);
    int n = BitConverter.ToUInt16(b, e + 6);
    int opt = BitConverter.ToUInt16(b, e + 20);
    long ib = BitConverter.ToInt64(b, e + 24 + 24);
    int s = e + 24 + opt;
    long tx0 = 0, tx1 = 0, rd0 = 0, rd1 = 0; uint rdRaw = 0, rdSize = 0;
    for (int i = 0; i < n; i++) {
      int o = s + i * 40;
      string name = Encoding.ASCII.GetString(b, o, 8).TrimEnd('\0');
      uint va = BitConverter.ToUInt32(b, o + 12), vs = BitConverter.ToUInt32(b, o + 8);
      uint rs = BitConverter.ToUInt32(b, o + 16), rp = BitConverter.ToUInt32(b, o + 20);
      if (name == ".text") { tx0 = ib + va; tx1 = ib + va + vs; }
      if (name == ".rdata") { rd0 = ib + va; rd1 = ib + va + vs; rdRaw = rp; rdSize = Math.Min(rs, vs); }
    }
    var sw = new StreamWriter(outPath);
    int count = 0;
    for (uint p = 0; p + 16 <= rdSize; p += 8) {
      long col = BitConverter.ToInt64(b, (int)(rdRaw + p));
      if (col < rd0 || col >= rd1) continue;
      // COL signature: dword 1 (x64), then typeDescriptor RVA, classDescriptor RVA, self RVA
      long colOff = col - rd0;
      if (colOff + 24 > rdSize) continue;
      if (BitConverter.ToUInt32(b, (int)(rdRaw + colOff)) != 1) continue;
      uint self = BitConverter.ToUInt32(b, (int)(rdRaw + colOff + 20));
      if (self != (uint)(col - ib)) continue;
      var slots = new List<string>();
      uint q = p + 8;
      while (q + 8 <= rdSize) {
        long v = BitConverter.ToInt64(b, (int)(rdRaw + q));
        if (v < tx0 || v >= tx1) break;
        slots.Add(v.ToString("x"));
        q += 8;
      }
      if (slots.Count == 0) continue;
      sw.WriteLine(string.Format("{0:x}\t{1:x}\t{2}\t{3}", rd0 + p + 8, col, slots.Count, string.Join(",", slots)));
      count++;
    }
    sw.Close();
    return count;
  }
}
"@
"vtables: " + [VtScan]::Scan($Exe, $Out)
