// Find analyzed string data and report every referencing function.
// @category HaloMCCVR.RE

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.DataIterator;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;

public class FindStringReferences extends GhidraScript {
    private long rva(Address address) {
        return address.subtract(currentProgram.getImageBase());
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (currentProgram == null || args.length == 0) {
            throw new IllegalArgumentException(
                "usage: FindStringReferences.java <case-insensitive-text> [...]");
        }

        int matches = 0;
        DataIterator dataIterator = currentProgram.getListing()
            .getDefinedData(true);
        while (dataIterator.hasNext()) {
            Data data = dataIterator.next();
            if (!data.hasStringValue())
                continue;
            String text = data.getDefaultValueRepresentation();
            if (text == null) {
                continue;
            }
            String lowerText = text.toLowerCase();
            String matchedNeedle = null;
            for (String arg : args) {
                String needle = arg.toLowerCase();
                if (lowerText.contains(needle)) {
                    matchedNeedle = needle;
                    break;
                }
            }
            if (matchedNeedle == null)
                continue;
            matches++;
            println("STRING needle=" + matchedNeedle + " rva=0x" +
                Long.toHexString(rva(data.getAddress())) + " address=" +
                data.getAddress() + " text=" + text);
            ReferenceIterator references = currentProgram.getReferenceManager()
                .getReferencesTo(data.getAddress());
            while (references.hasNext()) {
                Reference reference = references.next();
                Address from = reference.getFromAddress();
                Function function = getFunctionContaining(from);
                println("REFERENCE from=0x" + Long.toHexString(rva(from)) +
                    " function=" + (function == null ? "none" : "0x" +
                        Long.toHexString(rva(function.getEntryPoint()))) +
                    " name=" + (function == null ? "none" :
                        function.getName()));
            }
        }
        println("STRING_MATCHES " + matches);
    }
}
