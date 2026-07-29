import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Symbol;

public class InspectFunctions extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        for (String value : args) {
            Address address = toAddr(value);
            Function function = getFunctionAt(address);
            println("ADDRESS " + address);
            if (function != null) {
                println("  function=" + function.getName(true));
                println("  signature=" + function.getSignature());
                for (int index = 0; index < function.getParameterCount(); index++) {
                    println("  parameter" + index + "Length=" +
                            function.getParameter(index).getDataType().getLength());
                }
                println("  thunk=" + function.isThunk());
                Function target = function.getThunkedFunction(true);
                if (target != null) {
                    println("  target=" + target.getName(true));
                    println("  targetSignature=" + target.getSignature());
                    for (int index = 0; index < target.getParameterCount(); index++) {
                        println("  targetParameter" + index + "Length=" +
                                target.getParameter(index).getDataType().getLength());
                    }
                }
            }
            for (Symbol symbol : currentProgram.getSymbolTable().getSymbols(address)) {
                println("  symbol=" + symbol.getName(true) + " type=" + symbol.getSymbolType());
            }
        }
    }
}
