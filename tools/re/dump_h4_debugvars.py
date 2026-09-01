"""Offline dump of halo4.dll's debug-var table ({name_ptr, type, value_ptr,
help_ptr} entries, same layout FindDebugVarSlot walks at runtime).

Reads the DLL file, maps sections to build an RVA->offset map, then scans all
data for 8-aligned entries whose name_ptr and value_ptr both point inside the
image and whose name is a plausible C identifier. Prints name, type,
value RVA.
"""
import re
import struct
import sys

PATH = sys.argv[1]
b = open(PATH, "rb").read()
pe = struct.unpack_from("<I", b, 0x3C)[0]
assert b[pe:pe+4] == b"PE\0\0"
coff = pe + 4
nsec = struct.unpack_from("<H", b, coff + 2)[0]
optsz = struct.unpack_from("<H", b, coff + 16)[0]
opt = coff + 20
magic = struct.unpack_from("<H", b, opt)[0]
assert magic == 0x20B
image_base = struct.unpack_from("<Q", b, opt + 24)[0]
size_of_image = struct.unpack_from("<I", b, opt + 0x38)[0]
tab = opt + optsz
secs = []
for i in range(nsec):
    o = tab + i * 40
    name = b[o:o+8].split(b"\0")[0].decode()
    vs, va, rs, rp = struct.unpack_from("<IIII", b, o + 8)
    secs.append((name, va, vs, rs, rp))

def off(rva):
    for name, va, vs, rs, rp in secs:
        if va <= rva < va + max(vs, rs):
            d = rva - va
            if d < rs:
                return rp + d
            return None  # in virtual-only tail (zero fill)
    return None

def read_at_rva(rva, n):
    o = off(rva)
    if o is None or o + n > len(b):
        return None
    return b[o:o+n]

ident = re.compile(rb"^[A-Za-z_][A-Za-z0-9_]{2,79}$")
lo = image_base
hi = image_base + size_of_image
found = 0
for name, va, vs, rs, rp in secs:
    if name in (".text", ".rsrc", ".reloc", ".pdata"):
        continue
    end = rp + min(vs, rs)
    for o in range(rp, end - 32, 8):
        namep, typ, valp = struct.unpack_from("<QQQ", b, o)
        if not (lo < namep < hi and lo < valp < hi):
            continue
        if typ > 16:
            continue
        raw = read_at_rva(namep - image_base, 81)
        if not raw:
            continue
        s = raw.split(b"\0")[0]
        if not s or not ident.match(s):
            continue
        # previous byte must be NUL (string start) like the runtime scan
        entry_rva = va + (o - rp)
        print(f"{s.decode():60s} type={typ:2d} value_rva=0x{valp - image_base:08X} entry_rva=0x{entry_rva:08X}")
        found += 1
print(f"# total candidate entries: {found}", file=sys.stderr)
