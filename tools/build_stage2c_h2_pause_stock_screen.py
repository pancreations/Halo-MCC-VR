"""Reproduce the Stage 2C H2 pause stock-screen binary candidate.

Input must be the exact Stage 2B test DLL. This is a post-link equivalent of
C-H2-72 in src/common/halo2_render_logic.h + src/dll/vr.cpp: when the active
Halo 2 presentation has actually switched to head-locked pause mode, route the
frame through the existing shared stock-screen quad instead of applying H2's
claimed-frame Drop rule.
"""
from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import struct

EXPECTED_STAGE2B_SHA256 = (
    "0898e5ad980651ba93ab97e7ff7541da4c7dec8d353280f5697372a7575efbb2"
)
IMAGE_BASE = 0x180000000
PATCH_RVA = 0x00025667
RETURN_RVA = PATCH_RVA + 5
H2AA_RVA = 0x002EA000
CAVE_RVA = H2AA_RVA + 0x90
EXPECTED_PATCH = bytes.fromhex("0F B6 54 24 77")  # movzx edx,[rsp+77h]
EXPECTED_H2AA_VSIZE = 0x82
NEW_H2AA_VSIZE = 0xA8


def parse_pe(b: bytearray):
    pe = struct.unpack_from('<I', b, 0x3C)[0]
    assert b[pe:pe+4] == b'PE\0\0'
    coff = pe + 4
    nsec = struct.unpack_from('<H', b, coff + 2)[0]
    optsz = struct.unpack_from('<H', b, coff + 16)[0]
    opt = coff + 20
    image_base = struct.unpack_from('<Q', b, opt + 24)[0]
    assert image_base == IMAGE_BASE
    sect = opt + optsz
    sections = []
    for i in range(nsec):
        h = sect + i * 40
        name = bytes(b[h:h+8]).rstrip(b'\0').decode('ascii')
        vsize, rva, rawsize, raw = struct.unpack_from('<IIII', b, h + 8)
        sections.append((name, vsize, rva, rawsize, raw, h))
    return sections


def rva_to_raw(rva: int, sections):
    for name, vsize, base, rawsize, raw, _ in sections:
        if base <= rva < base + max(vsize, rawsize):
            return raw + (rva - base)
    raise AssertionError(f'RVA not mapped: 0x{rva:X}')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('input_dll', type=Path)
    ap.add_argument('output_dll', type=Path)
    args = ap.parse_args()

    b = bytearray(args.input_dll.read_bytes())
    actual_hash = hashlib.sha256(b).hexdigest()
    assert actual_hash == EXPECTED_STAGE2B_SHA256, (
        'Stage 2C must be applied to the exact Stage 2B DLL; got ' + actual_hash
    )
    sections = parse_pe(b)
    h2aa = next(s for s in sections if s[0] == '.h2aa')
    _, vsize, _, rawsize, _, h2aa_hdr = h2aa
    assert vsize == EXPECTED_H2AA_VSIZE, hex(vsize)
    assert rawsize >= 0x200

    patch_raw = rva_to_raw(PATCH_RVA, sections)
    cave_raw = rva_to_raw(CAVE_RVA, sections)
    assert bytes(b[patch_raw:patch_raw+5]) == EXPECTED_PATCH
    assert bytes(b[cave_raw:cave_raw+0x18]) == b'\xCC' * 0x18

    # Replace the existing load of exactLivePair with a jump into unused RX
    # padding already owned by Stage 2A's .h2aa section. The trampoline checks
    # both `halo2Title` (r8b) and the displayed g_pausePresentation local
    # ([rsp+72h]). Only that H2+pause intersection forces decision DIL to
    # SharedDefault (0). Then it replays the displaced instruction and returns.
    rel = CAVE_RVA - (PATCH_RVA + 5)
    b[patch_raw:patch_raw+5] = b'\xE9' + struct.pack('<i', rel)

    code = bytearray()
    code += bytes.fromhex('45 84 C0')          # test r8b,r8b (H2 frame?)
    code += bytes.fromhex('74 09')             # je normal
    code += bytes.fromhex('80 7C 24 72 00')    # cmp byte ptr [rsp+72h],0
    code += bytes.fromhex('74 02')             # je normal
    code += bytes.fromhex('31 FF')             # xor edi,edi = SharedDefault
    code += EXPECTED_PATCH                     # normal: displaced movzx edx...
    jump_from = CAVE_RVA + len(code)
    back_rel = RETURN_RVA - (jump_from + 5)
    code += b'\xE9' + struct.pack('<i', back_rel)
    assert len(code) == 0x18
    b[cave_raw:cave_raw+len(code)] = code

    # The new code remains within the existing .h2aa 4 KiB virtual page and
    # 0x200 raw allocation. Only VirtualSize needs to name the now-live bytes.
    struct.pack_into('<I', b, h2aa_hdr + 8, NEW_H2AA_VSIZE)

    args.output_dll.write_bytes(b)
    print('input SHA256 ', actual_hash)
    print('output SHA256', hashlib.sha256(b).hexdigest())
    print('patch RVA     ', hex(PATCH_RVA))
    print('cave RVA      ', hex(CAVE_RVA), 'bytes', len(code))


if __name__ == '__main__':
    main()
