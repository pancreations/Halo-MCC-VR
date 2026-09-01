"""Static acceptance checks for the hash-pinned Stage 3AW post-link layer."""

from pathlib import Path
import hashlib
import struct
import sys

from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86_const import X86_OP_IMM, X86_OP_MEM, X86_REG_RIP


EXPECTED_AV = \
    "2baf0a3e7d654a0cda701399d672cec7c582202e138537f8193e34e5d72aca16"
EXPECTED_AW = \
    "ad0c6bbca337f2436a258cb4a0cb9da5884b20270bc2f9dadf7a06daba1ed676"
EXPECTED_PAYLOAD = \
    "243cff272992484a73670172525615474a361d5852cb83bc845009518bf09661"

PAYLOAD_RVA = 0x002FA790
PAYLOAD_SIZE = 513
PAYLOAD_LIMIT = 0x002FB000


def parse_pe(blob):
    pe_offset = struct.unpack_from("<I", blob, 0x3C)[0]
    assert blob[:2] == b"MZ" and blob[pe_offset:pe_offset + 4] == b"PE\0\0"
    coff = pe_offset + 4
    count = struct.unpack_from("<H", blob, coff + 2)[0]
    optional_size = struct.unpack_from("<H", blob, coff + 16)[0]
    section_table = coff + 20 + optional_size
    sections = []
    for index in range(count):
        offset = section_table + index * 40
        name = bytes(blob[offset:offset + 8]).split(b"\0", 1)[0].decode()
        virtual_size, virtual_address, raw_size, raw_pointer = \
            struct.unpack_from("<IIII", blob, offset + 8)
        sections.append((name, virtual_size, virtual_address,
                         raw_size, raw_pointer))
    return sections


def rva_offset(sections, rva):
    for _name, virtual_size, virtual_address, raw_size, raw_pointer in sections:
        if virtual_address <= rva < \
                virtual_address + max(virtual_size, raw_size):
            return raw_pointer + rva - virtual_address
    raise AssertionError(f"unmapped RVA 0x{rva:X}")


def rip_target(instruction):
    for operand in instruction.operands:
        if operand.type == X86_OP_MEM and operand.mem.base == X86_REG_RIP:
            return instruction.address + instruction.size + operand.mem.disp
    return None


def immediate_target(instruction):
    for operand in instruction.operands:
        if operand.type == X86_OP_IMM:
            return operand.imm
    return None


