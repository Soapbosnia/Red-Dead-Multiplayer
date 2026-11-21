# Dump N qword entries of a vtable with function names.
# Usage: -postScript DumpVt.py <out> <addr> <count>
# @category RDRMP
args = list(getScriptArgs())
out = open(args[0], "w")
a = currentProgram.getAddressFactory().getAddress(args[1])
fm = currentProgram.getFunctionManager()
for i in range(int(args[2])):
    p = getLong(a.add(i * 8)) & 0xffffffffffffffff
    fa = currentProgram.getAddressFactory().getAddress("%x" % p)
    f = fm.getFunctionAt(fa) if fa else None
    out.write("+0x%03x %x %s\n" % (i * 8, p, f.getName() if f else "?"))
out.close()
