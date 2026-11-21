# Find a byte pattern (hex, e.g. "19181cff") in memory; list addresses + containing functions.
# Usage: -postScript FindBytes.py <out> <hex> [<hex>...]
# @category RDRMP
args = list(getScriptArgs())
out = open(args[0], "w")
fm = currentProgram.getFunctionManager()
mem = currentProgram.getMemory()
for hx in args[1:]:
    pat = bytearray.fromhex(hx) if hasattr(bytearray, "fromhex") else None
    import jarray
    b = jarray.array([((int(hx[i:i+2], 16) + 128) % 256) - 128 for i in range(0, len(hx), 2)], 'b')
    out.write("## %s\n" % hx)
    a = mem.getMinAddress()
    n = 0
    while n < 200:
        a = mem.findBytes(a, b, None, True, monitor)
        if a is None:
            break
        f = fm.getFunctionContaining(a)
        out.write("  %s %s\n" % (a, f.getName() if f else "?"))
        a = a.add(1)
        n += 1
out.close()
