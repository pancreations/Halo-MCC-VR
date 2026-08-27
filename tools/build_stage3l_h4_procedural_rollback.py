from pathlib import Path
import hashlib, struct, sys

EXPECTED_STAGE3I_SHA256 = "36eec37e0778b5a23726c724a4ebb5f9fcd9b46ebd05c4344ace12b2d3778677"

# Exact Stage 3H H4 post-link edits currently present in Stage 3I -> exact
# pre-Stage-3H Stage 3G/C-H4-50 bytes. H2 helper bytes and Stage 3I cleanup pin
# are deliberately outside this table.
PATCHES = [
    (0x48086,
     "3c04752b32c04883c428c3" + "90" * (0x2F - 11),
     "3c04752b0fb60557f125009084c074180fb6059bf125009084c0740c0fb60541f125009084c0744e32c04883c428c3",
     "restore pre-Stage3H H4 authored-capture policy"),
    (0x48110,
     "e93e742a009090909090909090909090",
     "4883ec28e8a7f8030032c04883c428c3",
     "restore pre-Stage3H H4 reticle proof"),
    (0x5360B,
     "488b8c2488000000e8e8b92900e96f000000" + "90" * (0x81 - 18),
     "33c038054aa625000f95c03943087571803d8fa52500007468803dd9a5250000745fe85ed2fdff84c0745666c743040100c6430600448864242844897c24204c8bac24900000004d8bcd4c8b8424880000008bd78bce41ffd6c6430400807b0200740733c9e84b0e0000807b06007519488d156b000000488b4c2438e8fc1d0f00",
     "restore pre-Stage3H normal H4 CUI gameplay pass"),
    (0x53D41,
     "488d4c2448e860b3290084c00f84fe0000000f28f0440f28c9e963000000" + "90" * (0x81 - 30),
     "410f28c0450f57c90f57f632dbe88d0e10006685c07f620f28c7e8800e10006685c07f55f30f101d77511300410f28c80f540d885213000f28d70f2fd90f54157b52130077330f2fda772ef30f1005284518000f2fc877210f2fd0771c0f28f1f30f5935bf511300660f7ef0250000807f3d0000807f0f95c384db0f848f000000",
     "remove Stage3H native-position override"),
    (0x53E37,
     "4c89f94889fae8e6b62900e912000000" + "90" * 10 + "f048ff",
     "b8b803000049634c0708498b44071890488984cf90110000eb08f048ff",
     "restore pre-Stage3H H4 reticle publication path"),
]

def parse_pe(blob):
    peoff = struct.unpack_from('<I', blob, 0x3c)[0]
    if blob[:2] != b'MZ' or blob[peoff:peoff+4] != b'PE\0\0':
        raise SystemExit('not a PE file')
    coff = peoff + 4
    nsects = struct.unpack_from('<H', blob, coff+2)[0]
    optsz = struct.unpack_from('<H', blob, coff+16)[0]
    opt = coff + 20
    table = opt + optsz
    sections=[]
    for i in range(nsects):
        o=table+i*40
        name=bytes(blob[o:o+8]).split(b'\0',1)[0].decode('ascii')
        vs,va,rs,rp=struct.unpack_from('<IIII',blob,o+8)
        sections.append((name,vs,va,rs,rp,o))
    return opt, sections

def rva_off(sections, rva):
    for name,vs,va,rs,rp,o in sections:
        if va <= rva < va + max(vs,rs):
            return rp + (rva-va)
    raise SystemExit(f'RVA not mapped: 0x{rva:X}')

def main():
    if len(sys.argv) != 3:
        raise SystemExit('usage: build_stage3l_h4_procedural_rollback.py <Stage3I.dll> <output.dll>')
    src=Path(sys.argv[1]); out=Path(sys.argv[2])
    blob=bytearray(src.read_bytes())
    sha=hashlib.sha256(blob).hexdigest()
    if sha != EXPECTED_STAGE3I_SHA256:
        raise SystemExit('wrong Stage 3I input DLL: ' + sha)
    opt, sections=parse_pe(blob)
    for rva, old_hex, new_hex, label in PATCHES:
        old=bytes.fromhex(old_hex); new=bytes.fromhex(new_hex)
        if len(old) != len(new):
            raise SystemExit(f'{label}: length mismatch {len(old)} != {len(new)}')
        o=rva_off(sections,rva)
        actual=bytes(blob[o:o+len(old)])
        if actual != old:
            raise SystemExit(f'{label}: Stage 3I guard failed at 0x{rva:X}: {actual.hex()}')
        blob[o:o+len(old)]=new
        print(f'{label}: RVA 0x{rva:X}, {len(old)} bytes')
    # Match existing post-link convention.
    struct.pack_into('<I', blob, opt+0x40, 0)
    out.write_bytes(blob)
    print('input sha256 ', sha)
    print('output sha256', hashlib.sha256(blob).hexdigest())
    print('output size  ', len(blob))

if __name__ == '__main__': main()
