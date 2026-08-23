// Generic headless editing-kit evidence dump.
// Usage: analyzeHeadless <project-dir> <project> -process <program>
//        -scriptPath tools/ghidra -postScript DumpKitEvidence.java
//        str:<substring> rva:<hex-rva> find:<substring>:<context>:<hex-rva>
// @category HaloMCCVR

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.DataIterator;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;

public class DumpKitEvidence extends GhidraScript {
    private Address fromRva(long rva) {
        return currentProgram.getImageBase().add(rva);
    }

    private String label(Function function) {
        if (function == null) return "<no function>";
        long rva = function.getEntryPoint().subtract(currentProgram.getImageBase());
        return function.getName() + " @ RVA 0x" + Long.toHexString(rva);
    }

    private void dumpReferences(Address target, String indent) {
        ReferenceIterator references = currentProgram.getReferenceManager()
            .getReferencesTo(target);
        while (references.hasNext()) {
            Reference reference = references.next();
            Function owner = getFunctionContaining(reference.getFromAddress());
            println(indent + "XREF " + reference.getFromAddress() + " " + label(owner));
        }
    }

    private void dumpStrings(String needle) {
        String lowerNeedle = needle.toLowerCase();
        DataIterator items = currentProgram.getListing().getDefinedData(true);
        while (items.hasNext()) {
            Data item = items.next();
            if (!item.hasStringValue()) continue;
            String value = item.getValue().toString();
            if (!value.toLowerCase().contains(lowerNeedle)) continue;
            println("STRING " + item.getAddress() + ": " + value);
            dumpReferences(item.getAddress(), "  ");
        }
    }

    private void dumpFunction(DecompInterface decompiler, long rva) throws Exception {
        Address requested = fromRva(rva);
        Function function = getFunctionContaining(requested);
        println("\n========== RVA 0x" + Long.toHexString(rva) + " -> " +
            label(function) + " ==========");
        if (function == null) return;
        println("CALLERS:");
        dumpReferences(function.getEntryPoint(), "  ");
        println("CALLS:");
        InstructionIterator instructions = currentProgram.getListing()
            .getInstructions(function.getBody(), true);
        while (instructions.hasNext()) {
            Instruction instruction = instructions.next();
            if (!instruction.getFlowType().isCall()) continue;
            for (Address target : instruction.getFlows()) {
                println("  " + instruction.getAddress() + " -> " + target + " " +
                    label(getFunctionAt(target)));
            }
        }
        DecompileResults result = decompiler.decompileFunction(function, 120, monitor);
        if (result.decompileCompleted() && result.getDecompiledFunction() != null)
            println("DECOMPILE:\n" + result.getDecompiledFunction().getC());
        else
            println("DECOMPILE FAILED: " + result.getErrorMessage());
    }

    private void findInFunction(
            DecompInterface decompiler, String needle, int context, long rva)
            throws Exception {
        Function function = getFunctionContaining(fromRva(rva));
        println("\n========== FIND \"" + needle + "\" IN " +
            label(function) + " ==========");
        if (function == null) return;
        DecompileResults result = decompiler.decompileFunction(function, 120, monitor);
        if (!result.decompileCompleted() || result.getDecompiledFunction() == null) {
            println("DECOMPILE FAILED: " + result.getErrorMessage());
            return;
        }
        String[] lines = result.getDecompiledFunction().getC().split("\\R", -1);
        String lowerNeedle = needle.toLowerCase();
        boolean[] selected = new boolean[lines.length];
        for (int line = 0; line < lines.length; ++line) {
            if (!lines[line].toLowerCase().contains(lowerNeedle)) continue;
            int first = Math.max(0, line - context);
            int last = Math.min(lines.length - 1, line + context);
            for (int selectedLine = first; selectedLine <= last; ++selectedLine)
                selected[selectedLine] = true;
        }
        boolean gap = false;
        for (int line = 0; line < lines.length; ++line) {
            if (!selected[line]) {
                gap = true;
                continue;
            }
            if (gap) println("  ...");
            gap = false;
            println(String.format("%5d: %s", line + 1, lines[line]));
        }
    }

    private void dumpBytes(long rva, int count) throws Exception {
        if (count < 1 || count > 256)
            throw new IllegalArgumentException("byte count must be 1..256");
        Address address = fromRva(rva);
        byte[] bytes = new byte[count];
        currentProgram.getMemory().getBytes(address, bytes);
        StringBuilder output = new StringBuilder();
        for (byte value : bytes) {
            if (output.length() != 0) output.append(' ');
            output.append(String.format("%02X", value & 0xff));
        }
        println("BYTES RVA 0x" + Long.toHexString(rva) + ": " + output);
    }

    @Override
    public void run() throws Exception {
        println("PROGRAM: " + currentProgram.getName());
        println("IMAGE BASE: " + currentProgram.getImageBase());
        DecompInterface decompiler = new DecompInterface();
        decompiler.toggleCCode(true);
        decompiler.toggleSyntaxTree(true);
        if (!decompiler.openProgram(currentProgram)) {
            printerr("Could not initialize decompiler");
            return;
        }
        try {
            for (String argument : getScriptArgs()) {
                if (argument.startsWith("str:"))
                    dumpStrings(argument.substring(4));
                else if (argument.startsWith("rva:"))
                    dumpFunction(decompiler,
                        Long.parseUnsignedLong(argument.substring(4), 16));
                else if (argument.startsWith("find:")) {
                    String[] parts = argument.substring(5).split(":", 3);
                    if (parts.length != 3)
                        throw new IllegalArgumentException(
                            "find syntax: find:<substring>:<context>:<hex-rva>");
                    findInFunction(decompiler, parts[0],
                        Integer.parseInt(parts[1]),
                        Long.parseUnsignedLong(parts[2], 16));
                }
                else if (argument.startsWith("bytes:")) {
                    String[] parts = argument.substring(6).split(":", 2);
                    if (parts.length != 2)
                        throw new IllegalArgumentException(
                            "bytes syntax: bytes:<hex-rva>:<count>");
                    dumpBytes(Long.parseUnsignedLong(parts[0], 16),
                        Integer.parseInt(parts[1]));
                }
            }
        }
        finally {
            decompiler.dispose();
        }
    }
}
