"""Stage 3CE verification - decodes the output against Stage 3CB.

 1. Byte-identical to 3CB outside exactly three regions: the 5-byte gate
    call at 0x2FB992, the payload at 0x2F9DA0..0x2F9E10 (zero in base), and
    the 4-byte selector displacement at 0x2F99DB.
 2. Gate decodes as `call <thunk>` with the thunk inside the payload.
 3. Thunk decode: sub rsp,0x28; seeds the 3BR viewport buffer (0x2F9BE8)
    from the stored capture viewport (0x2AE774); reads ONLY the live hide
    (0x2A8368), BBW/BBH (0x2AEB58/5C) and the 3BR constants (k512/khalf/
    kbias); guards NaN (jp) and <=0 (comiss/jbe) and zero backbuffer; writes
    only the shared buffer; main path ends `jmp 0x2F9B29` (the 3BR scale
    tail); the fallback re-balances rsp and ends `jmp 0x2F9A90` (the whole
    old thunk).
 4. Selector: the movss at 0x2F99D7 now targets 0x2A8368; the surrounding
    selector bytes (threshold multiply, flag tests, add/sub paths) are
    untouched.
 5. The 3BR thunk, its constants/buffer, the 3CB camera payload, the 3BX
    thunk chain and all prior artifacts are untouched; 12 sections; 3BJ
    absent.
"""
from pathlib import Path
import hashlib, struct, sys
import capstone
from capstone import x86
sys.path.insert(0, str(Path(__file__).parent))
from build_stage3br_h4_capture_scale import parse_pe, rva_off
from build_stage3ce_h4_live_capture_framing import (
    PAYLOAD_RVA, PAYLOAD_LIMIT, DEAD_3BN_SHA256, GATE_CALL_RVA, S3BR_THUNK_RVA,
    S3BR_SCALE_TAIL_RVA, CAPTURE_VIEWPORT_RVA, LIVE_HIDE_X_RVA,
    BBW_RVA, BBH_RVA, K512_RVA, KHALF_RVA, KBIAS_RVA, VPBUF_RVA,
    SELECTOR_LOAD_RVA)

HERE = Path(__file__).parent
BASE = HERE.parent / "built" / "Stage3CB-HaloMCCVR.dll"
OUT = HERE.parent / "built" / "Stage3CE-HaloMCCVR.dll"
EXPECTED_BASE = \
    "9ee60ca1b97934002473a0f970c4af8e94a79aa34cfc92116f1c06b4f5690885"

base = bytearray(BASE.read_bytes()); out = bytearray(OUT.read_bytes())
assert hashlib.sha256(base).hexdigest() == EXPECTED_BASE, "wrong 3CB base"
assert len(base) == len(out)
pe = parse_pe(out); assert pe["n"] == 12
off = lambda rva: rva_off(pe, rva)

# 1. regions
diff = [i for i in range(len(base)) if base[i] != out[i]]
allowed = (set(range(off(GATE_CALL_RVA), off(GATE_CALL_RVA) + 5)) |
           set(range(off(PAYLOAD_RVA), off(PAYLOAD_LIMIT))) |
           set(range(off(SELECTOR_LOAD_RVA) + 4, off(SELECTOR_LOAD_RVA) + 8)))
stray = [i for i in diff if i not in allowed]
assert not stray, f"stray changes at {[hex(i) for i in stray[:8]]}"
assert hashlib.sha256(bytes(base[off(PAYLOAD_RVA):off(PAYLOAD_LIMIT)])).hexdigest() == DEAD_3BN_SHA256, "base region is not the dead 3BN thunk"

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64); md.detail = True
def riptarget(i):
    for op in i.operands:
        if op.type == x86.X86_OP_MEM and op.mem.base == x86.X86_REG_RIP:
            return i.address + i.size + op.mem.disp
    return None

# 2. gate
g = list(md.disasm(bytes(out[off(GATE_CALL_RVA):off(GATE_CALL_RVA)+5]),
                   GATE_CALL_RVA))
assert len(g) == 1 and g[0].mnemonic == "call"
thunk = g[0].operands[0].imm
assert PAYLOAD_RVA <= thunk < PAYLOAD_LIMIT, hex(thunk)

# 3. thunk decode (ends at the first jmp leaving the payload; the fallback
# jmp is also outside, so walk the full payload and collect both exits)
t = list(md.disasm(bytes(out[off(thunk):off(PAYLOAD_LIMIT)]), thunk))
# trim trailing zero padding decoded as add [rax],al
while t and t[-1].bytes == b"\x00\x00":
    t.pop()
