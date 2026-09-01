"""Stage 3BM verification - decodes the output DLL, trusts nothing.

 1. Byte-identical to Stage 3BL outside the 7-byte splice at 0x2FBA58 and the
    payload at 0x2F9040.
 2. Splice decodes as call -> s3bm_tick + 2 NOPs; the tap's following
    `call s3bh_dump` is untouched.
 3. Tick semantics: stack balanced (7 pushes / sub 0x300 / add 0x300 /
    7 pops), writes -2 to s3bh_windows (0x2FB81C) and 1 to s3bh_dumped
    (0x2FB820), calls LogDirectory (0x1830), GetDesc(+0x50),
    CreateTexture2D(+0x28), CopyResource(+0x178), Map(+0x70), CreateFileW /
    WriteFile / CloseHandle through the IAT, Unmap(+0x78), Release(+0x10),
    LOG (0x1D90).  Staging desc: Usage=3, Bind=0, CPUAccess=0x20000, Misc=0.
    CreateFileW: GENERIC_WRITE, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL.
 4. The rejected 3BJ patch is absent; 3BL/3BK/3BI/3BH artifacts intact; 12
    sections.
"""
from pathlib import Path
import hashlib, struct
import capstone
from capstone import x86

HERE = Path(__file__).parent
BASE = HERE.parent / "built" / "Stage3BL-HaloMCCVR.dll"
OUT = HERE.parent / "built" / "Stage3BM-HaloMCCVR.dll"
EXPECTED_BASE = "1fd640f3151d9602951774e722681ec45ce54a5bb26e6c5b44d2c2e88583e930"
SPLICE_RVA = 0x2FBA58
PAYLOAD_RVA = 0x2F9040
PAYLOAD_LIMIT = 0x2FA000

base = bytearray(BASE.read_bytes()); out = bytearray(OUT.read_bytes())
assert hashlib.sha256(base).hexdigest() == EXPECTED_BASE, "wrong 3BL base"
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

n, secs = parse_pe(out)
assert n == 12
def off(rva):
    for name, va, vs, rp, rs in secs:
        if va <= rva < va + max(vs, rs): return rp + rva - va
    raise AssertionError(hex(rva))

# payload extent = first..last differing byte inside the page
diff = [i for i in range(len(base)) if base[i] != out[i]]
assert diff
po = off(PAYLOAD_RVA); pl = off(PAYLOAD_LIMIT)
pd = [i for i in diff if po <= i < pl]
pay_len = max(pd) + 1 - po
allowed = set(range(off(SPLICE_RVA), off(SPLICE_RVA) + 7)) | set(range(po, po + pay_len))
stray = [i for i in diff if i not in allowed]
assert not stray, f"stray changes at {[hex(i) for i in stray[:8]]}"
assert all(base[i] == 0 for i in range(po, pl)), "payload page was not free in the base"

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64); md.detail = True
def disasm(rva, size): return list(md.disasm(bytes(out[off(rva):off(rva)+size]), rva))
def riptarget(i):
    for op in i.operands:
        if op.type == x86.X86_OP_MEM and op.mem.base == x86.X86_REG_RIP:
            return i.address + i.size + op.mem.disp
    return None

# 2. splice
s = disasm(SPLICE_RVA, 16)
assert s[0].mnemonic == "call"; tick = int(s[0].op_str, 16)
assert PAYLOAD_RVA <= tick < PAYLOAD_RVA + pay_len, hex(tick)
assert s[1].mnemonic == "nop" and s[2].mnemonic == "nop"
assert s[3].mnemonic == "call" and int(s[3].op_str, 16) == 0x2FBA69, "s3bh_dump call changed"
assert s[4].mnemonic == "add" and s[4].op_str == "rsp, 0x68"

# 3. tick semantics
t = disasm(tick, PAYLOAD_RVA + pay_len - tick)
t = t[:next(k for k, i in enumerate(t) if i.mnemonic == 'ret') + 1]   # code only, strings follow
mn = [(i.mnemonic, i.op_str) for i in t]
pushes = [i for i in t if i.mnemonic == "push"]; pops = [i for i in t if i.mnemonic == "pop"]
assert len(pushes) == 7 and len(pops) == 7
assert [p.op_str for p in pushes] == list(reversed([p.op_str for p in pops]))
assert ("sub", "rsp, 0x300") in mn and ("add", "rsp, 0x300") in mn
assert mn.count(("add", "rsp, 0x300")) == 1 and t[-1].mnemonic == "ret" or any(i.mnemonic == "ret" for i in t)
writes = {}
for i in t:
    for op in i.operands:
        if op.type == x86.X86_OP_MEM and (op.access & capstone.CS_AC_WRITE):
            tg = riptarget(i)
            if tg is not None: writes.setdefault(tg, []).append(i)
assert 0x2FB81C in writes and any("0xfffffffe" in i.op_str or "-2" in i.op_str for i in writes[0x2FB81C]), "windows re-arm"
assert 0x2FB820 in writes and any(i.op_str.endswith(", 1") for i in writes[0x2FB820]), "dumped set"
for tg in writes:
    assert tg in (0x2FB81C, 0x2FB820) or PAYLOAD_RVA <= tg < PAYLOAD_LIMIT, f"writes outside scope {tg:#x}"
calls = [i for i in t if i.mnemonic == "call"]
direct = {int(i.op_str, 16) for i in calls if i.op_str.startswith("0x")}
assert 0x1830 in direct and 0x1D90 in direct, direct
iat = {riptarget(i) for i in calls if riptarget(i)}
assert {0x180220, 0x180350, 0x1800B8} <= iat, iat
vt = {i.op_str for i in calls if "rax +" in i.op_str}
assert {"qword ptr [rax + 0x50]", "qword ptr [rax + 0x28]", "qword ptr [rax + 0x178]",
        "qword ptr [rax + 0x70]", "qword ptr [rax + 0x78]", "qword ptr [rax + 0x10]"} <= vt, vt
# staging desc & CreateFileW args
assert ("mov", "dword ptr [rsp + 0x6c], 3") in mn
assert ("mov", "dword ptr [rsp + 0x70], 0") in mn
assert ("mov", "dword ptr [rsp + 0x74], 0x20000") in mn
assert ("mov", "dword ptr [rsp + 0x78], 0") in mn
assert ("mov", "edx, 0x40000000") in mn
assert ("mov", "dword ptr [rsp + 0x20], 2") in mn
assert ("mov", "dword ptr [rsp + 0x28], 0x80") in mn
assert ("mov", "r9d, 1") in mn
# name string present as UTF-16
assert "HaloMCCVR-h4-reticle-0.raw".encode("utf-16-le") in bytes(out[po:po+pay_len])

# 4. artifacts
assert bytes(out[off(0x199B3):off(0x199B3)+2]) == bytes.fromhex("b201"), "3BJ present"
for rva in (0xD0C5, 0xD229, 0x2763C, 0x53921, 0x2FB992):
    assert out[off(rva)] == 0xE8, hex(rva)
assert out[off(0x2FA2D7)] == 0xE9
assert struct.unpack_from("<4f", out, off(0x2FB800)) == struct.unpack("<4f", struct.pack("<ffff", 4134.312, 1346.196, 0, 1))
print(f"PASS: Stage 3BM verified -- {pay_len}-byte payload at {PAYLOAD_RVA:#x}, tick {tick:#x}; "
      "splice decodes; stack balanced; writes confined to s3bh_windows/s3bh_dumped/payload; "
      "IAT CreateFileW/WriteFile/CloseHandle, LogDirectory, LOG, D3D vtable calls present; 3BJ absent")
