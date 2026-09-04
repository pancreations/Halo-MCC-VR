// Print a bounded pointer-sized memory window around a table entry.
// @category HaloMCCVR.RE

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;

public class DumpMemoryPointers extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (currentProgram == null || args.length != 3) {
            throw new IllegalArgumentException(
                "usage: DumpMemoryPointers.java <address> <before> <after>");
        }
        Address center = currentProgram.getAddressFactory().getAddress(args[0]);
        int before = Integer.parseInt(args[1]);
        int after = Integer.parseInt(args[2]);
        int width = currentProgram.getDefaultPointerSize();
        if (center == null || before < 0 || after < 0 || before + after > 256)
            throw new IllegalArgumentException("invalid arguments");

        for (int index = -before; index <= after; index++) {
            Address at = center.add((long)index * width);
            long value = width == 8 ? getLong(at) : Integer.toUnsignedLong(getInt(at));
            Address target = currentProgram.getAddressFactory()
                .getDefaultAddressSpace().getAddress(value);
            String targetText = target == null ? "" : target.toString();
            println(String.format("%s [%+d] = 0x%0" + (width * 2) + "X %s",
                at, index, value, targetText));
        }
    }
}
