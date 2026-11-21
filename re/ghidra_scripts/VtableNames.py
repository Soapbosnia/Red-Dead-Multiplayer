# 1) Create functions at every .rdata qword pointer into .text that has no function.
# 2) Find "lea rax,[string]; ret" name getters and name them GetName_<string>.
# 3) Label vtables containing a getter (vt_<name>) and rename their unique FUN_ slots <name>__vf<k>.
# Run in write mode (no -readOnly).
# @category RDRMP
import re
from ghidra.app.cmd.disassemble import DisassembleCommand
from ghidra.app.cmd.function import CreateFunctionCmd
from ghidra.program.model.symbol import SourceType

prog = currentProgram
mem = prog.getMemory()
fm = prog.getFunctionManager()
st = prog.getSymbolTable()
listing = prog.getListing()
af = prog.getAddressFactory()

text = mem.getBlock(".text")
rdata = mem.getBlock(".rdata")
t0, t1 = text.getStart().getOffset(), text.getEnd().getOffset()
ID = re.compile(r"^[A-Za-z_][A-Za-z0-9_:]{2,80}$")


def addr(v):
    return af.getDefaultAddressSpace().getAddress(v)


# ---- 1) scan .rdata for code pointers ----
ptr_count = {}
slots = []  # (slot_addr, target)
a = rdata.getStart()
end = rdata.getEnd().getOffset()
off = a.getOffset()
off += (8 - off % 8) % 8
buf = bytearray(rdata.getSize())
rdata.getBytes(rdata.getStart(), buf)
base = rdata.getStart().getOffset()
import struct
for o in range(off - base, len(buf) - 7, 8):
    v = struct.unpack_from("<Q", buf, o)[0]
    if t0 <= v <= t1:
        ptr_count[v] = ptr_count.get(v, 0) + 1
        slots.append((base + o, v))
print("[VtableNames] code pointers in .rdata: %d (unique %d)" % (len(slots), len(ptr_count)))

created = 0
for v in sorted(ptr_count):
    if monitor.isCancelled():
        break
    ta = addr(v)
    if fm.getFunctionContaining(ta) is not None:
        continue
    if listing.getInstructionAt(ta) is None:
        if listing.getDefinedDataContaining(ta) is not None:
            continue
        DisassembleCommand(ta, None, True).applyTo(prog, monitor)
    if listing.getInstructionAt(ta) is None:
        continue
    if CreateFunctionCmd(ta).applyTo(prog, monitor):
        created += 1
print("[VtableNames] created functions: %d" % created)


# ---- 2) name getters ----
def read_cstr(a, maxlen=96):
    out = []
    try:
        for i in range(maxlen):
            b = mem.getByte(a.add(i)) & 0xff
            if b == 0:
                break
            if b < 0x20 or b > 0x7e:
                return None
            out.append(chr(b))
    except:
        return None
    return "".join(out)


getters = {}
for v in ptr_count:
    fn = fm.getFunctionAt(addr(v))
    if fn is None or fn.getBody().getNumAddresses() > 8:
        continue
    ins = listing.getInstructionAt(fn.getEntryPoint())
    if ins is None or ins.getMnemonicString() != "LEA" or ins.getRegister(0) is None:
        continue
    if ins.getRegister(0).getName() != "RAX":
        continue
    nxt = ins.getNext()
    if nxt is None or nxt.getMnemonicString() != "RET":
        continue
    refs = ins.getReferencesFrom()
    if len(refs) == 0:
        continue
    s = read_cstr(refs[0].getToAddress())
    if s is None or not ID.match(s):
        continue
    getters[v] = s.replace("::", "__")
    if fn.getName().startswith("FUN_") or fn.getName().startswith("LAB_"):
        fn.setName("GetName_%s" % getters[v], SourceType.ANALYSIS)
print("[VtableNames] name getters: %d" % len(getters))

# ---- 3) vtables ----
slot_map = {}
for (sa, v) in slots:
    slot_map[sa] = v
named_vt = 0
renamed = 0
for (sa, v) in slots:
    if v not in getters:
        continue
    name = getters[v]
    # walk back to the vtable start (first preceding non-code qword ends it)
    start = sa
    while (start - 8) in slot_map:
        start -= 8
    lab = "vt_%s" % name
    try:
        st.createLabel(addr(start), lab, SourceType.ANALYSIS)
        named_vt += 1
    except:
        pass
    k = 0
    p = start
    while p in slot_map and k < 64:
        tv = slot_map[p]
        fn = fm.getFunctionAt(addr(tv))
        if fn is not None and fn.getName().startswith("FUN_") and ptr_count.get(tv, 0) == 1:
            try:
                fn.setName("%s__vf%d" % (name, k), SourceType.ANALYSIS)
                renamed += 1
            except:
                pass
        k += 1
        p += 8
print("[VtableNames] vtables labelled: %d, slot functions renamed: %d" % (named_vt, renamed))
analyzeChanges(prog)
print("[VtableNames] done. functions now=%d" % fm.getFunctionCount())
