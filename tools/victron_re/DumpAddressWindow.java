import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;

public class DumpAddressWindow extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 1) {
            printerr("Usage: DumpAddressWindow.java <address> [instruction-count]");
            return;
        }
        Address address = toAddr(args[0]);
        int count = args.length > 1 ? Integer.decode(args[1]) : 32;
        Instruction instruction = getInstructionAt(address);
        if (instruction == null) instruction = getInstructionContaining(address);
        if (instruction == null) {
            printerr("No instruction at " + address);
            return;
        }
        for (int index = 0; index < count / 2; index++) {
            Instruction previous = instruction.getPrevious();
            if (previous == null) break;
            instruction = previous;
        }
        for (int index = 0; index < count; index++) {
            Function function = getFunctionContaining(instruction.getAddress());
            println(instruction.getAddress() + "  " + instruction + "  function=" +
                    (function == null ? "none" : function.getName(true)));
            instruction = instruction.getNext();
            if (instruction == null) break;
        }
    }
}
