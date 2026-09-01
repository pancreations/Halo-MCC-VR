"""Stage 3BN verification - decodes the output DLL, trusts nothing.

 1. Byte-identical to Stage 3BM outside the 5-byte gate call at 0x2FB992 and
    the payload at 0x2F93E0.
 2. Gate decodes as call -> s3bn_aspect_viewport; the 19 NOPs and the
    scissor re-assert after it are untouched.
 3. Thunk semantics: sub/add rsp 0x28; copies all 24 bytes of the stored
    viewport (0x2AE774) into a private copy; reads backbuffer Width/Height
    (0x2AEB58/0x2AEB5C) with zero guards; Height = Width*bbH/bbW (mulss then
    divss) stored at private+12; TopLeftY = (512-H)*0.5 - H*kbias stored at
    private+4; fallback path subtracts 104.0 from private TopLeftY; calls
    RSSetViewports (+0x160) with rcx=rbx, edx=1, r8=private; never writes
    the stored struct or anything outside the payload.
 4. Constants: 512.0, 0.5, 104/1346.196, 104.0.
 5. 3BJ absent; 3BM/3BL/3BK/3BI/3BH artifacts and 12 sections intact.
"""
from pathlib import Path
import hashlib, struct
import capstone
from capstone import x86

HERE = Path(__file__).parent
BASE = HERE.parent / "built" / "Stage3BM-HaloMCCVR.dll"
OUT = HERE.parent / "built" / "Stage3BN-HaloMCCVR.dll"
EXPECTED_BASE = "1873582cadb4937220ff3d26e1098e343c72fe9e0492bbc972d69a384227a206"
GATE_RVA = 0x2FB992
PAYLOAD_RVA = 0x2F93E0
PAYLOAD_LIMIT = 0x2FA000
STORED = 0x2AE774
BBW, BBH = 0x2AEB58, 0x2AEB5C

base = bytearray(BASE.read_bytes()); out = bytearray(OUT.read_bytes())
assert hashlib.sha256(base).hexdigest() == EXPECTED_BASE, "wrong 3BM base"
assert len(base) == len(out)

def parse_pe(blob):
    p = struct.unpack_from("<I", blob, 0x3C)[0]; coff = p + 4
    n = struct.unpack_from("<H", blob, coff + 2)[0]
    osz = struct.unpack_from("<H", blob, coff + 16)[0]
    st = coff + 20 + osz; secs = []
    for i in range(n):
        o = st + i * 40
        name = bytes(blob[o:o+8]).split(b"\0", 1)[0].decode()
        vs, va, rs, rp = struct.unpack_from("<IIII", blob, o+8)
        secs.append((name, va, vs, rp, rs))
    return n, secs

n, secs = parse_pe(out); assert n == 12
def off(rva):
    for name, va, vs, rp, rs in secs:
        if va <= rva < va + max(vs, rs): return rp + rva - va
    raise AssertionError(hex(rva))

diff = [i for i in range(len(base)) if base[i] != out[i]]
assert diff
po, pl = off(PAYLOAD_RVA), off(PAYLOAD_LIMIT)
pay_len = max(i for i in diff if po <= i < pl) + 1 - po
allowed = set(range(off(GATE_RVA), off(GATE_RVA) + 5)) | set(range(po, po + pay_len))
stray = [i for i in diff if i not in allowed]
assert not stray, f"stray changes at {[hex(i) for i in stray[:8]]}"
assert all(base[i] == 0 for i in range(po, pl)), "payload page not free in base"

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64); md.detail = True
def disasm(rva, size): return list(md.disasm(bytes(out[off(rva):off(rva)+size]), rva))
def riptarget(i):
    for op in i.operands:
        if op.type == x86.X86_OP_MEM and op.mem.base == x86.X86_REG_RIP:
            return i.address + i.size + op.mem.disp
    return None

# 2. gate
g = disasm(GATE_RVA, 24 + 15)
assert g[0].mnemonic == "call"; thunk = int(g[0].op_str, 16)
assert PAYLOAD_RVA <= thunk < PAYLOAD_RVA + pay_len
assert all(i.mnemonic == "nop" for i in g[1:20]), "19 nops"
assert g[20].mnemonic == "mov" and g[20].op_str == "rcx, rbx"
assert g[22].mnemonic == "lea" and riptarget(g[22]) == 0x2AE78C, "scissor re-assert"

