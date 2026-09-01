from pathlib import Path
import hashlib
import struct

ROOT = Path(__file__).parent.parent
BD = (ROOT / "built" / "Stage3BD-HaloMCCVR.dll").read_bytes()
OUT = (ROOT / "built" / "Stage3BH-HaloMCCVR.dll").read_bytes()
EXPECTED = "80573ed9fffd3a557dea96b80823e490ecdaea7c0063ee6a5e1bacc4486d48c4"

PAYLOAD_RVA = 0x2FB810
PAYLOAD_END = 0x2FB810 + 1580
GATE_RVA = 0x2FB850
TAP_RVA = 0x2FB9ED
DUMP_RVA = 0x2FBA69
TITLE_ADAPTER_RVA = 0x879C0
LOG_RVA = 0x1D90
DRAW_CALL_RVA = 0xD0C5
DRAWIDX_CALL_RVA = 0xD229
UPLOAD_CALL_RVA = 0x2763C


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


def rip_target(rva, instr_len, disp_off):
    disp = struct.unpack_from(
        "<i", read(OUT, out_s, rva, instr_len), disp_off)[0]
    return rva + instr_len + disp


def call_target(rva):
    site = read(OUT, out_s, rva, 5)
    assert site[0] == 0xE8, f"0x{rva:X} must be a rel32 call"
    return rva + 5 + struct.unpack_from("<i", site, 1)[0]


# 1. All three splices target the payload; surrounding code untouched.
assert call_target(DRAW_CALL_RVA) == GATE_RVA
assert call_target(DRAWIDX_CALL_RVA) == GATE_RVA
assert call_target(UPLOAD_CALL_RVA) == TAP_RVA
assert read(OUT, out_s, 0xD0BD, 8) == bytes.fromhex("418bf08bfa488bd9")
assert read(OUT, out_s, 0xD21E, 11) == bytes.fromhex("418be9418bf08bfa488bd9")
assert read(OUT, out_s, 0x27635, 7) == bytes.fromhex("488d0d145f1900"), \
    "stats-line lea rcx before the tap splice must be unchanged"
assert read(OUT, out_s, 0x27641, 7) == bytes.fromhex("44893d446e2800"), \
    "post-call code after the tap splice must be unchanged"

# 2. Gate semantics (identical structure to Stage 3BF, relocated).
assert rip_target(GATE_RVA, 7, 2) == 0x2AE770, "guard 1 = capture.active"
assert rip_target(GATE_RVA + 0x0D, 7, 2) == 0x2AE79C, "guard 2 = framing"
assert rip_target(GATE_RVA + 0x1A, 7, 2) == 0x2BA6C8 and \
    read(OUT, out_s, GATE_RVA + 0x1A, 7)[6] == 4, "guard 3 = Halo 4"
assert rip_target(GATE_RVA + 0x27, 7, 3) == 0x2AE298, "guard 4 = g_context"
assert rip_target(GATE_RVA + 0x3B, 7, 3) == 0x2AE458, "cmov false = discard"
assert rip_target(GATE_RVA + 0x42, 8, 4) == 0x2AE448, "cmov true = authored"
assert read(OUT, out_s, GATE_RVA + 0x78, 6) == \
    bytes.fromhex("ff90c8020000"), "OMGetRenderTargets +0x2C8"
assert rip_target(GATE_RVA + 0xA7, 7, 2) == 0x2FB818 and \
    read(OUT, out_s, GATE_RVA + 0xA7, 7)[6] == 8, "telemetry budget = 8"
assert read(OUT, out_s, GATE_RVA + 0xD2, 6) == \
    bytes.fromhex("ff90f8020000"), "RSGetViewports +0x2F8"
assert read(OUT, out_s, GATE_RVA + 0xF0, 6) == \
    bytes.fromhex("ff9000030000"), "RSGetScissorRects +0x300"
