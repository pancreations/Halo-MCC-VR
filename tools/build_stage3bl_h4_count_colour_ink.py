"""Stage 3BL - Halo 4 ink measurement counts colour ink, not just alpha.

Input: exact Stage 3BK DLL.  One 8-byte splice at 0x2FA2D7 (the welded
probe loop's tail) -> a ~25-byte thunk: for Halo 4 only, when the RGB sum
(R10D, already computed by the existing welded loop but used only for
telemetry) exceeds the alpha sum (EDI, the published ink), publish the RGB
sum.  All other titles unchanged.  See STAGE3BL notes.
"""
from pathlib import Path
import hashlib, struct, sys
sys.path.insert(0, str(Path(__file__).parent))
from postlink import build_payload

EXPECTED_STAGE3BK_SHA256 = \
    "32bbdee8135ed66f3c5d5934799dedbcde3a293cfd66b7b7bac92b00c57105b7"
PAYLOAD_RVA = 0x002FBFD8
PAYLOAD_LIMIT = 0x002FC000
SPLICE_RVA = 0x002FA2D7
SPLICE_OLD = bytes.fromhex("4531c0e9f93ad2ff")   # xor r8d,r8d; jmp 0x1DDD8
ACTIVE_TITLE_RVA = 0x002BA6C8
MEASURE_TAIL_RVA = 0x0001DDD8

CONTEXT = (
    (0x002FA26D, bytes.fromhex("0fb6448a03"),
     "welded loop reads texel alpha [rdx+rcx*4+3]"),
    (0x002FA274, bytes.fromhex("0fb6048a4101c2"),
     "welded loop sums R into r10d"),
    (0x002FA2A1, bytes.fromhex("44891554000000"),
     "RGB sum stored to telemetry dword"),
    (0x0001DD8B, bytes.fromhex("e9cbc42d00"),
     "measure loop rerouted to the welded loop (0x2FA25B)"),
    (MEASURE_TAIL_RVA, bytes.fromhex("488b0db9042900"),
     "measure tail Unmap sequence"),
    (0x0001DDF0, bytes.fromhex("8bc7"),
     "measure returns edi as the ink"),
    (0x000879C0, bytes.fromhex("0fb605012d2300"),
     "TitleAdapter_GetActiveTitle reads 0x2BA6C8"),
    # 3BK artifacts intact
    (0x002FB992, bytes.fromhex("e8"), "3BK gate splice"),
    (0x00053921, bytes.fromhex("e8"), "3BI capture-selection splice"),
    (0x000199B3, bytes.fromhex("b201"), "3BJ NOT included"),
)

def parse_pe(blob):
    p = struct.unpack_from("<I", blob, 0x3C)[0]; coff = p + 4
    n = struct.unpack_from("<H", blob, coff + 2)[0]
    osz = struct.unpack_from("<H", blob, coff + 16)[0]
    st = coff + 20 + osz; secs = []
    for i in range(n):
        o = st + i * 40
        name = bytes(blob[o:o+8]).split(b"\0",1)[0].decode()
        vs, va, rs, rp = struct.unpack_from("<IIII", blob, o+8)
        secs.append(dict(name=name, vs=vs, va=va, rs=rs, rp=rp))
    return dict(opt=coff+20, n=n, sections=secs)

def rva_off(pe, rva):
    for s in pe["sections"]:
        if s["va"] <= rva < s["va"] + max(s["vs"], s["rs"]):
            return s["rp"] + rva - s["va"]
    raise KeyError(hex(rva))

def main():
    src, out = Path(sys.argv[1]), Path(sys.argv[2])
    blob = bytearray(src.read_bytes())
    original_len = len(blob)
    sha = hashlib.sha256(blob).hexdigest()
    print("input", sha)
    if sha != EXPECTED_STAGE3BK_SHA256:
        raise SystemExit("wrong Stage3BK input: " + sha)
    pe = parse_pe(blob)
    if pe["n"] != 12:
        raise SystemExit("unexpected PE geometry")
    for rva, expect, label in CONTEXT:
        o = rva_off(pe, rva)
        if bytes(blob[o:o+len(expect)]) != expect:
            raise SystemExit(f"{label}: got "
                             f"{bytes(blob[o:o+len(expect)]).hex()} at 0x{rva:X}")
    so = rva_off(pe, SPLICE_RVA)
    if bytes(blob[so:so+8]) != SPLICE_OLD:
        raise SystemExit("splice bytes unexpected: " + bytes(blob[so:so+8]).hex())
    disp = struct.unpack_from("<i", blob, so+4)[0]
    if SPLICE_RVA + 3 + 5 + disp != MEASURE_TAIL_RVA:
        raise SystemExit("displaced jmp does not target the measure tail")
    po = rva_off(pe, PAYLOAD_RVA)
    pl = rva_off(pe, PAYLOAD_LIMIT-1)+1
    if any(blob[po:pl]):
        raise SystemExit("payload region not free")
    code, symbols = build_payload(
        Path(__file__).parent / "stage3bl_h4_count_colour_ink.S",
        PAYLOAD_RVA,
        defs=dict(ACTIVE_TITLE=ACTIVE_TITLE_RVA, MEASURE_TAIL=MEASURE_TAIL_RVA),
        want_symbols=("s3bl_colour_ink",))
    if PAYLOAD_RVA + len(code) > PAYLOAD_LIMIT:
        raise SystemExit(f"payload too large: {len(code)}")
    thunk = symbols["s3bl_colour_ink"]
    print(f"payload {len(code)} bytes at 0x{PAYLOAD_RVA:X}")
    blob[po:po+len(code)] = code
    jd = thunk - (SPLICE_RVA + 5)
    blob[so:so+8] = b"\xE9" + struct.pack("<i", jd) + b"\x90"*3
    print(f"splice 0x{SPLICE_RVA:X} -> jmp 0x{thunk:X} + 3 nops")
    if len(blob) != original_len:
        raise SystemExit("size changed")
    out.write_bytes(bytes(blob))
    print("output", hashlib.sha256(bytes(blob)).hexdigest(), len(blob))

if __name__ == "__main__":
    main()
