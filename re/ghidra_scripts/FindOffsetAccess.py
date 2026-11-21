# Find instructions that access memory at [reg + <offset>] in ANY operand (read or write); list containing functions.
# Usage: -postScript FindOffsetAccess.py <out_file.txt> <hex_offset> [<hex_offset> ...]
# @category RDRMP
from ghidra.program.model.scalar import Scalar

args = list(getScriptArgs())
out_path = args[0]
offsets = set(int(x, 16) for x in args[1:])
fm = currentProgram.getFunctionManager()
out = open(out_path, "w")
hits = 0
for ins in currentProgram.getListing().getInstructions(True):
    if monitor.isCancelled():
        break
    for i in range(ins.getNumOperands()):
        rep = ins.getDefaultOperandRepresentation(i)
        if "[" not in rep or "RIP" in rep:
            continue
        found = False
        for o in ins.getOpObjects(i):
            if isinstance(o, Scalar) and o.getUnsignedValue() in offsets:
                found = True
                break
        if found:
            fn = fm.getFunctionContaining(ins.getAddress())
            out.write("%s\t%s\t%s\n" % (ins.getAddress(), "%s@%s" % (fn.getName(), fn.getEntryPoint()) if fn else "?", ins))
            hits += 1
            break
out.close()
print("[FindOffsetAccess] %d hits -> %s" % (hits, out_path))
