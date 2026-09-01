"""Stage 3BU verification - decodes the output against Stage 3BT.

 1. Byte-identical to Stage 3BT outside exactly two regions: the 6-byte
    splice at 0x2C2BC and the payload at 0x2F9D10..0x2F9E10 (zero in base).
 2. Splice decodes as `jne <thunk>` with the thunk inside the payload.
 3. Thunk decode: sub/add rsp 0x40 balanced; reads RASTER_EYE, EYE_CACHE,
    SCENE_RTV, G_CONTEXT (and nothing else global); `cmp eax,1` + `ja`
    guard; exactly three indirect calls through [rax+0x38] (GetResource),
    [rax+0x178] (CopyResource), [rax+0x10] (Release), in that order; every
    memory WRITE is rsp-based; ends with `jmp 0x2C3E5`.
 4. Every fall-through path to the resume: the not-taken splice path is
    untouched (learning bytes at 0x2C2C2 intact).
 5. All prior artifacts intact (3BQ..3BT splices/thunks), 12 sections, 3BJ
    absent.
"""
from pathlib import Path
import hashlib, sys
import capstone
from capstone import x86
sys.path.insert(0, str(Path(__file__).parent))
from build_stage3br_h4_capture_scale import parse_pe, rva_off
from build_stage3bu_h4_scene_writeback import (
    PAYLOAD_RVA, PAYLOAD_LIMIT, SPLICE_RVA, RESUME_RVA,
    RASTER_EYE_RVA, EYE_CACHE_RVA, SCENE_RTV_RVA, G_CONTEXT_RVA)

HERE = Path(__file__).parent
BASE = HERE.parent / "built" / "Stage3BT-HaloMCCVR.dll"
OUT = HERE.parent / "built" / "Stage3BU-HaloMCCVR.dll"
EXPECTED_BASE = "4a1970734c5266688d2c61490fc485f74cbd3d39a3ffc568d0832791ada2279a"

base = bytearray(BASE.read_bytes()); out = bytearray(OUT.read_bytes())
assert hashlib.sha256(base).hexdigest() == EXPECTED_BASE, "wrong 3BT base"
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
t = []
for i in md.disasm(bytes(out[off(thunk):off(PAYLOAD_LIMIT)]), thunk):
    t.append(i)
    if i.mnemonic == "jmp" and not i.op_str.startswith("qword"):
        break
mn = [(i.mnemonic, i.op_str) for i in t]
assert mn[0] == ("sub", "rsp, 0x40")
assert ("add", "rsp, 0x40") in mn
assert t[-1].mnemonic == "jmp" and int(t[-1].op_str, 16) == RESUME_RVA
# globals read
targets = {riptarget(i) for i in t if riptarget(i) is not None}
for g in (RASTER_EYE_RVA, EYE_CACHE_RVA, SCENE_RTV_RVA, G_CONTEXT_RVA):
    assert g in targets, hex(g)
assert targets <= {RASTER_EYE_RVA, EYE_CACHE_RVA, SCENE_RTV_RVA,
                   G_CONTEXT_RVA}, [hex(x) for x in targets]
# guard
assert ("cmp", "eax, 1") in mn
assert any(i.mnemonic == "ja" for i in t)
# the three COM calls, in order
calls = [i for i in t if i.mnemonic == "call"]
assert len(calls) == 3, mn
disps = []
for c in calls:
    op = c.operands[0]
    assert op.type == x86.X86_OP_MEM and c.reg_name(op.mem.base) == "rax"
    disps.append(op.mem.disp)
assert disps == [0x38, 0x178, 0x10], [hex(d) for d in disps]
# eye-cache indexing
assert any(i.mnemonic == "mov" and "rcx + rax*8" in i.op_str for i in t)
# memory writes only to rsp-based slots
for i in t:
    for op in i.operands:
        if op.type == x86.X86_OP_MEM and (op.access & capstone.CS_AC_WRITE):
            assert i.reg_name(op.mem.base) == "rsp", f"non-stack write at {i.address:#x}"

# 4. fall-through learning path untouched
assert bytes(out[off(0x2C2C2):off(0x2C2C2)+7]) == bytes.fromhex("488b1d2f282800"), \
    "learning path clobbered"
assert bytes(out[off(0x2C3E5):off(0x2C3E5)+6]) == bytes.fromhex("8b0d5d9a2a00"), \
    "resume block clobbered"

# 5. artifacts
assert bytes(out[off(0x2F9A30):off(0x2F9A30)+4]) == bytes.fromhex("4883ec58"), "3BQ thunk"
assert bytes(out[off(0x2F98A0):off(0x2F98A0)+4]) == bytes.fromhex("4883ec58"), "3BP selector"
assert out[off(0x2A9DA)] == 0xE8 and out[off(0x2A701)] == 0xE8, "3BQ/3BS splices"
assert out[off(0x53921)] == 0xE8 and out[off(0x11E76)] == 0xE8 and out[off(0x11EB6)] == 0xE8
assert out[off(0x2FB992)] == 0xE8 and out[off(0x2F9066)] == 0xE8, "3BR/3BT splices"
assert bytes(out[off(0x199B3):off(0x199B3)+2]) == bytes.fromhex("b201"), "3BJ present"
print(f"PASS: 3BU verified -- jne 0x2C2BC -> thunk {thunk:#x}; reads only "
      f"rasterEye/eyeCache/sceneRtv/context; GetResource(0x38) -> "
      f"CopyResource(0x178) -> Release(0x10); stack-only writes; resumes at "
      f"{RESUME_RVA:#x}; learning path + all 3BQ..3BT artifacts intact; 3BJ absent")
