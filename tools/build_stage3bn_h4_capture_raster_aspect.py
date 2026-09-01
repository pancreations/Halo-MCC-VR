"""Stage 3BN - Halo 4 capture viewport takes the raster's aspect ratio.

Input: exact Stage 3BM DLL.  One 5-byte change: the 3BK gate call at
0x2FB992 (`call 0x2FBF64`) is re-pointed to the new thunk placed after the
3BM payload in the 3AS page tail.  See stage3bn_h4_capture_raster_aspect.S.
"""
from pathlib import Path
import hashlib, struct, sys
sys.path.insert(0, str(Path(__file__).parent))
from postlink import build_payload

EXPECTED_STAGE3BM_SHA256 = \
    "1873582cadb4937220ff3d26e1098e343c72fe9e0492bbc972d69a384227a206"
PAYLOAD_RVA = 0x002F93E0            # 16-aligned, after the 914-byte 3BM payload
PAYLOAD_LIMIT = 0x002FA000
GATE_CALL_RVA = 0x002FB992
GATE_OLD = bytes.fromhex("e8cd050000")   # call 0x2FBF64 (3BK thunk)
CAPTURE_VIEWPORT_RVA = 0x002AE774
BB_WIDTH_RVA = 0x002AEB58
BB_HEIGHT_RVA = 0x002AEB5C
KBIAS = 104.0 / 1346.196

CONTEXT = (
    (0x002FB997, bytes.fromhex("90" * 19), "3BK gate: 19 nops after the call"),
    (0x002FB9AA, bytes.fromhex("4889d9ba010000004c8d05d32dfbff"),
     "scissor re-assert follows"),
    (0x002FBF64, bytes.fromhex("4883ec28"), "3BK thunk still present (now unused)"),
    # Begin reads the backbuffer desc Width/Height at these globals
    (0x00011CC0, bytes.fromhex("8b0592ce2900"), "Begin loads g_gameBackbufferDesc.Width"),
    (0x00011CE1, bytes.fromhex("8b0575ce2900"), "Begin loads g_gameBackbufferDesc.Height"),
    (0x00011DBF, bytes.fromhex("0f1105aec92900"), "Begin stores captureViewport at 0x2AE774"),
    (0x002FB800, struct.pack("<ffff", 4134.312, 1346.196, 0.0, 1.0), "3BH framing constant"),
    # 3BM / 3BL / 3BI artifacts
    (0x002FBA58, bytes.fromhex("e8"), "3BM tap splice"),
    (0x002FA2D7, bytes.fromhex("e9"), "3BL splice"),
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
        name = bytes(blob[o:o+8]).split(b"\0", 1)[0].decode()
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
    if sha != EXPECTED_STAGE3BM_SHA256:
        raise SystemExit("wrong Stage3BM input DLL: " + sha)
    pe = parse_pe(blob)
    if pe["n"] != 12:
        raise SystemExit("unexpected PE geometry")
    for rva, expect, label in CONTEXT:
        o = rva_off(pe, rva)
        if bytes(blob[o:o+len(expect)]) != expect:
            raise SystemExit(f"{label}: expected {expect.hex()} at 0x{rva:X}, "
                             f"got {bytes(blob[o:o+len(expect)]).hex()}")
    # the two Begin loads must resolve to the RVAs we hand the payload
    if 0x11CC6 + struct.unpack_from("<i", blob, rva_off(pe, 0x11CC2))[0] != BB_WIDTH_RVA:
        raise SystemExit("backbuffer Width RVA mismatch")
    if 0x11CE7 + struct.unpack_from("<i", blob, rva_off(pe, 0x11CE3))[0] != BB_HEIGHT_RVA:
        raise SystemExit("backbuffer Height RVA mismatch")
    go = rva_off(pe, GATE_CALL_RVA)
    if bytes(blob[go:go+5]) != GATE_OLD:
        raise SystemExit("gate call unexpected: " + bytes(blob[go:go+5]).hex())
    po = rva_off(pe, PAYLOAD_RVA)
    pl = rva_off(pe, PAYLOAD_LIMIT-1)+1
    if any(blob[po:pl]):
        raise SystemExit("payload region not free")
    code, symbols = build_payload(
        Path(__file__).parent / "stage3bn_h4_capture_raster_aspect.S",
        PAYLOAD_RVA,
        defs=dict(CAPTURE_VIEWPORT=CAPTURE_VIEWPORT_RVA,
                  BB_WIDTH=BB_WIDTH_RVA, BB_HEIGHT=BB_HEIGHT_RVA),
        want_symbols=("s3bn_aspect_viewport", "s3bn_data"))
    if PAYLOAD_RVA + len(code) > PAYLOAD_LIMIT:
        raise SystemExit(f"payload too large: {len(code)}")
    thunk = symbols["s3bn_aspect_viewport"]; data = symbols["s3bn_data"]
    # constants must be exactly what the docstring says
    k512, khalf, kbias, k104 = struct.unpack_from("<ffff", code, data - PAYLOAD_RVA)
    if (k512, khalf, k104) != (512.0, 0.5, 104.0) or abs(kbias - KBIAS) > 1e-6:
        raise SystemExit(f"payload constants wrong: {k512} {khalf} {kbias} {k104}")
    print(f"payload {len(code)} bytes at 0x{PAYLOAD_RVA:X}; thunk 0x{thunk:X}, data 0x{data:X}; "
          f"kbias {kbias:.7f}")
    blob[po:po+len(code)] = code
    blob[go:go+5] = b"\xE8" + struct.pack("<i", thunk - (GATE_CALL_RVA + 5))
    print(f"gate 0x{GATE_CALL_RVA:X}: call 3BK thunk -> call 0x{thunk:X}")
    if len(blob) != original_len:
        raise SystemExit("size changed")
    out.write_bytes(bytes(blob))
    print("output", hashlib.sha256(bytes(blob)).hexdigest(), len(blob))


if __name__ == "__main__":
    main()
