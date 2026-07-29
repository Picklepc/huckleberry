import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.symbol.Reference;

import java.io.File;
import java.io.FileWriter;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

// Finds every SVC instruction, groups by immediate, and for a requested SVC number
// dumps the containing (wrapper) function's direct callers (decompiled). This locates
// SoftDevice primitives (e.g. sd_ecb_block_encrypt) and the code that uses them.
public class FindSvcAndCallers extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        File outDir = new File(args[0]);
        outDir.mkdirs();
        // optional: args[1..] = list of svc numbers (hex/dec) to dump wrappers+callers for
        Map<Long, List<Address>> svcSites = new LinkedHashMap<>();
        InstructionIterator it = currentProgram.getListing().getInstructions(true);
        while (it.hasNext()) {
            Instruction ins = it.next();
            String m = ins.getMnemonicString();
            if (m.equalsIgnoreCase("svc") || m.equalsIgnoreCase("swi")) {
                long imm = -1;
                try { imm = ins.getScalar(0).getUnsignedValue(); } catch (Exception e) {}
                svcSites.computeIfAbsent(imm, k -> new ArrayList<>()).add(ins.getAddress());
            }
        }
        StringBuilder sb = new StringBuilder();
        for (Map.Entry<Long, List<Address>> e : svcSites.entrySet()) {
            sb.append(String.format("SVC 0x%02x : %d site(s)", e.getKey(), e.getValue().size()));
            for (Address a : e.getValue()) {
                Function f = getFunctionContaining(a);
                sb.append("  ").append(a).append(f != null ? "(" + f.getEntryPoint() + ")" : "");
            }
            sb.append("\n");
        }
        try (FileWriter w = new FileWriter(new File(outDir, "svc_summary.txt"), StandardCharsets.UTF_8)) {
            w.write(sb.toString());
        }
        println(sb.toString());

        DecompInterface dec = new DecompInterface();
        dec.openProgram(currentProgram);
        try {
            for (int i = 1; i < args.length; i++) {
                long want = Long.decode(args[i]);
                List<Address> sites = svcSites.get(want);
                if (sites == null) { println("no SVC " + want); continue; }
                Map<Address, Function> wrappers = new LinkedHashMap<>();
                for (Address a : sites) {
                    Function f = getFunctionContaining(a);
                    if (f != null) wrappers.put(f.getEntryPoint(), f);
                }
                for (Function wrap : wrappers.values()) {
                    // dump the wrapper itself
                    writeFunc(dec, outDir, "WRAPPER_svc" + Long.toHexString(want) + "__", wrap);
                    // and its callers
                    Map<Address, Function> callers = new LinkedHashMap<>();
                    for (Reference r : getReferencesTo(wrap.getEntryPoint())) {
                        Function c = getFunctionContaining(r.getFromAddress());
                        if (c != null) callers.put(c.getEntryPoint(), c);
                    }
                    println("SVC 0x" + Long.toHexString(want) + " wrapper " + wrap.getEntryPoint()
                            + " has " + callers.size() + " caller(s)");
                    for (Function c : callers.values()) {
                        writeFunc(dec, outDir, "CALLER_svc" + Long.toHexString(want) + "__", c);
                    }
                }
            }
        } finally {
            dec.dispose();
        }
    }

    private void writeFunc(DecompInterface dec, File dir, String prefix, Function f) throws Exception {
        DecompileResults r = dec.decompileFunction(f, 120, monitor);
        String safe = f.getName(true).replaceAll("[^A-Za-z0-9_.-]", "_");
        File out = new File(dir, prefix + f.getEntryPoint() + "__" + safe + ".c");
        try (FileWriter w = new FileWriter(out, StandardCharsets.UTF_8)) {
            w.write("// " + f.getName(true) + " @ " + f.getEntryPoint() + "\n\n");
            w.write(r.decompileCompleted() ? r.getDecompiledFunction().getC()
                    : "// DECOMPILE FAILED: " + r.getErrorMessage() + "\n");
        }
    }
}
