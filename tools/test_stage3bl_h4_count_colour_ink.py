"""Stage 3BL verification - decodes the output DLL."""
from pathlib import Path
import hashlib, struct
import capstone
from capstone import x86

HERE = Path(__file__).parent
base = bytearray((HERE.parent/"built"/"Stage3BK-HaloMCCVR.dll").read_bytes())
out = bytearray((HERE.parent/"built"/"Stage3BL-HaloMCCVR.dll").read_bytes())
assert hashlib.sha256(base).hexdigest() == \
    "32bbdee8135ed66f3c5d5934799dedbcde3a293cfd66b7b7bac92b00c57105b7"
assert len(base) == len(out)

def parse(blob):
    p = struct.unpack_from("<I", blob, 0x3C)[0]; c=p+4
    n = struct.unpack_from("<H", blob, c+2)[0]
    osz = struct.unpack_from("<H", blob, c+16)[0]; st=c+20+osz
    return n, [ (bytes(blob[st+i*40:st+i*40+8]).split(b"\0")[0].decode(),) +
        struct.unpack_from("<IIII", blob, st+i*40+8) for i in range(n) ]
n, secs = parse(out)
assert n == 12
def off(rva):
    for name, vs, va, rs, rp in secs:
        if va <= rva < va + max(vs, rs): return rp + rva - va
    raise AssertionError(hex(rva))

SPLICE, THUNK, TAIL = 0x2FA2D7, 0x2FBFD8, 0x1DDD8
diff=[i for i in range(len(base)) if base[i]!=out[i]]
allowed=set(range(off(SPLICE),off(SPLICE)+8))|set(range(off(THUNK),off(THUNK)+25))
assert not [i for i in diff if i not in allowed], "stray changes"
assert diff, "no changes"

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64); md.detail=True
def dis(rva, size):
    return list(md.disasm(bytes(out[off(rva):off(rva)+size]), rva))
def rip(i):
    for op in i.operands:
        if op.type==x86.X86_OP_MEM and op.mem.base==x86.X86_REG_RIP:
            return i.address+i.size+op.mem.disp

s = dis(SPLICE, 8)
assert s[0].mnemonic=="jmp" and int(s[0].op_str,16)==THUNK
assert all(i.mnemonic=="nop" for i in s[1:])
t = dis(THUNK, 25)
assert t[0].mnemonic=="cmp" and rip(t[0])==0x2BA6C8 and \
    t[0].op_str.endswith(", 4"), "title gate"
assert t[1].mnemonic=="jne" and int(t[1].op_str,16)==t[4].address+t[4].size, \
    "non-H4 skips straight to the displaced tail"
assert t[2].mnemonic=="cmp" and t[2].op_str=="edi, r10d"
assert t[3].mnemonic=="jae"
assert t[4].mnemonic=="mov" and t[4].op_str=="edi, r10d", "colour ink publish"
assert t[5].mnemonic=="xor" and t[5].op_str=="r8d, r8d", "displaced xor"
assert t[6].mnemonic=="jmp" and int(t[6].op_str,16)==TAIL, "returns to tail"
# tail still returns edi
tl = dis(0x1DDF0, 2)
assert tl[0].mnemonic=="mov" and tl[0].op_str=="eax, edi"
# welded loop still sums alpha->edi and rgb->r10d
lp = dis(0x2FA26D, 5)
assert lp[0].op_str.startswith("eax, byte ptr [rdx + rcx*4 + 3]")
# 3BK/3BI artifacts + no 3BJ
assert out[off(0x2FB992)]==0xE8 and out[off(0x53921)]==0xE8
assert bytes(out[off(0x199B3):off(0x199B3)+2])==bytes.fromhex("b201")
for rva, size in ((0x2C0589,8),(0x2C5C16,1),(0x2C5C6B,1),(0x2BFF0C,1),
                  (0x2FBE40,292),(0x2FBF64,116)):
    assert out[off(rva):off(rva)+size]==base[off(rva):off(rva)+size]
print("PASS: Stage 3BL verified -- 33-byte diff confined; H4-only colour-ink "
      "publish decoded; other titles skip to the stock alpha path; tail "
      "returns edi; 3BK/3BI intact, 3BJ absent, 12 sections")
