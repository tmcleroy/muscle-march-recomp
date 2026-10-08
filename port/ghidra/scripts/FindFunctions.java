import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;

import java.util.TreeSet;

public class FindFunctions extends GhidraScript {
    private static final int MFLR_R0 = 0x7C0802A6;
    private static final int STWU_R1_MASK = 0xFFFF0000;
    private static final int STWU_R1 = 0x94210000;
    private static final int STW_R0_4_R1 = 0x90010004;

    private Memory memory;

    @Override
    protected void run() throws Exception {
        memory = currentProgram.getMemory();
        int created;
        int total = 0;
        do {
            created = 0;
            for (long start : collectCandidates()) {
                Address address = toAddr(start);
                if (currentProgram.getFunctionManager().getFunctionContaining(address) != null) {
                    continue;
                }
                if (memory.getInt(address) == 0) {
                    continue;
                }
                if (!disassemble(address)) {
                    continue;
                }
                if (createFunction(address, null) != null) {
                    created++;
                }
            }
            total += created;
            println("created " + created);
        } while (created > 0);
        println("total " + total);
    }

    private TreeSet<Long> collectCandidates() throws Exception {
        TreeSet<Long> candidates = new TreeSet<>();
        for (MemoryBlock block : memory.getBlocks()) {
            if (!block.isExecute() || !block.isInitialized()) {
                continue;
            }
            long begin = block.getStart().getOffset();
            long end = block.getEnd().getOffset();
            for (long a = begin; a + 3 <= end; a += 4) {
                int word = memory.getInt(toAddr(a));
                if ((word >>> 26) == 18 && (word & 3) == 1) {
                    long displacement = (word << 6) >> 6 & ~3;
                    long target = a + displacement;
                    if (target >= begin && target <= end) {
                        candidates.add(target);
                    }
                } else if (word == MFLR_R0) {
                    long start = prologueStart(a, begin, end);
                    if (start >= 0) {
                        candidates.add(start);
                    }
                }
            }
        }
        return candidates;
    }

    private long prologueStart(long mflr, long begin, long end) throws Exception {
        if (mflr - 4 >= begin && (memory.getInt(toAddr(mflr - 4)) & STWU_R1_MASK) == STWU_R1) {
            return mflr - 4;
        }
        for (long next = mflr + 4; next <= mflr + 12 && next + 3 <= end; next += 4) {
            int word = memory.getInt(toAddr(next));
            if ((word & STWU_R1_MASK) == STWU_R1 || word == STW_R0_4_R1) {
                return mflr;
            }
        }
        return -1;
    }
}
