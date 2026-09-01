// E-H2-71: what frame does Halo 2 compose the first-person weapon in, and
// what does each renderer draw it with?
//
// Static, official-kit only (H2EK halo2_tag_test.exe). Retail halo2.dll is
// never opened here - the user's standing directive.
//
// Usage: analyzeHeadless <project-dir> <project> -process halo2_tag_test.exe
//        -scriptPath tools/ghidra -postScript DumpH2FirstPersonCamera.java
//        [extra symbol substrings...]
//
// Prints, for every function whose name or source path matches the
// first-person weapon / render-camera vocabulary:
//   * its symbol, RVA and signature,
//   * every callee whose name matches the camera/matrix vocabulary,
//   * its decompilation (bounded), so the composition frame is readable.
// @category HaloMCCVR

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileOptions;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;

import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

public class DumpH2FirstPersonCamera extends GhidraScript {

    private static final String[] TARGETS = {
        "first_person",
        "firstperson",
        "draw_first",
        "render_first",
        "weapon_render",
        "fp_weapon",
    };

    private static final String[] CAMERA_WORDS = {
        "camera", "matrix", "orient", "forward", "up", "position",
        "view", "projection", "frame", "node", "marker", "aim",
        "render_view", "observer", "perspective", "compose",
    };

    private long rva(Address address) {
        return address.subtract(currentProgram.getImageBase());
    }

    private boolean matches(String haystack, String[] needles) {
        if (haystack == null) return false;
        final String lower = haystack.toLowerCase(Locale.ROOT);
        for (String needle : needles) {
            if (lower.contains(needle)) return true;
        }
        return false;
    }

    @Override
    public void run() throws Exception {
        final List<String> extra = new ArrayList<>();
        for (String argument : getScriptArgs()) extra.add(argument);

        final DecompInterface decompiler = new DecompInterface();
        decompiler.setOptions(new DecompileOptions());
        decompiler.openProgram(currentProgram);

        println("=== H2EK first-person weapon camera evidence ===");
        println("program " + currentProgram.getName()
            + " imageBase " + currentProgram.getImageBase());

        final FunctionIterator functions =
            currentProgram.getFunctionManager().getFunctions(true);
        int reported = 0;
        while (functions.hasNext() && !monitor.isCancelled()) {
            final Function function = functions.next();
            final String name = function.getName();
            boolean wanted = matches(name, TARGETS);
            for (String needle : extra) {
                if (!wanted && name.toLowerCase(Locale.ROOT)
                        .contains(needle.toLowerCase(Locale.ROOT))) {
                    wanted = true;
                }
            }
            if (!wanted) continue;
            ++reported;

            println("");
            println("################################################");
            println("FUNCTION " + name + "  RVA 0x"
                + Long.toHexString(rva(function.getEntryPoint())));
            println("  signature: " + function.getSignature().getPrototypeString());

            // Who calls it - the caller distinguishes the two renderers.
            final ReferenceIterator references = currentProgram
                .getReferenceManager().getReferencesTo(function.getEntryPoint());
            int callers = 0;
            while (references.hasNext() && callers < 24) {
                final Reference reference = references.next();
                final Function owner =
                    getFunctionContaining(reference.getFromAddress());
                println("  CALLER 0x"
                    + Long.toHexString(rva(reference.getFromAddress()))
                    + "  " + (owner == null ? "<none>" : owner.getName()));
                ++callers;
            }

            // Callees in the camera/matrix vocabulary.
            final InstructionIterator instructions = currentProgram.getListing()
                .getInstructions(function.getBody(), true);
            while (instructions.hasNext() && !monitor.isCancelled()) {
                final Instruction instruction = instructions.next();
                for (Reference reference : instruction.getReferencesFrom()) {
                    final Function callee =
                        getFunctionAt(reference.getToAddress());
                    if (callee == null) continue;
                    if (!matches(callee.getName(), CAMERA_WORDS)) continue;
                    println("    CALLS " + callee.getName() + " @ 0x"
                        + Long.toHexString(rva(callee.getEntryPoint()))
                        + "   from 0x"
                        + Long.toHexString(rva(instruction.getAddress())));
                }
            }

            final DecompileResults results =
                decompiler.decompileFunction(function, 90, monitor);
            if (results != null && results.decompileCompleted()
                && results.getDecompiledFunction() != null) {
                final String code = results.getDecompiledFunction().getC();
                println("  ---- decompiled ----");
                int printed = 0;
                for (String line : code.split("\n")) {
                    println("  " + line);
                    if (++printed > 220) {
                        println("  ... [truncated]");
                        break;
                    }
                }
            } else {
                println("  <decompilation unavailable>");
            }
        }
        println("");
        println("=== reported " + reported + " functions ===");
        decompiler.dispose();
    }
}
