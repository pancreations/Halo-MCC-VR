"""Stage 3BK verification - decodes the output DLL, trusts nothing.

 1. Byte-identical to Stage 3BI outside two regions: the 24-byte gate call
    sequence at 0x2FB992 and the 116-byte payload at 0x2FBF64.
 2. Gate patch decodes as call -> thunk + 19 NOPs, returning straight to
    the untouched scissor re-assert at 0x2FB9AA.
 3. Thunk semantics: copies all 24 bytes of the stored viewport (0x2AE774)
    into a private copy, subtracts exactly 104.0f from the PRIVATE
    TopLeftY (offset +4), never writes the stored struct, and calls
    RSSetViewports (vtable +0x160) with rcx=rbx, edx=1, r8=private copy.
    Stack balanced (sub/add 0x28).
 4. The rejected 3BJ patch is NOT present (0x199B3 is stock mov dl,1).
 5. 3BI/3BH artifacts, CREDIT/ODST bytes, 12 sections intact.
"""
from pathlib import Path
import hashlib
import struct

import capstone
from capstone import x86

HERE = Path(__file__).parent
BASE = HERE.parent / "built" / "Stage3BI-HaloMCCVR.dll"
OUT = HERE.parent / "built" / "Stage3BK-HaloMCCVR.dll"

EXPECTED_BASE = \
    "dad8a373ed30f3e31f42350fa85b575e2f7b146b8ba83d7f4cdf7333da653d22"

GATE_RVA = 0x2FB992
GATE_LEN = 24
PAYLOAD_RVA = 0x2FBF64
PAYLOAD_SIZE = 116
STORED_VIEWPORT = 0x2AE774

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


# 1. Diff confined to the two intended regions.
diff = [i for i in range(len(base)) if base[i] != out[i]]
allowed = set(range(off(GATE_RVA), off(GATE_RVA) + GATE_LEN)) | \
    set(range(off(PAYLOAD_RVA), off(PAYLOAD_RVA) + PAYLOAD_SIZE))
stray = [i for i in diff if i not in allowed]
assert not stray, f"stray changes at {[hex(i) for i in stray[:8]]}"
assert diff, "no changes at all"

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
md.detail = True


def disasm(blob, rva, size):
    return list(md.disasm(bytes(blob[off(rva):off(rva) + size]), rva))


def riptarget(i):
    for op in i.operands:
        if op.type == x86.X86_OP_MEM and op.mem.base == x86.X86_REG_RIP:
            return i.address + i.size + op.mem.disp
    return None


# 2. Gate patch decode.
g = disasm(out, GATE_RVA, GATE_LEN)
assert g[0].mnemonic == "call", "gate no longer calls"
thunk_rva = int(g[0].op_str, 16)
assert thunk_rva == PAYLOAD_RVA, f"gate calls {thunk_rva:#x}"
assert all(i.mnemonic == "nop" for i in g[1:]), "gate tail not nops"
assert sum(i.size for i in g) == GATE_LEN, "gate patch length"
# scissor re-assert untouched right after
sc = disasm(out, GATE_RVA + GATE_LEN, 15)
assert sc[0].mnemonic == "mov" and sc[0].op_str == "rcx, rbx"
assert sc[2].mnemonic == "lea" and riptarget(sc[2]) == 0x2AE78C, \
    "scissor re-assert changed"

# 3. Thunk semantics.
t = disasm(out, PAYLOAD_RVA, PAYLOAD_SIZE)
assert t[0].mnemonic == "sub" and t[0].op_str == "rsp, 0x28"
# 16-byte copy from stored viewport
assert t[1].mnemonic == "movups" and riptarget(t[1]) == STORED_VIEWPORT
assert t[2].mnemonic == "movups" and t[2].op_str.startswith("xmmword ptr")
private = riptarget(t[2])
# 8-byte tail copy (offsets +0x10)
assert t[3].mnemonic == "mov" and riptarget(t[3]) == STORED_VIEWPORT + 0x10
assert t[4].mnemonic == "mov" and riptarget(t[4]) == private + 0x10
# TopLeftY bias on the PRIVATE copy
assert t[5].mnemonic == "movss" and riptarget(t[5]) == private + 4, \
    "load is not private TopLeftY"
assert t[6].mnemonic == "subss", "no subtraction"
bias_rva = riptarget(t[6])
bias = struct.unpack_from("<f", out, off(bias_rva))[0]
assert bias == 104.0, f"bias is {bias}, not 104.0"
assert t[7].mnemonic == "movss" and riptarget(t[7]) == private + 4, \
    "store is not private TopLeftY"
# call setup and the D3D call
assert t[8].op_str == "rcx, rbx" and t[9].op_str == "edx, 1"
assert t[10].mnemonic == "lea" and riptarget(t[10]) == private, \
    "r8 does not point at the private copy"
assert t[11].op_str == "rax, qword ptr [rbx]"
assert t[12].mnemonic == "call" and "rax + 0x160" in t[12].op_str, \
    "not RSSetViewports"
assert t[13].mnemonic == "add" and t[13].op_str == "rsp, 0x28"
assert t[14].mnemonic == "ret"
# the stored struct is never written: no instruction stores to 0x2AE774..+24
for i in t:
    for op in i.operands:
        if op.type == x86.X86_OP_MEM and op.access & capstone.CS_AC_WRITE:
            tgt = riptarget(i)
            if tgt is not None:
                assert not (STORED_VIEWPORT <= tgt < STORED_VIEWPORT + 24), \
                    f"thunk writes the stored viewport at {i.address:#x}"
# the private copy lives inside the payload region (scratch, zero-init ok)
assert PAYLOAD_RVA <= private < PAYLOAD_RVA + PAYLOAD_SIZE, \
    "private copy outside payload"

# 4. 3BJ absent.
assert bytes(out[off(0x199B3):off(0x199B3) + 2]) == bytes.fromhex("b201"), \
    "the rejected 3BJ patch is present"

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

print("PASS: Stage 3BK verified -- gate call -> thunk decoded; thunk copies "
      "the stored viewport, biases only the PRIVATE TopLeftY by -104.0, "
      "never writes the stored struct, calls RSSetViewports(+0x160); 3BJ "
      "absent; 3BI/3BH payloads, CREDIT/ODST and 12 sections intact")
