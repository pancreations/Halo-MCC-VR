// Find raw little-endian pointer values even when Ghidra did not type a table.
// @category HaloMCCVR.RE

import java.nio.ByteBuffer;
import java.nio.ByteOrder;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSetView;
import ghidra.program.model.listing.Function;
import ghidra.program.model.mem.Memory;

public class FindPointerReferences extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (currentProgram == null || args.length != 1) {
            throw new IllegalArgumentException(
                "usage: FindPointerReferences.java <target-address>");
        }
        Address target = currentProgram.getAddressFactory().getAddress(args[0]);
        if (target == null) throw new IllegalArgumentException("bad address");

        int pointerSize = currentProgram.getDefaultPointerSize();
        ByteBuffer encoded = ByteBuffer.allocate(pointerSize)
            .order(currentProgram.getLanguage().isBigEndian()
                ? ByteOrder.BIG_ENDIAN : ByteOrder.LITTLE_ENDIAN);
        if (pointerSize == 8) encoded.putLong(target.getOffset());
        else encoded.putInt((int)target.getOffset());

        Memory memory = currentProgram.getMemory();
        AddressSetView initialized = memory.getAllInitializedAddressSet();
        Address cursor = initialized.getMinAddress();
        int matches = 0;
        while (cursor != null) {
            Address found = memory.findBytes(
                cursor, initialized.getMaxAddress(), encoded.array(), null, true, monitor);
            if (found == null) break;
            Function function = getFunctionContaining(found);
            println("POINTER from=" + found + " rva=0x" +
                Long.toHexString(found.subtract(currentProgram.getImageBase())) +
                " function=" + (function == null ? "none" : function.getEntryPoint()) +
                " name=" + (function == null ? "none" : function.getName()));
            matches++;
            cursor = found.add(1);
        }
        println("POINTER_MATCHES " + matches);
    }
}
