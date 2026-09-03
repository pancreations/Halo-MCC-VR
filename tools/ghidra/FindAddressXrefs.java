// Report direct references to one or more RVAs, including the owning function.
// Usage: -postScript FindAddressXrefs.java <hex-rva> [...]
// @category HaloMCCVR.RE

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;

public class FindAddressXrefs extends GhidraScript {
    private long rva(Address address) {
        return address.subtract(currentProgram.getImageBase());
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (currentProgram == null || args.length == 0) {
            throw new IllegalArgumentException(
                "usage: FindAddressXrefs.java <hex-rva> [...]");
        }
        for (String value : args) {
            long requested = Long.parseUnsignedLong(value, 16);
            Address target = currentProgram.getImageBase().add(requested);
            println("TARGET rva=0x" + Long.toHexString(requested) +
                " address=" + target);
            ReferenceIterator references = currentProgram
                .getReferenceManager().getReferencesTo(target);
            int count = 0;
            while (references.hasNext()) {
                Reference reference = references.next();
                Address from = reference.getFromAddress();
                Function function = getFunctionContaining(from);
                println("REFERENCE from=0x" + Long.toHexString(rva(from)) +
                    " type=" + reference.getReferenceType() +
                    " function=" + (function == null ? "none" : "0x" +
                        Long.toHexString(rva(function.getEntryPoint()))) +
                    " name=" + (function == null ? "none" :
                        function.getName()));
                count++;
            }
            println("XREFS " + count);
        }
    }
}
