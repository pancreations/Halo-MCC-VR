// Decompile one function and print a focused window around a text token.
// @category HaloMCCVR.RE

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileOptions;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;

public class DecompileAround extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (currentProgram == null || args.length != 3) {
            throw new IllegalArgumentException(
                "usage: DecompileAround.java <address> <token> <context-lines>");
        }
        Address address = currentProgram.getAddressFactory().getAddress(args[0]);
        Function function = address == null ? null : getFunctionContaining(address);
        if (function == null)
            throw new IllegalArgumentException("no function contains " + args[0]);
        int context = Integer.parseInt(args[2]);

        DecompInterface decompiler = new DecompInterface();
        try {
            decompiler.setOptions(new DecompileOptions());
            decompiler.openProgram(currentProgram);
            DecompileResults result = decompiler.decompileFunction(
                function, 180, monitor);
            if (result == null || !result.decompileCompleted() ||
                    result.getDecompiledFunction() == null) {
                throw new IllegalStateException(result == null ?
                    "null decompiler result" : result.getErrorMessage());
            }
            String[] lines = result.getDecompiledFunction().getC()
                .split("\\R", -1);
            int matches = 0;
            for (int index = 0; index < lines.length; index++) {
                if (!lines[index].contains(args[1]))
                    continue;
                matches++;
                int first = Math.max(0, index - context);
                int last = Math.min(lines.length - 1, index + context);
                println("MATCH line=" + (index + 1));
                for (int line = first; line <= last; line++)
                    println(String.format("%5d %s", line + 1, lines[line]));
            }
            println("TOKEN_MATCHES " + matches);
        }
        finally {
            decompiler.dispose();
        }
    }
}
