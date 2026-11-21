// Dump the vtable at each given address (as image-relative 0x140000000-style
// addresses). Prints slot offset, target and the function containing it.
//   .\_tools\gh.ps1 DumpVtable.java 0x141c559d0
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.symbol.Symbol;

public class DumpVtable extends GhidraScript {
    @Override
    public void run() throws Exception {
        Memory mem = currentProgram.getMemory();
        for (String arg : getScriptArgs()) {
            long base = Long.parseUnsignedLong(arg.replace("0x", ""), 16);
            Address vt = toAddr(base);
            Symbol sym = getSymbolAt(vt);
            println("===== vtable " + arg + " (" + (sym == null ? "no symbol" : sym.getName()) + ") =====");
            for (int slot = 0; slot < 140; slot++) {
                Address at = vt.add(slot * 8L);
                long target;
                try {
                    target = mem.getLong(at);
                } catch (Exception e) {
                    break;
                }
                if (target == 0) {
                    println(String.format("  +0x%03X  null", slot * 8));
                    continue;
                }
                Address t = toAddr(target);
                Function f = getFunctionContaining(t);
                println(String.format("  +0x%03X  %s  %s", slot * 8, t,
                        f == null ? "(no function)" : f.getName() + " @ " + f.getEntryPoint()));
            }
        }
    }
}
