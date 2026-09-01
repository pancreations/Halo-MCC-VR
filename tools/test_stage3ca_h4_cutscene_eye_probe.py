"""Static verification for Stage 3CA's diagnostic-only cutscene eye probe."""
from pathlib import Path
import hashlib
import sys

import capstone
from capstone import x86

sys.path.insert(0, str(Path(__file__).parent))
from build_stage3ca_h4_cutscene_eye_probe import (
    EXPECTED_INPUT_SHA256, SPLICE_RVA, S3BX_THUNK_RVA, S3BX_STATE_RVA,
    VALIDATION_DONE_RVA, VALIDATION_FN_RVA, VALIDATION_FN_HEAD,
    PAYLOAD_RVA, PAYLOAD_LIMIT, parse_pe, rva_off)

HERE = Path(__file__).parent
BASE = HERE.parent / "built" / "Stage3BX-HaloMCCVR.dll"
OUT = HERE.parent / "built" / "Stage3CA-HaloMCCVR.dll"
base = bytearray(BASE.read_bytes())
out = bytearray(OUT.read_bytes())
assert hashlib.sha256(base).hexdigest() == EXPECTED_INPUT_SHA256
assert len(base) == len(out)
pe = parse_pe(out)
assert pe["n"] == 12
off = lambda rva: rva_off(pe, rva)

diff = [index for index in range(len(base)) if base[index] != out[index]]
allowed = (set(range(off(SPLICE_RVA), off(SPLICE_RVA) + 6)) |
           set(range(off(PAYLOAD_RVA), off(PAYLOAD_LIMIT))))
assert not [index for index in diff if index not in allowed]
assert all(base[index] == 0
           for index in range(off(PAYLOAD_RVA), off(PAYLOAD_LIMIT)))
assert bytes(out[off(VALIDATION_FN_RVA):off(VALIDATION_FN_RVA) +
                 len(VALIDATION_FN_HEAD)]) == VALIDATION_FN_HEAD

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
md.detail = True
splice = list(md.disasm(bytes(out[off(SPLICE_RVA):off(SPLICE_RVA) + 6]),
                         SPLICE_RVA))
assert len(splice) == 1 and splice[0].mnemonic == "jne"
thunk = splice[0].operands[0].imm
assert PAYLOAD_RVA <= thunk < PAYLOAD_LIMIT

instructions = list(md.disasm(bytes(out[off(thunk):off(PAYLOAD_LIMIT)]), thunk))
instructions = instructions[:next(i for i, ins in enumerate(instructions)
                                 if ins.mnemonic == "jmp" and
                                 ins.operands[0].type == x86.X86_OP_IMM and
                                 ins.operands[0].imm == S3BX_THUNK_RVA) + 1]

def rip_target(ins):
    for operand in ins.operands:
        if operand.type == x86.X86_OP_MEM and operand.mem.base == x86.X86_REG_RIP:
            return ins.address + ins.size + operand.mem.disp
    return None

assert any(ins.mnemonic == "movzx" and rip_target(ins) == S3BX_STATE_RVA
           for ins in instructions)
assert any(ins.mnemonic == "cmp" and ins.operands[-1].type == x86.X86_OP_IMM and
           ins.operands[-1].imm == 4 for ins in instructions)
writes = [(ins, rip_target(ins)) for ins in instructions
          for operand in ins.operands
          if operand.type == x86.X86_OP_MEM and
          (operand.access & capstone.CS_AC_WRITE)]
assert sum(1 for _ins, target in writes if target == VALIDATION_DONE_RVA) == 1
local_writes = [(ins, target) for ins, target in writes
                if PAYLOAD_RVA <= target < PAYLOAD_LIMIT]
assert len(local_writes) == 2
assert all(target in (VALIDATION_DONE_RVA,) or
           PAYLOAD_RVA <= target < PAYLOAD_LIMIT for _ins, target in writes)
assert instructions[-1].mnemonic == "jmp"
assert instructions[-1].operands[0].imm == S3BX_THUNK_RVA

# No camera, snapshot, FOV, capture, compositor, capability or other-title byte
# is patched.  The full accepted BX classifier/writeback chain remains present.
assert bytes(out[off(0x2FACB0):off(0x2FACB0) + 4]) == bytes.fromhex("4883ec50")
assert bytes(out[off(0x2F9D10):off(0x2F9D10) + 4]) == bytes.fromhex("4883ec40")
assert bytes(out[off(0x56EBF):off(0x56EBF) + 5]) == bytes.fromhex("e89c7bfdff")
assert out[off(0x199B3):off(0x199B3) + 2] == bytes.fromhex("b201")

print(f"PASS: 3CA diagnostic verified -- exact 3BX base; state-4 one-shot "
      f"reset of existing eye validator {VALIDATION_DONE_RVA:#x}; chains to "
      f"BX {S3BX_THUNK_RVA:#x}; no camera/FOV/capture/gameplay/title behavior changed")
