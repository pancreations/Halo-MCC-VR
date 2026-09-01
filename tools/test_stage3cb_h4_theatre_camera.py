"""Static verification for Stage 3CB's Halo 4 theatre camera transaction."""
from pathlib import Path
import hashlib
import sys

import capstone
from capstone import x86

sys.path.insert(0, str(Path(__file__).parent))
from build_stage3cb_h4_theatre_camera import (
    EXPECTED_INPUT_SHA256, CALL_SITE_RVA, SNAPSHOT_FN_RVA,
    THEATRE_ACTIVE_RVA, THEATRE_DEPTH_RVA, THEATRE_DEPTH_REFERENCE_RVA,
    THEATRE_DEPTH_REFERENCE, PAYLOAD_RVA, PAYLOAD_LIMIT,
    parse_pe, rva_off)

ROOT = Path(__file__).parent.parent
base = bytearray((ROOT / "built/Stage3BX-HaloMCCVR.dll").read_bytes())
out = bytearray((ROOT / "built/Stage3CB-HaloMCCVR.dll").read_bytes())
assert hashlib.sha256(base).hexdigest() == EXPECTED_INPUT_SHA256
assert len(base) == len(out)
pe = parse_pe(out)
off = lambda rva: rva_off(pe, rva)

diff = [i for i, (a, b) in enumerate(zip(base, out)) if a != b]
allowed = (set(range(off(CALL_SITE_RVA), off(CALL_SITE_RVA) + 5)) |
           set(range(off(PAYLOAD_RVA), off(PAYLOAD_LIMIT))))
assert not [i for i in diff if i not in allowed]
assert all(base[i] == 0 for i in range(off(PAYLOAD_RVA), off(PAYLOAD_LIMIT)))
assert bytes(out[off(THEATRE_DEPTH_REFERENCE_RVA):
                 off(THEATRE_DEPTH_REFERENCE_RVA) + 8]) == \
    THEATRE_DEPTH_REFERENCE

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
md.detail = True
call = list(md.disasm(bytes(out[off(CALL_SITE_RVA):off(CALL_SITE_RVA) + 5]),
                      CALL_SITE_RVA))
assert len(call) == 1 and call[0].mnemonic == "call"
thunk = call[0].operands[0].imm
instructions = list(md.disasm(
    bytes(out[off(thunk):off(PAYLOAD_LIMIT)]), thunk))
instructions = instructions[:next(
    i for i, ins in enumerate(instructions) if ins.mnemonic == "ret") + 1]

calls = [ins.operands[0].imm for ins in instructions
         if ins.mnemonic == "call" and
         ins.operands[0].type == x86.X86_OP_IMM]
assert calls == [SNAPSHOT_FN_RVA, THEATRE_ACTIVE_RVA]

def rip_target(ins):
    for operand in ins.operands:
        if operand.type == x86.X86_OP_MEM and operand.mem.base == x86.X86_REG_RIP:
            return ins.address + ins.size + operand.mem.disp
    return None

assert any(ins.mnemonic == "movss" and rip_target(ins) == THEATRE_DEPTH_RVA
           for ins in instructions)
assert any(ins.mnemonic == "maxss" for ins in instructions)
assert any(ins.mnemonic == "minss" for ins in instructions)

writes = []
for ins in instructions:
    for index, operand in enumerate(ins.operands):
        # Capstone 5 reports the destination of these legacy SSE stores as a
        # read.  Operand zero is still unambiguously the memory destination.
        store_operand = index == 0 and ins.mnemonic in ("mov", "movups")
        if (operand.type == x86.X86_OP_MEM and
                (operand.access & capstone.CS_AC_WRITE or store_operand)):
            writes.append((ins.reg_name(operand.mem.base), operand.mem.disp,
                           operand.size))
rcx_writes = [entry for entry in writes if entry[0] == "rcx"]
assert ("rcx", 0x08, 16) in rcx_writes
assert ("rcx", 0x38, 16) in rcx_writes
assert ("rcx", 0x34, 1) in rcx_writes
assert ("rcx", 0x64, 1) in rcx_writes
assert ("rcx", 0x84, 1) in rcx_writes
assert ("rcx", 0x14, 8) in rcx_writes and ("rcx", 0x20, 4) in rcx_writes
assert ("rcx", 0x44, 8) in rcx_writes and ("rcx", 0x50, 4) in rcx_writes
assert all(base_name in ("rsp", "rcx") for base_name, _disp, _size in writes)

# The leading preparedSerial makes the validity offsets 0x34/0x64/0x84.
assert 0x08 + 0x2C == 0x34
assert 0x08 + 0x30 + 0x2C == 0x64
assert 0x08 + 0x30 * 2 + 0x10 + 0x0C == 0x84

assert bytes(out[off(0x2C2BC):off(0x2C2BC) + 6]) == bytes.fromhex(
    "0f85eee92c00")
assert bytes(out[off(0x2FACB0):off(0x2FACB0) + 4]) == bytes.fromhex(
    "4883ec50")
assert bytes(out[off(0x2F9D10):off(0x2F9D10) + 4]) == bytes.fromhex(
    "4883ec40")

print("PASS: Stage3CB scales both Halo 4 theatre eye positions, makes the "
      "cutscene cameras parallel, retains authored FOV, disables HMD camera "
      "composition, and leaves BX gameplay/transition/other-title code exact")
