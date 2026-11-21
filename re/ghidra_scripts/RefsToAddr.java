// List references to an arbitrary address (function or data), unlike RefsTo.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;

public class RefsToAddr extends GhidraScript {
    @Override
    public void run() throws Exception {
        for (String a : getScriptArgs()) {
            long v = Long.parseUnsignedLong(a.replace("0x", ""), 16);
            Address addr = toAddr(v);
            println("===== refs to " + addr + " =====");
            ReferenceIterator it = currentProgram.getReferenceManager().getReferencesTo(addr);
            int n = 0;
            while (it.hasNext() && n < 60) {
                Reference r = it.next();
                Address from = r.getFromAddress();
                Function caller = getFunctionContaining(from);
                println("  ref @ " + from + " type=" + r.getReferenceType() + " from "
                        + (caller == null ? "?" : caller.getName() + " @" + caller.getEntryPoint()));
                n++;
            }
            if (n == 0) println("  (none)");
        }
    }
}
