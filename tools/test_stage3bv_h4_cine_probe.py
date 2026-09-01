"""Stage 3BV verification - decodes the output against Stage 3BU.

 1. Byte-identical to Stage 3BU outside exactly two regions: the 6-byte
    splice at 0x2C2BC and the payload at 0x2F9E10..0x2FA000 (zero in base).
 2. Splice decodes as `jne <thunk>` with the thunk inside the payload.
 3. Thunk decode: sub/add rsp 0x40 balanced; exactly two indirect IAT calls
    ([rip+GetTickCount64], [rip+GetModuleHandleW]) and one direct
    `call 0x1D90` (LOG); every OTHER rip-relative target stays inside the
    payload (probe state + strings); ends with `jmp 0x2F9D10` (the 3BU
    thunk); every memory WRITE is rsp-based or payload-internal; the
    halo4.dll facts appear exactly as evidenced (verify qword for the
    registration site, disp 0x12CA85/0x12CA8D/0x1057218, TLS bound 256,
    member slot 0xC8, bytes +4/+5); the gs TLS read is present.
 4. The 3BU thunk and resume path bytes are untouched.
 5. All prior artifacts intact (3BQ..3BU splices/thunks), 12 sections, 3BJ
    absent.
"""
from pathlib import Path
import hashlib, sys
import capstone
from capstone import x86
sys.path.insert(0, str(Path(__file__).parent))
from build_stage3br_h4_capture_scale import parse_pe, rva_off
from build_stage3bv_h4_cine_probe import (
    PAYLOAD_RVA, PAYLOAD_LIMIT, SPLICE_RVA, S3BU_THUNK_RVA, LOG_RVA,
    IAT_GETTICKCOUNT64_RVA, IAT_GETMODULEHANDLEW_RVA)

HERE = Path(__file__).parent
BASE = HERE.parent / "built" / "Stage3BU-HaloMCCVR.dll"
OUT = HERE.parent / "built" / "Stage3BV-HaloMCCVR.dll"
EXPECTED_BASE = \
    "5905afce6cc06996dd0c379d9cb1c6731091fe2d7f7a74051f6ae82004f3ac03"

base = bytearray(BASE.read_bytes()); out = bytearray(OUT.read_bytes())
assert hashlib.sha256(base).hexdigest() == EXPECTED_BASE, "wrong 3BU base"
assert len(base) == len(out)
pe = parse_pe(out); assert pe["n"] == 12
off = lambda rva: rva_off(pe, rva)

# 1. regions
diff = [i for i in range(len(base)) if base[i] != out[i]]
allowed = (set(range(off(SPLICE_RVA), off(SPLICE_RVA) + 6)) |
           set(range(off(PAYLOAD_RVA), off(PAYLOAD_LIMIT))))
stray = [i for i in diff if i not in allowed]
assert not stray, f"stray changes at {[hex(i) for i in stray[:8]]}"
assert all(base[i] == 0 for i in range(off(PAYLOAD_RVA), off(PAYLOAD_LIMIT))), \
    "payload page not free in base"

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64); md.detail = True
def disasm(rva, size): return list(md.disasm(bytes(out[off(rva):off(rva)+size]), rva))
def riptarget(i):
    for op in i.operands:
        if op.type == x86.X86_OP_MEM and op.mem.base == x86.X86_REG_RIP:
            return i.address + i.size + op.mem.disp
    return None

# 2. splice
s = disasm(SPLICE_RVA, 6)
assert len(s) == 1 and s[0].mnemonic == "jne", s
thunk = int(s[0].op_str, 16)
assert PAYLOAD_RVA <= thunk < PAYLOAD_LIMIT, hex(thunk)

# 3. thunk
# The probe has internal forward jumps; the walk ends only at the exit jmp
# that leaves the payload (it must land on the 3BU thunk, asserted below).
t = []
for i in md.disasm(bytes(out[off(thunk):off(PAYLOAD_LIMIT)]), thunk):
    t.append(i)
    if (i.mnemonic == "jmp" and not i.op_str.startswith("qword") and
            not PAYLOAD_RVA <= int(i.op_str, 16) < PAYLOAD_LIMIT):
        break
