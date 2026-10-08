import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Program;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;

import java.io.ByteArrayInputStream;
import java.nio.ByteBuffer;
import java.nio.file.Files;
import java.nio.file.Paths;

public class LoadDol extends GhidraScript {
    private static final int TEXT_SECTIONS = 7;
    private static final int TOTAL_SECTIONS = 18;

    @Override
    protected void run() throws Exception {
        byte[] file = Files.readAllBytes(Paths.get(getScriptArgs()[0]));
        ByteBuffer header = ByteBuffer.wrap(file);
        Program program = currentProgram;
        Memory memory = program.getMemory();

        for (MemoryBlock block : memory.getBlocks()) {
            memory.removeBlock(block, monitor);
        }

        for (int i = 0; i < TOTAL_SECTIONS; i++) {
            int offset = header.getInt(i * 4);
            long address = header.getInt(0x48 + i * 4) & 0xFFFFFFFFL;
            int size = header.getInt(0x90 + i * 4);
            if (size == 0) {
                continue;
            }
            boolean executable = i < TEXT_SECTIONS;
            String name = executable ? ".text" + i : ".data" + (i - TEXT_SECTIONS);
            byte[] bytes = new byte[size];
            System.arraycopy(file, offset, bytes, 0, size);
            MemoryBlock block = memory.createInitializedBlock(name, toAddr(address), new ByteArrayInputStream(bytes), size, monitor, false);
            block.setRead(true);
            block.setWrite(!executable);
            block.setExecute(executable);
        }

        long bssAddress = header.getInt(0xD8) & 0xFFFFFFFFL;
        long bssSize = header.getInt(0xDC) & 0xFFFFFFFFL;
        long cursor = bssAddress;
        long bssEnd = bssAddress + bssSize;
        int part = 0;
        for (MemoryBlock block : memory.getBlocks()) {
            long start = block.getStart().getOffset();
            long end = block.getEnd().getOffset() + 1;
            if (end <= cursor || start >= bssEnd) {
                continue;
            }
            if (start > cursor) {
                createBss(memory, part++, cursor, start - cursor);
            }
            cursor = Math.max(cursor, end);
        }
        if (cursor < bssEnd) {
            createBss(memory, part, cursor, bssEnd - cursor);
        }

        Address entry = toAddr(header.getInt(0xE0) & 0xFFFFFFFFL);
        createFunction(entry, "entry");
        program.getSymbolTable().addExternalEntryPoint(entry);
    }

    private void createBss(Memory memory, int part, long address, long size) throws Exception {
        MemoryBlock bss = memory.createUninitializedBlock(".bss" + part, toAddr(address), size, false);
        bss.setRead(true);
        bss.setWrite(true);
        bss.setExecute(false);
    }
}
