// Print functions whose imported/demangled name contains a case-insensitive term.
// @category HaloMCCVR.RE

import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;

public class FindFunctionsByName extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (currentProgram == null || args.length != 1)
            throw new IllegalArgumentException(
                "usage: FindFunctionsByName.java <substring>");
        String needle = args[0].toLowerCase();
        int count = 0;
        FunctionIterator functions = currentProgram.getFunctionManager()
            .getFunctions(true);
        while (functions.hasNext()) {
            Function function = functions.next();
            String name = function.getName(true);
            if (!name.toLowerCase().contains(needle)) continue;
            println("FUNCTION address=" + function.getEntryPoint() +
                " name=" + name + " signature=" + function.getSignature());
            count++;
        }
        println("FUNCTION_MATCHES " + count);
    }
}
