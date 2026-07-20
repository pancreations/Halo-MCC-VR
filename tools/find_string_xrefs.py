"""Find x64 RIP-relative code references to ASCII strings in a PE image.

Usage: py -3 tools/find_string_xrefs.py <image> <substring>
Read-only helper for local reverse engineering; it never modifies the image.
"""

import struct
import sys

from capstone import CS_ARCH_X86, CS_MODE_64, Cs
from capstone.x86_const import X86_OP_MEM, X86_REG_RIP


def read_sections(data):
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    section_count = struct.unpack_from("<H", data, pe + 6)[0]
    optional_size = struct.unpack_from("<H", data, pe + 20)[0]
    optional = pe + 24
    image_base = struct.unpack_from("<Q", data, optional + 24)[0]
    table = optional + optional_size
    sections = []
    for index in range(section_count):
        off = table + index * 40
        name = data[off:off + 8].rstrip(bytes([0])).decode(errors="replace")
        virtual_size, rva, raw_size, raw = struct.unpack_from("<IIII", data, off + 8)
        sections.append((name, rva, virtual_size, raw, raw_size))
    return image_base, sections


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: find_string_xrefs.py <image> <substring>")
    path, query = sys.argv[1], sys.argv[2].encode("ascii")
    with open(path, "rb") as stream:
        data = stream.read()
    image_base, sections = read_sections(data)
    targets = []
    for name, rva, _, raw, raw_size in sections:
        block = data[raw:raw + raw_size]
        start = 0
        while True:
            found = block.find(query, start)
            if found < 0:
                break
            target_rva = rva + found
            targets.append((image_base + target_rva, target_rva, name))
            start = found + 1
    if not targets:
        raise SystemExit("string not found")
    for _, rva, name in targets:
        print(f"string {query!r} at {name} RVA 0x{rva:X}")

    # Halo's UI/console strings commonly live behind pointer tables. Include
    # absolute-VA and RVA table entries as secondary anchors so code that
    # references the table (rather than the bytes directly) is still found.
    anchors = list(targets)
    for target_va, target_rva, _ in targets:
        needles = ((struct.pack("<Q", target_va), "VA pointer"),
                   (struct.pack("<I", target_rva), "RVA pointer"))
        for name, rva, _, raw, raw_size in sections:
            block = data[raw:raw + raw_size]
            for needle, kind in needles:
                start = 0
                while True:
                    found = block.find(needle, start)
                    if found < 0:
                        break
                    anchor_rva = rva + found
                    anchors.append((image_base + anchor_rva, anchor_rva, name))
                    print(f"{kind} at {name} RVA 0x{anchor_rva:X} "
                          f"-> string RVA 0x{target_rva:X}")
                    start = found + 1

    text = next(section for section in sections if section[0] == ".text")
    _, text_rva, _, text_raw, text_size = text
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True
    for insn in md.disasm(data[text_raw:text_raw + text_size], image_base + text_rva):
        for operand in insn.operands:
            if operand.type != X86_OP_MEM or operand.mem.base != X86_REG_RIP:
                continue
            referenced = insn.address + insn.size + operand.mem.disp
            for target_va, target_rva, _ in anchors:
                width = len(query) if (target_va, target_rva, _) in targets else 8
                if target_va <= referenced < target_va + width:
                    code_rva = insn.address - image_base
                    print(f"xref RVA 0x{code_rva:X}: {insn.mnemonic} {insn.op_str} "
                          f"-> string RVA 0x{target_rva:X}")


if __name__ == "__main__":
    main()
