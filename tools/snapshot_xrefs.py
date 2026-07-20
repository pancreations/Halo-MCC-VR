"""Find code references to RVAs inside a pause_scan in-memory PE snapshot."""

import struct
import sys
from pathlib import Path
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86_const import X86_OP_IMM, X86_OP_MEM, X86_REG_RIP


def sections(image):
    pe = struct.unpack_from("<I", image, 0x3C)[0]
    if image[pe:pe + 4] != b"PE\x00\x00":
        raise ValueError("snapshot does not contain a PE image")
    count = struct.unpack_from("<H", image, pe + 6)[0]
    opt_size = struct.unpack_from("<H", image, pe + 20)[0]
    cursor = pe + 24 + opt_size
    result = []
    for _ in range(count):
        raw = image[cursor:cursor + 40]
        name = raw[:8].rstrip(b"\x00").decode(errors="replace")
        virtual_size, rva = struct.unpack_from("<II", raw, 8)
        characteristics = struct.unpack_from("<I", raw, 36)[0]
        result.append((name, rva, virtual_size, bool(characteristics & 0x20000000)))
        cursor += 40
    return result


def main():
    if len(sys.argv) >= 4 and sys.argv[1] == "fn":
        image = Path(sys.argv[2]).read_bytes()
        rva = int(sys.argv[3], 16)
        size = int(sys.argv[4], 0) if len(sys.argv) >= 5 else 512
        md = Cs(CS_ARCH_X86, CS_MODE_64)
        md.detail = True
        for insn in md.disasm(image[rva:rva + size], rva):
            print("0x%X  %-18s %-8s %s" %
                  (insn.address, insn.bytes.hex(), insn.mnemonic, insn.op_str))
        return
    if len(sys.argv) < 3:
        raise SystemExit("usage: snapshot_xrefs.py IMAGE.bin TARGET_RVA... | fn IMAGE.bin RVA [SIZE]")
    image = Path(sys.argv[1]).read_bytes()
    targets = {int(value, 16) for value in sys.argv[2:]}
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True
    hits = 0
    for name, rva, size, executable in sections(image):
        if not executable:
            continue
        code = image[rva:min(len(image), rva + size)]
        print("scanning %s rva=0x%X size=0x%X" % (name, rva, len(code)))
        pos = 0
        while pos < len(code):
            decoded = list(md.disasm(code[pos:pos + 0x10000], rva + pos))
            if not decoded:
                pos += 1
                continue
            for insn in decoded:
                resolved = []
                for operand in insn.operands:
                    if operand.type == X86_OP_MEM and operand.mem.base == X86_REG_RIP:
                        resolved.append(insn.address + insn.size + operand.mem.disp)
                    elif operand.type == X86_OP_IMM:
                        resolved.append(operand.imm)
                matched = targets.intersection(resolved)
                if matched:
                    print("  0x%X  %-18s %s %s -> %s" %
                          (insn.address, insn.bytes.hex(), insn.mnemonic, insn.op_str,
                           ",".join("0x%X" % x for x in sorted(matched))))
                    hits += 1
            end = decoded[-1].address + decoded[-1].size - rva
            pos = end if end > pos else pos + 1
    print("total references: %d" % hits)


if __name__ == "__main__":
    main()
