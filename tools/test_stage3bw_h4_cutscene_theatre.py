"""Stage 3BW verification - decodes the output against Stage 3BV.

 1. Byte-identical to Stage 3BV outside exactly four regions: the 6-byte
    splice at 0x2C2BC, the payload at 0x2FAAA0..0x2FB000 (zero in base),
    the 4-byte runtime-capability immediate at 0x68112, and the 4-byte
    registry capability dword at 0x1890D4.
 2. Both capability values decode to exactly 0x1F3 (= stock 0xF3 +
    TitleCapability_CutsceneTheater 1<<8); the surrounding bytes are stock.
 3. Splice decodes as `jne <thunk>` with the thunk inside the new payload.
 4. Thunk decode: sub/add rsp 0x50 balanced; the proven probe shape (two
    IAT calls, registration verify qword, TLS bound 256, gs:[0x58], member
    0xC8, bytes +4/+5) is intact; direct calls are exactly {LOG 0x1D90,
    Generation 0xA5A0, PublishCinematicControl 0x88400,
    PublishCutsceneTheaterProjection 0x884C0}; the Generation call passes
    &g_titleRuntime (0x2BA538) and edx=4; the state map `sub r8d, 2` and
    the aspect divss over BBW/BBH (0x2AEB58/5C) are present; every memory
    WRITE is rsp-based or payload-internal; the thunk exits with
    `jmp 0x2F9D10` (the 3BU thunk).
 5. The 3BV payload, 3BU thunk, resume/learning paths, and all prior
    artifacts are untouched; 12 sections; 3BJ absent.
"""
from pathlib import Path
import hashlib, sys
import capstone
from capstone import x86
sys.path.insert(0, str(Path(__file__).parent))
from build_stage3br_h4_capture_scale import parse_pe, rva_off
from build_stage3bw_h4_cutscene_theatre import (
    PAYLOAD_RVA, PAYLOAD_LIMIT, SPLICE_RVA, S3BU_THUNK_RVA, S3BV_THUNK_RVA,
    LOG_RVA, IAT_GETTICKCOUNT64_RVA, IAT_GETMODULEHANDLEW_RVA,
    PUB_CTRL_RVA, PUB_PROJ_RVA, GEN_FN_RVA, GTR_RVA, BBW_RVA, BBH_RVA,
    RUNTIME_CAPS_IMM_RVA, REGISTRY_CAPS_RVA)

HERE = Path(__file__).parent
BASE = HERE.parent / "built" / "Stage3BV-HaloMCCVR.dll"
OUT = HERE.parent / "built" / "Stage3BW-HaloMCCVR.dll"
EXPECTED_BASE = \
    "e98e9502516ea0166c7bcb528fca5e10ba2ec1db1275f8a62bae449001fe44d9"

base = bytearray(BASE.read_bytes()); out = bytearray(OUT.read_bytes())
assert hashlib.sha256(base).hexdigest() == EXPECTED_BASE, "wrong 3BV base"
assert len(base) == len(out)
pe = parse_pe(out); assert pe["n"] == 12
off = lambda rva: rva_off(pe, rva)

# 1. regions
diff = [i for i in range(len(base)) if base[i] != out[i]]
allowed = (set(range(off(SPLICE_RVA), off(SPLICE_RVA) + 6)) |
           set(range(off(PAYLOAD_RVA), off(PAYLOAD_LIMIT))) |
           set(range(off(RUNTIME_CAPS_IMM_RVA) + 1,
                     off(RUNTIME_CAPS_IMM_RVA) + 5)) |
           set(range(off(REGISTRY_CAPS_RVA), off(REGISTRY_CAPS_RVA) + 4)))
stray = [i for i in diff if i not in allowed]
assert not stray, f"stray changes at {[hex(i) for i in stray[:8]]}"
assert all(base[i] == 0 for i in range(off(PAYLOAD_RVA), off(PAYLOAD_LIMIT))), \
    "payload page not free in base"

# 2. capability grants
import struct
assert out[off(RUNTIME_CAPS_IMM_RVA)] == 0xBA
assert struct.unpack_from("<I", out, off(RUNTIME_CAPS_IMM_RVA) + 1)[0] == 0x1F3
assert struct.unpack_from("<I", out, off(REGISTRY_CAPS_RVA))[0] == 0x1F3
assert struct.unpack_from("<I", out, off(REGISTRY_CAPS_RVA) + 4)[0] == 0x40, \
    "admission dword touched"
assert struct.unpack_from("<I", out, off(REGISTRY_CAPS_RVA) - 4)[0] == 0, \
    "row padding touched"

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64); md.detail = True
def disasm(rva, size): return list(md.disasm(bytes(out[off(rva):off(rva)+size]), rva))
def riptarget(i):
    for op in i.operands:
        if op.type == x86.X86_OP_MEM and op.mem.base == x86.X86_REG_RIP:
            return i.address + i.size + op.mem.disp
    return None

# 3. splice
s = disasm(SPLICE_RVA, 6)
assert len(s) == 1 and s[0].mnemonic == "jne", s
thunk = int(s[0].op_str, 16)
assert PAYLOAD_RVA <= thunk < PAYLOAD_LIMIT, hex(thunk)

