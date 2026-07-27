import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolIterator;

import java.io.File;
import java.io.FileWriter;
import java.nio.charset.StandardCharsets;
import java.util.LinkedHashSet;
import java.util.Set;

public class FindFunctionCallers extends GhidraScript {
    private static String safeName(String value) {
        return value.replaceAll("[^A-Za-z0-9._-]", "_");
    }

    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 2) {
            printerr("Usage: FindFunctionCallers.java <output-dir> <symbol-substring> [...]");
            return;
        }

        File outputDir = new File(args[0]);
        outputDir.mkdirs();
        Set<Function> callers = new LinkedHashSet<>();
        SymbolIterator symbols = currentProgram.getSymbolTable().getAllSymbols(true);
        while (symbols.hasNext() && !monitor.isCancelled()) {
            Symbol symbol = symbols.next();
            boolean matches = false;
            for (int index = 1; index < args.length; index++) {
                if (symbol.getName(true).contains(args[index])) {
                    matches = true;
                    break;
                }
            }
            if (!matches) continue;

            Function target = getFunctionAt(symbol.getAddress());
            if (target == null) continue;
            println("== TARGET " + target.getName(true) + " @ " + target.getEntryPoint());
            for (Reference reference : getReferencesTo(target.getEntryPoint())) {
                Function caller = getFunctionContaining(reference.getFromAddress());
                println("  <- " + reference.getReferenceType() + " " +
                        (caller == null ? "<no function>" : caller.getName(true)) +
                        " @ " + reference.getFromAddress());
                if (caller != null && !caller.equals(target)) callers.add(caller);
            }
        }

        DecompInterface decompiler = new DecompInterface();
        decompiler.openProgram(currentProgram);
        try {
            for (Function caller : callers) {
                DecompileResults result = decompiler.decompileFunction(caller, 180, monitor);
                File output = new File(outputDir,
                        "CALLER__" + safeName(caller.getName()) + "__" +
                        caller.getEntryPoint() + ".c");
                try (FileWriter writer = new FileWriter(output, StandardCharsets.UTF_8)) {
                    writer.write("// " + caller.getName(true) + " @ " + caller.getEntryPoint() + "\n\n");
                    writer.write(result.decompileCompleted()
                            ? result.getDecompiledFunction().getC()
                            : "// DECOMPILE FAILED: " + result.getErrorMessage() + "\n");
                }
                println("  wrote " + output.getAbsolutePath());
            }
        } finally {
            decompiler.dispose();
        }
        println("FindFunctionCallers done; callers=" + callers.size());
    }
}
