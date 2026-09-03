// Print matching official-H4EK strings and owning xref functions without
// instruction windows. Arguments are case-insensitive substrings.
// @category HaloMCCVR.RE

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.DataIterator;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;

public class FindH4StringRefsBrief extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] needles = getScriptArgs();
        for (int i = 0; i < needles.length; ++i)
            needles[i] = needles[i].toLowerCase();
        DataIterator data = currentProgram.getListing().getDefinedData(true);
        int matches = 0;
        while (data.hasNext()) {
            Data item = data.next();
            if (!item.hasStringValue()) continue;
            String value = item.getValue().toString();
            String lower = value.toLowerCase();
            boolean wanted = needles.length == 0;
            for (String needle : needles)
                if (lower.contains(needle)) { wanted = true; break; }
            if (!wanted) continue;
            ++matches;
            Address address = item.getAddress();
            println("STRING RVA 0x" + Long.toHexString(
                address.subtract(currentProgram.getImageBase())) + ": " + value);
            ReferenceIterator refs = currentProgram.getReferenceManager()
                .getReferencesTo(address);
            while (refs.hasNext()) {
                Reference ref = refs.next();
                Function owner = getFunctionContaining(ref.getFromAddress());
                String function = owner == null ? "<no function>" :
                    owner.getName() + " RVA 0x" + Long.toHexString(
                        owner.getEntryPoint().subtract(currentProgram.getImageBase()));
                println("  XREF RVA 0x" + Long.toHexString(
                    ref.getFromAddress().subtract(currentProgram.getImageBase())) +
                    " in " + function);
            }
        }
        println("MATCHES: " + matches);
    }
}
