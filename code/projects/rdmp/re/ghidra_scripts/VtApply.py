# Apply vtables.tsv (from tools/VtScan.ps1): create missing slot functions, name
# "lea rax,[str]; ret" getters, label vtables vt_<Name>, and rename slot functions used by only one vtable.
# Usage: -postScript VtApply.py <vtables.tsv>   (write mode)
# @category RDRMP
import re
from ghidra.app.cmd.disassemble import DisassembleCommand
from ghidra.app.cmd.function import CreateFunctionCmd
from ghidra.program.model.symbol import SourceType

tsv = getScriptArgs()[0]
prog = currentProgram
mem = prog.getMemory()
fm = prog.getFunctionManager()
st = prog.getSymbolTable()
listing = prog.getListing()
sp = prog.getAddressFactory().getDefaultAddressSpace()
ID = re.compile(r"^[A-Za-z_][A-Za-z0-9_:<>]{2,80}$")

vts = []
use = {}
for line in open(tsv):
    p = line.strip().split("\t")
    if len(p) < 4:
        continue
    slots = [long(x, 16) for x in p[3].split(",")]
    vts.append((long(p[0], 16), slots))
    for t in set(slots):
        use[t] = use.get(t, 0) + 1
print("[VtApply] vtables=%d unique slot targets=%d" % (len(vts), len(use)))

# 1) create missing functions
created = 0
i = 0
for t in sorted(use):
    i += 1
    if i % 2000 == 0:
        print("[VtApply] create pass %d/%d (created %d)" % (i, len(use), created))
    a = sp.getAddress(t)
    if fm.getFunctionAt(a) is not None:
        continue
    if fm.getFunctionContaining(a) is not None:
        continue  # mid-function target (e.g. adjustor thunk inside); leave it
    if listing.getInstructionAt(a) is None:
        if listing.getDefinedDataContaining(a) is not None:
            continue
        DisassembleCommand(a, None, True).applyTo(prog, monitor)
        if listing.getInstructionAt(a) is None:
            continue
    if CreateFunctionCmd(a).applyTo(prog, monitor):
        created += 1
print("[VtApply] created functions: %d" % created)


# 2) name getters: 48 8D 05 <rel32> C3
def cstr(a):
    out = []
    try:
        for k in range(96):
            c = mem.getByte(a.add(k)) & 0xff
            if c == 0:
                return "".join(out)
            if c < 0x20 or c > 0x7e:
                return None
            out.append(chr(c))
    except:
        return None
    return None


getters = {}
for t in use:
    a = sp.getAddress(t)
    try:
        if (mem.getByte(a) & 0xff) != 0x48 or (mem.getByte(a.add(1)) & 0xff) != 0x8d \
                or (mem.getByte(a.add(2)) & 0xff) != 0x05 or (mem.getByte(a.add(7)) & 0xff) != 0xc3:
            continue
        s = cstr(a.add(7 + mem.getInt(a.add(3))))
    except:
        continue
    if s is None or not ID.match(s):
        continue
    nm = re.sub(r"[^A-Za-z0-9_]", "_", s)
    getters[t] = nm
    fn = fm.getFunctionAt(a)
    if fn is not None and (fn.getName().startswith("FUN_") or fn.getName().startswith("GetName_")):
        fn.setName("GetName_" + nm, SourceType.ANALYSIS)
print("[VtApply] name getters: %d" % len(getters))

# 3) label vtables + rename unique slots
labelled = renamed = 0
for (vt, slots) in vts:
    names = [getters[t] for t in slots if t in getters]
    if not names:
        continue
    name = names[0]
    try:
        st.createLabel(sp.getAddress(vt), "vt_" + name, SourceType.ANALYSIS)
        labelled += 1
    except:
        pass
    for k, t in enumerate(slots):
        if use.get(t, 0) != 1 or t in getters:
            continue
        fn = fm.getFunctionAt(sp.getAddress(t))
        if fn is not None and fn.getName().startswith("FUN_"):
            try:
                fn.setName("%s__vf%d" % (name, k), SourceType.ANALYSIS)
                renamed += 1
            except:
                pass
print("[VtApply] vtables labelled=%d slot functions renamed=%d" % (labelled, renamed))
print("[VtApply] done. functions now=%d" % fm.getFunctionCount())
