// Find raw ASCII byte strings and report every Ghidra reference to them.
// Useful for kit executables where auto-analysis did not create string data.
// @category HaloMCCVR.RE

import java.nio.charset.StandardCharsets;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSetView;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;

public class FindAsciiXrefs extends GhidraScript {
    private long rva(Address address) {
        return address.subtract(currentProgram.getImageBase());
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (currentProgram == null || args.length == 0) {
            throw new IllegalArgumentException(
                "usage: FindAsciiXrefs.java <exact-ascii-text> [...]");
        }

        Memory memory = currentProgram.getMemory();
        AddressSetView loaded = memory.getLoadedAndInitializedAddressSet();
        Address minimum = loaded.getMinAddress();
        Address maximum = loaded.getMaxAddress();
        for (String text : args) {
            byte[] bytes = text.getBytes(StandardCharsets.US_ASCII);
            Address cursor = minimum;
            int matches = 0;
            while (cursor != null && cursor.compareTo(maximum) <= 0) {
                Address hit = memory.findBytes(
                    cursor, maximum, bytes, null, true, monitor);
                if (hit == null)
                    break;
                matches++;
                println("ASCII text=" + text + " rva=0x" +
                    Long.toHexString(rva(hit)) + " address=" + hit);
                ReferenceIterator references = currentProgram
                    .getReferenceManager().getReferencesTo(hit);
                int xrefs = 0;
                while (references.hasNext()) {
                    Reference reference = references.next();
                    Address from = reference.getFromAddress();
                    Function function = getFunctionContaining(from);
                    println("REFERENCE from=0x" +
                        Long.toHexString(rva(from)) + " function=" +
                        (function == null ? "none" : "0x" +
                            Long.toHexString(rva(function.getEntryPoint()))) +
                        " name=" + (function == null ? "none" :
                            function.getName()));
                    xrefs++;
                }
                println("XREFS " + xrefs);
                if (xrefs == 0) {
                    Address last = hit.add(bytes.length - 1L);
                    InstructionIterator instructions = currentProgram
                        .getListing().getInstructions(true);
                    int operands = 0;
                    while (instructions.hasNext()) {
                        Instruction instruction = instructions.next();
                        for (int operand = 0;
                                operand < instruction.getNumOperands(); operand++) {
                            for (Object object : instruction.getOpObjects(operand)) {
                                if (!(object instanceof Address))
                                    continue;
                                Address target = (Address)object;
                                if (target.compareTo(hit) < 0 ||
                                        target.compareTo(last) > 0)
                                    continue;
                                Function function = getFunctionContaining(
                                    instruction.getAddress());
                                println("OPERAND from=0x" + Long.toHexString(
                                    rva(instruction.getAddress())) + " function=" +
                                    (function == null ? "none" : "0x" +
                                        Long.toHexString(rva(
                                            function.getEntryPoint()))) +
                                    " name=" + (function == null ? "none" :
                                        function.getName()) + " instruction=" +
                                    instruction);
                                operands++;
                            }
                        }
                    }
                    println("OPERANDS " + operands);
                }
                cursor = hit.add(bytes.length);
            }
            println("ASCII_MATCHES " + matches + " text=" + text);
        }
    }
}
