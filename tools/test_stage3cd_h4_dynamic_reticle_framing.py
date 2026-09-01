"""Prove Stage 3CD changes only Stage 3BP's framing-source displacement."""
from pathlib import Path
import hashlib
import struct
import sys

import capstone
from capstone import x86

sys.path.insert(0, str(Path(__file__).parent))
from build_stage3cd_h4_dynamic_reticle_framing import (
    EXPECTED_INPUT_SHA256, LOAD_RVA, LIVE_HIDE_X_RVA,
    LIVE_HIDE_X_REFERENCE_RVA, LIVE_HIDE_X_REFERENCE,
    LIVE_HIDE_X_STORE_RVA, LIVE_HIDE_X_STORE, parse_pe, rva_off)

ROOT = Path(__file__).parent.parent
base = bytearray((ROOT / "built/Stage3CC-HaloMCCVR.dll").read_bytes())
out = bytearray((ROOT / "built/Stage3CD-HaloMCCVR.dll").read_bytes())
assert hashlib.sha256(base).hexdigest() == EXPECTED_INPUT_SHA256
assert len(base) == len(out)
pe = parse_pe(out)
off = lambda rva: rva_off(pe, rva)

diff = [i for i, (a, b) in enumerate(zip(base, out)) if a != b]
assert diff and all(off(LOAD_RVA) + 4 <= i < off(LOAD_RVA) + 8 for i in diff)
assert bytes(out[off(LOAD_RVA):off(LOAD_RVA) + 4]) == bytes.fromhex(
    "f30f100d")

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
md.detail = True
ins = list(md.disasm(bytes(out[off(LOAD_RVA):off(LOAD_RVA) + 8]), LOAD_RVA))
assert len(ins) == 1 and ins[0].mnemonic == "movss"
assert ins[0].op_str.startswith("xmm1, dword ptr")
memory = next(op for op in ins[0].operands if op.type == x86.X86_OP_MEM)
assert memory.mem.base == x86.X86_REG_RIP
assert ins[0].address + ins[0].size + memory.mem.disp == LIVE_HIDE_X_RVA

# Pin the compiled report's six contiguous atomics. This proves the selected
# field is aimX/offscreen-hide, not base, Y, or scale.
loads = (
    (0x515D6, 0x2A8360), # baseX
    (0x515D0, 0x2A8364), # baseY
    (0x515C3, 0x2A8368), # aimX / live hide displacement
    (0x515B6, 0x2A836C), # aimY
    (0x515A9, 0x2A8370), # stockScale
    (0x51596, 0x2A8374), # writtenScale
)
for rva, target in loads:
    decoded = list(md.disasm(bytes(out[off(rva):off(rva) + 6]), rva))
    assert len(decoded) == 1 and decoded[0].mnemonic == "mov"
    mem = next(op for op in decoded[0].operands if op.type == x86.X86_OP_MEM)
    assert decoded[0].address + decoded[0].size + mem.mem.disp == target
assert bytes(out[off(LIVE_HIDE_X_REFERENCE_RVA):
                 off(LIVE_HIDE_X_REFERENCE_RVA) + 6]) == LIVE_HIDE_X_REFERENCE
publisher = list(md.disasm(
    bytes(out[off(LIVE_HIDE_X_STORE_RVA):off(LIVE_HIDE_X_STORE_RVA) + 8]),
    LIVE_HIDE_X_STORE_RVA))
assert len(publisher) == 1 and publisher[0].mnemonic == "movss"
published = next(op for op in publisher[0].operands if op.type == x86.X86_OP_MEM)
assert publisher[0].address + publisher[0].size + published.mem.disp == \
    LIVE_HIDE_X_RVA
assert bytes(out[off(LIVE_HIDE_X_STORE_RVA):
                 off(LIVE_HIDE_X_STORE_RVA) + 8]) == LIVE_HIDE_X_STORE

# Cutscene camera, theatre publisher, complete inherited reticle chain, and
# the probe terminator remain exact from Stage 3CC.
assert bytes(out[off(0x56EBF):off(0x56EBF) + 5]) == \
    bytes(base[off(0x56EBF):off(0x56EBF) + 5])
assert bytes(out[off(0x2C2BC):off(0x2C2BC) + 6]) == bytes.fromhex(
    "0f85dee72c00")
probe = b"S3AS reticle probe: alpha %u rgb %u n %u\0"
probe_at = bytes(base).find(probe)
assert probe_at >= 0 and bytes(out[probe_at:probe_at + len(probe)]) == probe

print("PASS: Stage3CD changes only the stale Stage3BP fixed framing load; "
      "it now cancels the live Halo 4 hide X while theatre/camera/reticle "
      "chains remain exact")
