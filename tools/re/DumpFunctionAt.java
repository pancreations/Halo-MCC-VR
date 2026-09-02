// Print reproducible structural evidence for one analyzed function.
// @category HaloMCCVR.RE

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileOptions;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;

public class DumpFunctionAt extends GhidraScript {
    private long rva(Address address) {
        return address.subtract(currentProgram.getImageBase());
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (currentProgram == null || args.length != 1) {
            throw new IllegalArgumentException(
                "usage: DumpFunctionAt.java <address>");
        }
        Address address = currentProgram.getAddressFactory().getAddress(args[0]);
        Function function = address == null ? null : getFunctionContaining(address);
        if (function == null) {
            throw new IllegalArgumentException(
                "no function contains " + args[0]);
        }

        println("FUNCTION program=" + currentProgram.getName() +
            " entry=" + function.getEntryPoint() +
            " rva=0x" + Long.toHexString(rva(function.getEntryPoint())) +
            " name=" + function.getName() +
            " signature=" + function.getSignature().getPrototypeString());

        ReferenceIterator callers = currentProgram.getReferenceManager()
            .getReferencesTo(function.getEntryPoint());
        while (callers.hasNext()) {
            Reference reference = callers.next();
            Function caller = getFunctionContaining(reference.getFromAddress());
            println("CALLER from=0x" +
                Long.toHexString(rva(reference.getFromAddress())) +
                " entry=" + (caller == null ? "none" : "0x" +
                    Long.toHexString(rva(caller.getEntryPoint()))) +
                " name=" + (caller == null ? "none" : caller.getName()));
        }

        InstructionIterator instructions = currentProgram.getListing()
            .getInstructions(function.getBody(), true);
        while (instructions.hasNext() && !monitor.isCancelled()) {
            Instruction instruction = instructions.next();
            for (Reference reference : instruction.getReferencesFrom()) {
                Function callee = getFunctionAt(reference.getToAddress());
                if (callee != null) {
                    Address entry = callee.getEntryPoint();
                    println("CALL at=0x" +
                        Long.toHexString(rva(instruction.getAddress())) +
                        " target=" +
                        (entry.getAddressSpace().equals(
                            currentProgram.getImageBase().getAddressSpace())
                            ? "0x" + Long.toHexString(rva(entry))
                            : entry.toString()) +
                        " name=" + callee.getName());
                }
            }
        }

        DecompInterface decompiler = new DecompInterface();
        try {
            decompiler.setOptions(new DecompileOptions());
            decompiler.openProgram(currentProgram);
            DecompileResults result =
                decompiler.decompileFunction(function, 180, monitor);
            if (result != null && result.decompileCompleted() &&
                    result.getDecompiledFunction() != null) {
                println("DECOMPILED_BEGIN");
                println(result.getDecompiledFunction().getC());
                println("DECOMPILED_END");
            }
            else {
                println("DECOMPILED_UNAVAILABLE " +
                    (result == null ? "null result" : result.getErrorMessage()));
            }
        }
        finally {
            decompiler.dispose();
        }
    }
}
