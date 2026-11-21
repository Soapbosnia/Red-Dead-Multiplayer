# For each target address: list callers (and callees), and decompile the callers.
# Usage: -postScript Callers.py <out_file.c> <depth_decompile_callers:0|1> <addr...>
# @category RDRMP
from ghidra.app.decompiler import DecompInterface, DecompileOptions

args = list(getScriptArgs())
out_path = args[0]
dec_callers = args[1] == "1"
targets = args[2:]
prog = currentProgram
fm = prog.getFunctionManager()
rm = prog.getReferenceManager()
af = prog.getAddressFactory()

d = DecompInterface()
d.setOptions(DecompileOptions())
d.openProgram(prog)


def decomp(fn):
    r = d.decompileFunction(fn, 120, monitor)
    if r and r.decompileCompleted():
        return r.getDecompiledFunction().getC()
    return "/* FAILED */\n"


f = open(out_path, "w")
done = set()
for t in targets:
    ta = af.getAddress(t)
    fn = fm.getFunctionContaining(ta)
    f.write("/* ################ TARGET %s (%s) ################ */\n" % (t, fn.getName() if fn else "?"))
    callers = []
    for r in rm.getReferencesTo(fn.getEntryPoint() if fn else ta):
        cf = fm.getFunctionContaining(r.getFromAddress())
        callers.append((r.getFromAddress(), r.getReferenceType(), cf))
    f.write("/* callers (%d):\n" % len(callers))
    for (fa, rt, cf) in callers:
        f.write("   %s %s in %s\n" % (fa, rt, cf.getName() + "@" + str(cf.getEntryPoint()) if cf else "?"))
    f.write("*/\n")
    if fn:
        callees = sorted(set("%s@%s" % (c.getName(), c.getEntryPoint()) for c in fn.getCalledFunctions(monitor)))
        f.write("/* callees: %s */\n" % ", ".join(callees))
        f.write(decomp(fn))
    if dec_callers:
        for (fa, rt, cf) in callers:
            if cf is None or cf.getEntryPoint() in done:
                continue
            done.add(cf.getEntryPoint())
            f.write("\n/* ==== caller %s @ %s ==== */\n" % (cf.getName(), cf.getEntryPoint()))
            f.write(decomp(cf))
    f.write("\n\n")
f.close()
print("[Callers] wrote %s" % out_path)
