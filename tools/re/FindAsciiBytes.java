// Find raw ASCII bytes even when auto-analysis did not define a string.
// @category HaloMCCVR.RE

import java.nio.charset.StandardCharsets;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSetView;
import ghidra.program.model.mem.Memory;

public class FindAsciiBytes extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (currentProgram == null || args.length == 0)
            throw new IllegalArgumentException(
                "usage: FindAsciiBytes.java <ASCII text> [...]");
        Memory memory = currentProgram.getMemory();
        AddressSetView initialized = memory.getAllInitializedAddressSet();
        for (String text : args) {
            byte[] needle = text.getBytes(StandardCharsets.US_ASCII);
            Address cursor = initialized.getMinAddress();
            int matches = 0;
            while (cursor != null) {
                Address found = memory.findBytes(
                    cursor, initialized.getMaxAddress(), needle, null, true,
                    monitor);
                if (found == null) break;
                println("ASCII text=" + text + " address=" + found +
                    " rva=0x" + Long.toHexString(
                        found.subtract(currentProgram.getImageBase())));
                matches++;
                cursor = found.add(1);
            }
            println("ASCII_MATCHES text=" + text + " count=" + matches);
        }
    }
}
