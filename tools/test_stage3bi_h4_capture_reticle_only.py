"""Stage 3BI verification.

Decodes the actual output DLL (no trust in the builder):
 1. Exact Stage 3BH parentage: every byte identical outside the two
    intended regions (splice 7 bytes, payload 292 bytes).
 2. The splice calls the payload and the two displaced instructions'
    semantics are reproduced inside it (mov rcx,rsi + call through
    g_halo4OrigCuiRenderCommand 0x2B9B18, null-guarded).
 3. The payload is gated on scope.redirectActive ([r15+0x3BA]) SAMPLED
    BEFORE the original call and tested AFTER, so a failed Begin leaves
    the user's stock pass untouched.
 4. The command header is read via the SEH SafeRead thunk 0x56C10, never
    dereferenced raw; renderer reads are bounds-checked (count!=0,<=0x60).
 5. Classification: 0x28+payload 0xC sets the inside flag to 1, 0x29
    clears it, both writing the SAME dword.
 6. Enforcement: top entry = renderer+0x878+(count-1)*0x34, x at +0x28;
    threshold = [0x2FB800] * 0.5; outside -> addss (the proven hide
    shift), inside -> subss (cancel an inherited shift); NaN skipped.
 7. The dispatch result byte is preserved to the caller; stack balanced;
    no nonvolatile register is written.
 8. Stage 3BH artifacts, CREDIT/ODST bytes and 12 sections intact.
"""
from pathlib import Path
import hashlib
import struct

import capstone
from capstone import x86

HERE = Path(__file__).parent
BASE = HERE.parent / "built" / "Stage3BH-HaloMCCVR.dll"
OUT = HERE.parent / "built" / "Stage3BI-HaloMCCVR.dll"

EXPECTED_BASE = \
    "80573ed9fffd3a557dea96b80823e490ecdaea7c0063ee6a5e1bacc4486d48c4"

SPLICE_RVA = 0x53921
PAYLOAD_RVA = 0x2FBE40
PAYLOAD_SIZE = 292

base = bytearray(BASE.read_bytes())
out = bytearray(OUT.read_bytes())
assert hashlib.sha256(base).hexdigest() == EXPECTED_BASE, "wrong 3BH base"
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


# 1. No byte changed outside the two permitted regions.
permitted = [(off(SPLICE_RVA), 7), (off(PAYLOAD_RVA), PAYLOAD_SIZE)]
diff = [i for i in range(len(base)) if base[i] != out[i]]
allowed = set()
for o, size in permitted:
    allowed.update(range(o, o + size))
stray = [i for i in diff if i not in allowed]
assert not stray, f"stray changes at {[hex(i) for i in stray[:8]]}"
assert diff, "no changes at all"

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
md.detail = True


def disasm(rva, size):
    return list(md.disasm(bytes(out[off(rva):off(rva) + size]), rva))


# 2. Splice decode.
sp = disasm(SPLICE_RVA, 7)
assert sp[0].mnemonic == "call" and \
    int(sp[0].op_str, 16) == PAYLOAD_RVA, "splice call wrong"
assert [i.mnemonic for i in sp[1:]] == ["nop", "nop"], "splice tail"

ins = disasm(PAYLOAD_RVA, PAYLOAD_SIZE)
text = {i.address: i for i in ins}


def riptarget(i):
    for op in i.operands:
        if op.type == x86.X86_OP_MEM and op.mem.base == x86.X86_REG_RIP:
            return i.address + i.size + op.mem.disp
    return None


# 2b. Original-call reproduction: load 0x2B9B18, null-guard, mov rcx,rsi,
#     call rax.
seq = list(ins)
i0 = seq[1]
assert i0.mnemonic == "mov" and riptarget(i0) == 0x2B9B18, "orig load"
assert seq[2].mnemonic == "test" and seq[2].op_str == "rax, rax"
assert seq[3].mnemonic == "je", "null guard"
fail_rva = int(seq[3].op_str, 16)
# 3. redirectActive gate sampled BEFORE the call...
g = seq[4]
assert g.mnemonic == "movzx" and "[r15 + 0x3ba]" in g.op_str, "gate read"
assert seq[5].mnemonic == "mov" and "byte ptr [rsp + 0x34]" in seq[5].op_str
assert seq[6].mnemonic == "mov" and seq[6].op_str == "rcx, rsi", \
    "displaced mov rcx,rsi"
