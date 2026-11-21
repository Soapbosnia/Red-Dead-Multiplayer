// List references to the function containing the given address.
// @category rdmp
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;

public class RefsTo extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        for (String a : args) {
            long addrVal = Long.parseUnsignedLong(a.replace("0x", ""), 16);
            Address addr = toAddr(addrVal);
            Function f = getFunctionContaining(addr);
            if (f == null) {
                println("no function at " + addr);
                continue;
            }
            println("===== " + f.getName() + " @ " + f.getEntryPoint() + " =====");
            ReferenceIterator it = currentProgram.getReferenceManager().getReferencesTo(f.getEntryPoint());
            int n = 0;
            while (it.hasNext() && n < 40) {
                Reference r = it.next();
                Address from = r.getFromAddress();
                Function caller = getFunctionContaining(from);
                println("  ref @ " + from + " from " + (caller == null ? "?" : caller.getName() + " @ " + caller.getEntryPoint()));
                n++;
            }
            if (n == 0) {
                println("  (no references)");
            }
        }
    }
}
