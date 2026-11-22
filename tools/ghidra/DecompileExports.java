// @category Recomp

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressIterator;
import ghidra.program.model.symbol.Symbol;

import java.io.FileWriter;
import java.io.PrintWriter;
import java.util.ArrayDeque;
import java.util.LinkedHashSet;
import java.util.Set;

public class DecompileExports extends GhidraScript {
    @Override
    public void run() throws Exception {
        String outPath = getScriptArgs()[0];
        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);
        Set<Function> done = new LinkedHashSet<>();
        ArrayDeque<Function> work = new ArrayDeque<>();
        AddressIterator it = currentProgram.getSymbolTable().getExternalEntryPointIterator();
        while (it.hasNext()) {
            Address a = it.next();
            Symbol s = getSymbolAt(a);
            Function f = getFunctionAt(a);
            if (f != null && s != null && s.getName().startsWith("APIDLL")) {
                work.add(f);
            }
        }
        try (PrintWriter w = new PrintWriter(new FileWriter(outPath))) {
            while (!work.isEmpty()) {
                Function f = work.poll();
                if (!done.add(f) || f.isThunk() || f.isExternal()) {
                    continue;
                }
                DecompileResults r = decomp.decompileFunction(f, 120, monitor);
                w.println("// ==== " + f.getName() + " @ " + f.getEntryPoint() + " ====");
                w.println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : "// decompile failed: " + r.getErrorMessage());
                for (Function callee : f.getCalledFunctions(monitor)) {
                    if (!done.contains(callee) && !callee.isExternal() && !callee.isThunk() && done.size() + work.size() < 400) {
                        work.add(callee);
                    }
                }
            }
        }
        decomp.dispose();
    }
}
