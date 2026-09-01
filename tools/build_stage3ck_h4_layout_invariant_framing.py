"""Stage 3CK - layout-invariant Halo 4 capture framing, on Stage 3CI."""
from pathlib import Path
import hashlib, struct, sys
sys.path.insert(0, str(Path(__file__).parent))
from postlink import build_payload
from build_stage3br_h4_capture_scale import parse_pe, rva_off

EXPECTED_INPUT_SHA256 = \
    "fea7add625c09eec37d44599b98d5690fcb9d840c5a0ac1766b69480dae627fd"
# Replaces the dead 3BN thunk (unreachable since 3BQ re-pointed the gate).
PAYLOAD_RVA = 0x002F93E0
PAYLOAD_LIMIT = 0x002F94D0
DEAD_3BN_SHA256 = \
    "e4e61358967df60898ca79c2795be6a53db55028f32913368a428b7d8ba2a868"
GATE_CALL_RVA = 0x002FB992
GATE_OLD = bytes.fromhex("e8f9e0ffff")      # call 0x2F9A90 (3BR thunk)
S3BR_THUNK_RVA = 0x002F9A90
S3BR_SCALE_TAIL_RVA = 0x002F9B29
CAPTURE_VIEWPORT_RVA = 0x002AE774
LIVE_HIDE_X_RVA = 0x002A8368
BBW_RVA, BBH_RVA = 0x002AEB58, 0x002AEB5C
K512_RVA, KHALF_RVA, KBIAS_RVA = 0x002F9BD0, 0x002F9BD4, 0x002F9BD8
VPBUF_RVA = 0x002F9BE8

CONTEXT = (
    (S3BR_THUNK_RVA, bytes.fromhex("4883ec28"), "3BR thunk head"),
    (S3BR_SCALE_TAIL_RVA, bytes.fromhex("f30f101daf000000"), "3BR scale tail"),
    (0x002F9BBD, bytes.fromhex("4883c428c3"), "3BR tail add/ret"),
    (K512_RVA, bytes.fromhex("000000440000003fb9379e3d"), "3BR consts"),
    (0x002F9BE0, bytes.fromhex("0000204000008043"), "3BR kscale/k256"),
    (0x00053E04, bytes.fromhex("f30f11355c452500"), "live hide publisher"),
    (0x000515C3, bytes.fromhex("8b0d9f6d2500"), "C-H4-48 reads live hide"),
    # 3CI's live un-hide must still be in place
    (0x002F99D7, bytes.fromhex("f30f100d"), "selector un-hide load"),
    (0x000199B3, bytes.fromhex("b201"), "3BJ NOT included"),
)


def main():
    src, out = Path(sys.argv[1]), Path(sys.argv[2])
    blob = bytearray(src.read_bytes())
    sha = hashlib.sha256(blob).hexdigest()
    print("input", sha)
    if sha != EXPECTED_INPUT_SHA256:
        raise SystemExit("wrong Stage 3CI input DLL: " + sha)
    pe = parse_pe(blob)
    if pe["n"] != 12:
        raise SystemExit("unexpected PE geometry")
    for rva, expect, label in CONTEXT:
        o = rva_off(pe, rva)
        if bytes(blob[o:o+len(expect)]) != expect:
            raise SystemExit(f"{label}: got {bytes(blob[o:o+len(expect)]).hex()}")
    # the un-hide must already read the LIVE value (Stage 3CI)
    o = rva_off(pe, 0x2F99D7)
    if 0x2F99D7 + 8 + struct.unpack_from("<i", blob, o+4)[0] != LIVE_HIDE_X_RVA:
        raise SystemExit("selector un-hide is not the live value")
    go = rva_off(pe, GATE_CALL_RVA)
    if bytes(blob[go:go+5]) != GATE_OLD:
        raise SystemExit("gate call unexpected: " + bytes(blob[go:go+5]).hex())
    po, pl = rva_off(pe, PAYLOAD_RVA), rva_off(pe, PAYLOAD_LIMIT-1)+1
    if hashlib.sha256(bytes(blob[po:pl])).hexdigest() != DEAD_3BN_SHA256:
        raise SystemExit("payload region is not the exact dead 3BN thunk")
    blob[po:pl] = bytes(pl - po)

    code, symbols = build_payload(
        Path(__file__).parent / "stage3ck_h4_layout_invariant_framing.S",
        PAYLOAD_RVA,
        defs=dict(CAPTURE_VIEWPORT=CAPTURE_VIEWPORT_RVA,
                  LIVE_HIDE_X=LIVE_HIDE_X_RVA, BBW=BBW_RVA, BBH=BBH_RVA,
                  K512=K512_RVA, KHALF=KHALF_RVA, KBIAS=KBIAS_RVA,
                  VPBUF=VPBUF_RVA, S3BR_SCALE_TAIL=S3BR_SCALE_TAIL_RVA,
                  S3BR_THUNK=S3BR_THUNK_RVA),
        want_symbols=("s3ck_layout_viewport",))
    if PAYLOAD_RVA + len(code) > PAYLOAD_LIMIT:
        raise SystemExit(f"payload too large: {len(code)}")
    thunk = symbols["s3ck_layout_viewport"]
    blob[po:po+len(code)] = code
    blob[go:go+5] = b"\xE8" + struct.pack("<i", thunk - (GATE_CALL_RVA + 5))
    print(f"payload {len(code)} bytes at 0x{PAYLOAD_RVA:X}; "
          f"gate -> 0x{thunk:X} (W = 17092536 / liveHide)")
    out.write_bytes(bytes(blob))
    print("output", hashlib.sha256(bytes(blob)).hexdigest(), len(blob))


if __name__ == "__main__":
    main()
