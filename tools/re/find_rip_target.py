"""Find x64 RIP-relative references to one or more PE RVAs (read-only)."""

from array import array
import sys

from capstone import CS_ARCH_X86, CS_MODE_64, Cs
from capstone.x86_const import X86_OP_MEM, X86_REG_RIP

from pe import PE


if len(sys.argv) < 3:
    raise SystemExit("usage: find_rip_target.py <pe> <hex-rva> [...]")

image = PE(sys.argv[1])
targets = {int(value, 16) for value in sys.argv[2:]}
section, code = image.text()
decoder = Cs(CS_ARCH_X86, CS_MODE_64)
decoder.detail = True

# A RIP-relative disp32 must end at `target - next_ip`. Find every possible
# disp32 byte position with four aligned vector passes, then locally decode
# around only those candidates. This avoids decoding a multi-megabyte kit image.
candidates = set()
for alignment in range(4):
    usable = (len(code) - alignment) & ~3
    view = array("i")
    view.frombytes(code[alignment:alignment + usable])
    if sys.byteorder != "little":
        view.byteswap()
    for index, displacement in enumerate(view):
        position = alignment + index * 4
        next_ip = section["va"] + position + 4
        if next_ip + displacement in targets:
            candidates.add(position)

reported = set()
for displacement_position in sorted(candidates):
    for start in range(max(0, displacement_position - 11),
                       displacement_position + 1):
        for instruction in decoder.disasm(
                code[start:displacement_position + 4], section["va"] + start):
            if instruction.address + instruction.size != (
                    section["va"] + displacement_position + 4):
                continue
            for operand in instruction.operands:
                if (operand.type == X86_OP_MEM and
                        operand.mem.base == X86_REG_RIP):
                    target = (instruction.address + instruction.size +
                              operand.mem.disp)
                    key = (instruction.address, target)
                    if target in targets and key not in reported:
                        reported.add(key)
                        print(
                            f"0x{instruction.address:X}: "
                            f"{instruction.mnemonic} {instruction.op_str} "
                            f"-> 0x{target:X}"
                        )
