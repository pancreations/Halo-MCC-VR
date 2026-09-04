// Find official-H4EK strings and the functions that reference them.
// Arguments are case-insensitive substrings; any argument may match.
// @category HaloMCCVR.RE

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.DataIterator;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;

public class FindH4Strings extends GhidraScript {
    private void dumpInstructionWindow(Address center) {
        Instruction first = currentProgram.getListing().getInstructionAt(center);
        if (first == null) {
            first = currentProgram.getListing().getInstructionContaining(center);
        }
        for (int i = 0; first != null && i < 10; ++i) {
            Instruction previous = currentProgram.getListing()
                .getInstructionBefore(first.getAddress());
            if (previous == null) {
                break;
            }
            first = previous;
        }
        Instruction instruction = first;
        for (int i = 0; instruction != null && i < 24; ++i) {
            println("    " + instruction.getAddress() + "  " + instruction);
            instruction = currentProgram.getListing()
                .getInstructionAfter(instruction.getAddress());
        }
    }

    private String functionLabel(Function function) {
        if (function == null) {
            return "<no function>";
        }
        long rva = function.getEntryPoint().subtract(currentProgram.getImageBase());
        return function.getName() + " @ RVA 0x" + Long.toHexString(rva);
    }

    @Override
    public void run() throws Exception {
        String[] arguments = getScriptArgs();
        if (arguments.length == 0) {
            printerr("usage: FindH4Strings.java <needle> [needle ...]");
            return;
        }
        for (int i = 0; i < arguments.length; ++i) {
            arguments[i] = arguments[i].toLowerCase();
        }

        DataIterator data = currentProgram.getListing().getDefinedData(true);
        int matches = 0;
        while (data.hasNext()) {
            Data item = data.next();
            if (!item.hasStringValue()) {
                continue;
            }
            String value = item.getValue().toString();
            String lower = value.toLowerCase();
            boolean wanted = false;
            for (String argument : arguments) {
                if (lower.contains(argument)) {
                    wanted = true;
                    break;
                }
            }
            if (!wanted) {
                continue;
            }

            ++matches;
            Address address = item.getAddress();
            long rva = address.subtract(currentProgram.getImageBase());
            println("STRING RVA 0x" + Long.toHexString(rva) + ": " + value);
            ReferenceIterator references = currentProgram.getReferenceManager()
                .getReferencesTo(address);
            while (references.hasNext()) {
                Reference reference = references.next();
                Function owner = getFunctionContaining(reference.getFromAddress());
                println("  XREF " + reference.getFromAddress() + " " +
                    functionLabel(owner));
                if (reference.getFromAddress().getOffset() <
                    currentProgram.getImageBase().getOffset() + 0x18000000L) {
                    dumpInstructionWindow(reference.getFromAddress());
                }
            }
        }
        println("MATCHES: " + matches);
    }
}
