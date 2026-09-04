// Print functions containing memory operands for every requested hex offset.
// @category HaloMCCVR.RE

import java.util.HashSet;
import java.util.Set;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;

public class FindFunctionsUsingOffsets extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (currentProgram == null || args.length == 0)
            throw new IllegalArgumentException(
                "usage: FindFunctionsUsingOffsets.java <hex-offset> [...]");
        String[] needles = new String[args.length];
        for (int i = 0; i < args.length; ++i) {
            String value = args[i].toLowerCase();
            needles[i] = value.startsWith("0x") ? value : "0x" + value;
        }
        int matches = 0;
        FunctionIterator functions = currentProgram.getFunctionManager()
            .getFunctions(true);
        while (functions.hasNext()) {
            Function function = functions.next();
            Set<String> found = new HashSet<>();
            InstructionIterator instructions = currentProgram.getListing()
                .getInstructions(function.getBody(), true);
            while (instructions.hasNext()) {
                Instruction instruction = instructions.next();
                String text = instruction.toString().toLowerCase();
                for (String needle : needles)
                    if (text.contains(needle)) found.add(needle);
            }
            if (found.size() != needles.length) continue;
            println("FUNCTION address=" + function.getEntryPoint() +
                " name=" + function.getName(true));
            matches++;
        }
        println("FUNCTION_MATCHES " + matches);
    }
}
