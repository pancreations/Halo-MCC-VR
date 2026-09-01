// Recon: what does the imported H2EK program actually give us?
// Counts functions/symbols and locates source-path strings, which is how the
// kit names its code (E-H2-36 used assert text and source paths, not symbols).
// @category HaloMCCVR

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.DataIterator;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;

import java.util.Locale;

public class DumpH2Recon extends GhidraScript {
    private long rva(Address address) {
        return address.subtract(currentProgram.getImageBase());
    }

    @Override
    public void run() throws Exception {
        println("program " + currentProgram.getName());
        println("imageBase " + currentProgram.getImageBase());

        int functions = 0;
        int named = 0;
        final FunctionIterator iterator =
            currentProgram.getFunctionManager().getFunctions(true);
        while (iterator.hasNext() && !monitor.isCancelled()) {
            final Function function = iterator.next();
            ++functions;
            if (!function.getName().startsWith("FUN_")) ++named;
        }
        println("functions=" + functions + " nonDefaultNames=" + named);

        final String[] needles = getScriptArgs().length > 0
            ? getScriptArgs()
            : new String[] {"first_person", "weapon", "render_view", "camera"};

        for (String needle : needles) {
            println("");
            println("### strings containing: " + needle);
            int hits = 0;
            final DataIterator data =
                currentProgram.getListing().getDefinedData(true);
            while (data.hasNext() && hits < 40 && !monitor.isCancelled()) {
                final Data item = data.next();
                final Object value = item.getValue();
                if (!(value instanceof String)) continue;
                final String text = (String) value;
                if (!text.toLowerCase(Locale.ROOT)
                        .contains(needle.toLowerCase(Locale.ROOT))) {
                    continue;
                }
                ++hits;
                println("  0x" + Long.toHexString(rva(item.getAddress()))
                    + "  \"" + (text.length() > 150
                        ? text.substring(0, 150) + "..." : text) + "\"");
                final ReferenceIterator references = currentProgram
                    .getReferenceManager().getReferencesTo(item.getAddress());
                int shown = 0;
                while (references.hasNext() && shown < 6) {
                    final Reference reference = references.next();
                    final Function owner =
                        getFunctionContaining(reference.getFromAddress());
                    println("      XREF 0x"
                        + Long.toHexString(rva(reference.getFromAddress()))
                        + "  in " + (owner == null ? "<none>"
                            : owner.getName() + " @ 0x"
                                + Long.toHexString(rva(owner.getEntryPoint()))));
                    ++shown;
                }
            }
            println("  (" + hits + " shown)");
        }
    }
}