assert rip_target(GATE_RVA + 0x14A, 7, 3) == 0x2AE774, "pin viewport"
assert read(OUT, out_s, GATE_RVA + 0x154, 6) == \
    bytes.fromhex("ff9060010000"), "RSSetViewports +0x160"
assert rip_target(GATE_RVA + 0x162, 7, 3) == 0x2AE78C, "pin scissor"
assert read(OUT, out_s, GATE_RVA + 0x16C, 6) == \
    bytes.fromhex("ff9068010000"), "RSSetScissorRects +0x168"
tail = read(OUT, out_s, GATE_RVA + 0x198, 5)
assert tail[0] == 0xE9 and GATE_RVA + 0x198 + 5 + \
    struct.unpack_from("<i", tail, 1)[0] == TITLE_ADAPTER_RVA, \
    "gate tail-jumps into TitleAdapter_GetActiveTitle"

# 3. Tap semantics: forwards LOG with five copied stack varargs, then the
#    one-shot dump gated on Halo 4 + third window.
assert read(OUT, out_s, TAP_RVA, 4) == bytes.fromhex("4883ec68"), \
    "tap frame is sub rsp,0x68"
assert read(OUT, out_s, TAP_RVA + 4, 8) == \
    bytes.fromhex("488b842490000000"), "tap copies caller [rsp+0x20]"
assert call_target(0x2FBA32) == LOG_RVA, "tap forwards the stats LOG"
assert rip_target(0x2FBA37, 7, 2) == 0x2BA6C8 and \
    read(OUT, out_s, 0x2FBA37, 7)[6] == 4, "dump gated on Halo 4"
assert rip_target(0x2FBA4F, 7, 2) == 0x2FB81C and \
    read(OUT, out_s, 0x2FBA4F, 7)[6] == 3, "dump waits for 3rd window"
assert call_target(0x2FBA5F) == DUMP_RVA, "tap calls the dump helper"

# 4. Dump semantics: source texture and device globals, staging desc patch,
#    the D3D vtable offsets, and the 32-iteration loop bounds.
assert rip_target(DUMP_RVA + 0x10, 7, 3) == 0x2AE440, \
    "dump reads g_authoredReticleTexture"
assert rip_target(0x2FBA89, 7, 3) == 0x2AE298, "dump reads g_context"
assert read(OUT, out_s, 0x2FBAA6, 6) == bytes.fromhex("488b01ff5050"), \
    "GetDesc via texture vtable +0x50"
assert read(OUT, out_s, 0x2FBAAC, 8) == \
    bytes.fromhex("c744245c03000000"), "desc.Usage = STAGING"
assert read(OUT, out_s, 0x2FBABC, 8) == \
    bytes.fromhex("c744246400000200"), "desc.CPUAccessFlags = READ"
# MipLevels (desc+0x08 -> [rsp+0x48]) and ArraySize (+0x0C -> [rsp+0x4C]) must
# NOT be overwritten: CopyResource requires identical mip/array counts, and the
# authored texture is mipped (the coverage probe walks desc.MipLevels), so
# forcing 1 here would make the copy silently do nothing.
_payload = read(OUT, out_s, PAYLOAD_RVA, PAYLOAD_END - PAYLOAD_RVA)
assert bytes.fromhex("c744244801000000") not in _payload, \
    "staging desc must not force MipLevels = 1"
assert bytes.fromhex("c744244c01000000") not in _payload, \
    "staging desc must not force ArraySize = 1"
assert rip_target(0x2FBACC, 7, 3) == 0x2AE290, "dump reads g_device"
assert read(OUT, out_s, 0x2FBAF5, 3) == bytes.fromhex("ff5028"), \
    "CreateTexture2D via device vtable +0x28"
assert read(OUT, out_s, 0x2FBB1C, 6) == bytes.fromhex("ff9078010000"), \
    "CopyResource via context vtable +0x178"
assert read(OUT, out_s, 0x2FBB47, 3) == bytes.fromhex("ff5070"), \
    "Map via context vtable +0x70"