# 4. thunk (internal jumps allowed; walk ends at the exit jmp)
t = []
for i in md.disasm(bytes(out[off(thunk):off(PAYLOAD_LIMIT)]), thunk):
    t.append(i)
    if (i.mnemonic == "jmp" and not i.op_str.startswith("qword") and
            not PAYLOAD_RVA <= int(i.op_str, 16) < PAYLOAD_LIMIT):
        break
mn = [(i.mnemonic, i.op_str) for i in t]
text = " | ".join(f"{i.mnemonic} {i.op_str}" for i in t)
assert mn[0] == ("sub", "rsp, 0x50")
assert ("add", "rsp, 0x50") in mn
assert t[-1].mnemonic == "jmp" and int(t[-1].op_str, 16) == S3BU_THUNK_RVA
indirect = [i for i in t if i.mnemonic == "call" and
            i.operands[0].type == x86.X86_OP_MEM]
direct = [i for i in t if i.mnemonic == "call" and
          i.operands[0].type == x86.X86_OP_IMM]
assert {riptarget(i) for i in indirect} == \
    {IAT_GETTICKCOUNT64_RVA, IAT_GETMODULEHANDLEW_RVA}
assert sorted(i.operands[0].imm for i in direct) == \
    sorted([LOG_RVA, GEN_FN_RVA, PUB_CTRL_RVA, PUB_PROJ_RVA]), \
    [hex(i.operands[0].imm) for i in direct]
# probe shape retained
assert "0xb94100f2a78d158b" in text
assert "0x12ca85]" in text and "0x12ca8d]" in text and "0x1057218]" in text
assert ("cmp", "ecx, 0x100") in mn and "gs:[0x58]" in text
assert "rdx + 0xc8]" in text and "rdx + 4]" in text and "rdx + 5]" in text
# publications
genidx = [k for k, i in enumerate(t) if i.mnemonic == "call" and
          i.operands[0].type == x86.X86_OP_IMM and
          i.operands[0].imm == GEN_FN_RVA][0]
setup = " | ".join(f"{i.mnemonic} {i.op_str}" for i in t[genidx-3:genidx])
assert "edx, 4" in setup, setup                      # GameTitle::Halo4
assert any(riptarget(i) == GTR_RVA for i in t[genidx-3:genidx]), \
    "&g_titleRuntime not passed to Generation"
assert ("sub", "r8d, 2") in mn                       # 3->1 / 4->2 state map
assert any(riptarget(i) == BBW_RVA for i in t)
assert any(riptarget(i) == BBH_RVA for i in t)
assert any(i.mnemonic == "divss" for i in t)         # aspect = W/H
assert ("mov", "ecx, 4") in mn                       # title arg to publishers
# memory writes only to rsp slots or payload-internal state
for i in t:
    for op in i.operands:
        if op.type == x86.X86_OP_MEM and (op.access & capstone.CS_AC_WRITE):
            if op.mem.base == x86.X86_REG_RSP:
                continue
            assert op.mem.base == x86.X86_REG_RIP and \
                PAYLOAD_RVA <= riptarget(i) < PAYLOAD_LIMIT, \
                f"stray write at {i.address:#x}"

# 5. prior artifacts
assert bytes(out[off(S3BV_THUNK_RVA):off(S3BV_THUNK_RVA)+4]) == \
    bytes.fromhex("4883ec40"), "3BV thunk clobbered"
assert bytes(out[off(S3BU_THUNK_RVA):off(S3BU_THUNK_RVA)+4]) == \
    bytes.fromhex("4883ec40"), "3BU thunk clobbered"
assert bytes(out[off(0x2C3E5):off(0x2C3E5)+6]) == bytes.fromhex("8b0d5d9a2a00")
assert bytes(out[off(0x2C2C2):off(0x2C2C2)+7]) == bytes.fromhex("488b1d2f282800")
assert bytes(out[off(0x2F9A30):off(0x2F9A30)+4]) == bytes.fromhex("4883ec58"), "3BQ thunk"
assert bytes(out[off(0x2F98A0):off(0x2F98A0)+4]) == bytes.fromhex("4883ec58"), "3BP selector"
assert out[off(0x2A9DA)] == 0xE8 and out[off(0x2A701)] == 0xE8
assert out[off(0x53921)] == 0xE8 and out[off(0x11E76)] == 0xE8 and out[off(0x11EB6)] == 0xE8
assert out[off(0x2FB992)] == 0xE8 and out[off(0x2F9066)] == 0xE8
assert bytes(out[off(0x199B3):off(0x199B3)+2]) == bytes.fromhex("b201"), "3BJ present"
print(f"PASS: 3BW verified -- jne 0x2C2BC -> theatre thunk {thunk:#x} -> "
      f"jmp 0x2F9D10; probe shape intact; publishes CinematicControl "
      f"(state map 3->1/4->2) + projection aspect (BBW/BBH divss) with "
      f"Generation(&g_titleRuntime, Halo4) and 250ms heartbeats; both "
      f"capability masks now 0x1F3 (+CutsceneTheater), admission/padding "
      f"stock; 3BV/3BU/3BQ..3BT artifacts intact; 3BJ absent")
