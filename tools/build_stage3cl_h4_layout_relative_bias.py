"""Stage 3CL - layout-relative capture bias, on Stage 3CI.
Restores the reticle capture on every authored layout: width keeps the
accepted W = 4|baseX| rule via the live hide value, and the vertical bias -
frozen at 104/1346.196 since 3BK - becomes the live proportional fraction
(4*0.237258) * baseY / hide, which reproduces the accepted value exactly at
the calibrated layout. Everything else (3BT capture chain, live un-hide,
3BU..3BX cutscene stack, Codex's theatre camera) is byte-identical to 3CI."""
from pathlib import Path
import hashlib, struct, sys
sys.path.insert(0, str(Path(__file__).parent))
from postlink import build_payload
from build_stage3br_h4_capture_scale import parse_pe, rva_off

EXPECTED_INPUT_SHA256 = \
    "fea7add625c09eec37d44599b98d5690fcb9d840c5a0ac1766b69480dae627fd"
PAYLOAD_RVA, PAYLOAD_LIMIT = 0x002F93E0, 0x002F94D0
DEAD_3BN_SHA256 = \
    "e4e61358967df60898ca79c2795be6a53db55028f32913368a428b7d8ba2a868"
GATE_CALL_RVA = 0x002FB992
GATE_OLD = bytes.fromhex("e8f9e0ffff")      # call 0x2F9A90 (3BR thunk)
S3BR_THUNK_RVA, S3BR_SCALE_TAIL_RVA = 0x002F9A90, 0x002F9B29
CAPTURE_VIEWPORT_RVA = 0x002AE774
BASE_X_RVA, BASE_Y_RVA, LIVE_HIDE_X_RVA = 0x002A8360, 0x002A8364, 0x002A8368
BBW_RVA, BBH_RVA = 0x002AEB58, 0x002AEB5C
K512_RVA, KHALF_RVA = 0x002F9BD0, 0x002F9BD4
VPBUF_RVA = 0x002F9BE8

CONTEXT = (
    (S3BR_THUNK_RVA, bytes.fromhex("4883ec28"), "3BR thunk head"),
    (S3BR_SCALE_TAIL_RVA, bytes.fromhex("f30f101daf000000"), "3BR scale tail"),
    (0x002F9BBD, bytes.fromhex("4883c428c3"), "3BR tail add/ret"),
    (K512_RVA, bytes.fromhex("000000440000003fb9379e3d"), "3BR consts"),
    (0x00053E04, bytes.fromhex("f30f11355c452500"), "hide publisher"),
    (0x002F99D7, bytes.fromhex("f30f100d"), "selector un-hide load (3CI live)"),
    (0x000199B3, bytes.fromhex("b201"), "3BJ NOT included"),
)


def main():
    src, out = Path(sys.argv[1]), Path(sys.argv[2])
    blob = bytearray(src.read_bytes())
    sha = hashlib.sha256(blob).hexdigest()
    print("input", sha)
    if sha != EXPECTED_INPUT_SHA256:
        raise SystemExit("wrong Stage 3CI input: " + sha)
    pe = parse_pe(blob)
    if pe["n"] != 12:
        raise SystemExit("unexpected PE geometry")
    for rva, expect, label in CONTEXT:
        o = rva_off(pe, rva)
        if bytes(blob[o:o+len(expect)]) != expect:
            raise SystemExit(f"{label}: got {bytes(blob[o:o+len(expect)]).hex()}")
    # the un-hide must read the live value (Stage 3CI)
    o = rva_off(pe, 0x2F99D7)
    if 0x2F99D7 + 8 + struct.unpack_from("<i", blob, o+4)[0] != LIVE_HIDE_X_RVA:
        raise SystemExit("selector un-hide is not the live value")
    # the C-H4-48 report reads baseX/baseY from the published atomics
    for rva, target in ((0x515D6, BASE_X_RVA), (0x515D0, BASE_Y_RVA)):
        o = rva_off(pe, rva)
        if blob[o] != 0x8B:
            raise SystemExit(f"report load at {rva:#x} unexpected")
        if rva + 6 + struct.unpack_from("<i", blob, o+2)[0] != target:
            raise SystemExit(f"report load at {rva:#x} does not read {target:#x}")
    go = rva_off(pe, GATE_CALL_RVA)
    if bytes(blob[go:go+5]) != GATE_OLD:
        raise SystemExit("gate call unexpected: " + bytes(blob[go:go+5]).hex())
    po, pl = rva_off(pe, PAYLOAD_RVA), rva_off(pe, PAYLOAD_LIMIT-1)+1
    if hashlib.sha256(bytes(blob[po:pl])).hexdigest() != DEAD_3BN_SHA256:
        raise SystemExit("payload region is not the exact dead 3BN thunk")
    blob[po:pl] = bytes(pl - po)
    code, symbols = build_payload(
        Path(__file__).parent / "stage3cl_h4_layout_relative_bias.S",
        PAYLOAD_RVA,
        defs=dict(CAPTURE_VIEWPORT=CAPTURE_VIEWPORT_RVA, BASE_Y=BASE_Y_RVA,
                  LIVE_HIDE_X=LIVE_HIDE_X_RVA, BBW=BBW_RVA, BBH=BBH_RVA,
                  K512=K512_RVA, KHALF=KHALF_RVA, VPBUF=VPBUF_RVA,
                  S3BR_SCALE_TAIL=S3BR_SCALE_TAIL_RVA,
                  S3BR_THUNK=S3BR_THUNK_RVA),
        want_symbols=("s3cl_layout_viewport",))
    if PAYLOAD_RVA + len(code) > PAYLOAD_LIMIT:
        raise SystemExit(f"payload too large: {len(code)}")
    thunk = symbols["s3cl_layout_viewport"]
    blob[po:po+len(code)] = code
    blob[go:go+5] = b"\xE8" + struct.pack("<i", thunk - (GATE_CALL_RVA + 5))
    print(f"payload {len(code)} bytes at 0x{PAYLOAD_RVA:X}; gate -> 0x{thunk:X}")
    out.write_bytes(bytes(blob))
    print("output", hashlib.sha256(bytes(blob)).hexdigest(), len(blob))


if __name__ == "__main__":
    main()
