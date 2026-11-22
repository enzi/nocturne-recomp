// @category Recomp

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.Listing;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.reloc.Relocation;

import java.util.Iterator;
import java.util.TreeSet;

public class SeedFunctionsFromRelocs extends GhidraScript {
    @Override
    public void run() throws Exception {
        MemoryBlock code = currentProgram.getMemory().getBlock("AUTO");
        Listing listing = currentProgram.getListing();
        for (int round = 1; round <= 10; round++) {
            TreeSet<Address> seeds = new TreeSet<>();
            Iterator<Relocation> it = currentProgram.getRelocationTable().getRelocations();
            while (it.hasNext()) {
                Address site = it.next().getAddress();
                if (code.contains(site) && listing.getInstructionContaining(site) == null) {
                    continue;
                }
                Address target = site.getNewAddress(currentProgram.getMemory().getInt(site) & 0xffffffffL);
                if (!code.contains(target)) {
                    continue;
                }
                if (getFunctionContaining(target) != null) {
                    continue;
                }
                Instruction ins = listing.getInstructionContaining(target);
                if (ins != null && !ins.getAddress().equals(target)) {
                    continue;
                }
                seeds.add(target);
            }
            int created = 0;
            for (Address a : seeds) {
                if (getFunctionContaining(a) != null) {
                    continue;
                }
                if (listing.getInstructionAt(a) == null && !disassemble(a)) {
                    continue;
                }
                if (createFunction(a, null) != null) {
                    created++;
                }
            }
            println("round " + round + ": seeds " + seeds.size() + ", functions created " + created);
            if (created == 0) {
                break;
            }
            analyzeChanges(currentProgram);
        }
    }
}
