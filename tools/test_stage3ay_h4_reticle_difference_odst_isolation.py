from pathlib import Path
import hashlib
import struct

ROOT = Path(__file__).parent.parent
BASE = ROOT / "built" / "Stage3AX-HaloMCCVR.dll"
OUT = ROOT / "built" / "Stage3AY-HaloMCCVR.dll"
EXPECTED_BASE = "0d32751585670c28bb7b98110a35b04817ec4f683fcc3ab3301c0941a4613053"
EXPECTED_OUT = "44850c02a97e284b35f00585479d9b0391bc190c758f15efd88bce733c2e50ad"


def pe(blob):
    p = struct.unpack_from("<I", blob, 0x3C)[0]
    count = struct.unpack_from("<H", blob, p + 6)[0]
    optional_size = struct.unpack_from("<H", blob, p + 20)[0]
    optional = p + 24
    table = optional + optional_size
    sections = []
    for i in range(count):
        h = table + i * 40
        name = blob[h:h + 8].split(b"\0", 1)[0].decode("ascii")
        vs, va, rs, rp = struct.unpack_from("<IIII", blob, h + 8)
        sections.append((name, vs, va, rs, rp, h))
    return optional, sections


def off(sections, rva):
    return next(rp + rva - va for _, vs, va, rs, rp, _ in sections
                if va <= rva < va + max(vs, rs))


base = BASE.read_bytes()
out = OUT.read_bytes()
assert hashlib.sha256(base).hexdigest() == EXPECTED_BASE
assert hashlib.sha256(out).hexdigest() == EXPECTED_OUT
base_opt, base_sections = pe(base)
out_opt, out_sections = pe(out)
assert len(base_sections) == len(out_sections) == 12
base_qd = next(s for s in base_sections if s[0] == ".s3qd")
out_qd = next(s for s in out_sections if s[0] == ".s3qd")
assert base_qd[1:5] == (0x8000, 0x2F3000, 0x8000, base_qd[4])
assert out_qd[1:5] == (0x9000, 0x2F3000, 0x9000, base_qd[4])
assert len(out) == len(base) + 0x1000
assert struct.unpack_from("<I", out, out_opt + 0x38)[0] == 0x2FC000

def read(rva, size):
    o = off(out_sections, rva)
    return out[o:o + size]


# Accepted pause implementation and the accepted ODST unpin bytes survive.
assert read(0x2FA890, 0x101) == base[off(base_sections, 0x2FA890):off(base_sections, 0x2FA890) + 0x101]
for file_offset in (0x2BFF0C, 0x2C0589, 0x2C5C16, 0x2C5C6B):
    assert out[file_offset] == base[file_offset]

# Rejected AX spatial zoom and forced ODST edge are disabled.
assert read(0x539C0, 5) == bytes.fromhex("e8 e5 6d 2a 00")
assert read(0x67CE5, 1) == b"\x75"
# Suppression texture can be sampled by the differential shader.
assert read(0x2F370, 1) == b"\x28"

# Every splice is a direct rel32 call into the appended, bounded payload.
splices = (0x53636, 0x536A9, 0x538E0, 0x53A70, 0x12121, 0x12182, 0x5C368)
for rva in splices:
    code = read(rva, 5)
    assert code[0] == 0xE8
    target = rva + 5 + struct.unpack_from("<i", code, 1)[0]
    assert 0x2FB000 <= target < 0x2FB758

# The payload is exact-hash pinned; its final 1000 bytes are the offline-
# compiled shader, not executable instructions.
payload = read(0x2FB000, 2040)
assert hashlib.sha256(payload).hexdigest() == \
    "da1d3247366f40a78c12eec8d1df0a6a098756c7e1621e13c5341ad3cfd5ccb1"
shader_offset = payload.index(b"DXBC")
assert len(payload) - shader_offset == 1000

# Only guarded sites, PE size fields, the final-section size fields, and the
# appended page differ from AX.
allowed_rvas = [
    (0x53636, 15), (0x536A9, 8), (0x538E0, 5), (0x538EE, 8),
    (0x539C0, 10), (0x53A70, 9), (0x12121, 22), (0x12182, 16),
    (0x5C368, 5),
    (0x67CE5, 1), (0x2F370, 1),
]
allowed_offsets = set()
for rva, size in allowed_rvas:
    start = off(base_sections, rva)
    allowed_offsets.update(range(start, start + size))
allowed_offsets.update(range(base_opt + 4, base_opt + 8))
allowed_offsets.update(range(base_opt + 0x38, base_opt + 0x3C))
allowed_offsets.update(range(base_qd[5] + 8, base_qd[5] + 12))
allowed_offsets.update(range(base_qd[5] + 16, base_qd[5] + 20))
changed = {i for i in range(len(base)) if base[i] != out[i]}
assert changed <= allowed_offsets, sorted(changed - allowed_offsets)[:20]

print("PASS Stage3AY H4 reticle differential + ODST optional-scan isolation")