mn = [(i.mnemonic, i.op_str) for i in t]
text = " | ".join(f"{i.mnemonic} {i.op_str}" for i in t)
assert mn[0] == ("sub", "rsp, 0x28")
exits = [i for i in t if i.mnemonic == "jmp"]
assert {i.operands[0].imm for i in exits} == \
    {S3BR_SCALE_TAIL_RVA, S3BR_THUNK_RVA}, [hex(i.operands[0].imm) for i in exits]
# exactly one rsp re-balance (the fallback's add before jmp to the old thunk)
assert mn.count(("add", "rsp, 0x28")) == 1
# reads
targets = {riptarget(i) for i in t if riptarget(i) is not None}
required = {CAPTURE_VIEWPORT_RVA, CAPTURE_VIEWPORT_RVA + 0x10,
            LIVE_HIDE_X_RVA, BBW_RVA, BBH_RVA, K512_RVA, KHALF_RVA,
            KBIAS_RVA, VPBUF_RVA}
assert required <= targets, [hex(x) for x in (required - targets)]
allowed_targets = required | {VPBUF_RVA + 4, VPBUF_RVA + 8, VPBUF_RVA + 12,
                              VPBUF_RVA + 0x10}
assert targets <= allowed_targets, [hex(x) for x in (targets - allowed_targets)]
# guards
assert any(i.mnemonic == "ucomiss" for i in t)
assert any(i.mnemonic == "jp" for i in t)
assert any(i.mnemonic == "comiss" for i in t)
assert any(i.mnemonic == "jbe" for i in t)
assert any(i.mnemonic == "divss" for i in t)          # H = W*bbH/bbW
# writes go only to the shared 3BR buffer
for i in t:
    for op in i.operands:
        if op.type == x86.X86_OP_MEM and (op.access & capstone.CS_AC_WRITE):
            assert op.mem.base == x86.X86_REG_RIP and \
                VPBUF_RVA <= riptarget(i) <= VPBUF_RVA + 0x10, \
                f"stray write at {i.address:#x}"

# 4. selector
s = list(md.disasm(bytes(out[off(SELECTOR_LOAD_RVA):off(SELECTOR_LOAD_RVA)+8]),
                   SELECTOR_LOAD_RVA))
assert len(s) == 1 and s[0].mnemonic == "movss" and \
    s[0].op_str.startswith("xmm1")
assert riptarget(s[0]) == LIVE_HIDE_X_RVA
# neighbors untouched: threshold multiply and the container-X read
assert bytes(out[off(0x2F99DF):off(0x2F99DF)+3]) == bytes.fromhex("0f28d1"), \
    "movaps xmm2,xmm1 after the load changed"
assert bytes(out[off(0x2F99CD):off(0x2F99CD)+5]) == bytes.fromhex("f30f104028")

# 5. artifacts
assert bytes(out[off(S3BR_THUNK_RVA):off(S3BR_THUNK_RVA)+4]) == \
    bytes.fromhex("4883ec28"), "3BR thunk clobbered"
assert bytes(out[off(0x2F9BBD):off(0x2F9BBD)+5]) == bytes.fromhex("4883c428c3")
assert bytes(out[off(K512_RVA):off(K512_RVA)+12]) == bytes.fromhex("000000440000003fb9379e3d")
assert bytes(out[off(0x2FACB0):off(0x2FACB0)+1]) != b"\x00", "3BX thunk region"
assert out[off(0x56EBF)] == 0xE8, "3CB camera splice"
assert bytes(out[off(0x2F9D10):off(0x2F9D10)+4]) == bytes.fromhex("4883ec40"), \
    "3BU thunk clobbered"
assert out[off(0x2A9DA)] == 0xE8 and out[off(0x2A701)] == 0xE8
assert out[off(0x53921)] == 0xE8 and out[off(0x11E76)] == 0xE8
assert bytes(out[off(0x2F98A0):off(0x2F98A0)+4]) == bytes.fromhex("4883ec58"), \
    "3BP selector head"
assert bytes(out[off(0x199B3):off(0x199B3)+2]) == bytes.fromhex("b201"), "3BJ"
print(f"PASS: 3CE verified -- gate 0x2FB992 -> live thunk {thunk:#x} "
      f"(W=liveHide, TLx=(512-W)/2, H=W*bbH/bbW + bias) -> 3BR scale tail; "
      f"NaN/<=0/zero-bb fallback to the intact 3BR thunk; selector un-hide "
      f"now live (0x2A8368), neighbors byte-identical; 3CB camera + 3BX "
      f"chain + all prior artifacts intact; 3BJ absent")
