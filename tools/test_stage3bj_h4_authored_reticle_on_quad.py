"""Stage 3BJ verification - decodes the output DLL, trusts nothing.

 1. Exactly two bytes differ from Stage 3BI, both at 0x199B3.
 2. Those bytes now decode as `xor dl, dl` (was `mov dl, 1`).
 3. The patched site is DOMINATED by `cmp bl, 4` -> so only Halo 4 reaches
    it; every other title already flows to the `xor dl,dl` at 0x199B7.
    Verified by decoding the whole guard chain and proving every exit of
    it targets 0x199B7.
 4. `dl` is not written again between the patch and both of its consumers
    (0x199F0 authored early-return, 0x19A72 procedural opacity), so
    forcing it to 0 really does reach both.
 5. Stage 3BI / 3BH artifacts and CREDIT/ODST bytes intact; 12 sections.
"""
from pathlib import Path
import hashlib
import struct

import capstone
from capstone import x86

HERE = Path(__file__).parent
BASE = HERE.parent / "built" / "Stage3BI-HaloMCCVR.dll"
OUT = HERE.parent / "built" / "Stage3BJ-HaloMCCVR.dll"

EXPECTED_BASE = \
    "dad8a373ed30f3e31f42350fa85b575e2f7b146b8ba83d7f4cdf7333da653d22"

PATCH_RVA = 0x199B3
FALSE_PATH_RVA = 0x199B7
CONSUMERS = (0x199F0, 0x19A72)

base = bytearray(BASE.read_bytes())
out = bytearray(OUT.read_bytes())
assert hashlib.sha256(base).hexdigest() == EXPECTED_BASE, "wrong 3BI base"
assert len(base) == len(out), "size changed"


def parse_pe(blob):
    p = struct.unpack_from("<I", blob, 0x3C)[0]
    coff = p + 4
    n = struct.unpack_from("<H", blob, coff + 2)[0]
    osz = struct.unpack_from("<H", blob, coff + 16)[0]
    st = coff + 20 + osz
    secs = []
    for i in range(n):
        o = st + i * 40
        name = bytes(blob[o:o + 8]).split(b"\0", 1)[0].decode()
        vs, va, rs, rp = struct.unpack_from("<IIII", blob, o + 8)
        secs.append((name, va, vs, rp, rs))
    return n, secs


n, secs = parse_pe(out)
assert n == 12, f"expected 12 sections, got {n}"


def off(rva):
    for name, va, vs, rp, rs in secs:
        if va <= rva < va + max(vs, rs):
            return rp + rva - va
    raise AssertionError(hex(rva))


# 1. Exactly two changed bytes, both at the patch site.
diff = [i for i in range(len(base)) if base[i] != out[i]]
po = off(PATCH_RVA)
assert diff == [po, po + 1], \
    f"expected exactly 2 changes at {po:#x}, got {[hex(d) for d in diff]}"
assert bytes(base[po:po + 2]) == bytes.fromhex("b201"), "base was not mov dl,1"

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
md.detail = True


def disasm(blob, rva, size):
    return list(md.disasm(bytes(blob[off(rva):off(rva) + size]), rva))


# 2. New encoding decodes as xor dl,dl and is still 2 bytes.
old = disasm(base, PATCH_RVA, 2)[0]
new = disasm(out, PATCH_RVA, 2)[0]
assert old.mnemonic == "mov" and old.op_str == "dl, 1", f"base: {old.op_str}"
assert new.mnemonic == "xor" and new.op_str == "dl, dl", f"new: {new.op_str}"
assert new.size == old.size == 2, "instruction length changed"

# 3. Dominator chain: decode 0x19998..0x199B7 and prove Halo-4-only.
chain = disasm(out, 0x19998, FALSE_PATH_RVA - 0x19998)
assert chain[0].mnemonic == "cmp" and chain[0].op_str == "bl, 4", \
    "patched site is not dominated by cmp bl,4"
branches = [i for i in chain if i.mnemonic in ("je", "jne")]
assert len(branches) == 4, f"expected 4 guard branches, got {len(branches)}"
for b in branches:
    assert int(b.op_str, 16) == FALSE_PATH_RVA, \
        f"guard at {b.address:#x} exits to {b.op_str}, not the false path"
# and the false path really is xor dl,dl
fp = disasm(out, FALSE_PATH_RVA, 2)[0]
assert fp.mnemonic == "xor" and fp.op_str == "dl, dl", "false path changed"
# the instruction between the patch and the false path is the skip jmp
skip = disasm(out, PATCH_RVA + 2, 2)[0]
assert skip.mnemonic == "jmp" and \
    int(skip.op_str, 16) == FALSE_PATH_RVA + 2, "skip jmp changed"

# 4. dl survives from the patch to both consumers: decode the whole span
#    and assert nothing else writes dl/edx/rdx in between.
span_end = max(CONSUMERS) + 2
span = disasm(out, PATCH_RVA, span_end - PATCH_RVA)
covered = {i.address for i in span}
for c in CONSUMERS:
    assert c in covered, f"consumer {c:#x} not on the decoded path"
    ins = [i for i in span if i.address == c][0]
    assert ins.mnemonic == "test" and ins.op_str == "dl, dl", \
        f"consumer {c:#x} is {ins.mnemonic} {ins.op_str}"
DL = {x86.X86_REG_DL, x86.X86_REG_DX, x86.X86_REG_EDX, x86.X86_REG_RDX}
writers = []
for i in span:
    if i.address in (PATCH_RVA, FALSE_PATH_RVA):
        continue            # the two intended assignments
    if i.address >= max(CONSUMERS):
        break
    _, regs_w = i.regs_access()
    if DL.intersection(regs_w):
        writers.append((hex(i.address), i.mnemonic, i.op_str))
assert not writers, f"dl is reassigned before its consumers: {writers}"

# 5. Artifacts intact.
assert struct.unpack_from("<4f", out, off(0x2FB800)) == \
    struct.unpack("<4f", struct.pack("<ffff", 4134.312, 1346.196, 0, 1)), \
    "3BH framing constant"
for rva in (0xD0C5, 0xD229, 0x2763C, 0x53921):
    assert out[off(rva)] == 0xE8, f"splice at {rva:#x} gone"
for rva, size in ((0x2C0589, 8), (0x2C5C16, 1), (0x2C5C6B, 1),
                  (0x2BFF0C, 1), (0x2FBE40, 292)):
    assert out[off(rva):off(rva) + size] == \
        base[off(rva):off(rva) + size], f"region at {rva:#x} changed"

print("PASS: Stage 3BJ verified -- exactly 2 bytes changed at 0x199B3, "
      "mov dl,1 -> xor dl,dl; site dominated by cmp bl,4 with all 4 guard "
      "exits to 0x199B7 (Halo 4 only); dl reaches both consumers unwritten; "
      "3BI/3BH payloads, CREDIT/ODST and 12 sections intact")