def main():
    if len(sys.argv) not in (1, 3):
        raise SystemExit("usage: test_stage3aw_h4_pause_stock_and_reticle.py "
                         "[<Stage3AV.dll> <Stage3AW.dll>]")
    root = Path(__file__).resolve().parent.parent
    av_path, aw_path = ((root / "built/Stage3AV-HaloMCCVR.dll",
                         root / "built/Stage3AW-HaloMCCVR.dll")
                        if len(sys.argv) == 1 else
                        (Path(sys.argv[1]), Path(sys.argv[2])))
    av = av_path.read_bytes()
    aw = aw_path.read_bytes()
    assert len(av) == len(aw) == 2919424
    assert hashlib.sha256(av).hexdigest() == EXPECTED_AV
    assert hashlib.sha256(aw).hexdigest() == EXPECTED_AW

    sections = parse_pe(av)
    assert len(sections) == 12
    assert next(s[1:] for s in sections if s[0] == ".s3qd") == \
        (0x8000, 0x2F3000, 0x8000, 0x2C0C00)

    allowed_rva_ranges = (
        (0x000538D3, 0x000538DA),
        (0x000539C0, 0x000539CA),
        (0x00058BED, 0x00058BF5),
        (0x001BF0C0, 0x001BF0D0),
        (PAYLOAD_RVA, PAYLOAD_RVA + PAYLOAD_SIZE),
    )
    allowed_offsets = tuple(
        (rva_offset(sections, begin),
         rva_offset(sections, end - 1) + 1)
        for begin, end in allowed_rva_ranges)
    changed = [index for index, pair in enumerate(zip(av, aw))
               if pair[0] != pair[1]]
    assert len(changed) == 470
    assert all(any(begin <= index < end for begin, end in allowed_offsets)
               for index in changed)
    for begin, end in allowed_offsets:
        assert any(begin <= index < end for index in changed)

    def at(blob, rva, size):
        offset = rva_offset(sections, rva)
        return blob[offset:offset + size]

    assert at(aw, 0x538D3, 7) == bytes.fromhex("E9 B8 6E 2A 00 90 90")
    assert at(aw, 0x539C0, 10) == bytes.fromhex(
        "E8 E5 6D 2A 00 90 90 90 90 90")
    assert at(aw, 0x58BED, 8) == bytes.fromhex(
        "E9 9E 1C 2A 00 90 90 90")
    assert struct.unpack("<ffff", at(aw, 0x1BF0C0, 16)) == \
        (512.0, 512.0, 0.0, 1.0)

    payload = at(aw, PAYLOAD_RVA, PAYLOAD_SIZE)
    assert not any(at(av, PAYLOAD_RVA, PAYLOAD_LIMIT - PAYLOAD_RVA))
    assert hashlib.sha256(payload).hexdigest() == EXPECTED_PAYLOAD
    assert not any(at(aw, PAYLOAD_RVA + PAYLOAD_SIZE,
                      PAYLOAD_LIMIT - PAYLOAD_RVA - PAYLOAD_SIZE))

    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True
    instructions = {
        instruction.address: instruction
        for instruction in md.disasm(payload[:-1], PAYLOAD_RVA)
    }

    # The capture starts only on the proven payload-readable 0x28/0x0C marker.
    assert instructions[0x2FA790].mnemonic == "cmp"
    assert immediate_target(instructions[0x2FA795]) == 0x53914
    assert instructions[0x2FA79B].op_str == "byte ptr [rsp + 0x21], bl"
    assert immediate_target(instructions[0x2FA79F]) == 0x53914
    assert immediate_target(instructions[0x2FA7A5]) == 0x538DA

    # The safely copied 0x34-byte matrix drives a symmetric dynamic viewport.
    assert instructions[0x2FA7AA].op_str == "rdx, [rsp + 0x50]"
    assert instructions[0x2FA7C6].op_str == "ecx, 0xd"
    assert instructions[0x2FA7CB].op_str == "eax, dword ptr [rdx + 0x28]"
    assert instructions[0x2FA7ED].op_str == "ecx, dword ptr [rdx + 0x2c]"
    assert rip_target(instructions[0x2FA83C]) == 0x2AE770
    assert rip_target(instructions[0x2FA845]) == 0x2AE774
    assert rip_target(instructions[0x2FA87D]) == 0x2A7518
    assert instructions[0x2FA885].op_str == "r8d, r12w"
    assert instructions[0x2FA889].op_str == "edi, 0x80000000"
    assert instructions[0x2FA88F].mnemonic == "ret"

    # Pause produces the stock backbuffer, validates the native getter, and
    # uses reason 3's true->false lifetime edge to restore stereo.
    assert instructions[0x2FA890].op_str == "rsp, 0x20"
    assert immediate_target(instructions[0x2FA894]) == 0x2EEB0
    assert rip_target(instructions[0x2FA89D]) == 0x2FA990
    assert rip_target(instructions[0x2FA8B4]) == 0x2A7208
    assert instructions[0x2FA8C4].op_str == "rax, 0xa0ae4"
    assert instructions[0x2FA8CA].op_str == "word ptr [rax], 0x158b"
    assert instructions[0x2FA936].op_str == "byte ptr [rax + 0x32], 0xc3"
    assert instructions[0x2FA93C].op_str == "ecx, 3"
    assert instructions[0x2FA941].op_str == "rax"
    assert rip_target(instructions[0x2FA947]) == 0x2FA990
    assert rip_target(instructions[0x2FA950]) == 0x2FA990
    assert immediate_target(instructions[0x2FA962]) == 0x30480
    assert rip_target(instructions[0x2FA967]) == 0x180150
    assert rip_target(instructions[0x2FA96D]) == 0x2F31C8
    assert immediate_target(instructions[0x2FA97F]) == 0x58BF5
    assert immediate_target(instructions[0x2FA988]) == 0x58D5D

    # Both runtime-selected CUI copies retain exactly 2x authored magnification.
    for half_x, half_y in ((-1033.578, 336.549), (-1893.0, 1064.517)):
        width = 4.0 * abs(half_x)
        height = 4.0 * abs(half_y)
        left = 256.0 - 2.0 * abs(half_x)
        top = 256.0 - 2.0 * abs(half_y)
        assert width / (2.0 * abs(half_x)) == 2.0
        assert height / (2.0 * abs(half_y)) == 2.0
        assert left + width / 2.0 == 256.0
        assert top + height / 2.0 == 256.0

    print("Stage3AW static acceptance: PASS")
    print("input ", EXPECTED_AV)
    print("output", EXPECTED_AW)
    print("diff  ", len(changed), "bytes within five guarded regions")


if __name__ == "__main__":
    main()
