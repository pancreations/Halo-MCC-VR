// Print a bounded instruction/byte window for signature construction.
// @category HaloMCCVR.RE

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Instruction;

public class DumpInstructionBytes extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (currentProgram == null || args.length != 2) {
            throw new IllegalArgumentException(
                "usage: DumpInstructionBytes.java <address> <instruction-count>");
        }
        Address address = currentProgram.getAddressFactory().getAddress(args[0]);
        int count = Integer.parseInt(args[1]);
        if (address == null || count < 1 || count > 256) {
            throw new IllegalArgumentException("invalid address or count");
        }

        Instruction instruction = getInstructionAt(address);
        if (instruction == null) {
            throw new IllegalArgumentException("no instruction at " + args[0]);
        }
        for (int index = 0; instruction != null && index < count; index++) {
            byte[] bytes = instruction.getBytes();
            StringBuilder encoded = new StringBuilder();
            for (byte value : bytes) {
                if (encoded.length() != 0) encoded.append(' ');
                encoded.append(String.format("%02X", value & 0xff));
            }
            println(instruction.getAddress() + "  " + encoded + "  " +
                instruction.toString());
            instruction = instruction.getNext();
        }
    }
}
