from pathlib import Path
import hashlib
import struct

ROOT = Path(__file__).parent.parent
BASE = (ROOT / "built" / "Stage3AZ-HaloMCCVR.dll").read_bytes()
OUT = (ROOT / "built" / "Stage3BA-HaloMCCVR.dll").read_bytes()
EXPECTED_BASE = "76727356f3ce4b053b09eccde6783a82ea5249d7ebfb9df9bc949b8822affb10"
EXPECTED_OUT = "03c5ff6e384428a1757008a1d2ae3b30c539267096b1f8a8619e955edefb48d8"


def sections(blob):
    pe = struct.unpack_from("<I", blob, 0x3C)[0]
    count = struct.unpack_from("<H", blob, pe + 6)[0]
    optional_size = struct.unpack_from("<H", blob, pe + 20)[0]
    table = pe + 24 + optional_size
    out = []
    for index in range(count):
        header = table + index * 40
        vs, va, rs, rp = struct.unpack_from("<IIII", blob, header + 8)
        out.append((vs, va, rs, rp))
    return out


def offset(image_sections, rva):
    return next(rp + rva - va for vs, va, rs, rp in image_sections
                if va <= rva < va + max(vs, rs))


assert hashlib.sha256(BASE).hexdigest() == EXPECTED_BASE
assert hashlib.sha256(OUT).hexdigest() == EXPECTED_OUT
assert len(BASE) == len(OUT)
image_sections = sections(OUT)
disabled = set()
for rva in (0x599C2, 0x843A8):
    start = offset(image_sections, rva)
    assert OUT[start:start + 5] == b"\x90" * 5
    disabled.update(range(start, start + 5))
changed = {index for index, (before, after) in enumerate(zip(BASE, OUT))
           if before != after}
assert changed == disabled

# The accepted pause payload and Stage3AZ's ODST isolation remain exact.
base_sections = sections(BASE)
for rva, size in ((0x2FA890, 0x101), (0x5C368, 5), (0x67CE5, 1)):
    before = offset(base_sections, rva)
    after = offset(image_sections, rva)
    assert OUT[after:after + size] == BASE[before:before + size]

print("PASS Stage3BA Halo 4 reticle feature fail-open; pause/ODST preserved")
