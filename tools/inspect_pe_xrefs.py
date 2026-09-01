"""Print x64 RIP-relative references to an ASCII string in a PE image."""

from pathlib import Path
import struct
import sys

from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86_const import X86_OP_MEM, X86_REG_RIP


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: inspect_pe_xrefs.py <image> <ascii-text>")
    blob = Path(sys.argv[1]).read_bytes()
    needle = sys.argv[2].encode("ascii")
    pe_offset = struct.unpack_from("<I", blob, 0x3C)[0]
    section_count = struct.unpack_from("<H", blob, pe_offset + 6)[0]
    optional_size = struct.unpack_from("<H", blob, pe_offset + 20)[0]
    section_table = pe_offset + 24 + optional_size
    sections = []
    for index in range(section_count):
        offset = section_table + index * 40
        name = blob[offset:offset + 8].split(b"\0", 1)[0].decode("ascii")
        virtual_size, virtual_address, raw_size, raw_pointer = struct.unpack_from(
            "<IIII", blob, offset + 8)
        sections.append((name, virtual_size, virtual_address, raw_size, raw_pointer))

    targets = []
    start = 0
    while True:
        found = blob.find(needle, start)
        if found < 0:
            break
        for name, virtual_size, virtual_address, raw_size, raw_pointer in sections:
            if raw_pointer <= found < raw_pointer + raw_size:
                targets.append((virtual_address + found - raw_pointer, name))
                break
        start = found + 1
    print("targets", ", ".join(f"0x{rva:X} ({name})" for rva, name in targets))

    text = next(section for section in sections if section[0] == ".text")
    _, _, virtual_address, raw_size, raw_pointer = text
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True
    md.skipdata = True
    for instruction in md.disasm(blob[raw_pointer:raw_pointer + raw_size], virtual_address):
        if instruction.id == 0:
            continue
        for operand in instruction.operands:
            if operand.type == X86_OP_MEM and operand.mem.base == X86_REG_RIP:
                target = instruction.address + instruction.size + operand.mem.disp
                if any(rva <= target < rva + len(needle) for rva, _ in targets):
                    print(f"0x{instruction.address:X}: {instruction.mnemonic} {instruction.op_str}")


if __name__ == "__main__":
    main()
