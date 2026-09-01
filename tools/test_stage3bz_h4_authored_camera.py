"""Static verification for Stage 3BZ's Halo 4 authored-camera gate."""
from pathlib import Path
import hashlib
import struct
import sys

import capstone
from capstone import x86

sys.path.insert(0, str(Path(__file__).parent))
from build_stage3bz_h4_authored_camera import (
    EXPECTED_INPUT_SHA256, CALL_SITE_RVA, SNAPSHOT_FN_RVA, S3BX_STATE_RVA,
    PAYLOAD_RVA, PAYLOAD_LIMIT, parse_pe, rva_off)

HERE = Path(__file__).parent
BASE = HERE.parent / "built" / "Stage3BX-HaloMCCVR.dll"
OUT = HERE.parent / "built" / "Stage3BZ-HaloMCCVR.dll"
base = bytearray(BASE.read_bytes())
out = bytearray(OUT.read_bytes())
assert hashlib.sha256(base).hexdigest() == EXPECTED_INPUT_SHA256
assert len(base) == len(out)
pe = parse_pe(out)
assert pe["n"] == 12
off = lambda rva: rva_off(pe, rva)

# Failed 3BY is not in the ancestry: compare directly with exact 3BX and admit
# only the one call plus the originally zero payload gap.
diff = [index for index in range(len(base)) if base[index] != out[index]]
allowed = (set(range(off(CALL_SITE_RVA), off(CALL_SITE_RVA) + 5)) |
           set(range(off(PAYLOAD_RVA), off(PAYLOAD_LIMIT))))
assert not [index for index in diff if index not in allowed]
assert all(base[index] == 0
           for index in range(off(PAYLOAD_RVA), off(PAYLOAD_LIMIT)))

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
md.detail = True
call = list(md.disasm(bytes(out[off(CALL_SITE_RVA):off(CALL_SITE_RVA) + 5]),
                      CALL_SITE_RVA))
assert len(call) == 1 and call[0].mnemonic == "call"
thunk = call[0].operands[0].imm
assert PAYLOAD_RVA <= thunk < PAYLOAD_LIMIT

instructions = list(md.disasm(bytes(out[off(thunk):off(PAYLOAD_LIMIT)]), thunk))
instructions = instructions[:next(i for i, ins in enumerate(instructions)
                                 if ins.mnemonic == "ret") + 1]
calls = [ins.operands[0].imm for ins in instructions
         if ins.mnemonic == "call" and
         ins.operands[0].type == x86.X86_OP_IMM]
assert calls == [SNAPSHOT_FN_RVA], calls

def rip_target(ins):
    for operand in ins.operands:
        if (operand.type == x86.X86_OP_MEM and
                operand.mem.base == x86.X86_REG_RIP):
            return ins.address + ins.size + operand.mem.disp
    return None

state_checks = [ins for ins in instructions
                if ins.mnemonic == "cmp" and rip_target(ins) == S3BX_STATE_RVA]
assert len(state_checks) == 1
assert state_checks[0].operands[1].type == x86.X86_OP_IMM
assert state_checks[0].operands[1].imm == 4

writes = []
for ins in instructions:
    for operand in ins.operands:
        if operand.type == x86.X86_OP_MEM and (operand.access & capstone.CS_AC_WRITE):
            writes.append((ins.mnemonic, ins.reg_name(operand.mem.base),
                           operand.mem.disp, operand.size))
assert ("mov", "rcx", 0x2C, 1) in writes
assert ("mov", "rcx", 0x5C, 1) in writes
assert ("mov", "rcx", 0x7C, 1) in writes
assert len([entry for entry in writes if entry[1] == "rcx"]) == 3
assert all(base_name in ("rsp", "rcx")
           for _mnemonic, base_name, _disp, _size in writes)

mn = [(ins.mnemonic, ins.op_str) for ins in instructions]
assert mn[0] == ("sub", "rsp, 0x28")
assert mn[-2:] == [("add", "rsp, 0x28"), ("ret", "")]
assert sum(1 for ins in instructions if ins.mnemonic == "jne") == 1
assert sum(1 for ins in instructions if ins.mnemonic == "je") == 1

# Accepted detector, capabilities and end-of-eye writeback remain exact.
assert bytes(out[off(0x2C2BC):off(0x2C2BC) + 6]) == bytes.fromhex("0f85eee92c00")
assert bytes(out[off(0x2FACB0):off(0x2FACB0) + 4]) == bytes.fromhex("4883ec50")
assert bytes(out[off(0x2F9D10):off(0x2F9D10) + 4]) == bytes.fromhex("4883ec40")
assert struct.unpack_from("<I", out, off(0x68111) + 1)[0] == 0x1F3
assert struct.unpack_from("<I", out, off(0x1890D4))[0] == 0x1F3

print(f"PASS: 3BZ verified -- exact 3BX base; call {CALL_SITE_RVA:#x} -> "
      f"{thunk:#x}; state-4 direct gate at {S3BX_STATE_RVA:#x}; only both eye "
      "FOV-valid flags and head-pose-valid cleared; stereo eye poses untouched")
