from pathlib import Path
import hashlib
import struct

ROOT = Path(__file__).parent.parent
BA = (ROOT / "built" / "Stage3BA-HaloMCCVR.dll").read_bytes()
AX = (ROOT / "built" / "Stage3AX-HaloMCCVR.dll").read_bytes()
OUT = (ROOT / "built" / "Stage3BB-HaloMCCVR.dll").read_bytes()
EXPECTED = "10e39cf66862f4e88eba245fc22da750c0817c4684a1af114c466703722a8192"


def sections(blob):
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


def read(blob, image_sections, rva, size):
    start = next(rp + rva - va for vs, va, rs, rp in image_sections
                 if va <= rva < va + max(vs, rs))
    return blob[start:start + size]


assert hashlib.sha256(OUT).hexdigest() == EXPECTED
ba_s, ax_s, out_s = sections(BA), sections(AX), sections(OUT)
for rva, size in ((0x53636, 15), (0x536A9, 8), (0x538E0, 5),
                  (0x538EE, 8), (0x53A70, 9), (0x12121, 22),
                  (0x12182, 16), (0x2F370, 1)):
    assert read(OUT, out_s, rva, size) == read(AX, ax_s, rva, size)
assert read(OUT, out_s, 0x599C2, 5) == bytes.fromhex("e8 49 01 00 00")
assert read(OUT, out_s, 0x843A8, 5) == bytes.fromhex("e8 63 57 fd ff")
assert read(OUT, out_s, 0x53634, 2) == bytes.fromhex("eb 56")

# Pause and ODST isolation are inherited unchanged from the safe BA build.
for rva, size in ((0x2FA890, 0x101), (0x5C368, 5), (0x67CE5, 1)):
    assert read(OUT, out_s, rva, size) == read(BA, ba_s, rva, size)

print("PASS Stage3BB native H4 reticle hide; replay impossible; pause/ODST preserved")