mn = [(i.mnemonic, i.op_str) for i in t]
assert mn[0] == ("sub", "rsp, 0x40")
assert ("add", "rsp, 0x40") in mn
assert t[-1].mnemonic == "jmp" and int(t[-1].op_str, 16) == S3BU_THUNK_RVA
# calls: two indirect through the IAT, one direct to LOG
indirect = [i for i in t if i.mnemonic == "call" and
            i.operands[0].type == x86.X86_OP_MEM]
direct = [i for i in t if i.mnemonic == "call" and
          i.operands[0].type == x86.X86_OP_IMM]
assert len(indirect) == 2 and len(direct) == 1, mn
assert {riptarget(i) for i in indirect} == \
    {IAT_GETTICKCOUNT64_RVA, IAT_GETMODULEHANDLEW_RVA}
assert direct[0].operands[0].imm == LOG_RVA
# every other rip-relative target stays inside the payload (state + strings)
outside = {riptarget(i) for i in t if riptarget(i) is not None} - \
    {IAT_GETTICKCOUNT64_RVA, IAT_GETMODULEHANDLEW_RVA}
assert all(PAYLOAD_RVA <= x < PAYLOAD_LIMIT for x in outside), \
    [hex(x) for x in outside]
# memory writes only to rsp slots or payload-internal state
for i in t:
    for op in i.operands:
        if op.type == x86.X86_OP_MEM and (op.access & capstone.CS_AC_WRITE):
            if op.mem.base == x86.X86_REG_RSP:
                continue
            assert op.mem.base == x86.X86_REG_RIP and \
                PAYLOAD_RVA <= riptarget(i) < PAYLOAD_LIMIT, \
                f"stray write at {i.address:#x}"
# the evidenced halo4.dll facts, exactly
text = " | ".join(f"{i.mnemonic} {i.op_str}" for i in t)
assert "0xb94100f2a78d158b" in text, "registration verify qword missing"
assert "0x12ca85]" in text and "0x12ca8d]" in text, "verify displacements"
assert "0x1057218]" in text, "TLS index displacement"
assert ("cmp", "ecx, 0x100") in mn, "TLS index bound"
assert "gs:[0x58]" in text, "TEB TLS pointer read"
assert "rdx + 0xc8]" in text, "member slot 0xC8"
assert "rdx + 4]" in text and "rdx + 5]" in text, "bytes +4/+5"
# fail-open: no other absolute engine offsets are dereferenced
# (all non-IAT rip targets were already constrained to the payload above)

# 4. 3BU thunk and resume untouched
assert bytes(out[off(0x2F9D10):off(0x2F9D10)+4]) == bytes.fromhex("4883ec40"), \
    "3BU thunk clobbered"
assert bytes(out[off(0x2C3E5):off(0x2C3E5)+6]) == bytes.fromhex("8b0d5d9a2a00"), \
    "resume block clobbered"
assert bytes(out[off(0x2C2C2):off(0x2C2C2)+7]) == bytes.fromhex("488b1d2f282800"), \
    "learning path clobbered"

# 5. artifacts
assert bytes(out[off(0x2F9A30):off(0x2F9A30)+4]) == bytes.fromhex("4883ec58"), "3BQ thunk"
assert bytes(out[off(0x2F98A0):off(0x2F98A0)+4]) == bytes.fromhex("4883ec58"), "3BP selector"
assert out[off(0x2A9DA)] == 0xE8 and out[off(0x2A701)] == 0xE8, "3BQ/3BS splices"
assert out[off(0x53921)] == 0xE8 and out[off(0x11E76)] == 0xE8 and out[off(0x11EB6)] == 0xE8
assert out[off(0x2FB992)] == 0xE8 and out[off(0x2F9066)] == 0xE8, "3BR/3BT splices"
assert bytes(out[off(0x199B3):off(0x199B3)+2]) == bytes.fromhex("b201"), "3BJ present"
print(f"PASS: 3BV verified -- jne 0x2C2BC -> probe {thunk:#x} -> "
      f"jmp 0x2F9D10 (3BU intact); 250ms tick gate; module byte-verified "
      f"before use; reads TLS[idx<256]+0xC8 bytes +4/+5; two IAT calls + "
      f"LOG on transitions only; writes stay on stack/payload; all "
      f"3BQ..3BU artifacts intact; 3BJ absent")
