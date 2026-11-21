#!/usr/bin/env python3
"""Find code references to a string inside a PE image.

Strings sitting in .rdata prove nothing on their own - they may be dead
leftovers. This tool locates the string, computes its RVA, then scans .text for
RIP-relative LEA/MOV instructions whose target is exactly that RVA. A hit means
shipping code actually references the string.

Usage:
    python xrefs.py <exe> "RDR2SAVE0.SAV" ["another string" ...]
"""

from __future__ import annotations

import struct
import sys

import capstone

from pe_info import PE


def find_string(pe: PE, needle: bytes) -> list[tuple[int, int]]:
    """Return (file_offset, rva) for each occurrence of needle."""
    out = []
    data = pe.data
    idx = data.find(needle)
    while idx != -1:
        rva = None
        for s in pe.sections:
            if s["rawptr"] <= idx < s["rawptr"] + s["rawsize"]:
                rva = s["vaddr"] + (idx - s["rawptr"])
                break
        if rva is not None:
            out.append((idx, rva))
        idx = data.find(needle, idx + 1)
    return out


def text_section(pe: PE) -> dict:
    for s in pe.sections:
        if s["name"] == ".text":
            return s
    raise SystemExit("no .text section")


def rip_relative_targets(pe: PE, text: dict):
    """Yield (file_offset_of_opcode, instruction_start, target_rva) for RIP-relative LEA/MOV."""
    raw = pe.data[text["rawptr"]:text["rawptr"] + text["rawsize"]]
    base_rva = text["vaddr"]
    n = len(raw)
    i = 0
    while i < n - 6:
        op = raw[i]
        if op in (0x8B, 0x8D):
            modrm = raw[i + 1]
            if (modrm & 0xC7) == 0x05:  # mod=00, rm=101 -> RIP+disp32
                disp = struct.unpack_from("<i", raw, i + 2)[0]
                rex = 1 if (i > 0 and 0x40 <= raw[i - 1] <= 0x4F) else 0
                start = i - rex
                end = i + 6  # opcode+modrm+disp32, from opcode index
                target = base_rva + end + disp
                yield start, base_rva + start, target
        i += 1


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    exe, needles = sys.argv[1], sys.argv[2:]
    data = open(exe, "rb").read()
    pe = PE(data)
    text = text_section(pe)
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = False

    for needle in needles:
        nb = needle.encode("latin-1")
        occurrences = find_string(pe, nb)
        print(f"\n=== {needle!r}: {len(occurrences)} occurrence(s) ===")
        if not occurrences:
            continue
        for off, rva in occurrences[:4]:
            print(f"  string @ file 0x{off:X}  rva 0x{rva:X}")
            hits = 0
            for start, insn_rva, target in rip_relative_targets(pe, text):
                if target != rva:
                    continue
                hits += 1
                file_off = text["rawptr"] + (insn_rva - text["vaddr"])
                print(f"    xref @ rva 0x{insn_rva:X} (file 0x{file_off:X})")
                lo = max(0, file_off - 24)
                chunk = data[lo:file_off + 32]
                for insn in md.disasm(chunk, pe.image_base + insn_rva - (file_off - lo)):
                    marker = "  <== " if insn.address == pe.image_base + insn_rva else "      "
                    print(f"    {marker}{insn.address:#012x}: {insn.mnemonic:<7} {insn.op_str}")
            if hits == 0:
                print("    (no RIP-relative xref found in .text)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
