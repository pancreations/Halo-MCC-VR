"""Static proof for the Stage 3CO reticle/theatre reconciliation."""
from pathlib import Path
import hashlib
import sys

import capstone
from capstone import x86

sys.path.insert(0, str(Path(__file__).parent))
from build_stage3br_h4_capture_scale import parse_pe, rva_off
from build_stage3co_h4_reticle_theatre_reconcile import (
    EXPECTED_INPUT_SHA256, EXPECTED_OUTPUT_SHA256, PAYLOAD_RVA, PAYLOAD_LIMIT,
    GATE_CALL_RVA, S3BR_THUNK_RVA, S3BR_SCALE_TAIL_RVA, SELECTOR_LOAD_RVA,
    CAPTURE_VIEWPORT_RVA, BASE_Y_RVA, LIVE_HIDE_X_RVA, BBW_RVA, BBH_RVA,
    K512_RVA, KHALF_RVA, VPBUF_RVA)

ROOT = Path(__file__).parent.parent
base = bytearray((ROOT / "built/Stage3CB-HaloMCCVR.dll").read_bytes())
out = bytearray((ROOT / "built/Stage3CO-HaloMCCVR.dll").read_bytes())
bt = bytearray((ROOT / "built/Stage3BT-HaloMCCVR.dll").read_bytes())
assert hashlib.sha256(base).hexdigest() == EXPECTED_INPUT_SHA256
assert hashlib.sha256(out).hexdigest() == EXPECTED_OUTPUT_SHA256
assert len(base) == len(out) == len(bt)
pe = parse_pe(out)
assert pe["n"] == 12
off = lambda rva: rva_off(pe, rva)

# Exactly three regions differ from the verified 3CB theatre base.
diff = {i for i, (a, b) in enumerate(zip(base, out)) if a != b}
allowed = (set(range(off(PAYLOAD_RVA), off(PAYLOAD_LIMIT))) |
           set(range(off(GATE_CALL_RVA), off(GATE_CALL_RVA) + 5)) |
           set(range(off(SELECTOR_LOAD_RVA) + 4,
                     off(SELECTOR_LOAD_RVA) + 8)))
assert diff and not (diff - allowed), [hex(i) for i in sorted(diff - allowed)[:8]]

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
md.detail = True


def rip_target(ins):
    for operand in ins.operands:
        if (operand.type == x86.X86_OP_MEM and
                operand.mem.base == x86.X86_REG_RIP):
            return ins.address + ins.size + operand.mem.disp
    return None


# The capture selector keeps the proven type/payload boundary and now removes
# the hide published for the current container rather than one frozen layout.
assert bytes(out[off(0x2F995D):off(0x2F9965)]) == \
    bytes.fromhex("66837c24420c752f")
selector = list(md.disasm(
    bytes(out[off(SELECTOR_LOAD_RVA):off(SELECTOR_LOAD_RVA) + 8]),
    SELECTOR_LOAD_RVA))
assert len(selector) == 1 and selector[0].mnemonic == "movss"
assert rip_target(selector[0]) == LIVE_HIDE_X_RVA

# Decode the live viewport thunk and prove its bounded inputs/outputs.
gate = list(md.disasm(bytes(out[off(GATE_CALL_RVA):off(GATE_CALL_RVA) + 5]),
                      GATE_CALL_RVA))
assert len(gate) == 1 and gate[0].mnemonic == "call"
thunk = gate[0].operands[0].imm
assert PAYLOAD_RVA <= thunk < PAYLOAD_LIMIT
instructions = list(md.disasm(
    bytes(out[off(thunk):off(PAYLOAD_LIMIT)]), thunk))
while instructions and instructions[-1].bytes == b"\x00\x00":
    instructions.pop()
external_exit_indices = [
    index for index, ins in enumerate(instructions)
    if (ins.mnemonic == "jmp" and ins.operands[0].type == x86.X86_OP_IMM and
        ins.operands[0].imm in {S3BR_SCALE_TAIL_RVA, S3BR_THUNK_RVA})]
