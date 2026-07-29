import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;

import java.io.File;
import java.io.FileWriter;
import java.nio.charset.StandardCharsets;
import java.util.LinkedHashMap;
import java.util.Map;

public class DumpFunctionCallers extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 2) {
            printerr("Usage: DumpFunctionCallers.java <output-dir> <function-address> [...]");
            return;
        }

        File outputDirectory = new File(args[0]);
        outputDirectory.mkdirs();
        DecompInterface decompiler = new DecompInterface();
        decompiler.openProgram(currentProgram);
        try {
            for (int targetIndex = 1; targetIndex < args.length; targetIndex++) {
                Address targetAddress = toAddr(args[targetIndex]);
                Map<Address, Function> callers = new LinkedHashMap<>();
                for (Reference reference : getReferencesTo(targetAddress)) {
                    Function caller = getFunctionContaining(reference.getFromAddress());
                    if (caller != null) callers.put(caller.getEntryPoint(), caller);
                }

                println("Target " + targetAddress + ": " + callers.size() + " direct caller(s)");
                for (Function caller : callers.values()) {
                    DecompileResults result = decompiler.decompileFunction(caller, 180, monitor);
                    String safeName = caller.getName(true).replaceAll("[^A-Za-z0-9_.-]", "_");
                    File output = new File(outputDirectory,
                            "TARGET__" + targetAddress + "__CALLER__" + caller.getEntryPoint()
                                    + "__" + safeName + ".c");
                    try (FileWriter writer = new FileWriter(output, StandardCharsets.UTF_8)) {
                        writer.write("// target " + targetAddress + "\n");
                        writer.write("// " + caller.getName(true) + " @ " + caller.getEntryPoint() + "\n\n");
                        writer.write(result.decompileCompleted()
                                ? result.getDecompiledFunction().getC()
                                : "// DECOMPILE FAILED: " + result.getErrorMessage() + "\n");
                    }
                    println("Wrote " + output.getAbsolutePath());
                }
            }
        } finally {
            decompiler.dispose();
        }
    }
}
