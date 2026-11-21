# Print the disassembly of the functions at the given addresses.
# Usage: -postScript Disasm.py <out_file.txt> <addr...>
# @category RDRMP
args = list(getScriptArgs())
out = open(args[0], "w")
fm = currentProgram.getFunctionManager()
af = currentProgram.getAddressFactory()
from ghidra.program.model.address import AddressSet
for s in args[1:]:
    if "-" in s:  # raw range "start-end": instructions present in the listing, function or not
        a, b = s.split("-")
        rng = AddressSet(af.getAddress(a), af.getAddress(b))
        out.write("; ==== range %s\n" % s)
        for ins in currentProgram.getListing().getInstructions(rng, True):
            fn = fm.getFunctionContaining(ins.getAddress())
            out.write("%s  %-24s %-40s %s\n" % (ins.getAddress(), " ".join("%02x" % (x & 0xff) for x in ins.getBytes()), ins,
                                                 fn.getName() if fn else ""))
        continue
    fn = fm.getFunctionContaining(af.getAddress(s))
    if fn is None:
        out.write("; %s: no function\n" % s)
        continue
    out.write("; ==== %s @ %s\n" % (fn.getName(), fn.getEntryPoint()))
    for ins in currentProgram.getListing().getInstructions(fn.getBody(), True):
        out.write("%s  %-24s %s\n" % (ins.getAddress(), " ".join("%02x" % (b & 0xff) for b in ins.getBytes()), ins))
out.close()
print("[Disasm] -> %s" % args[0])
