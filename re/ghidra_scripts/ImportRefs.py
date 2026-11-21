# List references to imported functions whose name matches a substring.
# Usage: -postScript ImportRefs.py <out_file> <substr>
# @category RDRMP
args = list(getScriptArgs())
out_path, sub = args[0], args[1]
prog = currentProgram
st = prog.getSymbolTable()
rm = prog.getReferenceManager()
fm = prog.getFunctionManager()
f = open(out_path, "w")
for s in st.getExternalSymbols():
    if sub.lower() not in s.getName().lower():
        continue
    f.write("## %s %s\n" % (s.getName(), s.getAddress()))
    for r in rm.getReferencesTo(s.getAddress()):
        fa = r.getFromAddress()
        f.write("  ptr/ref from %s %s\n" % (fa, r.getReferenceType()))
        for r2 in rm.getReferencesTo(fa):
            cf = fm.getFunctionContaining(r2.getFromAddress())
            f.write("     used at %s %s %s\n" % (r2.getFromAddress(), r2.getReferenceType(), "%s@%s" % (cf.getName(), cf.getEntryPoint()) if cf else "?"))
f.close()
