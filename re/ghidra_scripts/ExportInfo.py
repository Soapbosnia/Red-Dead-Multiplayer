# Ghidra Jython post-analysis script for the RDR MP mod.
# Exports functions, imports/exports, strings (with referencing functions),
# RTTI vftables / class names, and a keyword-filtered "net" shortlist.
# Usage: -postScript ExportInfo.py <out_root>
# @category RDRMP
import os
import re
from ghidra.program.util import DefinedDataIterator

args = getScriptArgs()
OUT_ROOT = args[0] if len(args) > 0 else "ghidra_out"

prog = currentProgram
name = prog.getName()
out = os.path.join(OUT_ROOT, name)
if not os.path.isdir(out):
    os.makedirs(out)

fm = prog.getFunctionManager()
st = prog.getSymbolTable()
rm = prog.getReferenceManager()
listing = prog.getListing()

NET_RE = re.compile(
    r"(net|session|peer|socket|packet|sync|multiplay|lobby|matchmak|gamer|rline|"
    r"posse|freeroam|free_roam|clone|voice|host|presence|invite|party|\bmp_|_mp\b|"
    r"wsa|recv|sendto|connect|bandwidth|replicat|snapshot|ownership|rl[A-Z])",
    re.IGNORECASE)


def w(fname, lines):
    f = open(os.path.join(out, fname), "w")
    for l in lines:
        f.write(l.encode("utf-8") if isinstance(l, unicode) else l)
        f.write("\n")
    f.close()


def log(msg):
    print("[ExportInfo:%s] %s" % (name, msg))


# --- functions ---
func_lines = ["addr\tsize\tname\tnamespace"]
count = 0
for fn in fm.getFunctions(True):
    if monitor.isCancelled():
        break
    ns = fn.getParentNamespace()
    func_lines.append("%s\t%d\t%s\t%s" % (
        fn.getEntryPoint(), fn.getBody().getNumAddresses(), fn.getName(),
        "" if ns is None or ns.isGlobal() else ns.getName(True)))
    count += 1
w("functions.tsv", func_lines)
log("functions: %d" % count)

# --- imports ---
imp = []
ext = prog.getExternalManager()
for lib in ext.getExternalLibraryNames():
    imp.append("=== %s ===" % lib)
    it = ext.getExternalLocations(lib)
    while it.hasNext():
        imp.append("  %s" % it.next().getLabel())
w("imports.txt", imp)

# --- exports ---
exp = []
for a in st.getExternalEntryPointIterator():
    s = st.getPrimarySymbol(a)
    exp.append("%s\t%s" % (a, s.getName(True) if s else "?"))
w("exports.txt", exp)

# --- RTTI / classes (vftables named by the RTTI analyzer) ---
vft = []
classes = set()
for s in st.getSymbols("vftable"):
    ns = s.getParentNamespace()
    cname = ns.getName(True)
    classes.add(cname)
    vft.append("%s\t%s" % (s.getAddress(), cname))
vft.sort(key=lambda x: x.split("\t")[1])
w("rtti_vftables.tsv", ["addr\tclass"] + vft)
cl = sorted(classes)
w("rtti_classes.txt", cl)
w("rtti_classes_net.txt", [c for c in cl if NET_RE.search(c)])
log("vftables: %d, classes: %d" % (len(vft), len(cl)))


# --- strings with referencing functions ---
def ref_funcs(addr):
    names = set()
    for r in rm.getReferencesTo(addr):
        f = fm.getFunctionContaining(r.getFromAddress())
        if f is not None:
            names.add("%s@%s" % (f.getName(), f.getEntryPoint()))
        else:
            names.add("?@%s" % r.getFromAddress())
        if len(names) >= 8:
            break
    return sorted(names)


str_lines = ["addr\tstring\treferenced_by"]
net_lines = ["addr\tstring\treferenced_by"]
n = 0
for d in DefinedDataIterator.definedStrings(prog):
    if monitor.isCancelled():
        break
    v = d.getValue()
    if v is None:
        continue
    sv = unicode(v).replace("\t", "\\t").replace("\r", "\\r").replace("\n", "\\n")
    if len(sv) < 4:
        continue
    refs = ",".join(ref_funcs(d.getAddress()))
    line = u"%s\t%s\t%s" % (d.getAddress(), sv, refs)
    str_lines.append(line)
    if NET_RE.search(sv):
        net_lines.append(line)
    n += 1
w("strings.tsv", str_lines)
w("strings_net.tsv", net_lines)
log("strings: %d (net-ish: %d)" % (n, len(net_lines) - 1))
log("DONE -> %s" % out)