assert len(external_exit_indices) == 2
# Bytes after the fallback's final external jump are aligned float data, not
# executable instructions. Do not ask Capstone to assign operand access to it.
instructions = instructions[:external_exit_indices[-1] + 1]
assert (instructions[0].mnemonic, instructions[0].op_str) == \
    ("sub", "rsp, 0x28")
exits = {ins.operands[0].imm for ins in instructions
         if ins.mnemonic == "jmp"}
assert exits == {S3BR_SCALE_TAIL_RVA, S3BR_THUNK_RVA}
assert sum(ins.mnemonic == "add" and ins.op_str == "rsp, 0x28"
           for ins in instructions) == 1

targets = {rip_target(ins) for ins in instructions
           if rip_target(ins) is not None}
required = {CAPTURE_VIEWPORT_RVA, CAPTURE_VIEWPORT_RVA + 0x10,
            BASE_Y_RVA, LIVE_HIDE_X_RVA, BBW_RVA, BBH_RVA,
            K512_RVA, KHALF_RVA, VPBUF_RVA}
assert required <= targets, [hex(value) for value in required - targets]
allowed_targets = required | {VPBUF_RVA + 4, VPBUF_RVA + 8,
                              VPBUF_RVA + 12, VPBUF_RVA + 0x10}
# The one private proportional-bias constant lives inside this payload.
assert all(target in allowed_targets or PAYLOAD_RVA <= target < PAYLOAD_LIMIT
           for target in targets)
assert any(ins.mnemonic == "ucomiss" for ins in instructions)
assert any(ins.mnemonic == "jp" for ins in instructions)
assert any(ins.mnemonic == "comiss" for ins in instructions)
assert any(ins.mnemonic == "jbe" for ins in instructions)
assert sum(ins.mnemonic == "divss" for ins in instructions) == 2
for ins in instructions:
    for operand in ins.operands:
        if (operand.type == x86.X86_OP_MEM and
                operand.access & capstone.CS_AC_WRITE):
            assert operand.mem.base == x86.X86_REG_RIP
            assert VPBUF_RVA <= rip_target(ins) <= VPBUF_RVA + 0x10

# The complete authored-reticle machinery remains the proven 3BT chain except
# for the two explicit live-layout inputs above.
capture_regions = (
    (0x00011A00, 0x00011F00), (0x00027600, 0x00027700),
    (0x00053800, 0x00053A00), (0x0000D0A0, 0x0000D260),
    (0x002F9000, 0x002F9D10), (0x002FB800, 0x002FBFF0),
    (0x002FA000, 0x002FA36C))
exceptions = (set(range(off(PAYLOAD_RVA), off(PAYLOAD_LIMIT))) |
              set(range(off(GATE_CALL_RVA), off(GATE_CALL_RVA) + 5)) |
              set(range(off(SELECTOR_LOAD_RVA) + 4,
                        off(SELECTOR_LOAD_RVA) + 8)))
for lo, hi in capture_regions:
    for index in range(off(lo), off(hi)):
        if index not in exceptions:
            assert out[index] == bt[index], f"3BT chain differs at {index:#x}"

# Every theatre/cutscene byte remains exactly Stage 3CB, including 3BU's
# scene write-back and the 3BX classifier feeding the 3CB camera wrapper.
keep = ((0x0002C2BC, 6), (0x00056EBF, 5), (0x00068111, 5),
        (0x001890D4, 4), (0x002F9D10, 0x80), (0x002F9E10, 0x80),
        (0x002FA36C, 0x94), (0x002FAAA0, 0x200),
        (0x002FACB0, 0x300))
for rva, size in keep:
    assert bytes(out[off(rva):off(rva) + size]) == \
        bytes(base[off(rva):off(rva) + size])

assert bytes(out[off(0x199B3):off(0x199B3) + 2]) == bytes.fromhex("b201")
print("PASS: Stage 3CO = exact 3CB cutscene/theatre stack + complete 3BU/3BT "
      "authored-reticle chain; only per-container un-hide and viewport framing "
      "follow the live Halo 4 layout; invalid values fall back to accepted "
      "3BR; the unsupported 3CN discriminator removal is absent")