assert seq[7].mnemonic == "call" and seq[7].op_str == "rax", "original call"
# result byte preserved immediately
assert seq[8].mnemonic == "mov" and \
    "byte ptr [rsp + 0x30], al" in seq[8].op_str, "result save"
# ...and tested AFTER
assert seq[9].mnemonic == "cmp" and \
    "byte ptr [rsp + 0x34], 0" in seq[9].op_str, "gate test"
assert seq[10].mnemonic == "je", "gate bail"
done_rva = int(seq[10].op_str, 16)

# 4. Bounds checks + SafeRead through the SEH thunk.
mnems = [(i.mnemonic, i.op_str) for i in ins]
assert ("mov", "ecx, dword ptr [rsi + 0x870]") in mnems, "count read"
assert ("cmp", "ecx, 0x60") in mnems, "count bound"
saferead = [i for i in ins if i.mnemonic == "call" and
            i.op_str == "0x56c10"]
assert len(saferead) == 1, "SafeRead call"
# raw command dereference must not exist: no memory operand based on r13
for i in ins:
    for op in i.operands:
        if op.type == x86.X86_OP_MEM and op.mem.base == x86.X86_REG_R13:
            raise AssertionError(f"raw [r13] deref at {i.address:#x}")

# 5. Classification writes the same flag dword.
flag_writes = [(i, riptarget(i)) for i in ins
               if i.mnemonic == "mov" and "dword ptr [rip" in i.op_str]
setters = [t for i, t in flag_writes if i.op_str.endswith(", 1")]
clearers = [t for i, t in flag_writes if i.op_str.endswith(", 0")]
assert len(setters) == 1 and len(clearers) == 1, "flag writes"
assert setters[0] == clearers[0], "set/clear different dwords"
flag_rva = setters[0]
cmp28 = [i for i in ins if i.mnemonic == "cmp" and i.op_str == "eax, 0x28"]
cmp29 = [i for i in ins if i.mnemonic == "cmp" and i.op_str == "eax, 0x29"]
cmp0c = [i for i in ins if i.mnemonic == "cmp" and
         "word ptr [rsp + 0x2e], 0xc" in i.op_str]
assert cmp28 and cmp29 and cmp0c, "classification compares"

# 6. Enforcement maths.
assert ("lea", "eax, [rcx - 1]") in mnems, "count-1"
assert ("imul", "rax, rax, 0x34") in mnems, "stride"
assert ("lea", "rax, [rsi + rax + 0x878]") in mnems, "entries base"
loads = [i for i in ins if i.mnemonic == "movss" and
         "xmm0, dword ptr [rax + 0x28]" in i.op_str]
assert len(loads) == 1, "x load"
c = [i for i in ins if i.mnemonic == "movss" and riptarget(i) == 0x2FB800]
assert len(c) == 1 and c[0].op_str.startswith("xmm1"), "4*halfWidth load"
half = [i for i in ins if i.mnemonic == "mulss"]
assert len(half) == 1, "threshold multiply"
half_rva = riptarget(half[0])
assert struct.unpack_from("<I", out, off(half_rva))[0] == 0x3F000000, "0.5f"
assert struct.unpack_from("<I", out, off(flag_rva))[0] == 0, \
    "inside flag must initialise to 0"
nan = [i for i in ins if i.mnemonic == "ucomiss"]
assert len(nan) == 1 and nan[0].op_str == "xmm0, xmm0", "NaN guard"
assert text[nan[0].address + nan[0].size].mnemonic == "jp", "NaN skip"
adds = [i for i in ins if i.mnemonic == "addss"]
subs = [i for i in ins if i.mnemonic == "subss"]
assert len(adds) == 1 and len(subs) == 1, "one shift each way"
stores = [i for i in ins if i.mnemonic == "movss" and
          i.op_str == "dword ptr [rax + 0x28], xmm0"]
