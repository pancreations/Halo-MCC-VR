from pathlib import Path
import hashlib
import struct

ROOT = Path(__file__).parent.parent
BB = (ROOT / "built" / "Stage3BB-HaloMCCVR.dll").read_bytes()
OUT = (ROOT / "built" / "Stage3BD-HaloMCCVR.dll").read_bytes()
EXPECTED = "2e19f93af12f8538f37bb7375a5faa8bf0210b74697a87cccc5d29c637068422"

CONST_RVA = 0x2FB800
OLD_CONST_RVA = 0x1BF0C0
H4_LOAD_RVA = 0x11C77
H4_LOAD_NEXT_RVA = 0x11C7F
RETICLE_SIZE = 512.0


def sections(blob):
    pe = struct.unpack_from("<I", blob, 0x3C)[0]
    count = struct.unpack_from("<H", blob, pe + 6)[0]
    optional_size = struct.unpack_from("<H", blob, pe + 20)[0]
    table = pe + 24 + optional_size
    return [struct.unpack_from("<IIII", blob, table + i * 40 + 8)
            for i in range(count)]


def off(secs, rva):
    return next(rp + rva - va for vs, va, rs, rp in secs
                if va <= rva < va + max(vs, rs))


def read(blob, secs, rva, size):
    o = off(secs, rva)
    return blob[o:o + size]


assert hashlib.sha256(OUT).hexdigest() == EXPECTED, "unexpected output hash"
bb_s, out_s = sections(BB), sections(OUT)
assert len(OUT) == len(BB), "size changed"
assert len(out_s) == 12, "post-build PE section count must stay 12"

# 1. The Halo 4 branch guard is intact and still gates this load.
assert read(OUT, out_s, 0x11C73, 4) == bytes.fromhex("3c047516"), \
    "Halo 4 title compare/branch must be unchanged"

# 2. The load is still a movdqa and now resolves to the private constant.
load = read(OUT, out_s, H4_LOAD_RVA, 8)
assert load[:4] == bytes.fromhex("660f6f0d"), "must remain movdqa xmm1,[rip+..]"
disp = struct.unpack_from("<i", load, 4)[0]
assert H4_LOAD_NEXT_RVA + disp == CONST_RVA, "load must reach the new constant"
assert CONST_RVA % 16 == 0, "movdqa operand must be 16-byte aligned"

# 3. The new constant produces a 4x centred zoom, and seeds MinDepth/MaxDepth.
w, h, mind, maxd = struct.unpack("<ffff", read(OUT, out_s, CONST_RVA, 16))
assert (w, h) == (2048.0, 2048.0), f"expected 2048x2048, got {w}x{h}"
assert (mind, maxd) == (0.0, 1.0), "depth range must stay 0..1"
assert w / RETICLE_SIZE == 4.0, "must match the Halo 3 / ODST 4x ratio"
assert (RETICLE_SIZE - w) * 0.5 == -768.0, "TopLeft must be -768 (centred)"

# 4. The shared constant and its OTHER referent are untouched.
assert struct.unpack("<ffff", read(OUT, out_s, OLD_CONST_RVA, 16)) == \
    (512.0, 512.0, 0.0, 1.0), "shared constant must not be edited"
assert read(OUT, out_s, 0x152C1F, 4) == read(BB, bb_s, 0x152C1F, 4), \
    "the other referent of the shared constant must be unchanged"

# 5. The Stage 3AX capture edge is restored, so a capture actually runs.
assert read(BB, bb_s, 0x53634, 2) == bytes.fromhex("eb56")
assert read(OUT, out_s, 0x53634, 2) == bytes.fromhex("7456")

# 6. Accepted Stage 3AL CREDIT + ODST bytes survive untouched.
for rva, size in ((0x2C0589, 8), (0x2C5C16, 1), (0x2C5C6B, 1), (0x2BFF0C, 1)):
    assert read(OUT, out_s, rva, size) == read(BB, bb_s, rva, size)

# 7. Nothing outside the three intended regions moved. In particular no other
#    title's capture framing may change: the non-Halo-4 path at 0x11C8D and
#    the shared scale/offset math at 0x11D0A.. must be byte-identical.
allowed = ((0x11C7B, 4), (0x53634, 1), (CONST_RVA, 16))
permitted = set()
for rva, size in allowed:
    start = off(out_s, rva)
    permitted.update(range(start, start + size))
changed = {i for i in range(len(BB)) if BB[i] != OUT[i]}
assert changed <= permitted, "unexpected bytes changed outside the patch sites"
assert read(OUT, out_s, 0x11C8D, 32) == read(BB, bb_s, 0x11C8D, 32), \
    "non-Halo-4 capture framing must be untouched"
assert read(OUT, out_s, 0x11D0A, 96) == read(BB, bb_s, 0x11D0A, 96), \
    "shared viewport scale/offset math must be untouched"

print("PASS Stage3BD H4 capture 4x centre zoom; shared constant and other "
      "titles untouched; CREDIT/ODST preserved; 12 sections")