assert read(OUT, out_s, 0x2FBC46, 3) == bytes.fromhex("ff5078"), \
    "Unmap via context vtable +0x78"
assert read(OUT, out_s, 0x2FBC10, 4) == bytes.fromhex("4183fe20"), \
    "column loop bound 32"
assert read(OUT, out_s, 0x2FBC31, 3) == bytes.fromhex("83ff20"), \
    "row loop bound 32"
# The sampler must inspect EVERY texel of each 16x16 cell, not one of them:
# a 16-iteration dx loop, a 16-row dy loop, and a running max over the cell.
assert read(OUT, out_s, 0x2FBBE2, 3) == bytes.fromhex("83f910"), \
    "inner dx loop must run 16 times per cell row"
assert read(OUT, out_s, 0x2FBB9F, 5) == bytes.fromhex("ba10000000"), \
    "each cell must start with 16 rows to scan"
assert read(OUT, out_s, 0x2FBBD9, 7) == bytes.fromhex("4139c0440f42c0"), \
    "running max over the cell (cmp r8d,eax / cmovb r8d,eax)"

# 5. The capture framing now reproduces the engine's OWN measured viewport
#    (S3BG telemetry 2026-08-28 18:44: tl=-1811,-417 wh=4134x1346) through
#    Begin's (512-W)/2 centring, instead of the square 2048 that stretched
#    the art and overrode the engine at every draw.
_w, _h, _mind, _maxd = struct.unpack("<ffff", read(OUT, out_s, 0x2FB800, 16))
assert abs(_w - 4134.312) < 0.01 and abs(_h - 1346.196) < 0.01, (_w, _h)
assert (_mind, _maxd) == (0.0, 1.0), "depth range must stay 0..1"
assert abs((512 - _w) * 0.5 + 1811.156) < 0.01, "TopLeftX = measured -1811"
assert abs((512 - _h) * 0.5 + 417.098) < 0.01, "TopLeftY = measured -417"
assert abs(_w / _h - 1033.578 / 336.549) < 1e-4, \
    "framing must keep the live CUI aspect ratio"
load = read(OUT, out_s, 0x11C77, 8)
assert load[:4] == bytes.fromhex("660f6f0d")
assert 0x11C7F + struct.unpack_from("<i", load, 4)[0] == 0x2FB800
assert struct.unpack("<ffff", read(OUT, out_s, 0x1BF0C0, 16)) == \
    (512.0, 512.0, 0.0, 1.0)
assert read(OUT, out_s, 0x53634, 2) == bytes.fromhex("7456")

# 6. CREDIT + ODST bytes survive untouched.
for rva, size in ((0x2C0589, 8), (0x2C5C16, 1), (0x2C5C6B, 1), (0x2BFF0C, 1)):
    assert read(OUT, out_s, rva, size) == read(BD, bd_s, rva, size)

# 7. Nothing outside the intended regions changed relative to Stage 3BD.
allowed = ((DRAW_CALL_RVA + 1, 4), (DRAWIDX_CALL_RVA + 1, 4),
           (UPLOAD_CALL_RVA + 1, 4),
           (0x2FB800, 16),                    # the re-measured framing constant
           (PAYLOAD_RVA, PAYLOAD_END - PAYLOAD_RVA))
permitted = set()
for rva, size in allowed:
    start = off(out_s, rva)
    permitted.update(range(start, start + size))
changed = {i for i in range(len(BD)) if BD[i] != OUT[i]}
assert changed <= permitted, "unexpected bytes changed outside the patch sites"
assert read(OUT, out_s, 0xD0CA, 64) == read(BD, bd_s, 0xD0CA, 64)
assert read(OUT, out_s, 0xD22E, 64) == read(BD, bd_s, 0xD22E, 64)

print("PASS Stage3BH pin + telemetry + one-shot capture map; all three "
      "splices and the dump decoded from welded bytes; Stage3BD identity "
      "intact; CREDIT/ODST preserved; 12 sections")
