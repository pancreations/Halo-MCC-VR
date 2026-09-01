"""Stage 3BX verification - decodes the output against Stage 3BW.

 1. Byte-identical to 3BW outside exactly two regions: the 6-byte splice at
    0x2C2BC and the payload at 0x2FACB0..0x2FB000 (zero in base). The 3BW
    capability grants (0x1F3 at 0x68112 / 0x1890D4) are therefore intact by
    construction, and re-asserted below.
 2. Splice decodes as `jne <thunk>` inside the new payload; thunk exits with
    `jmp 0x2F9D10` (the 3BU write-back).
 3. The ODST rule is present and complete: the camera is taken from engine
    TLS block +0x58, its type word is compared against 6, all four current
    limits (+0xC8/+0xCC/+0xD0/+0xD4) and all four rates (+0xD8/+0xDC/+0xE0/
    +0xE4) are tested by `and 0x7FFFFFFF` + `cmp 0x38D1B717` + `ja`, and the
    tick count (+0xF0) is range-checked (js / cmp 360000 / ja) before any
    rate is trusted.
 4. Publication safety: exactly one call each to LOG, Generation,
    PublishCinematicControl and PublishCutsceneTheaterProjection; the
    Unknown state (6) and the no-proof states (<3) skip publication
    (`cmp eax,3`/`jb`, `cmp eax,6`/`je` before the Generation call); the
    control argument can only be 1 or 2 and is 2 only on state 4.
 5. Probe shape retained (250 ms gate, registration verify qword, TLS bound,
    member +0xC8, in-progress byte +5); every memory write is rsp-based or
    payload-internal; prior artifacts intact; 12 sections; 3BJ absent.
"""
from pathlib import Path
import hashlib, struct, sys
import capstone
from capstone import x86
sys.path.insert(0, str(Path(__file__).parent))
from build_stage3br_h4_capture_scale import parse_pe, rva_off
from build_stage3bx_h4_look_constraints import (
    PAYLOAD_RVA, PAYLOAD_LIMIT, SPLICE_RVA, S3BW_THUNK_RVA, S3BU_THUNK_RVA,
    LOG_RVA, IAT_GETTICKCOUNT64_RVA, IAT_GETMODULEHANDLEW_RVA,
    PUB_CTRL_RVA, PUB_PROJ_RVA, GEN_FN_RVA, GTR_RVA, BBW_RVA, BBH_RVA,
    RUNTIME_CAPS_IMM_RVA, REGISTRY_CAPS_RVA)

HERE = Path(__file__).parent
BASE = HERE.parent / "built" / "Stage3BW-HaloMCCVR.dll"
OUT = HERE.parent / "built" / "Stage3BX-HaloMCCVR.dll"
EXPECTED_BASE = \
    "9d6bba764e93dd6cd7b2482e0131765057196f92da725836277497f4996620f1"
EPSILON_BITS = 0x38D1B717

base = bytearray(BASE.read_bytes()); out = bytearray(OUT.read_bytes())
assert hashlib.sha256(base).hexdigest() == EXPECTED_BASE, "wrong 3BW base"
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
assert struct.unpack_from("<I", out, off(RUNTIME_CAPS_IMM_RVA) + 1)[0] == 0x1F3
assert struct.unpack_from("<I", out, off(REGISTRY_CAPS_RVA))[0] == 0x1F3

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64); md.detail = True
def disasm(rva, size): return list(md.disasm(bytes(out[off(rva):off(rva)+size]), rva))
def riptarget(i):
    for op in i.operands:
        if op.type == x86.X86_OP_MEM and op.mem.base == x86.X86_REG_RIP:
            return i.address + i.size + op.mem.disp
    return None

# 2. splice + exit
s = disasm(SPLICE_RVA, 6)
assert len(s) == 1 and s[0].mnemonic == "jne", s
thunk = int(s[0].op_str, 16)
assert PAYLOAD_RVA <= thunk < PAYLOAD_LIMIT, hex(thunk)
t = []
for i in md.disasm(bytes(out[off(thunk):off(PAYLOAD_LIMIT)]), thunk):
    t.append(i)
    if (i.mnemonic == "jmp" and not i.op_str.startswith("qword") and
            not PAYLOAD_RVA <= int(i.op_str, 16) < PAYLOAD_LIMIT):
        break
mn = [(i.mnemonic, i.op_str) for i in t]
text = " | ".join(f"{i.mnemonic} {i.op_str}" for i in t)
assert mn[0] == ("sub", "rsp, 0x50") and ("add", "rsp, 0x50") in mn
assert t[-1].mnemonic == "jmp" and int(t[-1].op_str, 16) == S3BU_THUNK_RVA

# 3. the ODST rule, complete
assert "r11 + 0x58]" in text, "cinematic camera not read from TLS block +0x58"
assert ("cmp", "word ptr [rdx + 2], 6") in mn, "camera type check missing"
limit_disps = [0xC8, 0xCC, 0xD0, 0xD4]
rate_disps = [0xD8, 0xDC, 0xE0, 0xE4]
read_disps = [op.mem.disp for i in t for op in i.operands
              if op.type == x86.X86_OP_MEM and
              i.reg_name(op.mem.base) == "rdx" and not (op.access & capstone.CS_AC_WRITE)]
for d in limit_disps + rate_disps + [0xF0]:
    assert d in read_disps, f"constraint field {d:#x} never read"
masks = [i for i in t if i.mnemonic == "and" and i.op_str.endswith("0x7fffffff")]
cmps = [i for i in t if i.mnemonic == "cmp" and
        i.operands[-1].type == x86.X86_OP_IMM and
        i.operands[-1].imm == EPSILON_BITS]