# 3. thunk
t = disasm(thunk, PAYLOAD_RVA + pay_len - thunk)
t = t[:next(k for k, i in enumerate(t) if i.mnemonic == "ret") + 1]
mn = [(i.mnemonic, i.op_str) for i in t]
assert mn[0] == ("sub", "rsp, 0x28") and ("add", "rsp, 0x28") in mn
assert mn.count(("add", "rsp, 0x28")) == 1
# stored -> private copy (16 + 8 bytes)
assert t[1].mnemonic == "movups" and riptarget(t[1]) == STORED
assert t[2].mnemonic == "movups"; private = riptarget(t[2])
assert t[3].mnemonic == "mov" and riptarget(t[3]) == STORED + 0x10
assert t[4].mnemonic == "mov" and riptarget(t[4]) == private + 0x10
assert PAYLOAD_RVA <= private < PAYLOAD_LIMIT   # zero-initialised tail past the last nonzero byte
# backbuffer loads + zero guards
loads = [riptarget(i) for i in t if i.mnemonic == "mov" and i.op_str.startswith("e") and riptarget(i)]
assert BBW in loads and BBH in loads, loads
assert sum(1 for i in t if i.mnemonic == "test") >= 2
assert sum(1 for i in t if i.mnemonic == "je") == 2, "two zero guards"
# aspect math
assert sum(1 for i in t if i.mnemonic == "cvtsi2ss") == 2
assert any(i.mnemonic == "movss" and riptarget(i) == private + 8 for i in t), "reads private Width"
assert ("mulss", "xmm0, xmm1") in mn and ("divss", "xmm0, xmm2") in mn
assert any(i.mnemonic == "movss" and riptarget(i) == private + 12 and i.op_str.startswith("dword") for i in t), "stores private Height"
# TopLeftY
consts = {}
for i in t:
    if i.mnemonic in ("subss", "mulss", "movss") and riptarget(i) and not i.op_str.startswith("dword"):
        consts[i.mnemonic + "@" + hex(riptarget(i))] = struct.unpack_from("<f", out, off(riptarget(i)))[0]
vals = sorted(consts.values())
assert 512.0 in vals and 0.5 in vals and 104.0 in vals, vals
kb = [v for v in vals if abs(v - 104.0 / 1346.196) < 1e-6]
assert kb, vals
tly_stores = [i for i in t if i.mnemonic == "movss" and i.op_str.startswith("dword") and riptarget(i) == private + 4]
assert len(tly_stores) == 2, "aspect path and fallback path each store TopLeftY"
# no write to the stored struct / outside the payload
for i in t:
    for op in i.operands:
        if op.type == x86.X86_OP_MEM and (op.access & capstone.CS_AC_WRITE):
            tg = riptarget(i)
            if tg is not None:
                assert PAYLOAD_RVA <= tg < PAYLOAD_LIMIT, f"write outside payload at {i.address:#x}"
                assert not (STORED <= tg < STORED + 24)
# the D3D call
k = next(k for k, i in enumerate(t) if i.mnemonic == "call")
assert "rax + 0x160" in t[k].op_str
assert ("mov", "rcx, rbx") in mn and ("mov", "edx, 1") in mn
assert any(i.mnemonic == "lea" and riptarget(i) == private for i in t)
assert t[-1].mnemonic == "ret"

# 5. artifacts
assert bytes(out[off(0x199B3):off(0x199B3)+2]) == bytes.fromhex("b201"), "3BJ present"
for rva in (0xD0C5, 0xD229, 0x2763C, 0x53921, 0x2FBA58):
    assert out[off(rva)] == 0xE8, hex(rva)
assert out[off(0x2FA2D7)] == 0xE9
assert out[off(0x2FBF64):off(0x2FBF64)+4] == bytes.fromhex("4883ec28"), "3BK thunk bytes"
assert struct.unpack_from("<4f", out, off(0x2FB800)) == struct.unpack("<4f", struct.pack("<ffff", 4134.312, 1346.196, 0, 1))
print(f"PASS: Stage 3BN verified -- gate -> {thunk:#x}; private copy at {private:#x}; "
      f"Height = W*bbH/bbW, TopLeftY = (512-H)*0.5 - H*{104/1346.196:.7f}; fallback -104; "
      "stored struct never written; RSSetViewports(+0x160); 3BJ absent; artifacts intact")
