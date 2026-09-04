// Report every analyzed reference to an exact address.
// @category HaloMCCVR.RE

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;

public class FindReferencesToAddress extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (currentProgram == null || args.length != 1) {
            throw new IllegalArgumentException(
                "usage: FindReferencesToAddress.java <target-address>");
        }
        Address target = currentProgram.getAddressFactory().getAddress(args[0]);
        if (target == null) throw new IllegalArgumentException("bad address");
        ReferenceIterator references = currentProgram.getReferenceManager()
            .getReferencesTo(target);
        int count = 0;
        while (references.hasNext()) {
            Reference reference = references.next();
            Address from = reference.getFromAddress();
            Function function = getFunctionContaining(from);
            println("REFERENCE target=" + target + " from=" + from +
                " from_rva=0x" + Long.toHexString(
                    from.subtract(currentProgram.getImageBase())) +
                " function=" + (function == null ? "none" :
                    function.getEntryPoint()) + " function_rva=" +
                (function == null ? "none" : "0x" + Long.toHexString(
                    function.getEntryPoint().subtract(
                        currentProgram.getImageBase()))) + " name=" +
                (function == null ? "none" : function.getName()));
            count++;
        }
        println("REFERENCE_MATCHES " + count);
    }
}
