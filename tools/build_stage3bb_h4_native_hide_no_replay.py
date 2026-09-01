"""Stage 3BB: restore the proven H4 native hider with replay hard-disabled."""

from pathlib import Path
import hashlib
import struct
import sys

EXPECTED_BA = "03c5ff6e384428a1757008a1d2ae3b30c539267096b1f8a8619e955edefb48d8"
EXPECTED_AX = "0d32751585670c28bb7b98110a35b04817ec4f683fcc3ab3301c0941a4613053"


def pe_sections(blob):
    pe = struct.unpack_from("<I", blob, 0x3C)[0]
    count = struct.unpack_from("<H", blob, pe + 6)[0]
    optional_size = struct.unpack_from("<H", blob, pe + 20)[0]
    table = pe + 24 + optional_size
    result = []
    for index in range(count):
        h = table + index * 40
        vs, va, rs, rp = struct.unpack_from("<IIII", blob, h + 8)
        result.append((vs, va, rs, rp))
    return result


def offset(sections, rva):
    return next(rp + rva - va for vs, va, rs, rp in sections
                if va <= rva < va + max(vs, rs))


def main():
    if len(sys.argv) != 4:
        raise SystemExit("usage: build_stage3bb_h4_native_hide_no_replay.py <Stage3BA.dll> <Stage3AX.dll> <output.dll>")
    ba_path, ax_path, output = map(Path, sys.argv[1:])
    ba = bytearray(ba_path.read_bytes())
    ax = ax_path.read_bytes()
    if hashlib.sha256(ba).hexdigest() != EXPECTED_BA:
        raise SystemExit("wrong Stage3BA input")
    if hashlib.sha256(ax).hexdigest() != EXPECTED_AX:
        raise SystemExit("wrong Stage3AX reference")
    ba_sections = pe_sections(ba)
    ax_sections = pe_sections(ax)

    # Remove every AY/AZ replay/differential modification by restoring the
    # exact guarded AX bytes. The appended payload remains dormant evidence.
    restore = (
        (0x53636, 15), (0x536A9, 8), (0x538E0, 5), (0x538EE, 8),
        (0x53A70, 9), (0x12121, 22), (0x12182, 16), (0x2F370, 1),
    )
    for rva, size in restore:
        dst = offset(ba_sections, rva)
        src = offset(ax_sections, rva)
        ba[dst:dst + size] = ax[src:src + size]

    # Restore both optional hook install calls disabled by the safety build.
    calls = (
        (0x599C2, bytes.fromhex("e8 49 01 00 00")),
        (0x843A8, bytes.fromhex("e8 63 57 fd ff")),
    )
    for rva, code in calls:
        dst = offset(ba_sections, rva)
        if ba[dst:dst + 5] != b"\x90" * 5:
            raise SystemExit(f"install site 0x{rva:X} is not Stage3BA-disabled")
        ba[dst:dst + 5] = code

    # The capture predicate's successful branch used to enter the replay.
    # Force it to the existing normal-pass label. The dispatcher then changes
    # only the proven reticle transform; every CUI command executes once.
    branch = offset(ba_sections, 0x53634)
    if ba[branch:branch + 2] != b"\x74\x56":
        raise SystemExit("unexpected replay branch")
    ba[branch] = 0xEB

    output.write_bytes(ba)
    print("output", hashlib.sha256(ba).hexdigest(), len(ba))


if __name__ == "__main__":
    main()