assert len(masks) == 8 and len(cmps) == 8, (len(masks), len(cmps))
assert sum(1 for i in t if i.mnemonic == "ja") >= 8, "freedom branches missing"
# tick range check precedes the rate tests
tickidx = min(k for k, i in enumerate(t) if any(
    op.type == x86.X86_OP_MEM and op.mem.disp == 0xF0 for op in i.operands))
rateidx = min(k for k, i in enumerate(t) if any(
    op.type == x86.X86_OP_MEM and op.mem.disp == 0xD8 and
    i.reg_name(op.mem.base) == "rdx" for op in i.operands))
assert tickidx < rateidx, "rates trusted before the tick range check"
assert any(i.mnemonic == "js" for i in t[tickidx:rateidx]), "negative ticks unguarded"
assert any(i.mnemonic == "cmp" and i.operands[-1].type == x86.X86_OP_IMM and
           i.operands[-1].imm == 360000 for i in t[tickidx:rateidx]), \
    "tick upper bound missing"

# 4. publication safety
direct = [i for i in t if i.mnemonic == "call" and
          i.operands[0].type == x86.X86_OP_IMM]
targets = [i.operands[0].imm for i in direct]
assert sorted(targets) == sorted([LOG_RVA, GEN_FN_RVA, PUB_CTRL_RVA,
                                  PUB_PROJ_RVA]), [hex(x) for x in targets]
genidx = [k for k, i in enumerate(t) if i.mnemonic == "call" and
          i.operands[0].type == x86.X86_OP_IMM and
          i.operands[0].imm == GEN_FN_RVA][0]
pre = t[:genidx]
premn = [(i.mnemonic, i.op_str) for i in pre]
assert ("cmp", "eax, 3") in premn and any(i.mnemonic == "jb" for i in pre), \
    "no-proof states not skipped"
assert ("cmp", "eax, 6") in premn and any(i.mnemonic == "je" for i in pre), \
    "Unknown state not skipped"
assert any(riptarget(i) == GTR_RVA for i in pre[-4:]) and \
    ("mov", "edx, 4") in premn, "Generation call args"
ctrlidx = [k for k, i in enumerate(t) if i.mnemonic == "call" and
           i.operands[0].type == x86.X86_OP_IMM and
           i.operands[0].imm == PUB_CTRL_RVA][0]
between = [(i.mnemonic, i.op_str) for i in t[genidx:ctrlidx]]
assert ("mov", "r8d, 1") in between and ("mov", "r8d, 2") in between, between
assert ("cmp", "r8d, 4") in between, "AuthoredLocked not gated on state 4"
assert ("mov", "ecx, 4") in between, "title arg"
# aspect publication
assert any(riptarget(i) == BBW_RVA for i in t) and \
    any(riptarget(i) == BBH_RVA for i in t)
assert any(i.mnemonic == "divss" for i in t)

# 5. probe shape + write discipline
assert "0xb94100f2a78d158b" in text
assert "0x12ca85]" in text and "0x12ca8d]" in text and "0x1057218]" in text
assert ("cmp", "ecx, 0x100") in mn and "gs:[0x58]" in text
assert "r11 + 0xc8]" in text and "rdx + 5]" in text
for i in t:
    for op in i.operands:
        if op.type == x86.X86_OP_MEM and (op.access & capstone.CS_AC_WRITE):
            if op.mem.base == x86.X86_REG_RSP:
                continue
            assert op.mem.base == x86.X86_REG_RIP and \
                PAYLOAD_RVA <= riptarget(i) < PAYLOAD_LIMIT, \
                f"stray write at {i.address:#x}"
assert bytes(out[off(S3BW_THUNK_RVA):off(S3BW_THUNK_RVA)+4]) == \
    bytes.fromhex("4883ec50"), "3BW thunk clobbered"
assert bytes(out[off(S3BU_THUNK_RVA):off(S3BU_THUNK_RVA)+4]) == \
    bytes.fromhex("4883ec40"), "3BU thunk clobbered"
assert bytes(out[off(0x2C3E5):off(0x2C3E5)+6]) == bytes.fromhex("8b0d5d9a2a00")
assert bytes(out[off(0x2C2C2):off(0x2C2C2)+7]) == bytes.fromhex("488b1d2f282800")
assert bytes(out[off(0x2F9A30):off(0x2F9A30)+4]) == bytes.fromhex("4883ec58")
assert bytes(out[off(0x2F98A0):off(0x2F98A0)+4]) == bytes.fromhex("4883ec58")
assert out[off(0x2A9DA)] == 0xE8 and out[off(0x2A701)] == 0xE8
assert out[off(0x53921)] == 0xE8 and out[off(0x11E76)] == 0xE8 and out[off(0x11EB6)] == 0xE8
assert out[off(0x2FB992)] == 0xE8 and out[off(0x2F9066)] == 0xE8
assert bytes(out[off(0x199B3):off(0x199B3)+2]) == bytes.fromhex("b201"), "3BJ present"
print(f"PASS: 3BX verified -- jne 0x2C2BC -> {thunk:#x} -> jmp 0x2F9D10; "
      f"ODST rule complete (camera TLS+0x58, type==6, 4 limits + 4 rates via "
      f"|x|>1e-4, tick range before rates); AuthoredLocked only on state 4; "
      f"Unknown/no-proof publish nothing; capability grants intact; all "
      f"3BQ..3BW artifacts intact; 3BJ absent")
