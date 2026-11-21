#!/usr/bin/env python3
"""Find RIP-relative code references to an absolute virtual address.

Usage: python find_xref_va.py <exe> <hex-va> [context_bytes]
"""

from __future__ import annotations

import struct
import sys

import capstone

from pe_info import PE


def main() -> int:
    exe, va_text = sys.argv[1], sys.argv[2]
    va = int(va_text, 16)
    data = open(exe, "rb").read()
    pe = PE(data)
    text = next(s for s in pe.sections if s["name"] == ".text")
    raw = data[text["rawptr"]:text["rawptr"] + text["rawsize"]]
    base_rva = text["vaddr"]
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)

    hits = []
    n = len(raw)
    i = 0
    while i < n - 6:
        if raw[i] in (0x8B, 0x8D) and (raw[i + 1] & 0xC7) == 0x05:
            disp = struct.unpack_from("<i", raw, i + 2)[0]
            rex = 1 if (i > 0 and 0x40 <= raw[i - 1] <= 0x4F) else 0
            start = i - rex
            target = pe.image_base + base_rva + i + 6 + disp
            if target == va:
                hits.append(base_rva + start)
        i += 1

    print(f"target 0x{va:X}: {len(hits)} reference(s)")
    for rva in hits[:20]:
        off = text["rawptr"] + (rva - base_rva)
        print(f"\n  at rva 0x{rva:X} (file 0x{off:X})")
        chunk = data[max(0, off - 32):off + 48]
        for insn in md.disasm(chunk, pe.image_base + rva - (off - max(0, off - 32))):
            mark = "  <==" if insn.address == pe.image_base + rva else "     "
            print(f"  {mark} {insn.address:#012x}: {insn.mnemonic:<7} {insn.op_str}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
