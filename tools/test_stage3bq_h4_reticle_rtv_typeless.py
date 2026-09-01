"""Stage 3BQ verification - decodes the output DLL, trusts nothing.

 1. Byte-identical to Stage 3BP outside the 6-byte splice at 0x2A9DA and the
    payload at 0x2F9A30; the site's r8d=0 before and reload after untouched.
 2. Splice decodes as call s3bq_create_rtv + nop.
 3. Thunk semantics: sub/add rsp 0x58; rdx and r9 saved before the first COM
    call; the first call is the exact displaced pair (mov rax,[rcx]; call
    [rax+0x48]) with r8 untouched before it; jns to done; on failure the
    device is reloaded from g_device (0x2AE290) and null-guarded, the desc is
    built at [rsp+0x28] with Format from g_xrFormat (0x2AE2A0),
    ViewDimension=4, 12 zero bytes for the union, rdx/r9 restored,
    r8=&desc, and the same vtable slot 0x48 is called again; single ret; no
    writes outside the stack frame; no non-volatile register writes.
 4. 3BP/3BO/3BN/3BM/3BL artifacts and 12 sections intact; 3BJ absent.
"""
from pathlib import Path
import hashlib, struct, sys
import capstone
from capstone import x86
sys.path.insert(0, str(Path(__file__).parent))
from build_stage3bq_h4_reticle_rtv_typeless import (
    PAYLOAD_RVA, PAYLOAD_LIMIT, SPLICE_RVA, G_DEVICE_RVA, XR_FORMAT_RVA, parse_pe)

HERE = Path(__file__).parent
BASE = HERE.parent / "built" / "Stage3BP-HaloMCCVR.dll"
OUT = HERE.parent / "built" / "Stage3BQ-HaloMCCVR.dll"
EXPECTED_BASE = "4298f4a7b9997b93123534d9c15906eeb94fb17a544a5192a1a63723765af0c5"

base = bytearray(BASE.read_bytes()); out = bytearray(OUT.read_bytes())
assert hashlib.sha256(base).hexdigest() == EXPECTED_BASE, "wrong 3BP base"
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
allowed = set(range(off(SPLICE_RVA), off(SPLICE_RVA) + 6)) | set(range(po, pay_end))
stray = [i for i in diff if i not in allowed]
assert not stray, f"stray changes at {[hex(i) for i in stray[:8]]}"
assert all(base[i] == 0 for i in range(po, pl)), "payload page not free in base"
assert bytes(out[off(0x2A9D3):off(0x2A9D3)+3]) == bytes.fromhex("4533c0"), "r8d=0 kept"
assert bytes(out[off(0x2A9E0):off(0x2A9E0)+4]) == bytes.fromhex("8b4c2450"), "reload kept"

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64); md.detail = True
def disasm(rva, size): return list(md.disasm(bytes(out[off(rva):off(rva)+size]), rva))
def riptarget(i):
    for op in i.operands:
        if op.type == x86.X86_OP_MEM and op.mem.base == x86.X86_REG_RIP:
            return i.address + i.size + op.mem.disp
    return None

# 2. splice
s = disasm(SPLICE_RVA, 6)
assert s[0].mnemonic == "call"; thunk = int(s[0].op_str, 16)
assert thunk == PAYLOAD_RVA and s[1].mnemonic == "nop"

