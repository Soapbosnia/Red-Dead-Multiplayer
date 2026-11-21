// Find a C string in the program's memory and list references to it.
//   .\_tools\gh.ps1 FindString.java BlipInfo
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;

public class FindString extends GhidraScript {
    @Override
    public void run() throws Exception {
        Memory mem = currentProgram.getMemory();
        for (String needle : getScriptArgs()) {
            byte[] pattern = (needle + "\0").getBytes("US-ASCII");
            println("===== string '" + needle + "' =====");
            int found = 0;
            for (MemoryBlock block : mem.getBlocks()) {
                if (!block.isInitialized() || block.isExecute()) {
                    continue;
                }
                Address start = block.getStart();
                long size = block.getSize();
                byte[] data = new byte[(int) Math.min(size, 64L * 1024 * 1024)];
                try {
                    mem.getBytes(start, data);
                } catch (Exception e) {
                    continue;
                }
                outer:
                for (int i = 0; i + pattern.length <= data.length; i++) {
                    for (int k = 0; k < pattern.length; k++) {
                        if (data[i + k] != pattern[k]) {
                            continue outer;
                        }
                    }
                    Address addr = start.add(i);
                    println("  at " + addr);
                    ReferenceIterator it = currentProgram.getReferenceManager().getReferencesTo(addr);
                    int n = 0;
                    while (it.hasNext() && n < 12) {
                        Reference r = it.next();
                        Function f = getFunctionContaining(r.getFromAddress());
                        println("    ref " + r.getFromAddress() + " from "
                                + (f == null ? "?" : f.getName() + " @" + f.getEntryPoint()));
                        n++;
                    }
                    if (++found >= 6) {
                        break;
                    }
                }
                if (found >= 6) {
                    break;
                }
            }
            if (found == 0) {
                println("  (not found)");
            }
        }
    }
}
