"""Stage 3BP - the Halo 4 capture keeps only the weapon screen's reticle
container; polyart containers (damage / grenade indicators) go offscreen.

Input: exact Stage 3BO DLL.  Two changes: the 3BI splice call at 0x53921 is
re-pointed from the 3BI selector (0x2FBE40) to the new selector placed after
the 3BO HLSL text in the 3AS page tail.  See stage3bp_h4_capture_weapon_only.S.
"""
from pathlib import Path
import hashlib, struct, sys
sys.path.insert(0, str(Path(__file__).parent))
from postlink import build_payload

EXPECTED_STAGE3BO_SHA256 = \
    "129a24df07edf78ca1919e2ddaa36663d15fcbb34d5981b7fc0ab1e49b107f1b"
PAYLOAD_RVA = 0x002F98A0            # 16-aligned, after the 3BO HLSL text (ends 0x2F9894)
PAYLOAD_LIMIT = 0x002FA000
SPLICE_RVA = 0x00053921
SPLICE_OLD = bytes.fromhex("e81a852a00") + b"\x90\x90"   # call 0x2FBE40 (3BI) + 2 nops
G_ORIG_CUI_RENDER_RVA = 0x002B9B18
HALO4_SAFE_READ_RVA = 0x00056C10
CAPTURE_FRAMING_CONST_RVA = 0x002FB800

CONTEXT = (
    (HALO4_SAFE_READ_RVA, bytes.fromhex("4883ec28488bc2488bd1488bc8e8"), "Halo4SafeRead prologue"),
    (CAPTURE_FRAMING_CONST_RVA, struct.pack("<ffff", 4134.312, 1346.196, 0.0, 1.0), "3BH framing constant"),
    (0x002FBE40, bytes.fromhex("4883ec38"), "3BI selector still present (now unused)"),
    # 3BO artifacts
    (0x00011E76, bytes.fromhex("e8"), "3BO fast-path call"),
    (0x00011EB6, bytes.fromhex("e8"), "3BO log-gate call"),
    (0x002F9540, b"\nTexture2D srcTex", "3BO HLSL text"),
    (0x002F9892, b"\n\0", "3BO HLSL text last byte + NUL (851 bytes from 0x2F9540)"),
    (0x002FB992, bytes.fromhex("e849daffff"), "3BN gate call"),
    (0x002FBA58, bytes.fromhex("e8"), "3BM tap splice"),
    (0x002FA2D7, bytes.fromhex("e9"), "3BL splice"),
    (0x000199B3, bytes.fromhex("b201"), "3BJ NOT included"),
)


def parse_pe(blob):
    p = struct.unpack_from("<I", blob, 0x3C)[0]; coff = p + 4
    n = struct.unpack_from("<H", blob, coff + 2)[0]
    osz = struct.unpack_from("<H", blob, coff + 16)[0]
    st = coff + 20 + osz; secs = []
    for i in range(n):
        o = st + i * 40
        name = bytes(blob[o:o+8]).split(b"\0", 1)[0].decode()
        vs, va, rs, rp = struct.unpack_from("<IIII", blob, o+8)
        secs.append(dict(name=name, vs=vs, va=va, rs=rs, rp=rp))
    return dict(n=n, sections=secs)


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
    if sha != EXPECTED_STAGE3BO_SHA256:
        raise SystemExit("wrong Stage3BO input DLL: " + sha)
    pe = parse_pe(blob)
    if pe["n"] != 12:
        raise SystemExit("unexpected PE geometry")
    for rva, expect, label in CONTEXT:
        o = rva_off(pe, rva)
        if bytes(blob[o:o+len(expect)]) != expect:
            raise SystemExit(f"{label}: expected {expect.hex()} at 0x{rva:X}, "
                             f"got {bytes(blob[o:o+len(expect)]).hex()}")
    so = rva_off(pe, SPLICE_RVA)
    if bytes(blob[so:so+7]) != SPLICE_OLD:
        raise SystemExit("splice site unexpected: " + bytes(blob[so:so+7]).hex())
    if SPLICE_RVA + 5 + struct.unpack_from("<i", blob, so + 1)[0] != 0x2FBE40:
        raise SystemExit("splice does not target the 3BI selector")
    po = rva_off(pe, PAYLOAD_RVA)
    pl = rva_off(pe, PAYLOAD_LIMIT-1)+1
    if any(blob[po:pl]):
        raise SystemExit("payload region not free")

    code, symbols = build_payload(
        Path(__file__).parent / "stage3bp_h4_capture_weapon_only.S",
        PAYLOAD_RVA,
        defs=dict(G_ORIG_CUI_RENDER=G_ORIG_CUI_RENDER_RVA,
                  HALO4_SAFE_READ=HALO4_SAFE_READ_RVA,
                  CAPTURE_FRAMING_CONST=CAPTURE_FRAMING_CONST_RVA),
        want_symbols=("s3bp_capture_select", "s3bp_data"))
    if PAYLOAD_RVA + len(code) > PAYLOAD_LIMIT:
        raise SystemExit(f"payload too large: {len(code)}")
    sel = symbols["s3bp_capture_select"]; data = symbols["s3bp_data"]
    inside, poly, half = struct.unpack_from("<IIf", code, data - PAYLOAD_RVA)
    if (inside, poly, half) != (0, 0, 0.5):
        raise SystemExit(f"payload data wrong: {inside} {poly} {half}")
    print(f"payload {len(code)} bytes at 0x{PAYLOAD_RVA:X}; selector 0x{sel:X}, data 0x{data:X}")
    blob[po:po+len(code)] = code
    blob[so:so+5] = b"\xE8" + struct.pack("<i", sel - (SPLICE_RVA + 5))
    print(f"splice 0x{SPLICE_RVA:X}: call 3BI 0x2FBE40 -> call 0x{sel:X}")
    if len(blob) != original_len:
        raise SystemExit("size changed")
    out.write_bytes(bytes(blob))
    print("output", hashlib.sha256(bytes(blob)).hexdigest(), len(blob))


if __name__ == "__main__":
    main()
