from pathlib import Path
import hashlib
import struct

ROOT = Path(__file__).parent.parent
BB = (ROOT / "built" / "Stage3BB-HaloMCCVR.dll").read_bytes()
OUT = (ROOT / "built" / "Stage3BC-HaloMCCVR.dll").read_bytes()
EXPECTED = "574cb44dcce6d89a8418db54b4dc24e6dc9f2105b96d689a88c779d5864c36e1"

PAYLOAD_RVA = 0x2FB800
SCRATCH_RVA = 0x2FB900
SAFE_READ_RVA = 0x56C10
GATE_RETURN_RVA = 0x53849


def sections(blob):
    pe = struct.unpack_from("<I", blob, 0x3C)[0]
    count = struct.unpack_from("<H", blob, pe + 6)[0]
    optional_size = struct.unpack_from("<H", blob, pe + 20)[0]
    table = pe + 24 + optional_size
    return [struct.unpack_from("<IIII", blob, table + i * 40 + 8)
            for i in range(count)]


def read(blob, secs, rva, size):
    start = next(rp + rva - va for vs, va, rs, rp in secs
                 if va <= rva < va + max(vs, rs))
    return blob[start:start + size]


assert hashlib.sha256(OUT).hexdigest() == EXPECTED, "unexpected output hash"
bb_s, out_s = sections(BB), sections(OUT)
assert len(OUT) == len(BB), "size changed"
assert len(out_s) == 12, "post-build PE section count must stay 12"

# 1. The Stage 3AX capture edge is restored (Stage 3BB forced it to EB).
assert read(BB, bb_s, 0x53634, 2) == bytes.fromhex("eb56")
assert read(OUT, out_s, 0x53634, 2) == bytes.fromhex("7456")

# 2. The dispatcher payload read is spliced to the stub, rest NOP.
gate = read(OUT, out_s, 0x53835, 20)
assert gate[0] == 0xE9
assert 0x53835 + 5 + struct.unpack_from("<i", gate, 1)[0] == PAYLOAD_RVA
assert gate[5:] == b"\x90" * 15, "displaced bytes must be NOP-filled"

# 3. The stub: two Halo4SafeRead calls, both rip-relative reads hit the
#    scratch slot, and it returns into the dispatcher.
stub = read(OUT, out_s, PAYLOAD_RVA, 77)
assert stub[:4] == bytes.fromhex("498d4d04"), "payload+0 read preserved"
assert stub[24:28] == bytes.fromhex("498d4d08"), "payload+4 read added"
for call_off, next_off in ((15, 20), (41, 46)):
    assert stub[call_off] == 0xE8
    target = PAYLOAD_RVA + next_off + struct.unpack_from(
        "<i", stub, call_off + 1)[0]
    assert target == SAFE_READ_RVA, f"call at +{call_off} must reach SafeRead"
for lea_off, next_off, opcode_len in ((34, 41, 3), (50, 56, 2)):
    disp = struct.unpack_from("<i", stub, lea_off + opcode_len)[0]
    assert PAYLOAD_RVA + next_off + disp == SCRATCH_RVA, \
        f"rip-relative at +{lea_off} must address the scratch slot"
assert stub[56:61] == bytes.fromhex("25ffffff7f"), "sign-mask on the float"
assert stub[72] == 0xE9
assert PAYLOAD_RVA + 77 + struct.unpack_from("<i", stub, 73)[0] == \
    GATE_RETURN_RVA, "stub must return into the dispatcher"

# 4. The scratch slot is a real 4 bytes of free page, not code.
assert read(OUT, out_s, SCRATCH_RVA, 4) == b"\0\0\0\0"

# 5. Accepted Stage 3AL CREDIT + ODST bytes survive untouched.
for rva, size in ((0x2C0589, 8), (0x2C5C16, 1), (0x2C5C6B, 1), (0x2BFF0C, 1)):
    assert read(OUT, out_s, rva, size) == read(BB, bb_s, rva, size)

# 6. Nothing outside the three intended regions moved.
allowed = ((0x53634, 1), (0x53835, 20), (PAYLOAD_RVA, 77))
changed = {i for i in range(len(BB)) if BB[i] != OUT[i]}
permitted = set()
for rva, size in allowed:
    start = next(rp + rva - va for vs, va, rs, rp in out_s
                 if va <= rva < va + max(vs, rs))
    permitted.update(range(start, start + size))
assert changed <= permitted, "unexpected bytes changed outside the patch sites"

print("PASS Stage3BC H4 reticle discrimination; capture edge restored; "
      "CREDIT/ODST preserved; 12 sections")
