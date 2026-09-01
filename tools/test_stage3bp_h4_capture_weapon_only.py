"""Stage 3BP verification - decodes the output DLL, trusts nothing.

 1. Byte-identical to Stage 3BO outside the 5-byte splice call at 0x53921
    and the payload at 0x2F98A0.  The 2 nops after the call are untouched.
 2. Splice decodes as call -> s3bp_capture_select.
 3. Selector semantics, by decode:
    - sub/add rsp 0x58 on every exit; null-guarded g_halo4OrigCuiRenderCommand;
      rdx/r8/r9 saved before Halo4SafeRead and restored before the original
      call; rcx=rsi at the call; result byte preserved and returned;
    - the header is read ONCE, through Halo4SafeRead (no raw [r13] deref),
      only when redirectActive and r13 != 0, BEFORE the original call;
    - pre-call: type 0x20 or 0x1F while inside -> poly := 1 and enforce;
    - post-call: 0x28+payload 0xC -> inside := 1, poly := 0; 0x29 -> both 0;
      then enforce;
    - enforce: count from renderer+0x870 with 0 / >0x60 guards, top entry
      at +0x878 + (count-1)*0x34, x at +0x28, NaN guard, threshold
      2*halfWidth from the 3BH framing constant, effective inside =
      inside && !poly, exactly two stores to [rax+0x28];
    - no writes to non-volatile registers; no other memory writes outside
      the payload data and the top-entry x.
 4. 3BI selector bytes at 0x2FBE40 untouched; 3BO/3BN/3BM/3BL artifacts and
    12 sections intact; 3BJ absent.
"""
from pathlib import Path
import hashlib, struct, sys
import capstone
from capstone import x86
sys.path.insert(0, str(Path(__file__).parent))
from build_stage3bp_h4_capture_weapon_only import (
    PAYLOAD_RVA, PAYLOAD_LIMIT, SPLICE_RVA, G_ORIG_CUI_RENDER_RVA,
    HALO4_SAFE_READ_RVA, CAPTURE_FRAMING_CONST_RVA, parse_pe)

HERE = Path(__file__).parent
BASE = HERE.parent / "built" / "Stage3BO-HaloMCCVR.dll"
OUT = HERE.parent / "built" / "Stage3BP-HaloMCCVR.dll"
EXPECTED_BASE = "129a24df07edf78ca1919e2ddaa36663d15fcbb34d5981b7fc0ab1e49b107f1b"

base = bytearray(BASE.read_bytes()); out = bytearray(OUT.read_bytes())
assert hashlib.sha256(base).hexdigest() == EXPECTED_BASE, "wrong 3BO base"
assert len(base) == len(out)
pe = parse_pe(out); assert pe["n"] == 12
def off(rva):
    for s in pe["sections"]:
        if s["va"] <= rva < s["va"] + max(s["vs"], s["rs"]): return s["rp"] + rva - s["va"]
    raise AssertionError(hex(rva))

# 1. regions
diff = [i for i in range(len(base)) if base[i] != out[i]]
po, pl = off(PAYLOAD_RVA), off(PAYLOAD_LIMIT)
pay_end = max(i for i in diff if po <= i < pl) + 1
allowed = set(range(off(SPLICE_RVA), off(SPLICE_RVA) + 5)) | set(range(po, pay_end))
stray = [i for i in diff if i not in allowed]
assert not stray, f"stray changes at {[hex(i) for i in stray[:8]]}"
assert all(base[i] == 0 for i in range(po, pl)), "payload page not free in base"
assert bytes(out[off(SPLICE_RVA)+5:off(SPLICE_RVA)+7]) == b"\x90\x90"

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64); md.detail = True
def disasm(rva, size): return list(md.disasm(bytes(out[off(rva):off(rva)+size]), rva))
def riptarget(i):
    for op in i.operands:
        if op.type == x86.X86_OP_MEM and op.mem.base == x86.X86_REG_RIP:
            return i.address + i.size + op.mem.disp
    return None

# 2. splice
s = disasm(SPLICE_RVA, 5)
assert s[0].mnemonic == "call"; sel = int(s[0].op_str, 16)
assert sel == PAYLOAD_RVA

