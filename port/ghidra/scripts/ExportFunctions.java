import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;

import java.io.PrintWriter;
import java.nio.file.Paths;

public class ExportFunctions extends GhidraScript {
    @Override
    protected void run() throws Exception {
        try (PrintWriter out = new PrintWriter(Paths.get(getScriptArgs()[0]).toFile(), "UTF-8")) {
            out.println("address,size");
            FunctionIterator functions = currentProgram.getFunctionManager().getFunctions(true);
            for (Function function : functions) {
                out.printf("0x%08x,%d%n", function.getEntryPoint().getOffset(), function.getBody().getNumAddresses());
            }
        }
    }
}
