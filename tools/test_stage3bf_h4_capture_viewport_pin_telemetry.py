from pathlib import Path
import hashlib
import struct

ROOT = Path(__file__).parent.parent
BD = (ROOT / "built" / "Stage3BD-HaloMCCVR.dll").read_bytes()
OUT = (ROOT / "built" / "Stage3BF-HaloMCCVR.dll").read_bytes()
EXPECTED = "a1d23fc8e67e077c7b72c53c33f7d12606dd15cbbe2a48dbcd0303a18bee67f3"

PAYLOAD_RVA = 0x2FB810
PAYLOAD_END = 0x2FB810 + 652
GATE_RVA = 0x2FB820
TITLE_ADAPTER_RVA = 0x879C0
DRAW_CALL_RVA = 0xD0C5
DRAWIDX_CALL_RVA = 0xD229


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
bd_s, out_s = sections(BD), sections(OUT)
assert len(OUT) == len(BD), "size changed"
assert len(out_s) == 12, "post-build PE section count must stay 12"

# 1. Both census title calls target the gate; the hooks' prologues are intact.
for rva in (DRAW_CALL_RVA, DRAWIDX_CALL_RVA):
    site = read(OUT, out_s, rva, 5)
    assert site[0] == 0xE8, f"0x{rva:X} must stay a rel32 call"
    disp = struct.unpack_from("<i", site, 1)[0]
    assert rva + 5 + disp == GATE_RVA, f"0x{rva:X} must call the gate"
assert read(OUT, out_s, 0xD0BD, 8) == bytes.fromhex("418bf08bfa488bd9")
assert read(OUT, out_s, 0xD21E, 11) == bytes.fromhex("418be9418bf08bfa488bd9")


def rip_target(rva, instr_len, disp_off):
    disp = struct.unpack_from(
        "<i", read(OUT, out_s, rva, instr_len), disp_off)[0]
    return rva + instr_len + disp


# 2. Gate semantics decoded from the welded bytes.
assert read(OUT, out_s, GATE_RVA, 2) == b"\x80\x3d", "gate opens with cmp byte"
assert rip_target(GATE_RVA, 7, 2) == 0x2AE770, "guard 1 = capture.active"
assert rip_target(GATE_RVA + 0x0D, 7, 2) == 0x2AE79C, \
    "guard 2 = framingCaptured"
assert rip_target(GATE_RVA + 0x1A, 7, 2) == 0x2BA6C8 and \
    read(OUT, out_s, GATE_RVA + 0x1A, 7)[6] == 4, \
    "guard 3 = active title byte == 4 (Halo 4)"
assert rip_target(GATE_RVA + 0x27, 7, 3) == 0x2AE298, \
    "guard 4 = the game's immediate context"
assert rip_target(GATE_RVA + 0x3B, 7, 3) == 0x2AE458, "cmov false = discard"
assert rip_target(GATE_RVA + 0x42, 8, 4) == 0x2AE448, "cmov true = authored"
assert read(OUT, out_s, GATE_RVA + 0x78, 6) == \
    bytes.fromhex("ff90c8020000"), "OMGetRenderTargets via vtable +0x2C8"
assert read(OUT, out_s, GATE_RVA + 0x99, 3) == bytes.fromhex("ff5010"), \
    "IUnknown::Release"
# telemetry: budget-capped, reads the live viewport+scissor BEFORE the pin
assert rip_target(GATE_RVA + 0xA7, 7, 2) == 0x2FB818 and \
    read(OUT, out_s, GATE_RVA + 0xA7, 7)[6] == 8, \
    "telemetry budget compare against 8 at s3bf_vp_budget"
assert read(OUT, out_s, GATE_RVA + 0xD2, 6) == \
    bytes.fromhex("ff90f8020000"), "RSGetViewports via vtable +0x2F8"
assert read(OUT, out_s, GATE_RVA + 0xF0, 6) == \
    bytes.fromhex("ff9000030000"), "RSGetScissorRects via vtable +0x300"
# the pin, unchanged from Stage 3BE
assert rip_target(GATE_RVA + 0x14A, 7, 3) == 0x2AE774, \
    "pin passes the saved captureViewport"
assert read(OUT, out_s, GATE_RVA + 0x154, 6) == \
    bytes.fromhex("ff9060010000"), "RSSetViewports via vtable +0x160"
assert rip_target(GATE_RVA + 0x162, 7, 3) == 0x2AE78C, \
    "pin passes the saved captureScissor"
assert read(OUT, out_s, GATE_RVA + 0x16C, 6) == \
    bytes.fromhex("ff9068010000"), "RSSetScissorRects via vtable +0x168"
tail = read(OUT, out_s, GATE_RVA + 0x198, 5)
assert tail[0] == 0xE9 and GATE_RVA + 0x198 + 5 + \
    struct.unpack_from("<i", tail, 1)[0] == TITLE_ADAPTER_RVA, \
    "gate must tail-jump into the real TitleAdapter_GetActiveTitle"

# 3. Stage 3BD identity intact underneath.
assert struct.unpack("<ffff", read(OUT, out_s, 0x2FB800, 16)) == \
    (2048.0, 2048.0, 0.0, 1.0)
load = read(OUT, out_s, 0x11C77, 8)
assert load[:4] == bytes.fromhex("660f6f0d")
assert 0x11C7F + struct.unpack_from("<i", load, 4)[0] == 0x2FB800
assert struct.unpack("<ffff", read(OUT, out_s, 0x1BF0C0, 16)) == \
    (512.0, 512.0, 0.0, 1.0), "shared constant must stay untouched"
assert read(OUT, out_s, 0x53634, 2) == bytes.fromhex("7456")

# 4. CREDIT + ODST bytes survive untouched.
for rva, size in ((0x2C0589, 8), (0x2C5C16, 1), (0x2C5C6B, 1), (0x2BFF0C, 1)):
    assert read(OUT, out_s, rva, size) == read(BD, bd_s, rva, size)

# 5. Nothing outside the intended regions changed relative to Stage 3BD.
allowed = ((DRAW_CALL_RVA + 1, 4), (DRAWIDX_CALL_RVA + 1, 4),
           (PAYLOAD_RVA, PAYLOAD_END - PAYLOAD_RVA))
permitted = set()
for rva, size in allowed:
    start = off(out_s, rva)
    permitted.update(range(start, start + size))
changed = {i for i in range(len(BD)) if BD[i] != OUT[i]}
assert changed <= permitted, "unexpected bytes changed outside the patch sites"
assert read(OUT, out_s, 0xD0CA, 64) == read(BD, bd_s, 0xD0CA, 64)
assert read(OUT, out_s, 0xD22E, 64) == read(BD, bd_s, 0xD22E, 64)

print("PASS Stage3BF draw-time pin + engine-viewport telemetry; gate decoded "
      "from welded bytes; Stage3BD identity intact; CREDIT/ODST preserved; "
      "12 sections")
