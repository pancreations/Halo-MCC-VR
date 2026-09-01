"""Find rip-relative references in halo4.dll .text to a target RVA range.

Approximation: for every dword at file position p inside .text, treat p+4 as
the end of an instruction; the reference target is rva(p) + 4 + disp. Also try
p+5..p+8 ends (instructions with imm8/imm32 after the disp) so mov [rip+d],imm
forms are caught too.
"""
import struct
import sys

PATH = r"N:\SteamLibrary\steamapps\common\Halo The Master Chief Collection\halo4\halo4.dll"
lo = int(sys.argv[1], 16)
hi = int(sys.argv[2], 16)
b = open(PATH, "rb").read()
pe = struct.unpack_from("<I", b, 0x3C)[0]
coff = pe + 4
nsec = struct.unpack_from("<H", b, coff + 2)[0]
optsz = struct.unpack_from("<H", b, coff + 16)[0]
opt = coff + 20
tab = opt + optsz
text = None
for i in range(nsec):
    o = tab + i * 40
    name = b[o:o+8].split(b"\0")[0].decode()
    vs, va, rs, rp = struct.unpack_from("<IIII", b, o + 8)
    if name == ".text":
        text = (va, vs, rs, rp)
assert text
va, vs, rs, rp = text
end = rp + rs
found = 0
for p in range(rp, end - 4):
    disp = struct.unpack_from("<i", b, p)[0]
    if disp == 0:
        continue
    base_rva = va + (p - rp)
    for tail in (4, 5, 8):
        target = base_rva + tail + disp
        if lo <= target < hi:
            ctx = b[max(p - 8, 0):p + 8].hex()
            print(f"disp at rva 0x{base_rva:08X} (instr-end +{tail}) -> 0x{target:08X}  bytes ...{ctx}...")
            found += 1
            break
    if found > 40:
        break
print("total", found, file=sys.stderr)
