import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.scalar.Scalar;

import java.io.File;
import java.io.FileWriter;
import java.nio.charset.StandardCharsets;
import java.util.LinkedHashMap;
import java.util.LinkedHashSet;
import java.util.Map;
import java.util.Set;

public class FindOffsetStores extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 2) {
            printerr("Usage: FindOffsetStores.java <output-dir> <offset> [...]");
            return;
        }

        File outputDirectory = new File(args[0]);
        outputDirectory.mkdirs();
        Set<Long> offsets = new LinkedHashSet<>();
        for (int index = 1; index < args.length; index++) {
            offsets.add(Long.decode(args[index]));
        }

        Map<Address, Function> functions = new LinkedHashMap<>();
        InstructionIterator instructions = currentProgram.getListing().getInstructions(true);
        while (instructions.hasNext() && !monitor.isCancelled()) {
            Instruction instruction = instructions.next();
            String mnemonic = instruction.getMnemonicString().toLowerCase();
            if (!mnemonic.startsWith("str")) {
                continue;
            }

            boolean match = false;
            for (int operandIndex = 0; operandIndex < instruction.getNumOperands(); operandIndex++) {
                for (Object object : instruction.getOpObjects(operandIndex)) {
                    if (object instanceof Scalar
                            && offsets.contains(((Scalar) object).getUnsignedValue())) {
                        match = true;
                    }
                }
            }
            if (!match) {
                continue;
            }

            Function function = getFunctionContaining(instruction.getAddress());
            println("STORE " + instruction.getAddress() + " " + instruction
                    + " function=" + (function == null ? "none" : function.getName(true)));
            Instruction context = instruction;
            for (int before = 0; before < 4; before++) {
                context = context.getPrevious();
                if (context == null) break;
                println("  BEFORE " + context.getAddress() + " " + context);
            }
            context = instruction;
            for (int after = 0; after < 4; after++) {
                context = context.getNext();
                if (context == null) break;
                println("  AFTER  " + context.getAddress() + " " + context);
            }
            if (function != null) {
                functions.put(function.getEntryPoint(), function);
            }
        }

        DecompInterface decompiler = new DecompInterface();
        decompiler.openProgram(currentProgram);
        try {
            for (Map.Entry<Address, Function> entry : functions.entrySet()) {
                DecompileResults result = decompiler.decompileFunction(entry.getValue(), 180, monitor);
                File output = new File(outputDirectory, "STORE_FUNCTION__" + entry.getKey() + ".c");
                try (FileWriter writer = new FileWriter(output, StandardCharsets.UTF_8)) {
                    writer.write("// " + entry.getValue().getName(true) + " @ " + entry.getKey() + "\n\n");
                    writer.write(result.decompileCompleted()
                            ? result.getDecompiledFunction().getC()
                            : "// DECOMPILE FAILED: " + result.getErrorMessage() + "\n");
                }
                println("WROTE " + output.getAbsolutePath());
            }
        } finally {
            decompiler.dispose();
        }
    }
}
