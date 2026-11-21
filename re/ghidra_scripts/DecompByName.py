# Decompile every function whose name matches a regex.
# Usage: -postScript DecompByName.py <out_file.c> <regex> [max]
# @category RDRMP
import re
from ghidra.app.decompiler import DecompInterface, DecompileOptions

args = list(getScriptArgs())
# cmd.exe mangles | ( ) in analyzeHeadless.bat args, so pass "@<file>" to read the regex from a file.
pat = args[1]
if pat.startswith("@"):
    pat = open(pat[1:]).read().strip()
out_path, rx = args[0], re.compile(pat)
mx = int(args[2]) if len(args) > 2 else 400
prog = currentProgram
d = DecompInterface()
d.setOptions(DecompileOptions())
d.openProgram(prog)
fns = [f for f in prog.getFunctionManager().getFunctions(True) if rx.search(f.getName())]
fns.sort(key=lambda f: f.getName())
out = open(out_path, "w")
out.write("/* %d matches:\n" % len(fns))
for f in fns:
    out.write("   %s @ %s size=%d\n" % (f.getName(), f.getEntryPoint(), f.getBody().getNumAddresses()))
out.write("*/\n\n")
for f in fns[:mx]:
    r = d.decompileFunction(f, 120, monitor)
    out.write("/* ==== %s @ %s ==== */\n" % (f.getName(), f.getEntryPoint()))
    out.write(r.getDecompiledFunction().getC() if r and r.decompileCompleted() else "/* FAILED */\n")
    out.write("\n")
out.close()
print("[DecompByName] %d matches -> %s" % (len(fns), out_path))
