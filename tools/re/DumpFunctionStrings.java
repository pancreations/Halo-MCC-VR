// Print strings referenced by one function body.
// @category HaloMCCVR.RE

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressIterator;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;

public class DumpFunctionStrings extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (currentProgram == null || args.length != 1)
            throw new IllegalArgumentException(
                "usage: DumpFunctionStrings.java <function-address>");
        Address address = currentProgram.getAddressFactory().getAddress(args[0]);
        Function function = address == null ? null : getFunctionContaining(address);
        if (function == null) throw new IllegalArgumentException("no function");
        int count = 0;
        AddressIterator addresses = function.getBody().getAddresses(true);
        while (addresses.hasNext()) {
            Address from = addresses.next();
            for (Reference reference : getReferencesFrom(from)) {
                Data data = getDataAt(reference.getToAddress());
                if (data == null || !data.hasStringValue()) continue;
                println("STRING from=" + from + " target=" +
                    reference.getToAddress() + " text=" +
                    data.getDefaultValueRepresentation());
                count++;
            }
        }
        println("STRING_MATCHES " + count);
    }
}
