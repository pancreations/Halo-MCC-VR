"""E-H2-71 step 2: MATCH the kit's interpolated-first-person-frame getter to
the pinned retail module, and verify it structurally.

The kit (H2EK halo2_tag_test.exe, first_person_weapons.cpp FUN_00706e04)
explained the behaviour:

    build_frame(camera, position, forward, up)          # kit 0x4B9C80
    if (user_data.flags & 4) {
        chosen = user_data + 0x20C8                     # current tick frame
        if (publish_to_renderer == 0)                   # CLASSIC only
            if (interpolated_frame(user, 1, tmp))       # kit 0x4D7A90
                chosen = tmp
        inverse(chosen, inv)                            # kit 0x4B9DA0
        compose(camera)                                 # kit 0x4BA290
    }

This script does NOT discover behaviour from retail. It disassembles the
already-pinned retail builder (E-H2-45: +0x8181F0) and reports the call
sites and the branch that guards them, so the retail homolog of the kit
getter can be identified by that structure and then verified by its own
body. Read-only; the game is never launched.
"""
from pathlib import Path
import struct
import sys

from capstone import Cs, CS_ARCH_X86, CS_MODE_64

DLL = Path(r"N:\SteamLibrary\steamapps\common\Halo The Master Chief Collection"
           r"\halo2\halo2.dll")
BUILDER_RVA = 0x8181F0          # E-H2-45, already pinned and verified
INTERP_READ_RVA = 0x722850      # E-H2-32, already pinned (node matrices)
RESET_RVA = 0x723010            # E-H2-22, already pinned


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


def rva_to_offset(sections, rva):
    for virtual_address, virtual_size, raw_size, raw_pointer in sections:
        if virtual_address <= rva < virtual_address + max(virtual_size, raw_size):
            return raw_pointer + rva - virtual_address
    raise KeyError(hex(rva))


def main():
    blob = DLL.read_bytes()
    sections = parse_pe(blob)
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True

    start = rva_to_offset(sections, BUILDER_RVA)
    window = blob[start:start + 0x1200]

    print(f"retail builder +0x{BUILDER_RVA:X} (E-H2-45), first 0x1200 bytes")
    print("looking for: the conditional guarding a call, on the publish path\n")

    calls = []
    branches = []
    for insn in md.disasm(window, BUILDER_RVA):
        if insn.mnemonic == "call" and insn.op_str.startswith("0x"):
            target = int(insn.op_str, 16)
            calls.append((insn.address, target))
        if insn.mnemonic.startswith("j") and insn.mnemonic != "jmp":
            branches.append((insn.address, insn.mnemonic, insn.op_str))

    print(f"{len(calls)} direct calls in window:")
    seen = {}
    for address, target in calls:
        seen.setdefault(target, []).append(address)
    for target in sorted(seen):
        sites = ", ".join(f"0x{a:X}" for a in seen[target])
        near_reset = " <-- pinned interpolator reset" \
            if target == RESET_RVA else ""
        near_read = " <-- pinned interpolator read" \
            if target == INTERP_READ_RVA else ""
        print(f"  call 0x{target:X}  from {sites}{near_reset}{near_read}")

    # Functions in the same translation unit sit next to the pinned ones.
    print("\ncall targets within +/-0x4000 of the pinned interpolator "
          "functions (same translation unit is emitted together):")
    for target in sorted(seen):
        for name, anchor in (("read", INTERP_READ_RVA), ("reset", RESET_RVA)):
            if abs(target - anchor) <= 0x4000:
                print(f"  0x{target:X}  is {target - anchor:+#x} from the "
                      f"pinned {name} 0x{anchor:X}")

    print("\nconditional branches (first 40):")
    for address, mnemonic, operand in branches[:40]:
        print(f"  0x{address:X}  {mnemonic} {operand}")


if __name__ == "__main__":
    sys.exit(main())
