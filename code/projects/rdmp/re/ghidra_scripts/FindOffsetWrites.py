# Find instructions that WRITE to memory at [reg + <offset>] (e.g. a struct field), list the containing functions.
# Usage: -postScript FindOffsetWrites.py <out_file.txt> <hex_offset> [<hex_offset> ...]
# @category RDRMP
from ghidra.program.model.scalar import Scalar

args = list(getScriptArgs())
out_path = args[0]
offsets = set(int(x, 16) for x in args[1:])
prog = currentProgram
fm = prog.getFunctionManager()
listing = prog.getListing()
out = open(out_path, "w")
hits = 0
for ins in listing.getInstructions(True):
    if monitor.isCancelled():
        break
    mn = ins.getMnemonicString()
    if not (mn.startswith("MOV") or mn in ("AND", "OR", "XOR", "ADD", "SUB", "INC", "DEC", "LEA")):
        continue
    if mn == "LEA":
        # LEA reg, [reg + off]: address of the field taken (written through a pointer later)
        rep1 = ins.getDefaultOperandRepresentation(1) if ins.getNumOperands() > 1 else ""
        for o in ins.getOpObjects(1):
            if isinstance(o, Scalar) and o.getUnsignedValue() in offsets and "RIP" not in rep1:
                fn = fm.getFunctionContaining(ins.getAddress())
                out.write("%s\t%s\t%s\n" % (ins.getAddress(), "%s@%s" % (fn.getName(), fn.getEntryPoint()) if fn else "?", ins))
                hits += 1
                break
        continue
    if ins.getNumOperands() < 1:
        continue
    # operand 0 must be a memory operand containing one of the offsets
    objs = ins.getOpObjects(0)
    rep = ins.getDefaultOperandRepresentation(0)
    if "[" not in rep:
        continue
    for o in objs:
        if isinstance(o, Scalar) and o.getUnsignedValue() in offsets:
            fn = fm.getFunctionContaining(ins.getAddress())
            out.write("%s\t%s\t%s\n" % (ins.getAddress(), "%s@%s" % (fn.getName(), fn.getEntryPoint()) if fn else "?", ins))
            hits += 1
            break
out.close()
print("[FindOffsetWrites] %d hits -> %s" % (hits, out_path))