assert len(stores) == 2, "one store per branch"
# the flag test selects the branch: jne jumps to the subss (inside) branch
flagtest = [i for i in ins if i.mnemonic == "cmp" and
            riptarget(i) == flag_rva and i.op_str.endswith(", 0")]
assert len(flagtest) == 1, "flag test"
after = text[flagtest[0].address + flagtest[0].size]
assert after.mnemonic == "jne", "branch on inside"
inside_rva = int(after.op_str, 16)
assert adds[0].address < inside_rva <= subs[0].address, \
    "addss outside / subss inside"
com = [i for i in ins if i.mnemonic == "comiss"]
assert len(com) == 2 and all(i.op_str == "xmm0, xmm2" for i in com), \
    "two threshold compares"
# idempotence: outside compare jumps AWAY when already offscreen (jae),
# inside compare jumps AWAY when already on target (jb)
outside_j = text[com[0].address + com[0].size]
inside_j = text[com[1].address + com[1].size]
assert outside_j.mnemonic == "jae" and int(outside_j.op_str, 16) == done_rva
assert inside_j.mnemonic == "jb" and int(inside_j.op_str, 16) == done_rva

# 7. Epilogues: result reload + balanced stack, and the fail path.
d = text[done_rva]
assert d.mnemonic == "movzx" and "byte ptr [rsp + 0x30]" in d.op_str, \
    "done reloads result"
assert text[done_rva + d.size].op_str == "rsp, 0x38", "done epilogue"
f = text[fail_rva]
assert f.mnemonic == "xor" and f.op_str == "eax, eax", "fail returns 0"
subs_rsp = [i for i in ins if i.mnemonic == "sub" and
            i.op_str == "rsp, 0x38"]
adds_rsp = [i for i in ins if i.mnemonic == "add" and
            i.op_str == "rsp, 0x38"]
assert len(subs_rsp) == 1 and len(adds_rsp) == 2, "stack balance"
# no push/pop at all (nothing nonvolatile is clobbered, nothing to save)
assert not any(i.mnemonic in ("push", "pop") for i in ins), "no pushes"
# no nonvolatile register is written (rbx/rbp/rdi/rsi/r12-r15 must survive
# for the caller: rbx=0 compares, rdi=transform id, r13=command)
BAD = {x86.X86_REG_RBX, x86.X86_REG_EBX, x86.X86_REG_RBP,
       x86.X86_REG_RDI, x86.X86_REG_EDI, x86.X86_REG_RSI,
       x86.X86_REG_R12, x86.X86_REG_R13, x86.X86_REG_R14, x86.X86_REG_R15}
for i in ins:
    if i.mnemonic in ("call", "ret", "nop") or i.mnemonic.startswith("j"):
        continue
    _, regs_w = i.regs_access()
    hit = BAD.intersection(regs_w)
    assert not hit, f"nonvolatile write at {i.address:#x}: {i.mnemonic}"

# 8. 3BH artifacts + carried bytes.
assert struct.unpack_from("<4f", out, off(0x2FB800)) == \
    struct.unpack("<4f", struct.pack("<ffff", 4134.312, 1346.196, 0, 1)), \
    "3BH framing constant"
for rva in (0xD0C5, 0xD229, 0x2763C):
    assert out[off(rva)] == 0xE8, f"3BH splice at {rva:#x} gone"
for rva, size in ((0x2C0589, 8), (0x2C5C16, 1), (0x2C5C6B, 1),
                  (0x2BFF0C, 1)):
    assert out[off(rva):off(rva) + size] == \
        base[off(rva):off(rva) + size], f"carried bytes at {rva:#x}"

print("PASS: Stage 3BI verified -- splice + gated selector flip decoded, "
      "SafeRead-protected, same-shift-as-hide, result preserved, "
      f"flag@{flag_rva:#x} half@{half_rva:#x}; base byte-identical outside "
      "2 regions; 3BH artifacts, CREDIT/ODST and 12 sections intact")
