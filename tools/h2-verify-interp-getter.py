"""E-H2-71 step 3: verify the retail homolog of the kit's interpolated
first-person frame getter.

Kit order inside first_person_weapons.cpp's builder (FUN_00706e04):
    build_frame   kit 0x4B9C80
    interp_frame  kit 0x4D7A90   <- only when publish_to_renderer == 0
    inverse       kit 0x4B9DA0
    compose       kit 0x4BA290

Retail builder +0x8181F0 calls, in the same order:
    0x729BA0 @ 0x818418, 0x7226F0 @ 0x81843F, 0x729C90 @ 0x818458,
    0x72A150 @ 0x818469

This prints the retail instructions around that call so the guard can be
read, plus the head of the candidate getter so its shape can be checked
against the kit's (per-user banks, a validity test, a blend, bool return).
Read-only; the game is never launched.
"""
from pathlib import Path
import struct
import sys

from capstone import Cs, CS_ARCH_X86, CS_MODE_64

DLL = Path(r"N:\SteamLibrary\steamapps\common\Halo The Master Chief Collection"
           r"\halo2\halo2.dll")
GUARD_WINDOW = (0x8183F0, 0x818480)
CANDIDATE_GETTER = 0x7226F0
SIBLING_READ = 0x722850          # pinned by E-H2-32


def parse_pe(blob):
    pe = struct.unpack_from("<I", blob, 0x3C)[0]
    coff = pe + 4
    sections = struct.unpack_from("<H", blob, coff + 2)[0]
    optional = struct.unpack_from("<H", blob, coff + 16)[0]
    table = coff + 20 + optional
    out = []
    for index in range(sections):
        offset = table + index * 40
        virtual_size, virtual_address, raw_size, raw_pointer = \
            struct.unpack_from("<IIII", blob, offset + 8)
        out.append((virtual_address, virtual_size, raw_size, raw_pointer))
    return out


def offset_of(sections, rva):
    for virtual_address, virtual_size, raw_size, raw_pointer in sections:
        if virtual_address <= rva < virtual_address + max(virtual_size, raw_size):
            return raw_pointer + rva - virtual_address
    raise KeyError(hex(rva))


def show(blob, sections, md, start, end, title):
    print(f"\n=== {title}: 0x{start:X}..0x{end:X} ===")
    data = blob[offset_of(sections, start):offset_of(sections, end)]
    for insn in md.disasm(data, start):
        print(f"  0x{insn.address:X}  {insn.mnemonic:<8} {insn.op_str}")


def main():
    blob = DLL.read_bytes()
    sections = parse_pe(blob)
    md = Cs(CS_ARCH_X86, CS_MODE_64)

    show(blob, sections, md, GUARD_WINDOW[0], GUARD_WINDOW[1],
         "retail builder: the guard around the interpolated-frame call")
    show(blob, sections, md, CANDIDATE_GETTER, CANDIDATE_GETTER + 0xC0,
         f"candidate getter 0x{CANDIDATE_GETTER:X}")
    show(blob, sections, md, SIBLING_READ, SIBLING_READ + 0x60,
         f"pinned sibling read 0x{SIBLING_READ:X} (shape reference)")

    entry = blob[offset_of(sections, CANDIDATE_GETTER):
                 offset_of(sections, CANDIDATE_GETTER) + 24]
    print("\ncandidate getter entry bytes: "
          + " ".join(f"{b:02X}" for b in entry))
    count = blob.count(entry)
    print(f"that 24-byte entry appears {count} time(s) in the module "
          f"({'UNIQUE - usable as a signature' if count == 1 else 'NOT unique'})")


if __name__ == "__main__":
    sys.exit(main())
