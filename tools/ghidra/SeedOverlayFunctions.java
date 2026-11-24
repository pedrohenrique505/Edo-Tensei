// Semeia funcoes em overlays MWo3 antes de CreateGapFunctions e ExportPS2Functions:
//   1. Ponteiros da secao .ctors (construtores estaticos C++ do overlay)
//   2. Ponteiros alinhados em .data que apontam para dentro de .text / .text.ctors
//      (vtables, tabelas de estado e callbacks)
//   3. Primeiro endereco de .text (0x006C6D40)
//
// @category PS2
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.mem.MemoryBlock;

import java.util.TreeSet;

public class SeedOverlayFunctions extends GhidraScript {
    @Override
    public void run() throws Exception {
        FunctionManager fm = currentProgram.getFunctionManager();
        AddressSet execSet = new AddressSet();
        Address textStart = null;

        for (MemoryBlock block : currentProgram.getMemory().getBlocks()) {
            if (block.isExecute() && block.isInitialized()) {
                execSet.add(block.getStart(), block.getEnd());
                if (block.getName().equals(".text")) {
                    textStart = block.getStart();
                }
            }
        }

        TreeSet<Address> seeds = new TreeSet<>();
        if (textStart != null) {
            seeds.add(textStart);
        }

        // Varre .ctors e .data em busca de ponteiros alinhados para regioes executaveis
        for (MemoryBlock block : currentProgram.getMemory().getBlocks()) {
            String name = block.getName();
            if (!block.isInitialized() || (!name.equals(".ctors") && !name.equals(".data"))) {
                continue;
            }
            Address cur = block.getStart();
            Address end = block.getEnd();
            while (cur.compareTo(end) <= 0 && cur.getOffset() + 3 <= end.getOffset()) {
                if (monitor.isCancelled()) break;
                int word = getInt(cur);
                long unsigned = word & 0xFFFFFFFFL;
                if ((unsigned & 3L) == 0L) {
                    Address target = cur.getNewAddress(unsigned);
                    if (execSet.contains(target)) {
                        seeds.add(target);
                    }
                }
                if (cur.getOffset() + 4 > end.getOffset()) break;
                cur = cur.add(4);
            }
        }

        int created = 0;
        for (Address target : seeds) {
            if (monitor.isCancelled()) break;
            if (fm.getFunctionAt(target) != null) {
                continue;
            }
            if (getInt(target) == 0) {
                continue;
            }
            if (getInstructionAt(target) == null) {
                disassemble(target);
            }
            if (getInstructionAt(target) != null) {
                Function f = createFunction(target, null);
                if (f != null) {
                    ++created;
                }
            }
        }
        println("SeedOverlayFunctions: " + seeds.size() + " candidatos, " + created + " funcoes criadas");
    }
}
