#!/usr/bin/env python3
"""Find x64 E8 rel32 instructions targeting selected PE RVAs.

This is a fast read-only candidate finder. Executable sections can contain
embedded data, so every reported site still requires disassembly/proof.
"""

from __future__ import annotations

import struct
import sys
from pathlib import Path


def load_sections(data: bytes) -> list[tuple[int, bytes]]:
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe : pe + 4] != b"PE\0\0":
        raise ValueError("not a PE image")
    count = struct.unpack_from("<H", data, pe + 6)[0]
    optional_size = struct.unpack_from("<H", data, pe + 20)[0]
    cursor = pe + 24 + optional_size
    sections: list[tuple[int, bytes]] = []
    for _ in range(count):
        virtual_size, rva, raw_size, raw_offset = struct.unpack_from(
            "<IIII", data, cursor + 8
        )
        characteristics = struct.unpack_from("<I", data, cursor + 36)[0]
        if characteristics & 0x20000000:
            usable = min(raw_size, max(virtual_size, raw_size))
            sections.append((rva, data[raw_offset : raw_offset + usable]))
        cursor += 40
    return sections


def main() -> int:
    if len(sys.argv) < 3:
        print(f"usage: {Path(sys.argv[0]).name} <pe> <hex-rva> [...]", file=sys.stderr)
        return 2
    data = Path(sys.argv[1]).read_bytes()
    targets = {int(value, 16) for value in sys.argv[2:]}
    hits = 0
    for section_rva, section in load_sections(data):
        for offset in range(0, len(section) - 4):
            if section[offset] != 0xE8:
                continue
            displacement = struct.unpack_from("<i", section, offset + 1)[0]
            call_rva = section_rva + offset
            target = call_rva + 5 + displacement
            if target in targets:
                print(f"0x{call_rva:08X} -> 0x{target:08X}")
                hits += 1
    print(f"total hits: {hits}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
