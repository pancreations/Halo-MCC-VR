"""Stage 3BR - the H4 capture draws 2.5x larger about the reticle centre.
Input: exact Stage 3BQ DLL.  One 5-byte change: the gate call at 0x2FB992
(-> 3BN thunk 0x2F93E0) is re-pointed to s3br_scaled_viewport."""
from pathlib import Path
import hashlib, struct, sys
sys.path.insert(0, str(Path(__file__).parent))
from postlink import build_payload

EXPECTED_INPUT_SHA256 = \
    "c9d95aa9aabeb2ed07c232d292c3ab4bbcd91300c2d67edd6921aabef0078f5d"
PAYLOAD_RVA = 0x002F9A90            # after the 3BQ thunk (ends 0x2F9A8B)
PAYLOAD_LIMIT = 0x002F9C50
GATE_CALL_RVA = 0x002FB992
GATE_OLD = bytes.fromhex("e849daffff")   # call 0x2F93E0 (3BN thunk)
CAPTURE_VIEWPORT_RVA = 0x002AE774
BB_WIDTH_RVA = 0x002AEB58
BB_HEIGHT_RVA = 0x002AEB5C

CONTEXT = (
    (0x002FB997, bytes.fromhex("90" * 19), "gate: 19 nops after the call"),
    (0x002F93E0, bytes.fromhex("4883ec28"), "3BN thunk still present (now unused)"),
    (0x002F9A30, bytes.fromhex("4883ec58"), "3BQ thunk present"),
    (0x00011CC0, bytes.fromhex("8b0592ce2900"), "Begin loads backbuffer Width"),
    (0x00011CE1, bytes.fromhex("8b0575ce2900"), "Begin loads backbuffer Height"),
    (0x000199B3, bytes.fromhex("b201"), "3BJ NOT included"),
)


def parse_pe(blob):
    p = struct.unpack_from("<I", blob, 0x3C)[0]; coff = p + 4
    n = struct.unpack_from("<H", blob, coff + 2)[0]
    osz = struct.unpack_from("<H", blob, coff + 16)[0]
    st = coff + 20 + osz; secs = []
    for i in range(n):
        o = st + i * 40
        vs, va, rs, rp = struct.unpack_from("<IIII", blob, o+8)
        secs.append(dict(vs=vs, va=va, rs=rs, rp=rp))
    return dict(n=n, sections=secs)


def rva_off(pe, rva):
    for s in pe["sections"]:
        if s["va"] <= rva < s["va"] + max(s["vs"], s["rs"]):
            return s["rp"] + rva - s["va"]
    raise KeyError(hex(rva))


def main():
    src, out = Path(sys.argv[1]), Path(sys.argv[2])
    blob = bytearray(src.read_bytes())
    sha = hashlib.sha256(blob).hexdigest()
    print("input", sha)
    if sha != EXPECTED_INPUT_SHA256:
        raise SystemExit("wrong input DLL: " + sha)
    pe = parse_pe(blob)
    if pe["n"] != 12:
        raise SystemExit("unexpected PE geometry")
    for rva, expect, label in CONTEXT:
        o = rva_off(pe, rva)
        if bytes(blob[o:o+len(expect)]) != expect:
            raise SystemExit(f"{label}: got {bytes(blob[o:o+len(expect)]).hex()}")
    go = rva_off(pe, GATE_CALL_RVA)
    if bytes(blob[go:go+5]) != GATE_OLD:
        raise SystemExit("gate call unexpected: " + bytes(blob[go:go+5]).hex())
    po, pl = rva_off(pe, PAYLOAD_RVA), rva_off(pe, PAYLOAD_LIMIT-1)+1
    if any(blob[po:pl]):
        raise SystemExit("payload region not free")
    code, symbols = build_payload(
        Path(__file__).parent / "stage3br_h4_capture_scale.S", PAYLOAD_RVA,
        defs=dict(CAPTURE_VIEWPORT=CAPTURE_VIEWPORT_RVA,
                  BB_WIDTH=BB_WIDTH_RVA, BB_HEIGHT=BB_HEIGHT_RVA),
        want_symbols=("s3br_scaled_viewport", "s3br_data"))
    if PAYLOAD_RVA + len(code) > PAYLOAD_LIMIT:
        raise SystemExit(f"payload too large: {len(code)}")
    thunk = symbols["s3br_scaled_viewport"]; data = symbols["s3br_data"]
    vals = struct.unpack_from("<6f", code, data - PAYLOAD_RVA)
    if vals != (512.0, 0.5, struct.unpack("<f", struct.pack("<I", 0x3D9E37B9))[0],
                104.0, 2.5, 256.0):
        raise SystemExit(f"payload constants wrong: {vals}")
    print(f"payload {len(code)} bytes at 0x{PAYLOAD_RVA:X}; thunk 0x{thunk:X}, data 0x{data:X}")
    blob[po:po+len(code)] = code
    blob[go:go+5] = b"\xE8" + struct.pack("<i", thunk - (GATE_CALL_RVA + 5))
    print(f"gate 0x{GATE_CALL_RVA:X}: call 3BN -> call 0x{thunk:X}")
    out.write_bytes(bytes(blob))
    print("output", hashlib.sha256(bytes(blob)).hexdigest(), len(blob))


if __name__ == "__main__":
    main()
