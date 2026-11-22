// @category Recomp

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;

import java.io.FileWriter;
import java.io.PrintWriter;
import java.nio.charset.StandardCharsets;
import java.util.LinkedHashSet;
import java.util.Set;

public class DecompileByString extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);
        try (PrintWriter w = new PrintWriter(new FileWriter(args[0]))) {
            for (int i = 1; i < args.length; i++) {
                Set<Function> funcs = new LinkedHashSet<>();
                if (args[i].startsWith("0x")) {
                    Function f = getFunctionContaining(toAddr(Long.parseLong(args[i].substring(2), 16)));
                    if (f != null) {
                        funcs.add(f);
                    }
                } else {
                    byte[] needle = (args[i] + "\0").getBytes(StandardCharsets.US_ASCII);
                    Address a = currentProgram.getMinAddress();
                    while ((a = find(a, needle)) != null) {
                        w.println("// string \"" + args[i] + "\" at " + a);
                        for (Reference r : getReferencesTo(a)) {
                            Function f = getFunctionContaining(r.getFromAddress());
                            if (f != null) {
                                funcs.add(f);
                            }
                        }
                        a = a.add(1);
                    }
                }
                for (Function f : funcs) {
                    DecompileResults r = decomp.decompileFunction(f, 120, monitor);
                    w.println("// ==== " + args[i] + " -> " + f.getName() + " @ " + f.getEntryPoint() + " ====");
                    w.println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : "// decompile failed: " + r.getErrorMessage());
                }
            }
        }
        decomp.dispose();
    }
}
