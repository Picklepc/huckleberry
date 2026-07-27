// FindStringXrefs.java -- locate ASCII strings, their code references, and
// decompile the owning functions in an already analyzed Ghidra program.
//
// Usage:
//   -postScript FindStringXrefs.java <output-dir> <search text> [...]
//
// @category Victron

import java.io.File;
import java.io.FileWriter;
import java.nio.charset.StandardCharsets;
import java.util.LinkedHashSet;
import java.util.Set;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.Reference;

public class FindStringXrefs extends GhidraScript {
    private static String safeName(String value) {
        return value.replaceAll("[^A-Za-z0-9._-]", "_");
    }

    private void decompile(Function function, File outputDir, DecompInterface decompiler)
            throws Exception {
        DecompileResults result = decompiler.decompileFunction(function, 180, monitor);
        String body;
        if (result.decompileCompleted()) {
            body = result.getDecompiledFunction().getC();
        } else {
            body = "// DECOMPILE FAILED: " + result.getErrorMessage() + "\n";
        }

        String filename = safeName(function.getName()) + "__" +
                function.getEntryPoint() + ".c";
        File output = new File(outputDir, filename);
        try (FileWriter writer = new FileWriter(output, StandardCharsets.UTF_8)) {
            writer.write("// " + function.getName() + " @ " +
                    function.getEntryPoint() + "\n\n");
            writer.write(body);
        }
        println("  wrote " + output.getAbsolutePath());
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args == null || args.length < 2) {
            println("Usage: FindStringXrefs.java <output-dir> <search text> [...]");
            return;
        }

        File outputDir = new File(args[0]);
        if (!outputDir.exists() && !outputDir.mkdirs()) {
            throw new IllegalStateException("Cannot create " + outputDir);
        }

        Memory memory = currentProgram.getMemory();
        Set<Function> functions = new LinkedHashSet<>();
        for (int argIndex = 1; argIndex < args.length; argIndex++) {
            String needle = args[argIndex];
            byte[] bytes = needle.getBytes(StandardCharsets.UTF_8);
            println("== STRING: " + needle);

            int matches = 0;
            for (MemoryBlock block : memory.getBlocks()) {
                Address cursor = block.getStart();
                while (cursor != null && cursor.compareTo(block.getEnd()) <= 0) {
                    Address found = memory.findBytes(
                            cursor, block.getEnd(), bytes, null, true, monitor);
                    if (found == null) {
                        break;
                    }
                    matches++;
                    println("  bytes @ " + found);
                    for (Reference reference : getReferencesTo(found)) {
                        Address from = reference.getFromAddress();
                        Function function = getFunctionContaining(from);
                        println("    <- " + reference.getReferenceType() + " " +
                                (function == null ? "<no function>" : function.getName()) +
                                " @ " + from);
                        if (function != null) {
                            functions.add(function);
                        }
                    }
                    cursor = found.next();
                }
            }
            println("  matches: " + matches);
        }

        DecompInterface decompiler = new DecompInterface();
        decompiler.openProgram(currentProgram);
        try {
            for (Function function : functions) {
                println("== FUNCTION " + function.getName() + " @ " +
                        function.getEntryPoint());
                decompile(function, outputDir, decompiler);
            }
        } finally {
            decompiler.dispose();
        }
        println("FindStringXrefs done; functions=" + functions.size());
    }
}
