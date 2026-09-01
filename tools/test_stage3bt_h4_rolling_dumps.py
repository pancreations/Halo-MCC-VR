"""Stage 3BR+3BS+3BT verification - decodes the cumulative output vs 3BQ.

 1. Byte-identical to Stage 3BQ outside exactly five regions: the gate call
    (0x2FB992), the 3BS splice (0x2A701, 13 bytes), the 3BT splice (0x2F9066,
    9 bytes), and the three payloads (0x2F9A90.., 0x2F9C50.., 0x2F9CD0..).
 2. 3BR thunk: the full 3BN computation (stored copy, aspect Height, biased
    TopLeftY, zero-guards, 3BK fallback) plus the scale: two mulss-by-k
    stores to W/H and two 256-k*(256-TL) stores to TLx/TLy; constants 512 /
    0.5 / 104-bias / 104 / 2.5 / 256; RSSetViewports(+0x160) on a private
    struct; the stored struct never written.
 3. 3BS thunk: movzx esi,dl; H4 title compare -> esi=1; displaced global
    load ([0x2AE490]) and movzx ebp,cl re-executed; only esi/rax/ebp written.
 4. 3BT thunk: cmp eax,4; jbe ret; eax=1 stored to the 3BM counter
    (0x2F9040); nothing else written.  The splice's fall-through keeps the
    windows=-2 store at 0x2F906F.
 5. 3BQ/3BP/3BO/3BN/3BM/3BL artifacts and 12 sections intact; 3BJ absent.
"""
from pathlib import Path
import hashlib, struct, sys
import capstone
from capstone import x86
sys.path.insert(0, str(Path(__file__).parent))
from build_stage3br_h4_capture_scale import parse_pe, PAYLOAD_RVA as R_RVA, PAYLOAD_LIMIT as R_LIM
from build_stage3bs_h4_identity_per_upload import (PAYLOAD_RVA as S_RVA, PAYLOAD_LIMIT as S_LIM,
    SPLICE_RVA as S_SPLICE, ACTIVE_TITLE_RVA, UPLOAD_GLOB_RVA)
from build_stage3bt_h4_rolling_dumps import (PAYLOAD_RVA as T_RVA, PAYLOAD_LIMIT as T_LIM,
    SPLICE_RVA as T_SPLICE, DUMP_COUNTER_RVA)

HERE = Path(__file__).parent
BASE = HERE.parent / "built" / "Stage3BQ-HaloMCCVR.dll"
OUT = HERE.parent / "built" / "Stage3BT-HaloMCCVR.dll"
EXPECTED_BASE = "c9d95aa9aabeb2ed07c232d292c3ab4bbcd91300c2d67edd6921aabef0078f5d"
GATE = 0x2FB992
STORED = 0x2AE774

base = bytearray(BASE.read_bytes()); out = bytearray(OUT.read_bytes())
assert hashlib.sha256(base).hexdigest() == EXPECTED_BASE, "wrong 3BQ base"
assert len(base) == len(out)
pe = parse_pe(out); assert pe["n"] == 12
def off(rva):
    for s in pe["sections"]:
        if s["va"] <= rva < s["va"] + max(s["vs"], s["rs"]): return s["rp"] + rva - s["va"]
    raise AssertionError(hex(rva))

# 1. regions
diff = [i for i in range(len(base)) if base[i] != out[i]]
allowed = (set(range(off(GATE), off(GATE) + 5)) |
           set(range(off(S_SPLICE), off(S_SPLICE) + 13)) |
           set(range(off(T_SPLICE), off(T_SPLICE) + 9)) |
           set(range(off(R_RVA), off(R_LIM))) |
           set(range(off(S_RVA), off(S_LIM))) |
           set(range(off(T_RVA), off(T_LIM))))
stray = [i for i in diff if i not in allowed]
assert not stray, f"stray changes at {[hex(i) for i in stray[:8]]}"
for lo, hi in ((R_RVA, R_LIM), (S_RVA, S_LIM), (T_RVA, T_LIM)):
    assert all(base[i] == 0 for i in range(off(lo), off(hi))), "payload page not free in base"

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64); md.detail = True
def disasm(rva, size): return list(md.disasm(bytes(out[off(rva):off(rva)+size]), rva))
def riptarget(i):
    for op in i.operands:
        if op.type == x86.X86_OP_MEM and op.mem.base == x86.X86_REG_RIP:
            return i.address + i.size + op.mem.disp
    return None
def cut(t): return t[:next(k for k, i in enumerate(t) if i.mnemonic == "ret") + 1]

# 2. 3BR
g = disasm(GATE, 5)
assert g[0].mnemonic == "call" and int(g[0].op_str, 16) == R_RVA
t = cut(disasm(R_RVA, R_LIM - R_RVA))
mn = [(i.mnemonic, i.op_str) for i in t]
data = max(riptarget(i) for i in t if riptarget(i) and R_RVA < riptarget(i) < R_LIM
           and i.mnemonic in ("movss", "mulss", "subss")) - 5 * 4  # k512 is the lowest constant
