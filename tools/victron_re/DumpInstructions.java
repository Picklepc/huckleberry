import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolIterator;

import java.io.File;
import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.List;

public class DumpInstructions extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 2) {
            printerr("Usage: DumpInstructions.java <output-file> <symbol-substring> [...]");
            return;
        }

        List<String> targets = new ArrayList<>();
        for (int i = 1; i < args.length; i++) {
            for (String part : args[i].split(",")) {
                String target = part.trim();
                if (!target.isEmpty()) targets.add(target);
            }
        }

        try (PrintWriter output = new PrintWriter(new File(args[0]), "UTF-8")) {
            SymbolIterator symbols = currentProgram.getSymbolTable().getAllSymbols(true);
            while (symbols.hasNext() && !monitor.isCancelled()) {
                Symbol symbol = symbols.next();
                String fullName = symbol.getName(true);
                boolean matches = false;
                for (String target : targets) {
                    if (fullName.contains(target)) {
                        matches = true;
                        break;
                    }
                }
                if (!matches) continue;

                Function function = currentProgram.getFunctionManager().getFunctionAt(symbol.getAddress());
                if (function == null) continue;
                output.printf("\n===== %s @ %s =====%n", fullName, function.getEntryPoint());
                InstructionIterator instructions = currentProgram.getListing().getInstructions(function.getBody(), true);
                while (instructions.hasNext()) {
                    Instruction instruction = instructions.next();
                    output.printf("%s  %s%n", instruction.getAddress(), instruction.toString());
                }
            }
        }
    }
}