# 3. selector
data_rva = PAYLOAD_RVA + pay_end - po - 12       # 3 dwords of data at the end
inside_rva, poly_rva, half_rva = data_rva, data_rva + 4, data_rva + 8
assert struct.unpack_from("<IIf", out, off(data_rva)) == (0, 0, 0.5)
t = disasm(sel, data_rva - sel)
mn = [(i.mnemonic, i.op_str) for i in t]
by_addr = {i.address: i for i in t}
assert mn[0] == ("sub", "rsp, 0x58")
assert mn.count(("add", "rsp, 0x58")) == 2 and mn.count(("ret", "")) >= 4
assert t[1].mnemonic == "mov" and riptarget(t[1]) == G_ORIG_CUI_RENDER_RVA
assert mn[2] == ("test", "rax, rax") and mn[3][0] == "je"
# saves of the original arguments
assert ("mov", "qword ptr [rsp + 0x28], rdx") in mn and ("mov", "qword ptr [rsp + 0x30], r8") in mn \
    and ("mov", "qword ptr [rsp + 0x38], r9") in mn and ("mov", "qword ptr [rsp + 0x20], rax") in mn
# redirectActive sampled from [r15+0x3BA] before anything else touches the stream
k_scope = next(k for k, (m, o) in enumerate(mn) if m == "movzx" and o == "r10d, byte ptr [r15 + 0x3ba]")
k_safe = next(k for k, i in enumerate(t) if i.mnemonic == "call" and i.op_str == hex(HALO4_SAFE_READ_RVA))
k_orig = next(k for k, (m, o) in enumerate(mn) if m == "call" and o == "rax")
assert k_scope < k_safe < k_orig, "header read must come before the original call"
assert mn[k_safe-3:k_safe] == [("mov", "rcx, r13"), ("lea", "rdx, [rsp + 0x40]"), ("mov", "r8d, 4")]
# guards before SafeRead: redirectActive and r13
pre = mn[k_scope:k_safe]
assert ("test", "r10b, r10b") in pre and ("test", "r13, r13") in pre
# no raw dereference of the command pointer anywhere
for i in t:
    for op in i.operands:
        if op.type == x86.X86_OP_MEM:
            assert op.mem.base != x86.X86_REG_R13, f"raw [r13] at {i.address:#x}"
# pre-call polyart classification
k_pre = next(k for k, (m, o) in enumerate(mn) if m == "cmp" and o == "eax, 0x20")
assert k_safe < k_pre < k_orig
assert ("cmp", "eax, 0x1f") in mn[k_pre:k_orig]
poly_sets = [i for i in t if i.mnemonic == "mov" and riptarget(i) == poly_rva and i.op_str.endswith(", 1")]
assert len(poly_sets) == 1 and k_safe < t.index(poly_sets[0]) < k_orig, "poly := 1 only pre-call"
# the pre-call poly path checks inside first
kp = t.index(poly_sets[0])
assert any(i.mnemonic == "cmp" and riptarget(i) == inside_rva for i in t[k_pre:kp])
# restore + call
assert mn[k_orig-5:k_orig] == [("mov", "rax, qword ptr [rsp + 0x20]"), ("mov", "rdx, qword ptr [rsp + 0x28]"),
                              ("mov", "r8, qword ptr [rsp + 0x30]"), ("mov", "r9, qword ptr [rsp + 0x38]"),
                              ("mov", "rcx, rsi")]
