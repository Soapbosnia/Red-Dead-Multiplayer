import os
import sys

import capstone

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pe_info import PE

data = open(os.path.join(os.environ.get("RDR_DIR", "."), "RDR.exe"), "rb").read()
pe = PE(data)
text = next(s for s in pe.sections if s["name"] == ".text")


def off_of(rva):
    return text["rawptr"] + (rva - text["vaddr"])


start = int(sys.argv[1], 16)
count = int(sys.argv[2]) if len(sys.argv) > 2 else 160
off = off_of(start)
code = data[off:off + count * 7]
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
for insn in md.disasm(code, pe.image_base + start):
    print(f"{insn.address:#012x}: {insn.mnemonic:<8} {insn.op_str}")
