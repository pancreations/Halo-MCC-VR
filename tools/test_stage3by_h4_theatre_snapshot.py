"""Static verification for the Stage 3BY Halo 4 theatre-camera delta."""
from pathlib import Path
import hashlib
import struct
import sys

import capstone
from capstone import x86

sys.path.insert(0, str(Path(__file__).parent))
from build_stage3by_h4_theatre_snapshot import (
    EXPECTED_INPUT_SHA256, CALL_SITE_RVA, CALL_SITE_OLD, SNAPSHOT_FN_RVA,
    THEATRE_ACTIVE_RVA, THEATRE_ACTIVE_BODY, PAYLOAD_RVA, PAYLOAD_LIMIT,
    parse_pe, rva_off)

HERE = Path(__file__).parent
BASE = HERE.parent / "built" / "Stage3BX-HaloMCCVR.dll"
OUT = HERE.parent / "built" / "Stage3BY-HaloMCCVR.dll"

base = bytearray(BASE.read_bytes())
out = bytearray(OUT.read_bytes())
assert hashlib.sha256(base).hexdigest() == EXPECTED_INPUT_SHA256
assert len(base) == len(out)
pe = parse_pe(out)
assert pe["n"] == 12
off = lambda rva: rva_off(pe, rva)

# No existing stage byte may change apart from the replaced call and the empty
# payload gap.  This keeps the accepted 3BU/3BW/3BX chain intact.
diff = [i for i in range(len(base)) if base[i] != out[i]]
allowed = (set(range(off(CALL_SITE_RVA), off(CALL_SITE_RVA) + 5)) |
           set(range(off(PAYLOAD_RVA), off(PAYLOAD_LIMIT))))
assert not [i for i in diff if i not in allowed]
assert all(base[i] == 0 for i in range(off(PAYLOAD_RVA), off(PAYLOAD_LIMIT)))
assert bytes(out[off(THEATRE_ACTIVE_RVA):off(THEATRE_ACTIVE_RVA) +
                 len(THEATRE_ACTIVE_BODY)]) == THEATRE_ACTIVE_BODY

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
md.detail = True
call = list(md.disasm(bytes(out[off(CALL_SITE_RVA):off(CALL_SITE_RVA) + 5]),
                      CALL_SITE_RVA))
assert len(call) == 1 and call[0].mnemonic == "call"
thunk = call[0].operands[0].imm
assert PAYLOAD_RVA <= thunk < PAYLOAD_LIMIT

instructions = list(md.disasm(bytes(out[off(thunk):off(PAYLOAD_LIMIT)]), thunk))
ret_index = next(i for i, ins in enumerate(instructions)
                 if ins.mnemonic == "ret")
instructions = instructions[:ret_index + 1]
direct_calls = [ins.operands[0].imm for ins in instructions
                if ins.mnemonic == "call" and
                ins.operands[0].type == x86.X86_OP_IMM]
assert direct_calls == [SNAPSHOT_FN_RVA, THEATRE_ACTIVE_RVA], direct_calls

# The only output writes are the three validity flags.  Do not admit a camera,
# eye pose, controller, or scene mutation into this feature transaction.
writes = []
for ins in instructions:
    for operand in ins.operands:
        if operand.type == x86.X86_OP_MEM and (operand.access & capstone.CS_AC_WRITE):
            writes.append((ins.mnemonic, ins.reg_name(operand.mem.base),
                           operand.mem.disp, operand.size))
assert ("mov", "rcx", 0x2C, 1) in writes
assert ("mov", "rcx", 0x5C, 1) in writes
assert ("mov", "rcx", 0x7C, 1) in writes
assert all(base in ("rsp", "rcx") for _mn, base, _disp, _size in writes)
assert len([w for w in writes if w[1] == "rcx"]) == 3

# Original snapshot failure is returned unchanged; a compositor false result
# restores the original true before return, so this cannot reject a good frame.
mn = [(ins.mnemonic, ins.op_str) for ins in instructions]
assert mn[0] == ("sub", "rsp, 0x38") and mn[-2:] == [
    ("add", "rsp, 0x38"), ("ret", "")]
assert any(ins.mnemonic == "test" and ins.op_str == "al, al"
           for ins in instructions)
assert any(ins.mnemonic == "mov" and ins.op_str == "al, byte ptr [rsp + 0x28]"
           for ins in instructions)

# Keep the BX theatre classifier and its chain target exactly where acceptance
# established them; BY is only a snapshot gate.
assert bytes(out[off(0x2C2BC):off(0x2C2BC) + 6]) == bytes.fromhex("0f85eee92c00")
assert bytes(out[off(0x2FACB0):off(0x2FACB0) + 4]) == bytes.fromhex("4883ec50")
assert bytes(out[off(0x2F9D10):off(0x2F9D10) + 4]) == bytes.fromhex("4883ec40")

print(f"PASS: 3BY snapshot gate verified -- call {CALL_SITE_RVA:#x} -> "
      f"{thunk:#x}; original snapshot then shared theatre-active only; "
      "only eye FOV validity and head-pose validity are cleared; BX chain intact")
