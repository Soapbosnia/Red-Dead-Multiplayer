# Search initialized memory for 8-byte little-endian pointers to each target address.
# For each hit, report the location, what refers to that location, and nearby pointers
# (useful for finding vtables / type tables that hold a name string).
# Usage: -postScript FindPtrs.py <out_file.txt> <addr...>
# @category RDRMP
import struct
from ghidra.program.model.address import AddressSet

args = list(getScriptArgs())
out_path = args[0]
targets = args[1:]
prog = currentProgram
mem = prog.getMemory()
fm = prog.getFunctionManager()
rm = prog.getReferenceManager()
af = prog.getAddressFactory()
st = prog.getSymbolTable()

init = AddressSet()
for b in mem.getBlocks():
    if b.isInitialized() and b.getName() in (".rdata", ".data", "_RDATA"):
        init.add(b.getStart(), b.getEnd())


def fmt(a):
    fn = fm.getFunctionContaining(a)
    return "%s@%s" % (fn.getName(), fn.getEntryPoint()) if fn else str(a)


f = open(out_path, "w")
for t in targets:
    ta = af.getAddress(t)
    pat = struct.pack("<Q", ta.getOffset())
    f.write("##### %s\n" % t)
    hits = 0
    for rng in init:
        start = rng.getMinAddress()
        while start is not None and start.compareTo(rng.getMaxAddress()) <= 0 and hits < 20:
            h = mem.findBytes(start, rng.getMaxAddress(), pat, None, True, monitor)
            if h is None:
                break
            hits += 1
            refs = [fmt(r.getFromAddress()) for r in rm.getReferencesTo(h)][:6]
            # dump the 8 qwords around the hit (vtable / table context)
            ctx = []
            for k in range(-6, 14):
                try:
                    v = mem.getLong(h.add(k * 8)) & 0xffffffffffffffff
                    va = af.getAddress("%x" % v) if 0x140000000 <= v < 0x1a0000000 else None
                    lab = ""
                    if va is not None:
                        fn = fm.getFunctionAt(va)
                        s = st.getPrimarySymbol(va)
                        lab = fn.getName() if fn else (s.getName() if s else "")
                    ctx.append("%+d:%x %s" % (k, v, lab))
                except:
                    pass
            f.write("  hit %s  refs-to-hit: %s\n" % (h, ", ".join(refs) or "-"))
            f.write("     " + " | ".join(ctx) + "\n")
            start = h.add(8)
    if hits == 0:
        f.write("  (no pointer found)\n")
f.close()
print("[FindPtrs] -> %s" % out_path)
