# Create functions at .pdata RUNTIME_FUNCTION BeginAddresses that Ghidra missed.
# Skips chained-unwind entries (UNW_FLAG_CHAININFO), which are fragments of a parent function.
# Then runs auto-analysis on the changes.
# @category RDRMP
from ghidra.app.cmd.disassemble import DisassembleCommand
from ghidra.app.cmd.function import CreateFunctionCmd

prog = currentProgram
mem = prog.getMemory()
fm = prog.getFunctionManager()
base = prog.getImageBase()
pdata = mem.getBlock(".pdata")

a = pdata.getStart()
end = pdata.getEnd()
cands = []
chained = 0
while a.add(11).compareTo(end) <= 0:
    begin = mem.getInt(a) & 0xffffffff
    unwind = mem.getInt(a.add(8)) & 0xffffffff
    a = a.add(12)
    if begin == 0:
        break
    fa = base.add(begin)
    if fm.getFunctionContaining(fa) is not None:
        continue
    if unwind & 1:
        # indirect entry (points into another RUNTIME_FUNCTION) -> fragment
        chained += 1
        continue
    flags = (mem.getByte(base.add(unwind)) & 0xff) >> 3
    if flags & 0x4:
        chained += 1
        continue
    cands.append(fa)

print("[FixPdata] candidates=%d skipped_chained=%d" % (len(cands), chained))
created = 0
failed = 0
for i, fa in enumerate(cands):
    if monitor.isCancelled():
        break
    if fm.getFunctionContaining(fa) is not None:
        continue
    if getInstructionAt(fa) is None:
        DisassembleCommand(fa, None, True).applyTo(prog, monitor)
    cmd = CreateFunctionCmd(fa)
    if cmd.applyTo(prog, monitor):
        created += 1
    else:
        failed += 1
    if i % 1000 == 0:
        print("[FixPdata] %d/%d" % (i, len(cands)))
print("[FixPdata] created=%d failed=%d; analyzing changes..." % (created, failed))
analyzeChanges(prog)
print("[FixPdata] done. functions now=%d" % fm.getFunctionCount())
