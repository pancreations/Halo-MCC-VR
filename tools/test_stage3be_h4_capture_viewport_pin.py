from pathlib import Path
import hashlib
import struct

ROOT = Path(__file__).parent.parent
BD = (ROOT / "built" / "Stage3BD-HaloMCCVR.dll").read_bytes()
OUT = (ROOT / "built" / "Stage3BE-HaloMCCVR.dll").read_bytes()
EXPECTED = "741a0e69e848b2cc49113e4a21e555bfd6cac78f7a482b623e213ee72d48ec8a"

PAYLOAD_RVA = 0x2FB810
PAYLOAD_END = 0x2FB982          # data(8) + gate + message, 370 bytes
GATE_RVA = 0x2FB818
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

# 1. Both census title calls now target the gate; the byte before each call
#    (the hooks' own prologues) is untouched.
for rva in (DRAW_CALL_RVA, DRAWIDX_CALL_RVA):
    site = read(OUT, out_s, rva, 5)
    assert site[0] == 0xE8, f"0x{rva:X} must stay a rel32 call"
    disp = struct.unpack_from("<i", site, 1)[0]
    assert rva + 5 + disp == GATE_RVA, f"0x{rva:X} must call the gate"
assert read(OUT, out_s, 0xD0BD, 8) == bytes.fromhex("418bf08bfa488bd9"), \
    "Draw hook prologue (context into rbx) must be unchanged"
assert read(OUT, out_s, 0xD21E, 11) == \
    bytes.fromhex("418be9418bf08bfa488bd9"), \
    "DrawIndexed hook prologue must be unchanged"

# 2. Gate semantics, decoded from the welded bytes (not trusted from the
#    builder): guard chain reads the disassembly-proven capture-state
#    globals, re-assert passes the saved viewport/scissor, tail jump returns
#    into the real title adapter.
def rip_target(rva, instr_len, disp_off):
    disp = struct.unpack_from(
        "<i", read(OUT, out_s, rva, instr_len), disp_off)[0]
    return rva + instr_len + disp

assert read(OUT, out_s, GATE_RVA, 2) == b"\x80\x3d", "gate opens with cmp byte"
assert rip_target(GATE_RVA, 7, 2) == 0x2AE770, "guard 1 = capture.active"
assert rip_target(GATE_RVA + 0x0D, 7, 2) == 0x2AE79C, \
    "guard 2 = framingCaptured"
assert rip_target(GATE_RVA + 0x1A, 7, 2) == 0x2BA6C8 and \
    read(OUT, out_s, GATE_RVA + 0x1A, 7)[6] == 4, \
    "guard 3 = active title byte == 4 (Halo 4)"
assert rip_target(GATE_RVA + 0x27, 7, 3) == 0x2AE298, \
    "guard 4 = the game's immediate context"
assert rip_target(GATE_RVA + 0x3B, 7, 3) == 0x2AE458, "cmov false = discard RTV"
assert rip_target(GATE_RVA + 0x42, 8, 4) == 0x2AE448, "cmov true = authored RTV"
# OMGetRenderTargets / Release / RSSetViewports / RSSetScissorRects offsets
assert read(OUT, out_s, GATE_RVA + 0x78, 6) == \
    bytes.fromhex("ff90c8020000"), "call [vtbl+0x2C8] OMGetRenderTargets"
assert read(OUT, out_s, GATE_RVA + 0x95, 3) == bytes.fromhex("ff5010"), \
    "call [vtbl+0x10] IUnknown::Release"
assert rip_target(GATE_RVA + 0xA7, 7, 3) == 0x2AE774, \
    "re-assert passes the saved captureViewport"
assert read(OUT, out_s, GATE_RVA + 0xB1, 6) == \
    bytes.fromhex("ff9060010000"), "call [vtbl+0x160] RSSetViewports"
assert rip_target(GATE_RVA + 0xBF, 7, 3) == 0x2AE78C, \
    "re-assert passes the saved captureScissor"
assert read(OUT, out_s, GATE_RVA + 0xC9, 6) == \
    bytes.fromhex("ff9068010000"), "call [vtbl+0x168] RSSetScissorRects"
tail = read(OUT, out_s, GATE_RVA + 0xF5, 5)
assert tail[0] == 0xE9 and GATE_RVA + 0xF5 + 5 + \
    struct.unpack_from("<i", tail, 1)[0] == TITLE_ADAPTER_RVA, \
    "gate must tail-jump into the real TitleAdapter_GetActiveTitle"

# 3. The Stage 3BD identity underneath is intact: private 4x constant, the
#    retargeted movdqa, the capture edge, and the untouched shared constant.
w, h, mind, maxd = struct.unpack("<ffff", read(OUT, out_s, 0x2FB800, 16))
assert (w, h, mind, maxd) == (2048.0, 2048.0, 0.0, 1.0)
load = read(OUT, out_s, 0x11C77, 8)
assert load[:4] == bytes.fromhex("660f6f0d")
assert 0x11C7F + struct.unpack_from("<i", load, 4)[0] == 0x2FB800
assert struct.unpack("<ffff", read(OUT, out_s, 0x1BF0C0, 16)) == \
    (512.0, 512.0, 0.0, 1.0), "shared constant must stay untouched"
assert read(OUT, out_s, 0x53634, 2) == bytes.fromhex("7456")

# 4. Accepted Stage 3AL CREDIT + ODST bytes survive untouched.
for rva, size in ((0x2C0589, 8), (0x2C5C16, 1), (0x2C5C6B, 1), (0x2BFF0C, 1)):
    assert read(OUT, out_s, rva, size) == read(BD, bd_s, rva, size)

# 5. Nothing outside the three intended regions changed relative to
#    Stage 3BD: the two 4-byte call displacements and the payload region.
allowed = ((DRAW_CALL_RVA + 1, 4), (DRAWIDX_CALL_RVA + 1, 4),
           (PAYLOAD_RVA, PAYLOAD_END - PAYLOAD_RVA))
permitted = set()
for rva, size in allowed:
    start = off(out_s, rva)
    permitted.update(range(start, start + size))
changed = {i for i in range(len(BD)) if BD[i] != OUT[i]}
assert changed <= permitted, "unexpected bytes changed outside the patch sites"
# The census hooks' Halo 2 logic after both splices is byte-identical.
assert read(OUT, out_s, 0xD0CA, 64) == read(BD, bd_s, 0xD0CA, 64)
assert read(OUT, out_s, 0xD22E, 64) == read(BD, bd_s, 0xD22E, 64)

print("PASS Stage3BE draw-time capture framing pin; gate decoded from welded "
      "bytes; Stage3BD identity intact; CREDIT/ODST preserved; 12 sections")
