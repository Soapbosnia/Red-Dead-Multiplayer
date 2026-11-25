// Java version of FindOffsetWrites.py (headless Ghidra here has no PyGhidra): instructions that write memory at
// [reg + <offset>], with the containing function.
//   re\gh.ps1 FindOffsetWritesJ.java "<out_file> 0xb40 [0x...]"
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.scalar.Scalar;

import java.io.PrintWriter;
import java.util.HashSet;
import java.util.Set;

public class FindOffsetWritesJ extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        Set<Long> offsets = new HashSet<>();
        for (int i = 1; i < args.length; ++i) offsets.add(Long.parseUnsignedLong(args[i].replace("0x", ""), 16));
        try (PrintWriter out = new PrintWriter(args[0])) {
            for (Instruction ins : currentProgram.getListing().getInstructions(true)) {
                if (monitor.isCancelled()) break;
                String mn = ins.getMnemonicString();
                if (!(mn.startsWith("MOV") || mn.equals("AND") || mn.equals("OR") || mn.equals("XOR") || mn.equals("ADD") ||
                      mn.equals("SUB") || mn.equals("INC") || mn.equals("DEC") || mn.startsWith("BT"))) continue;
                if (ins.getNumOperands() < 1) continue;
                String rep = ins.getDefaultOperandRepresentation(0);
                if (!rep.contains("[") || rep.contains("RIP")) continue;
                boolean hit = false;
                for (Object o : ins.getOpObjects(0))
                    if (o instanceof Scalar && offsets.contains(((Scalar) o).getUnsignedValue())) hit = true;
                if (!hit) continue;
                Function fn = getFunctionContaining(ins.getAddress());
                out.println(ins.getAddress() + "\t" + (fn == null ? "?" : fn.getName() + "@" + fn.getEntryPoint()) + "\t" + ins);
            }
        }
    }
}
