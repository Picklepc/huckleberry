import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;

import java.io.File;
import java.io.FileOutputStream;

public class DumpMemory extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length != 3) {
            printerr("Usage: DumpMemory.java <output-file> <address> <length>");
            return;
        }

        Address start = toAddr(args[1]);
        int length = Integer.decode(args[2]);
        byte[] bytes = new byte[length];
        int count = currentProgram.getMemory().getBytes(start, bytes);
        if (count != length) {
            printerr("Requested " + length + " bytes, read " + count);
        }

        try (FileOutputStream output = new FileOutputStream(new File(args[0]))) {
            output.write(bytes, 0, Math.max(count, 0));
        }
        println("Wrote " + count + " bytes from " + start + " to " + args[0]);
    }
}
