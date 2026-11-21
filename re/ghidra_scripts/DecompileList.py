# Decompile a list of functions (by address) to a .c file.
# Usage: -postScript DecompileList.py <out_file.c> <addr1> <addr2> ...
# Also reports .pdata entries whose BeginAddress is not covered by any function.
# @category RDRMP
from ghidra.app.decompiler import DecompInterface, DecompileOptions

args = list(getScriptArgs())
out_path = args[0]
addrs = args[1:]
prog = currentProgram
fm = prog.getFunctionManager()
af = prog.getAddressFactory()

# --- .pdata coverage ---
mem = prog.getMemory()
pdata = mem.getBlock(".pdata")
base = prog.getImageBase()
missing = 0
total = 0
if pdata is not None:
    a = pdata.getStart()
    end = pdata.getEnd()
    while a.add(11).compareTo(end) <= 0:
        rva = mem.getInt(a) & 0xffffffff
        if rva == 0:
            break
        total += 1
        fa = base.add(rva)
        if fm.getFunctionContaining(fa) is None:
            missing += 1
        a = a.add(12)
print("[DecompileList] pdata entries=%d not-in-function=%d functions=%d" % (total, missing, fm.getFunctionCount()))

d = DecompInterface()
d.setOptions(DecompileOptions())
d.openProgram(prog)
f = open(out_path, "w")
for s in addrs:
    fn = fm.getFunctionAt(af.getAddress(s)) or fm.getFunctionContaining(af.getAddress(s))
    if fn is None:
        f.write("/* %s: no function */\n\n" % s)
        continue
    r = d.decompileFunction(fn, 120, monitor)
    f.write("/* ==== %s @ %s ==== */\n" % (fn.getName(), fn.getEntryPoint()))
    if r and r.decompileCompleted():
        f.write(r.getDecompiledFunction().getC())
    else:
        f.write("/* FAILED */\n")
    f.write("\n\n")
f.close()
print("[DecompileList] wrote %s" % out_path)
