// Decompile the function containing each address passed as a script argument.
// Usage: analyzeHeadless <proj> <name> -process -noanalysis -scriptPath <dir> \
//          -postScript DecompileAt.java 0x1400cce00 0x1400cd151
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;

public class DecompileAt extends GhidraScript {
    @Override
    public void run() throws Exception {
        DecompInterface di = new DecompInterface();
        di.toggleCCode(true);
        di.toggleSyntaxTree(true);
        di.openProgram(currentProgram);

        for (String arg : getScriptArgs()) {
            Address addr = currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(arg);
            Function fn = getFunctionContaining(addr);
            if (fn == null) {
                println("NO FUNCTION containing " + arg);
                continue;
            }
            DecompileResults res = di.decompileFunction(fn, 120, monitor);
            println("===== " + fn.getName() + " @ " + fn.getEntryPoint() + " =====");
            if (res != null && res.decompileCompleted() && res.getDecompiledFunction() != null) {
                println(res.getDecompiledFunction().getC());
            } else {
                println("decompile failed: " + (res == null ? "null" : res.getErrorMessage()));
            }
        }
        di.dispose();
    }
}
