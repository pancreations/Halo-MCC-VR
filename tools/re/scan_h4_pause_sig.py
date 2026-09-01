"""Scan halo4.dll for the H3/ODST native-pause-owner code shape and close
variants: a small evaluator that ends by storing a boolean register to a
rip-relative global.

Primary pattern (halo3/halo3odst kNativePauseOwnerSig):
  E8 ?? ?? ?? ?? 84 C0 74 18 B9 03 00 00 00
  E8 ?? ?? ?? ?? 84 C0 75 0A 8B D1 40 8A CE
  E8 ?? ?? ?? ?? 40 88 35 ?? ?? ?? ?? E9 ?? ?? ?? ??

Also tries relaxed variants (different jz distance byte, dil instead of sil).
Prints every hit with the resolved flag RVA.
"""
import struct
import sys

PATH = sys.argv[1]
b = open(PATH, "rb").read()
pe = struct.unpack_from("<I", b, 0x3C)[0]
coff = pe + 4
nsec = struct.unpack_from("<H", b, coff + 2)[0]
optsz = struct.unpack_from("<H", b, coff + 16)[0]
opt = coff + 20
tab = opt + optsz
secs = []
for i in range(nsec):
    o = tab + i * 40
    name = b[o:o+8].split(b"\0")[0].decode()
    vs, va, rs, rp = struct.unpack_from("<IIII", b, o + 8)
    secs.append((name, va, vs, rs, rp))

def rva_of(off):
    for name, va, vs, rs, rp in secs:
        if rp <= off < rp + rs:
            return va + (off - rp)
    return None

def scan(sig_hex, label):
    toks = sig_hex.split()
    pat = bytes(int(t, 16) if t != "??" else 0 for t in toks)
    mask = bytes(0xFF if t != "??" else 0 for t in toks)
    n = len(pat)
    hits = []
    # anchor on first fixed byte run for speed: just brute force with step 1
    first = pat[0]
    i = b.find(bytes([first]))
    while i != -1 and i + n <= len(b):
        ok = True
        for j in range(n):
            if mask[j] and b[i + j] != pat[j]:
                ok = False
                break
        if ok:
            hits.append(i)
        i = b.find(bytes([first]), i + 1)
    for h in hits:
        rva = rva_of(h)
        print(f"{label}: file 0x{h:X} rva 0x{rva:X}" if rva is not None else f"{label}: file 0x{h:X} (no rva)")
    return hits

# exact H3 shape
scan("E8 ?? ?? ?? ?? 84 C0 74 18 B9 03 00 00 00 E8 ?? ?? ?? ?? 84 C0 75 0A 8B D1 40 8A CE E8 ?? ?? ?? ?? 40 88 35 ?? ?? ?? ?? E9", "exact-h3-shape")
# relaxed: wildcard the jz distance and the mov ecx immediate
scan("E8 ?? ?? ?? ?? 84 C0 74 ?? B9 ?? 00 00 00 E8 ?? ?? ?? ?? 84 C0 75 ?? 8B D1 40 8A CE E8 ?? ?? ?? ?? 40 88 35", "relaxed-a")
# any: test al,al ; setcc-free bool store of sil/dil to rip global right after a call
scan("E8 ?? ?? ?? ?? 40 88 35 ?? ?? ?? ?? E9", "tail-sil-store")
scan("E8 ?? ?? ?? ?? 40 88 3D ?? ?? ?? ?? E9", "tail-dil-store")
print("done", file=sys.stderr)
