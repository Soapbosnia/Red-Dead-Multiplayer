// Disassemble N instructions starting at an address (works in undefined ranges).
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Listing;

public class DisasmAt extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        Address addr = toAddr(Long.parseUnsignedLong(args[0].replace("0x", ""), 16));
        int count = args.length > 1 ? Integer.parseInt(args[1]) : 60;
        Listing listing = currentProgram.getListing();
        Function fn = getFunctionContaining(addr);
        println("===== disasm " + addr + " in " + (fn == null ? "?" : fn.getName()) + " =====");
        Address at = addr;
        for (int i = 0; i < count; ++i) {
            Instruction insn = listing.getInstructionAt(at);
            if (insn == null) {
                try {
                    disassemble(at);
                } catch (Exception e) {
                    println("  disasm failed at " + at);
                    break;
                }
                insn = listing.getInstructionAt(at);
            }
            if (insn == null) break;
            println("  " + insn.getAddress() + ": " + insn.toString());
            at = insn.getAddress().add(insn.getLength());
        }
    }
}