# constants: locate by value
def f32(rva): return struct.unpack_from("<f", out, off(rva))[0]
consts = sorted({round(f32(riptarget(i)), 7) for i in t
                 if riptarget(i) and i.mnemonic in ("mulss", "subss", "movss")
                 and not i.op_str.startswith("dword") and R_RVA < riptarget(i) < R_LIM
                 and abs(f32(riptarget(i))) > 1e-9})
assert 512.0 in consts and 0.5 in consts and 104.0 in consts and 2.5 in consts and 256.0 in consts, consts
assert any(abs(c - 104.0/1346.196) < 1e-6 for c in consts), consts
# stored copy in, private out
assert t[1].mnemonic == "movups" and riptarget(t[1]) == STORED
private = riptarget(t[2]); assert R_RVA < private < R_LIM
# W and H scaled: two mulss followed by stores to private+8 / private+12
stores = [riptarget(i) for i in t if i.mnemonic == "movss" and i.op_str.startswith("dword")]
for slot in (private, private + 4, private + 8, private + 12):
    assert slot in stores, hex(slot)
assert stores.count(private + 4) >= 2   # biased TLy then scaled TLy (or fallback)
assert ("mulss", "xmm0, xmm3") in mn and ("subss", "xmm1, xmm0") in mn
assert sum(1 for i in t if i.mnemonic == "cvtsi2ss") == 2
assert sum(1 for i in t if i.mnemonic == "je") == 2, "two zero guards"
k = next(i for i in t if i.mnemonic == "call")
assert "rax + 0x160" in k.op_str
assert ("mov", "rcx, rbx") in mn and ("mov", "edx, 1") in mn
assert any(i.mnemonic == "lea" and riptarget(i) == private for i in t)
for i in t:
    for op in i.operands:
        if op.type == x86.X86_OP_MEM and (op.access & capstone.CS_AC_WRITE):
            tg = riptarget(i)
            assert tg is not None and R_RVA <= tg < R_LIM, f"write outside payload at {i.address:#x}"
            assert not (STORED <= tg < STORED + 24)

# 3. 3BS
s = disasm(S_SPLICE, 13)
assert s[0].mnemonic == "call" and int(s[0].op_str, 16) == S_RVA
assert all(i.mnemonic == "nop" for i in s[1:9])
u = cut(disasm(S_RVA, S_LIM - S_RVA))
um = [(i.mnemonic, i.op_str) for i in u]
assert um[0] == ("movzx", "esi, dl")
assert u[1].mnemonic == "cmp" and riptarget(u[1]) == ACTIVE_TITLE_RVA and u[1].op_str.endswith(", 4")
assert um[2][0] == "jne" and um[3] == ("mov", "esi, 1")
assert u[4].mnemonic == "mov" and u[4].op_str.startswith("rax") and riptarget(u[4]) == UPLOAD_GLOB_RVA
assert um[5] == ("movzx", "ebp, cl") and um[6] == ("ret", "")
for i in u:
    for op in i.operands:
        if op.type == x86.X86_OP_REG and (op.access & capstone.CS_AC_WRITE):
            assert i.reg_name(op.reg) in ("esi", "rax", "ebp"), i.reg_name(op.reg)
        assert not (op.type == x86.X86_OP_MEM and (op.access & capstone.CS_AC_WRITE))

# 4. 3BT
w = disasm(T_SPLICE, 9)
assert w[0].mnemonic == "call" and int(w[0].op_str, 16) == T_RVA
assert all(i.mnemonic == "nop" for i in w[1:5])
assert bytes(out[off(0x2F906F):off(0x2F906F)+10]) == bytes.fromhex("c705a3270000feffffff")
v = cut(disasm(T_RVA, T_LIM - T_RVA))
vm = [(i.mnemonic, i.op_str) for i in v]
assert vm[0] == ("cmp", "eax, 4") and vm[1][0] == "jbe" and vm[2] == ("mov", "eax, 1")
assert v[3].mnemonic == "mov" and riptarget(v[3]) == DUMP_COUNTER_RVA and v[3].op_str.endswith(", eax")
assert vm[4] == ("ret", "")

# 5. artifacts
assert bytes(out[off(0x2F9A30):off(0x2F9A30)+4]) == bytes.fromhex("4883ec58"), "3BQ thunk"
assert bytes(out[off(0x2F98A0):off(0x2F98A0)+4]) == bytes.fromhex("4883ec58"), "3BP selector"
assert out[off(0x2A9DA)] == 0xE8, "3BQ splice"
assert out[off(0x53921)] == 0xE8 and out[off(0x11E76)] == 0xE8 and out[off(0x11EB6)] == 0xE8
assert bytes(out[off(0x199B3):off(0x199B3)+2]) == bytes.fromhex("b201"), "3BJ present"
assert out[off(0x2FA2D7)] == 0xE9 and out[off(0x2FBA58)] == 0xE8
print(f"PASS: 3BR/3BS/3BT verified -- gate -> {R_RVA:#x} (3BN maths + k=2.5 scale about 256, "
      f"private struct, stored never written); 0x2A701 -> {S_RVA:#x} (H4 => esi=1, displaced pair kept); "
      f"0x2F9066 -> {T_RVA:#x} (counter wraps 4->1); artifacts intact; 3BJ absent")