assert mn[k_orig+1] == ("mov", "byte ptr [rsp + 0x48], al")
# the enforce subroutine: called twice (pre + post), placed after the main body
enforce_calls = [i for i in t if i.mnemonic == "call" and i.op_str.startswith("0x") and int(i.op_str, 16) != HALO4_SAFE_READ_RVA]
assert len(enforce_calls) == 2 and len({int(i.op_str, 16) for i in enforce_calls}) == 1
enf = int(enforce_calls[0].op_str, 16)
assert enf > t[k_orig].address
# post-call classification (main body only, between the original call and the subroutine)
post = [i for i in t[k_orig+1:] if i.address < enf]
assert any(i.mnemonic == "cmp" and i.op_str == "eax, 0x28" for i in post)
assert any(i.mnemonic == "cmp" and i.op_str == "word ptr [rsp + 0x42], 0xc" for i in post)
assert any(i.mnemonic == "cmp" and i.op_str == "eax, 0x29" for i in post)
inside_sets = [i.op_str[-1] for i in post if i.mnemonic == "mov" and riptarget(i) == inside_rva and i.op_str.startswith("dword ptr")]
poly_clears = [i for i in post if i.mnemonic == "mov" and riptarget(i) == poly_rva and i.op_str.endswith(", 0")]
assert sorted(inside_sets) == ["0", "1"] and len(poly_clears) == 2, (inside_sets, len(poly_clears))
assert not any(i.mnemonic == "mov" and riptarget(i) == poly_rva and i.op_str.endswith(", 1") for i in post), "poly never set post-call"
e = [i for i in t if i.address >= enf]
em = [(i.mnemonic, i.op_str) for i in e]
assert em[0] == ("test", "rsi, rsi") and ("mov", "ecx, dword ptr [rsi + 0x870]") in em
assert ("cmp", "ecx, 0x60") in em and ("imul", "rax, rax, 0x34") in em and ("lea", "rax, [rsi + rax + 0x878]") in em
assert ("movss", "xmm0, dword ptr [rax + 0x28]") in em and ("ucomiss", "xmm0, xmm0") in em
assert any(i.mnemonic == "movss" and riptarget(i) == CAPTURE_FRAMING_CONST_RVA for i in e)
assert any(i.mnemonic == "mulss" and riptarget(i) == half_rva for i in e)
assert any(i.mnemonic == "mov" and riptarget(i) == inside_rva and i.op_str.startswith("edx") for i in e)
assert any(i.mnemonic == "cmp" and riptarget(i) == poly_rva for i in e)
assert em.count(("movss", "dword ptr [rax + 0x28], xmm0")) == 2
assert ("subss", "xmm0, xmm1") in em and ("addss", "xmm0, xmm1") in em
# writes: only volatile regs, payload data, stack locals and [rax+0x28]
nonvol = {"rbx", "rbp", "rsi", "rdi", "r12", "r13", "r14", "r15", "rsp"}
for i in t:
    if i.mnemonic in ("call", "ret", "push", "pop", "sub", "add") and ("rsp" in i.op_str or i.mnemonic in ("call", "ret")):
        continue
    for op in i.operands:
        if op.type == x86.X86_OP_REG and (op.access & capstone.CS_AC_WRITE):
            assert i.reg_name(op.reg) not in nonvol, f"writes {i.reg_name(op.reg)} at {i.address:#x}"
        if op.type == x86.X86_OP_MEM and (op.access & capstone.CS_AC_WRITE):
            tg = riptarget(i)
            if tg is not None:
                assert data_rva <= tg < data_rva + 8, f"rip write outside data at {i.address:#x}"
            else:
                assert op.mem.base in (x86.X86_REG_RSP, x86.X86_REG_RAX), f"write via {i.op_str} at {i.address:#x}"
                if op.mem.base == x86.X86_REG_RAX: assert op.mem.disp == 0x28

# 4. artifacts
assert bytes(out[off(0x2FBE40):off(0x2FBE40)+4]) == bytes.fromhex("4883ec38"), "3BI selector"
assert bytes(out[off(0x199B3):off(0x199B3)+2]) == bytes.fromhex("b201"), "3BJ present"
assert out[off(0x11E76)] == 0xE8 and out[off(0x11EB6)] == 0xE8, "3BO calls"
assert bytes(out[off(0x2FB992):off(0x2FB992)+5]) == bytes.fromhex("e849daffff"), "3BN gate"
for rva in (0xD0C5, 0xD229, 0x2763C, 0x2FBA58):
    assert out[off(rva)] == 0xE8, hex(rva)
assert out[off(0x2FA2D7)] == 0xE9
print(f"PASS: Stage 3BP verified -- splice 0x53921 -> {sel:#x}; header read once via SafeRead before the "
      f"original call; 0x20/0x1F inside -> poly + offscreen before the draw; 0x28/0x29 bookkeeping; "
      f"enforce {enf:#x} called twice; data at {data_rva:#x}; 3BI/3BO/3BN artifacts intact; 3BJ absent")
