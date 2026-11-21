// Walk the analysed functions forward from an address and print the N-th one, plus its first body
// instructions (a native thunk jumps through a pointer, so the target identifies the implementation).
//   .\_tools\gh.ps1 NthFunction.java 0x1401E6D20 193 200
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.Listing;

public class NthFunction extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        long start = Long.parseUnsignedLong(args[0].replace("0x", ""), 16);
        int first = args[1].startsWith("0x") ? Integer.parseInt(args[1].substring(2), 16) : Integer.parseInt(args[1]);
        int last = args[2].startsWith("0x") ? Integer.parseInt(args[2].substring(2), 16) : Integer.parseInt(args[2]);
        Address addr = toAddr(start);
        Function fn = getFunctionContaining(addr);
        if (fn == null) {
            Function after = getFunctionAfter(addr);
            fn = after;
        }
        if (fn == null) {
            println("no function at/after " + args[0]);
            return;
        }
        Address at = fn.getEntryPoint();
        for (int i = 0; i < last; i++) {
            Function f = getFunctionAt(at);
            if (f == null) {
                println("no function at " + at + " (index " + i + ")");
                return;
            }
            if (i >= first) {
                println("index " + i + ": " + at + " name " + f.getName());
                Listing listing = currentProgram.getListing();
                Instruction ins = listing.getInstructionAt(at);
                for (int k = 0; k < 4 && ins != null; k++) {
                    println("    " + ins);
                    ins = ins.getNext();
                }
            }
            Address next = f.getBody().getMaxAddress().add(1);
            Function after = getFunctionAfter(next);
            if (after == null) {
                println("no function after " + next + " (index " + i + ")");
                return;
            }
            at = after.getEntryPoint();
        }
    }
}
