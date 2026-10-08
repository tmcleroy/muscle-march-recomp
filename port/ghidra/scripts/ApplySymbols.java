import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.SourceType;

import java.nio.file.Files;
import java.nio.file.Paths;
import java.util.List;

public class ApplySymbols extends GhidraScript {
    @Override
    protected void run() throws Exception {
        int applied = 0;
        for (String path : getScriptArgs()) {
            if (!Files.exists(Paths.get(path))) {
                continue;
            }
            List<String> lines = Files.readAllLines(Paths.get(path));
            for (String line : lines.subList(1, lines.size())) {
                String[] fields = line.split(",");
                if (fields.length < 2) {
                    continue;
                }
                Address address = toAddr(Long.parseLong(fields[0].trim().substring(2), 16));
                if (apply(address, fields[1].trim())) {
                    applied++;
                }
            }
        }
        println("applied " + applied + " symbols");
    }

    private boolean apply(Address address, String name) {
        Function function = getFunctionAt(address);
        try {
            if (function != null) {
                function.setName(name, SourceType.USER_DEFINED);
            } else {
                createLabel(address, name, true, SourceType.USER_DEFINED);
            }
            return true;
        } catch (Exception duplicate) {
            String unique = name + "_" + address;
            try {
                if (function != null) {
                    function.setName(unique, SourceType.USER_DEFINED);
                } else {
                    createLabel(address, unique, true, SourceType.USER_DEFINED);
                }
                return true;
            } catch (Exception failure) {
                return false;
            }
        }
    }
}
