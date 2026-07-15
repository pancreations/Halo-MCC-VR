# Offline disassembler for halo3.dll (on-disk PE -> RVA addressing).
# Read-only. Never modifies the game file.
#   py -3 pedis.py fn   <rva_hex> [maxbytes]     - disassemble a function
#   py -3 pedis.py scan <off_hex> [off_hex ...]  - find [reg+off] memory operands
import sys, struct
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

PATH = r"N:\SteamLibrary\steamapps\common\Halo The Master Chief Collection\halo3\halo3.dll"

def load():
    data = open(PATH, "rb").read()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    assert data[pe:pe+4] == b"PE\0\0"
    nsec = struct.unpack_from("<H", data, pe+6)[0]
    optsz = struct.unpack_from("<H", data, pe+20)[0]
    imgbase = struct.unpack_from("<Q", data, pe+24+24)[0]
    secs = []
    off = pe + 24 + optsz
    for i in range(nsec):
        b = data[off:off+40]
        name = b[0:8].rstrip(b"\0").decode(errors="ignore")
        vsize, vaddr, rawsize, rawptr = struct.unpack_from("<IIII", b, 8)
        chars = struct.unpack_from("<I", b, 36)[0]
        secs.append(dict(name=name, va=vaddr, vsize=vsize, raw=rawptr, rawsize=rawsize, exec=bool(chars & 0x20000000)))
        off += 40
    return data, secs, imgbase

def rva2off(secs, rva):
    for s in secs:
        if s["va"] <= rva < s["va"] + max(s["vsize"], s["rawsize"]):
            d = rva - s["va"]
            if d < s["rawsize"]:
                return s["raw"] + d
    return None

md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = True
data, secs, imgbase = load()

def fn(rva, maxb=1400):
    o = rva2off(secs, rva)
    if o is None:
        print("rva not mapped"); return
    code = data[o:o+maxb]
    depth = 0
    for i in md.disasm(code, rva):
        marks = ""
        for op in i.operands:
            if op.type == 3 and op.mem.disp != 0:  # X86_OP_MEM
                marks = "   ; disp=0x%X" % (op.mem.disp & 0xFFFFFFFF)
        print("%08X  %-24s %s %s%s" % (i.address, i.bytes.hex(), i.mnemonic, i.op_str, marks))
        if i.mnemonic == "ret":
            depth += 1
            if depth >= 1 and i.address - rva > 40:
                break

def scan(offsets):
    # capstone's disasm() stops dead at the first undecodable byte; a linear
    # sweep of a real .text hits data islands constantly. Resync by advancing
    # one byte and restarting, or whole sections are silently skipped.
    want = set(offsets)
    hits = 0
    for s in [x for x in secs if x["exec"]]:
        code = data[s["raw"]:s["raw"]+s["rawsize"]]
        print("== section %s rva=%08X size=%X" % (s["name"], s["va"], s["rawsize"]))
        pos = 0
        n = len(code)
        while pos < n:
            last = pos
            for i in md.disasm(code[pos:], s["va"] + pos):
                last = i.address - s["va"] + i.size
                for op in i.operands:
                    if op.type == 3 and op.mem.disp in want and op.mem.base != 0 and op.mem.index == 0:
                        print("%08X  %-20s %s %s" % (i.address, i.bytes.hex(), i.mnemonic, i.op_str))
                        hits += 1
            pos = last + 1 if last <= pos else last
    print("== total hits: %d" % hits)

def off2rva(secs, off):
    for s in secs:
        if s["raw"] <= off < s["raw"] + s["rawsize"]:
            return s["va"] + (off - s["raw"])
    return None

def sig(pattern):
    import re
    pat = b""
    for tok in pattern.split():
        pat += b"." if tok == "??" else re.escape(bytes([int(tok, 16)]))
    hits = [m.start() for m in re.finditer(pat, data, re.DOTALL)]
    print("matches: %d" % len(hits))
    for h in hits:
        print("  file 0x%X  rva 0x%X" % (h, off2rva(secs, h)))

if __name__ == "__main__":
    cmd = sys.argv[1]
    if cmd == "sig":
        sig(sys.argv[2])
    elif cmd == "fn":
        fn(int(sys.argv[2], 16), int(sys.argv[3]) if len(sys.argv) > 3 else 1400)
    elif cmd == "scan":
        scan([int(x, 16) for x in sys.argv[2:]])
