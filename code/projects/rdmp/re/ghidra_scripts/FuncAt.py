# Print the function containing each address (and optionally decompile them).
# Usage: -postScript FuncAt.py <out_file.c> <decompile:0|1> <addr...>
# @category RDRMP
from ghidra.app.decompiler import DecompInterface, DecompileOptions

args = list(getScriptArgs())
out_path, dec = args[0], args[1] == "1"
prog = currentProgram
fm = prog.getFunctionManager()
af = prog.getAddressFactory()
d = DecompInterface()
d.setOptions(DecompileOptions())
d.openProgram(prog)
f = open(out_path, "w")
seen = []
for s in args[2:]:
    fn = fm.getFunctionContaining(af.getAddress(s))
    f.write("/* %s -> %s */\n" % (s, "%s@%s size=%d" % (fn.getName(), fn.getEntryPoint(), fn.getBody().getNumAddresses()) if fn else "none"))
    if fn and fn not in seen:
        seen.append(fn)
if dec:
    for fn in seen:
        r = d.decompileFunction(fn, 120, monitor)
        f.write("\n/* ==== %s @ %s ==== */\n" % (fn.getName(), fn.getEntryPoint()))
        f.write(r.getDecompiledFunction().getC() if r and r.decompileCompleted() else "/* FAILED */\n")
f.close()
print("[FuncAt] -> %s" % out_path)
