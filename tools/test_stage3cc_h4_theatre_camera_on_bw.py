"""Prove Stage 3CC is Stage 3BW plus only the theatre-camera delta."""
from pathlib import Path
import hashlib
import sys

import capstone
from capstone import x86

sys.path.insert(0, str(Path(__file__).parent))
from build_stage3cc_h4_theatre_camera_on_bw import (
    EXPECTED_INPUT_SHA256, CALL_SITE_RVA, SNAPSHOT_FN_RVA,
    THEATRE_ACTIVE_RVA, THEATRE_DEPTH_RVA, PAYLOAD_RVA, PAYLOAD_LIMIT,
    parse_pe, rva_off)

ROOT = Path(__file__).parent.parent
base = bytearray((ROOT / "built/Stage3BW-HaloMCCVR.dll").read_bytes())
out = bytearray((ROOT / "built/Stage3CC-HaloMCCVR.dll").read_bytes())
assert hashlib.sha256(base).hexdigest() == EXPECTED_INPUT_SHA256
assert len(base) == len(out)
pe = parse_pe(out)
off = lambda rva: rva_off(pe, rva)

diff = [i for i, (a, b) in enumerate(zip(base, out)) if a != b]
allowed = (set(range(off(CALL_SITE_RVA), off(CALL_SITE_RVA) + 5)) |
           set(range(off(PAYLOAD_RVA), off(PAYLOAD_LIMIT))))
assert not [i for i in diff if i not in allowed]
assert all(base[i] == 0 for i in range(off(PAYLOAD_RVA), off(PAYLOAD_LIMIT)))

# CB overwrote the zero terminator after this live reticle-probe format string.
# Stage 3CC must retain that whole range exactly from 3BW.
probe = b"S3AS reticle probe: alpha %u rgb %u n %u\0"
probe_at = bytes(base).find(probe)
assert probe_at >= 0 and bytes(base).find(probe, probe_at + 1) < 0
assert bytes(out[probe_at:probe_at + len(probe)]) == probe
bad_cb_start = off(0x002FA36C)
bad_cb_end = off(0x002FA400)
assert bytes(out[bad_cb_start:bad_cb_end]) == bytes(base[bad_cb_start:bad_cb_end])

# Stage 3BW's complete reticle and theatre chains remain intact.
assert bytes(out[off(0x2C2BC):off(0x2C2BC) + 6]) == bytes.fromhex(
    "0f85dee72c00")
for rva in (0x2A9DA, 0x2A701, 0x53921, 0x11E76, 0x11EB6,
            0x2FB992, 0x2F9066):
    assert out[off(rva)] in (0xE8, 0x0F)

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
md.detail = True
call = list(md.disasm(bytes(out[off(CALL_SITE_RVA):off(CALL_SITE_RVA) + 5]),
                      CALL_SITE_RVA))
assert len(call) == 1 and call[0].mnemonic == "call"
thunk = call[0].operands[0].imm
assert thunk == PAYLOAD_RVA
instructions = list(md.disasm(
    bytes(out[off(thunk):off(PAYLOAD_LIMIT)]), thunk))
instructions = instructions[:next(
    i for i, ins in enumerate(instructions) if ins.mnemonic == "ret") + 1]
calls = [ins.operands[0].imm for ins in instructions
         if ins.mnemonic == "call" and ins.operands[0].type == x86.X86_OP_IMM]
assert calls == [SNAPSHOT_FN_RVA, THEATRE_ACTIVE_RVA]

def rip_target(ins):
    for operand in ins.operands:
        if operand.type == x86.X86_OP_MEM and operand.mem.base == x86.X86_REG_RIP:
            return ins.address + ins.size + operand.mem.disp
    return None

assert any(ins.mnemonic == "movss" and rip_target(ins) == THEATRE_DEPTH_RVA
           for ins in instructions)
writes = []
for ins in instructions:
    for index, operand in enumerate(ins.operands):
        store = index == 0 and ins.mnemonic in ("mov", "movups")
        if operand.type == x86.X86_OP_MEM and (
                operand.access & capstone.CS_AC_WRITE or store):
            writes.append((ins.reg_name(operand.mem.base), operand.mem.disp,
                           operand.size))
rcx = [entry for entry in writes if entry[0] == "rcx"]
for required in (("rcx", 0x08, 16), ("rcx", 0x38, 16),
                 ("rcx", 0x34, 1), ("rcx", 0x64, 1),
                 ("rcx", 0x84, 1), ("rcx", 0x14, 8),
                 ("rcx", 0x20, 4), ("rcx", 0x44, 8),
                 ("rcx", 0x50, 4)):
    assert required in rcx
assert all(base_name in ("rsp", "rcx") for base_name, _disp, _size in writes)

print("PASS: Stage3CC is exact Stage3BW plus only the isolated Halo 4 "
      "theatre-camera call/wrapper; reticle chain and probe terminator exact")
