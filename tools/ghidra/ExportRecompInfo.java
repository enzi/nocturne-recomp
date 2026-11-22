// @category Recomp

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.AddressRange;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.symbol.FlowType;
import ghidra.program.model.symbol.Reference;

import java.io.FileWriter;
import java.io.PrintWriter;

public class ExportRecompInfo extends GhidraScript {
    @Override
    public void run() throws Exception {
        String outPath = getScriptArgs()[0];
        try (PrintWriter w = new PrintWriter(new FileWriter(outPath))) {
            for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
                if (f.isExternal()) {
                    continue;
                }
                w.printf("func %08x %s%s", f.getEntryPoint().getOffset(), f.getName(), f.hasNoReturn() ? " noreturn" : "");
                for (AddressRange r : f.getBody()) {
                    w.printf(" %08x-%08x", r.getMinAddress().getOffset(), r.getMaxAddress().getOffset() + 1);
                }
                w.println();
            }
            for (Instruction ins : currentProgram.getListing().getInstructions(true)) {
                FlowType ft = ins.getFlowType();
                if (!ft.isJump() || !ft.isComputed()) {
                    continue;
                }
                w.printf("jump %08x", ins.getAddress().getOffset());
                for (Reference r : ins.getReferencesFrom()) {
                    if (r.getReferenceType().isFlow()) {
                        w.printf(" %08x", r.getToAddress().getOffset());
                    }
                }
                w.println();
            }
        }
    }
}
