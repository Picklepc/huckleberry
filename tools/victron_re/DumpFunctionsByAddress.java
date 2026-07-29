import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;

import java.io.File;
import java.io.FileWriter;
import java.nio.charset.StandardCharsets;

public class DumpFunctionsByAddress extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 2) {
            printerr("Usage: DumpFunctionsByAddress.java <output-dir> <address> [...]");
            return;
        }

        File outputDirectory = new File(args[0]);
        outputDirectory.mkdirs();
        DecompInterface decompiler = new DecompInterface();
        decompiler.openProgram(currentProgram);
        try {
            for (int index = 1; index < args.length; index++) {
                Address address = toAddr(args[index]);
                Function function = getFunctionAt(address);
                if (function == null) {
                    if (getInstructionAt(address) == null) disassemble(address);
                    createFunction(address, null);
                    function = getFunctionAt(address);
                }
                if (function == null) {
                    printerr("No function at " + address);
                    continue;
                }

                DecompileResults result = decompiler.decompileFunction(function, 180, monitor);
                File output = new File(outputDirectory, "FUNCTION__" + address + ".c");
                try (FileWriter writer = new FileWriter(output, StandardCharsets.UTF_8)) {
                    writer.write("// " + function.getName(true) + " @ " + address + "\n\n");
                    writer.write(result.decompileCompleted()
                            ? result.getDecompiledFunction().getC()
                            : "// DECOMPILE FAILED: " + result.getErrorMessage() + "\n");
                }
                println("Wrote " + output.getAbsolutePath());
            }
        } finally {
            decompiler.dispose();
        }
    }
}
