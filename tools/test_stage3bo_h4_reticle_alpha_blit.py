"""Stage 3BO verification - decodes the output DLL, trusts nothing.

 1. Byte-identical to Stage 3BN outside four regions: the 10-byte fast-path
    site (0x11E76), the 9-byte log gate (0x11EB6), the 8-byte HLSL pointer
    slot (0x2A6770) and the payload (thunks 0x2F94D0.., text 0x2F9540..).
 2. Fast-path site decodes as call s3bo_fastpath; jmp 0x11E82; 3 nops; the
    `xor bl,bl` jump target at 0x11E80 and the resume at 0x11E82 are intact.
    The thunk keeps the multisample rule, gates on ACTIVE_TITLE==4 and
    r12d==512, writes only bl, and every path ends in ret.
 3. Log gate decodes as call s3bo_loggate + 4 nops; the `je` at 0x11EBF is
    intact.  The thunk defines eax from bl, gates the same way, yields
    `cmp eax,eax` (ZF=1) for the gated case and the original
    `cmp [loggedPath],eax` otherwise; ends in ret on both paths.
 4. HLSL slot holds ImageBase+0x2F9540 and still has a DIR64 relocation; the
    text there is exactly NEW_HLSL, compiles (vs_main, ps_linearize, ps_pass)
    with d3dcompiler_47, and differs from the shipped text only inside the
    two pixel shaders (vs_main and lin() identical; the 512x512 guard present).
 5. 3BN/3BM/3BL/3BI artifacts and 12 sections intact; 3BJ absent.
"""
from pathlib import Path
import ctypes, hashlib, struct, sys
import capstone
from capstone import x86
sys.path.insert(0, str(Path(__file__).parent))
from build_stage3bo_h4_reticle_alpha_blit import (
    NEW_HLSL, OLD_HLSL, HLSL_RVA, HLSL_SLOT_RVA, HLSL_OLD_RVA, PAYLOAD_RVA,
    PAYLOAD_LIMIT, FASTPATH_SITE, LOGGATE_SITE, ACTIVE_TITLE_RVA,
    LOGGED_PATH_RVA, IMAGE_BASE, parse_pe, has_dir64_reloc)

HERE = Path(__file__).parent
BASE = HERE.parent / "built" / "Stage3BN-HaloMCCVR.dll"
OUT = HERE.parent / "built" / "Stage3BO-HaloMCCVR.dll"
EXPECTED_BASE = "1e440f34218b3e86aa03fff9405c6ea1c71b15440f64229c25bd73ec222ddb6c"

base = bytearray(BASE.read_bytes()); out = bytearray(OUT.read_bytes())
assert hashlib.sha256(base).hexdigest() == EXPECTED_BASE, "wrong 3BN base"
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
allowed = (set(range(off(FASTPATH_SITE), off(FASTPATH_SITE) + 10)) |
           set(range(off(LOGGATE_SITE), off(LOGGATE_SITE) + 9)) |
           set(range(off(HLSL_SLOT_RVA), off(HLSL_SLOT_RVA) + 8)) |
           set(range(po, pay_end)))
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
def until_all_rets(insns, n):
    k = [j for j, i in enumerate(insns) if i.mnemonic == "ret"]
    assert len(k) >= n, "too few rets"
    return insns[:k[n-1] + 1]
def writes(insn):
    regs = set()
    for op in insn.operands:
        if op.type == x86.X86_OP_REG and (op.access & capstone.CS_AC_WRITE):
            regs.add(insn.reg_name(op.reg))
        if op.type == x86.X86_OP_MEM and (op.access & capstone.CS_AC_WRITE):
            regs.add("MEM")
    return regs

# 2. fast-path site + thunk
s = disasm(FASTPATH_SITE, 19)    # 10-byte site + xor bl,bl + the 7-byte resume mov
assert s[0].mnemonic == "call"; fast = int(s[0].op_str, 16)
assert s[1].mnemonic == "jmp" and int(s[1].op_str, 16) == 0x11E82
assert [i.mnemonic for i in s[2:5]] == ["nop"] * 3
assert s[5].address == 0x11E80 and (s[5].mnemonic, s[5].op_str) == ("xor", "bl, bl")
assert s[6].address == 0x11E82 and s[6].mnemonic == "mov" and riptarget(s[6]) == 0x2AE298, "resume loads g_context"
t = until_all_rets(disasm(fast, 64), 2)
mn = [(i.mnemonic, i.op_str) for i in t]
assert mn[0] == ("cmp", "dword ptr [rdi + 0x14], 1") and mn[1][0] == "ja"
assert t[2].mnemonic == "cmp" and riptarget(t[2]) == ACTIVE_TITLE_RVA and t[2].op_str.endswith(", 4")
assert t[2].op_str.startswith("byte ptr"), "title byte compare"
assert mn[3][0] == "jne" and mn[4] == ("cmp", "r12d, 0x200") and mn[5][0] == "jne"
slow = int(mn[1][1], 16); fastl = int(mn[3][1], 16); assert int(mn[5][1], 16) == fastl
assert t[6].address == slow and mn[6] == ("xor", "bl, bl") and mn[7] == ("ret", "")
assert t[8].address == fastl and mn[8] == ("mov", "bl, 1") and mn[9] == ("ret", "")
for i in t:
    w = writes(i) - {"rflags", "eflags", "rip"}
    assert w <= {"bl"}, f"fastpath writes {w} at {i.address:#x}"

