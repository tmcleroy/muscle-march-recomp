import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;

public class DecompileFunctions extends GhidraScript {
    private static final int TIMEOUT_SECONDS = 60;

    @Override
    protected void run() throws Exception {
        DecompInterface decompiler = new DecompInterface();
        decompiler.openProgram(currentProgram);
        for (String argument : getScriptArgs()) {
            Address address = toAddr(Long.parseLong(argument.substring(2), 16));
            Function function = getFunctionContaining(address);
            if (function == null) {
                println("no function at " + argument);
                continue;
            }
            DecompileResults results = decompiler.decompileFunction(function, TIMEOUT_SECONDS, monitor);
            println(function.getName() + " at " + function.getEntryPoint());
            println(results.getDecompiledFunction() == null ? "decompilation failed" : results.getDecompiledFunction().getC());
        }
    }
}
