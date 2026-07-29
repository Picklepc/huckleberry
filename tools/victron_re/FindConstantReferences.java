import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.scalar.Scalar;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;

import java.io.File;
import java.io.FileWriter;
import java.nio.charset.StandardCharsets;
import java.util.LinkedHashMap;
import java.util.Map;

public class FindConstantReferences extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 2) {
            printerr("Usage: FindConstantReferences.java <output-dir> [--operands-only] <value> [...]");
            return;
        }

        File outputDirectory = new File(args[0]);
        outputDirectory.mkdirs();
        Memory memory = currentProgram.getMemory();
        Map<Address, Function> functions = new LinkedHashMap<>();
        boolean operandsOnly = args.length > 2 && "--operands-only".equals(args[1]);
        int firstValueIndex = operandsOnly ? 2 : 1;

        for (int index = firstValueIndex; index < args.length; index++) {
            long value = Long.decode(args[index]);
            if (!operandsOnly) {
                for (int width : new int[] {2, 4}) {
                    byte[] bytes = new byte[width];
                    for (int byteIndex = 0; byteIndex < width; byteIndex++) {
                        bytes[byteIndex] = (byte) (value >> (byteIndex * 8));
                    }
                    Address cursor = memory.getMinAddress();
                    while (cursor != null && !monitor.isCancelled()) {
                        Address match = memory.findBytes(cursor, bytes, null, true, monitor);
                        if (match == null) {
                            break;
                        }
                        println(String.format("MATCH value=0x%x width=%d address=%s", value, width, match));
                        Function containingFunction = getFunctionContaining(match);
                        if (containingFunction != null) {
                            println("  CONTAINING function=" + containingFunction.getName(true));
                            functions.put(containingFunction.getEntryPoint(), containingFunction);
                        }
                        Data containingData = getDataContaining(match);
                        if (containingData != null) {
                            println("  DATA address=" + containingData.getAddress()
                                    + " type=" + containingData.getDataType().getName());
                        }
                        for (int back = 0; back <= 64; back++) {
                            Address target = match.subtract(back);
                            ReferenceIterator references = currentProgram.getReferenceManager().getReferencesTo(target);
                            while (references.hasNext()) {
                                Reference reference = references.next();
                                Function function = getFunctionContaining(reference.getFromAddress());
                                println("  REF target=" + target + " from=" + reference.getFromAddress() + " "
                                        + reference.getReferenceType() + " function="
                                        + (function == null ? "none" : function.getName(true)));
                                if (function != null) {
                                    functions.put(function.getEntryPoint(), function);
                                }
                            }
                        }
                        cursor = match.next();
                    }
                }
            }

            InstructionIterator instructions = currentProgram.getListing().getInstructions(true);
            while (instructions.hasNext() && !monitor.isCancelled()) {
                Instruction instruction = instructions.next();
                for (int operandIndex = 0; operandIndex < instruction.getNumOperands(); operandIndex++) {
                    for (Object object : instruction.getOpObjects(operandIndex)) {
                        if (object instanceof Scalar
                                && ((Scalar) object).getUnsignedValue() == value) {
                            Function function = getFunctionContaining(instruction.getAddress());
                            println(String.format("OPERAND value=0x%x address=%s instruction=%s function=%s",
                                    value, instruction.getAddress(), instruction,
                                    function == null ? "none" : function.getName(true)));
                            if (function != null) {
                                functions.put(function.getEntryPoint(), function);
                            }
                        }
                    }
                }
            }
        }

        DecompInterface decompiler = new DecompInterface();
        decompiler.openProgram(currentProgram);
        try {
            for (Map.Entry<Address, Function> entry : functions.entrySet()) {
                DecompileResults result = decompiler.decompileFunction(entry.getValue(), 180, monitor);
                File output = new File(outputDirectory, "FUNCTION__" + entry.getKey() + ".c");
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
