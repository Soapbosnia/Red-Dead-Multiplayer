# Decompile a function and its callees down to a given depth.
# Usage: -postScript CalleeTree.py <out_file.c> <depth> <max_funcs> <addr...>
# Skips functions larger than 6000 bytes below the root (big helpers / allocators).
# @category RDRMP
from ghidra.app.decompiler import DecompInterface, DecompileOptions

args = list(getScriptArgs())
out_path = args[0]
depth = int(args[1])
max_funcs = int(args[2])
roots = args[3:]
prog = currentProgram
fm = prog.getFunctionManager()
af = prog.getAddressFactory()

d = DecompInterface()
d.setOptions(DecompileOptions())
d.openProgram(prog)

seen = set()
order = []
queue = []
for r in roots:
    fn = fm.getFunctionContaining(af.getAddress(r))
    if fn:
        queue.append((fn, 0, None))
while queue and len(order) < max_funcs:
    fn, lvl, parent = queue.pop(0)
    ep = fn.getEntryPoint()
    if ep in seen:
        continue
    seen.add(ep)
    if lvl > 0 and fn.getBody().getNumAddresses() > 6000:
        order.append((fn, lvl, parent, True))
        continue
    order.append((fn, lvl, parent, False))
    if lvl < depth:
        for c in sorted(fn.getCalledFunctions(monitor), key=lambda x: x.getEntryPoint()):
            if c.isExternal() or c.isThunk():
                continue
            queue.append((c, lvl + 1, fn))

f = open(out_path, "w")
f.write("/* call tree:\n")
for fn, lvl, parent, big in order:
    f.write("%s%s@%s size=%d%s\n" % ("  " * lvl, fn.getName(), fn.getEntryPoint(),
                                     fn.getBody().getNumAddresses(), " [skipped: big]" if big else ""))
f.write("*/\n\n")
for fn, lvl, parent, big in order:
    if big:
        continue
    r = d.decompileFunction(fn, 120, monitor)
    f.write("/* ==== [L%d] %s @ %s (from %s) ==== */\n" % (
        lvl, fn.getName(), fn.getEntryPoint(), parent.getName() if parent else "-"))
    f.write(r.getDecompiledFunction().getC() if r and r.decompileCompleted() else "/* FAILED */\n")
    f.write("\n")
f.close()
print("[CalleeTree] %d functions -> %s" % (len(order), out_path))