# 3. log gate + thunk
g = disasm(LOGGATE_SITE, 11)
assert g[0].mnemonic == "call"; gate = int(g[0].op_str, 16)
assert [i.mnemonic for i in g[1:5]] == ["nop"] * 4
assert g[5].address == 0x11EBF and g[5].mnemonic == "je" and int(g[5].op_str, 16) == 0x11F12
u = until_all_rets(disasm(gate, 64), 2)
mu = [(i.mnemonic, i.op_str) for i in u]
assert mu[0] == ("movzx", "eax, bl")
assert u[1].mnemonic == "cmp" and riptarget(u[1]) == ACTIVE_TITLE_RVA and u[1].op_str.startswith("byte ptr") and u[1].op_str.endswith(", 4")
assert mu[2][0] == "jne" and mu[3] == ("cmp", "r12d, 0x200") and mu[4][0] == "jne"
normal = int(mu[2][1], 16); assert int(mu[4][1], 16) == normal
assert mu[5] == ("cmp", "eax, eax") and mu[6] == ("ret", "")
assert u[7].address == normal and u[7].mnemonic == "cmp" and riptarget(u[7]) == LOGGED_PATH_RVA and u[7].op_str.endswith(", eax")
assert mu[8] == ("ret", "")
for i in u:
    w = writes(i) - {"rflags", "eflags", "rip"}
    assert w <= {"eax"}, f"loggate writes {w} at {i.address:#x}"
# thunks live in the payload and end before the text
assert PAYLOAD_RVA <= fast < gate < HLSL_RVA and u[-1].address + u[-1].size <= HLSL_RVA

# 4. HLSL
assert struct.unpack_from("<Q", out, off(HLSL_SLOT_RVA))[0] == IMAGE_BASE + HLSL_RVA
assert struct.unpack_from("<Q", base, off(HLSL_SLOT_RVA))[0] == IMAGE_BASE + HLSL_OLD_RVA
assert has_dir64_reloc(out, pe, HLSL_SLOT_RVA)
text = bytes(out[off(HLSL_RVA):off(HLSL_RVA) + len(NEW_HLSL) + 1])
assert text == NEW_HLSL + b"\0"
assert off(HLSL_RVA) + len(NEW_HLSL) == pay_end, "text is the last thing in the payload (its NUL is the free page's zero)"
assert bytes(out[off(HLSL_OLD_RVA):off(HLSL_OLD_RVA) + len(OLD_HLSL)]) == OLD_HLSL, "shipped text untouched"
old_head = OLD_HLSL.split(b"float4 ps_linearize", 1)[0]
new_head = NEW_HLSL.split(b"float4 fix(", 1)[0]
assert old_head == new_head, "vertex shader / lin() changed"
assert b"if (w != 512 || h != 512) return c;" in NEW_HLSL
assert NEW_HLSL.count(b"fix(srcTex.Sample(smp, i.uv))") == 2
d3d = ctypes.WinDLL("d3dcompiler_47")
for entry, target in (("vs_main", "vs_5_0"), ("ps_linearize", "ps_5_0"), ("ps_pass", "ps_5_0")):
    blob = ctypes.c_void_p(); err = ctypes.c_void_p()
    hr = d3d.D3DCompile(text, len(NEW_HLSL), None, None, None, entry.encode(), target.encode(),
                        0, 0, ctypes.byref(blob), ctypes.byref(err))
    assert hr == 0, f"{entry} failed to compile: {hr:#x}"

# 5. artifacts
assert bytes(out[off(0x199B3):off(0x199B3)+2]) == bytes.fromhex("b201"), "3BJ present"
assert bytes(out[off(0x2FB992):off(0x2FB992)+5]) == bytes.fromhex("e849daffff"), "3BN gate"
for rva in (0xD0C5, 0xD229, 0x2763C, 0x53921, 0x2FBA58):
    assert out[off(rva)] == 0xE8, hex(rva)
assert out[off(0x2FA2D7)] == 0xE9
assert bytes(out[off(0x2F93E0):off(0x2F93E0)+4]) == bytes.fromhex("4883ec28"), "3BN thunk"
print(f"PASS: Stage 3BO verified -- Blit fast-path -> {fast:#x} (H4 && dstH==512 -> shader path, "
      f"else original), log gate -> {gate:#x} (silent for that blit), HLSL slot -> {HLSL_RVA:#x} "
      f"({len(NEW_HLSL)} bytes, compiles, 512x512-only alpha=max(a,rgb) + un-premultiply); "
      "3BJ absent; artifacts intact")
