# List references to addresses (data or code) with ref type and containing function.
# Optionally decompile the referencing functions.
# Usage: -postScript RefsTo.py <out_file.c> <decompile:0|1> <filter:ALL|WRITE|READ|CALL> <addr...>
# @category RDRMP
from ghidra.app.decompiler import DecompInterface, DecompileOptions

args = list(getScriptArgs())
out_path, dec, flt = args[0], args[1] == "1", args[2]
targets = args[3:]
prog = currentProgram
fm = prog.getFunctionManager()
rm = prog.getReferenceManager()
af = prog.getAddressFactory()
d = DecompInterface()
d.setOptions(DecompileOptions())
d.openProgram(prog)

f = open(out_path, "w")
todo = []
seen = set()
for t in targets:
    ta = af.getAddress(t)
    f.write("/* ##### refs to %s\n" % t)
    for r in rm.getReferencesTo(ta):
        rt = r.getReferenceType()
        if flt == "WRITE" and not rt.isWrite():
            continue
        if flt == "READ" and not rt.isRead():
            continue
        if flt == "CALL" and not rt.isCall():
            continue
        cf = fm.getFunctionContaining(r.getFromAddress())
        f.write("   %s %-20s %s\n" % (r.getFromAddress(), rt,
                                       "%s@%s" % (cf.getName(), cf.getEntryPoint()) if cf else "?"))
        if cf and cf.getEntryPoint() not in seen:
            seen.add(cf.getEntryPoint())
            todo.append(cf)
    f.write("*/\n")
if dec:
    for cf in todo:
        r = d.decompileFunction(cf, 120, monitor)
        f.write("\n/* ==== %s @ %s ==== */\n" % (cf.getName(), cf.getEntryPoint()))
        f.write(r.getDecompiledFunction().getC() if r and r.decompileCompleted() else "/* FAILED */\n")
f.close()
print("[RefsTo] %d functions -> %s" % (len(todo), out_path))
