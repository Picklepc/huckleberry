import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.symbol.Reference;

// For each function address arg, list every data reference it makes, and for each
// referenced pointer-sized location dump the 4-byte value there and, recursively for a
// couple of levels, the value(s) at that target (to walk vtables / descriptor structs).
// Also, if a resolved value points at code, name the containing function.
public class ResolvePointers extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        Memory mem = currentProgram.getMemory();
        for (String a : args) {
            Address fa = toAddr(a);
            Function f = getFunctionContaining(fa);
            println("=== " + a + (f != null ? " (" + f.getName() + ")" : "") + " ===");
            if (f == null) { dumpStruct(mem, fa, 16); continue; }
            for (Address ins = f.getBody().getMinAddress(); ins != null && ins.compareTo(f.getBody().getMaxAddress()) <= 0; ) {
                var cu = getInstructionAt(ins);
                if (cu == null) { ins = ins.add(2); continue; }
                for (Reference r : cu.getReferencesFrom()) {
                    if (r.getReferenceType().isData()) {
                        Address t = r.getToAddress();
                        try {
                            long v = mem.getInt(t) & 0xffffffffL;
                            Address vp = toAddr(v & ~1L);
                            Function tf = getFunctionContaining(vp);
                            println(String.format("  %s -> [%s]=0x%08x %s", ins, t, v,
                                    tf != null ? "FUNC " + tf.getName() + "@" + tf.getEntryPoint() : ""));
                            // if the target is a struct/vtable in flash, dump a few words
                            if (tf == null && mem.contains(vp)) dumpStruct(mem, vp, 8);
                        } catch (Exception e) {}
                    }
                }
                ins = cu.getMaxAddress().add(1);
            }
        }
    }

    private void dumpStruct(Memory mem, Address base, int words) {
        for (int i = 0; i < words; i++) {
            try {
                Address at = base.add(i * 4L);
                long v = mem.getInt(at) & 0xffffffffL;
                Function tf = getFunctionContaining(toAddr(v & ~1L));
                println(String.format("      [%s+0x%02x]=0x%08x %s", base, i * 4, v,
                        tf != null ? "FUNC " + tf.getName() + "@" + tf.getEntryPoint() : ""));
            } catch (Exception e) {}
        }
    }
}
