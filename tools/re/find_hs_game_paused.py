"""Find halo4.dll's `game_paused` HS function definition and its evaluator.

1. Map file offset <-> RVA via section headers.
2. Locate the exact "game_paused" C string (NUL-bounded).
3. Scan the image for 8-aligned qwords equal to ImageBase+string_rva
   (candidate hs_function_definition name fields).
4. Dump the surrounding 0x40 bytes of each referencing record so the
   definition layout (return type, evaluator pointer) can be read.
"""
import struct

PATH = r"N:\SteamLibrary\steamapps\common\Halo The Master Chief Collection\halo4\halo4.dll"
b = open(PATH, "rb").read()
pe = struct.unpack_from("<I", b, 0x3C)[0]
coff = pe + 4
nsec = struct.unpack_from("<H", b, coff + 2)[0]
optsz = struct.unpack_from("<H", b, coff + 16)[0]
opt = coff + 20
image_base = struct.unpack_from("<Q", b, opt + 24)[0]
tab = opt + optsz
secs = []
for i in range(nsec):
    o = tab + i * 40
    name = b[o:o+8].split(b"\0")[0].decode()
    vs, va, rs, rp = struct.unpack_from("<IIII", b, o + 8)
    secs.append((name, va, vs, rs, rp))
    print(f"# section {name:8s} va=0x{va:08X} vs=0x{vs:08X} raw=0x{rs:08X}@0x{rp:08X}")

def to_rva(off):
    for name, va, vs, rs, rp in secs:
        if rp <= off < rp + rs:
            return va + (off - rp)
    return None

def to_off(rva):
    for name, va, vs, rs, rp in secs:
        if va <= rva < va + max(vs, rs):
            d = rva - va
            return rp + d if d < rs else None
    return None

# exact NUL-bounded string
needle = b"\x00game_paused\x00"
hits = []
i = b.find(needle)
while i != -1:
    hits.append(i + 1)
    i = b.find(needle, i + 1)
print("string hits:", [hex(h) for h in hits], [hex(to_rva(h)) for h in hits])

for h in hits:
    rva = to_rva(h)
    va = image_base + rva
    target = struct.pack("<Q", va)
    j = b.find(target)
    while j != -1:
        ref_rva = to_rva(j)
        print(f"\nreference qword at file 0x{j:08X} rva 0x{ref_rva:08X}")
        start = j - 0x18
        row = b[start:start + 0x48]
        for k in range(0, len(row), 8):
            q = struct.unpack_from("<Q", row, k)[0]
            note = ""
            if image_base < q < image_base + 0x04A3F000:
                r = q - image_base
                o = to_off(r)
                if o is not None:
                    s = b[o:o+48].split(b"\0")[0]
                    if s and all(32 <= c < 127 for c in s):
                        note = f" -> str {s.decode()!r}"
                    else:
                        note = f" -> rva 0x{r:08X}"
                else:
                    note = f" -> rva 0x{r:08X} (virtual)"
            print(f"  +0x{start + k - j:03X}: {q:016X}{note}")
        j = b.find(target, j + 1)
