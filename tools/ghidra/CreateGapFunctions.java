// Cria funções nos buracos de código que a auto-análise deixou fora de qualquer
// função (ex.: folhas de C++ "jr ra" alcançadas só por vtable/JALR indireto).
// Sem isso, o PS2Recomp não gera código para elas e o runtime cai em
// "guest-branch:missing-target".
//
// Em SLES_556.05 (Naruto UN5), a seção executável única (SECTION4, 0x100000..0x617a80)
// mistura código (.text até ~0x413000) e dados (.rodata/.data a partir de ~0x413320).
// Por isso limitamos a varredura de SECTION4 à região de código (< 0x00413000).
//
// @category PS2
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressRange;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.mem.MemoryBlock;

public class CreateGapFunctions extends GhidraScript {
    private static final int JR_RA = 0x03E00008;
    private static final long CODE_END_LIMIT = 0x00413000L;

    @Override
    public void run() throws Exception {
        FunctionManager fm = currentProgram.getFunctionManager();
        AddressSet text = new AddressSet();
        for (MemoryBlock block : currentProgram.getMemory().getBlocks()) {
            if (!block.isExecute() || !block.isInitialized()) {
                continue;
            }
            if (block.getName().startsWith(".text")) {
                text.add(block.getStart(), block.getEnd());
            } else if (block.getName().equals("SECTION4")) {
                Address start = block.getStart();
                Address limit = start.getNewAddress(CODE_END_LIMIT - 1);
                Address end = block.getEnd().compareTo(limit) < 0 ? block.getEnd() : limit;
                text.add(start, end);
            }
        }
        AddressSet covered = new AddressSet();
        for (Function f : fm.getFunctions(true)) {
            covered.add(f.getBody());
        }
        AddressSet gapsSet = text.subtract(covered);

        int created = 0, gaps = 0;
        for (AddressRange r : gapsSet.getAddressRanges()) {
            if (monitor.isCancelled()) break;
            Address gapEnd = r.getMaxAddress().add(1);
            if (!containsJrRa(r.getMinAddress(), gapEnd)) {
                continue;
            }
            ++gaps;
            Address cur = align4(r.getMinAddress());
            while (cur.compareTo(gapEnd) < 0) {
                Function existing = fm.getFunctionContaining(cur);
                if (existing != null) {
                    cur = cur.add(4);
                    continue;
                }
                if (getInt(cur) == 0) {
                    cur = cur.add(4);
                    continue;
                }
                if (getInstructionAt(cur) == null) {
                    disassemble(cur);
                }
                Function nf = (getInstructionAt(cur) != null) ? createFunction(cur, null) : null;
                if (nf != null) {
                    ++created;
                    AddressRange body = nf.getBody().getRangeContaining(cur);
                    cur = align4(body != null ? body.getMaxAddress().add(1) : cur.add(4));
                } else {
                    cur = cur.add(4);
                }
            }
        }
        println("CreateGapFunctions: " + gaps + " buracos com jr ra, " + created + " funções criadas");
    }

    private Address align4(Address a) {
        long r = a.getOffset() & 3;
        return r == 0 ? a : a.add(4 - r);
    }

    private boolean containsJrRa(Address start, Address endExclusive) throws Exception {
        for (Address a = align4(start); a.compareTo(endExclusive) < 0; a = a.add(4)) {
            if (getInt(a) == JR_RA) {
                return true;
            }
        }
        return false;
    }
}