# 3. thunk
t = disasm(thunk, pay_end - po)
t = t[:next(k for k, i in enumerate(t) if i.mnemonic == "ret") + 1]
mn = [(i.mnemonic, i.op_str) for i in t]
assert mn[0] == ("sub", "rsp, 0x58") and mn.count(("add", "rsp, 0x58")) == 1 and mn.count(("ret", "")) == 1
assert mn[1] == ("mov", "qword ptr [rsp + 0x40], rdx")
assert mn[2] == ("mov", "qword ptr [rsp + 0x48], r9")
calls = [k for k, i in enumerate(t) if i.mnemonic == "call"]
assert len(calls) == 2
k1, k2 = calls
assert mn[k1-1] == ("mov", "rax, qword ptr [rcx]") and mn[k1] == ("call", "qword ptr [rax + 0x48]")
assert not any("r8" in o for m, o in mn[:k1]), "r8 untouched before the first call (null desc)"
assert mn[k1+1] == ("test", "eax, eax") and mn[k1+2][0] == "jns"
done = int(mn[k1+2][1], 16)
# retry path
mid = t[k1+3:k2+1]
mm = [(i.mnemonic, i.op_str) for i in mid]
dev = [i for i in mid if i.mnemonic == "mov" and riptarget(i) == G_DEVICE_RVA]
assert dev and dev[0].op_str.startswith("rcx"), "device reloaded"
assert ("test", "rcx, rcx") in mm and any(m == "je" and int(o, 16) == done for m, o in mm)
fmt = [i for i in mid if i.mnemonic == "mov" and riptarget(i) == XR_FORMAT_RVA]
assert fmt and fmt[0].op_str.startswith("eax"), "format from g_xrFormat"
assert ("mov", "dword ptr [rsp + 0x28], eax") in mm
assert ("mov", "dword ptr [rsp + 0x2c], 4") in mm, "TEXTURE2D dimension"
assert ("xor", "eax, eax") in mm and ("mov", "qword ptr [rsp + 0x30], rax") in mm \
    and ("mov", "dword ptr [rsp + 0x38], eax") in mm, "union zeroed"
assert ("mov", "rdx, qword ptr [rsp + 0x40]") in mm and ("mov", "r9, qword ptr [rsp + 0x48]") in mm
assert ("lea", "r8, [rsp + 0x28]") in mm
assert mm[-2] == ("mov", "rax, qword ptr [rcx]") and mm[-1] == ("call", "qword ptr [rax + 0x48]")
assert done == t[k2+1].address, "jns lands on the epilogue"
# writes: stack frame only; volatile registers only
nonvol = {"rbx", "rbp", "rsi", "rdi", "r12", "r13", "r14", "r15"}
for i in t:
    if i.mnemonic in ("call", "ret"): continue
    for op in i.operands:
        if op.type == x86.X86_OP_REG and (op.access & capstone.CS_AC_WRITE):
            assert i.reg_name(op.reg) not in nonvol, f"writes {i.reg_name(op.reg)} at {i.address:#x}"
        if op.type == x86.X86_OP_MEM and (op.access & capstone.CS_AC_WRITE):
            assert riptarget(i) is None and op.mem.base == x86.X86_REG_RSP, f"write {i.op_str} at {i.address:#x}"

# 4. artifacts
assert bytes(out[off(0x53921):off(0x53921)+1]) == b"\xE8"
assert bytes(out[off(0x2F98A0):off(0x2F98A0)+4]) == bytes.fromhex("4883ec58"), "3BP selector"
assert out[off(0x11E76)] == 0xE8 and out[off(0x11EB6)] == 0xE8, "3BO calls"
assert bytes(out[off(0x2FB992):off(0x2FB992)+5]) == bytes.fromhex("e849daffff"), "3BN gate"
assert bytes(out[off(0x199B3):off(0x199B3)+2]) == bytes.fromhex("b201"), "3BJ present"
for rva in (0xD0C5, 0xD229, 0x2763C, 0x2FBA58):
    assert out[off(rva)] == 0xE8, hex(rva)
assert out[off(0x2FA2D7)] == 0xE9
print(f"PASS: Stage 3BQ verified -- 0x2A9DA -> call {thunk:#x}; original null-desc create first "
      f"(r8 untouched), jns; on failure device reloaded+guarded, desc Format=g_xrFormat/TEXTURE2D/"
      "MipSlice 0, rdx/r9 restored, retry via the same vtable slot; stack-only writes; artifacts intact")
