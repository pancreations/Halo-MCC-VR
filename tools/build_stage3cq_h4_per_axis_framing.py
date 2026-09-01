"""Stage 3CQ - per-axis live capture framing, on Stage 3CB.
See stage3cq_h4_per_axis_framing.S: W = live hide (4*halfW), H = KH*halfH
(each axis from its own live canvas extent; the calibrated canvas reproduces
the accepted net framing exactly on both axes), selector un-hide live.
Everything else byte-identical to 3CB (= proven 3BU chain + cutscene stack +
theatre camera)."""
from pathlib import Path
import hashlib, struct, sys
sys.path.insert(0, str(Path(__file__).parent))
from postlink import build_payload
from build_stage3br_h4_capture_scale import parse_pe, rva_off

EXPECTED_INPUT_SHA256 = \
    "9ee60ca1b97934002473a0f970c4af8e94a79aa34cfc92116f1c06b4f5690885"
PAYLOAD_RVA, PAYLOAD_LIMIT = 0x002F93E0, 0x002F94D0
DEAD_3BN_SHA256 = \
    "e4e61358967df60898ca79c2795be6a53db55028f32913368a428b7d8ba2a868"
GATE_CALL_RVA = 0x002FB992
GATE_OLD = bytes.fromhex("e8f9e0ffff")          # call accepted 3BR thunk
S3BR_THUNK_RVA, S3BR_SCALE_TAIL_RVA = 0x002F9A90, 0x002F9B29
SELECTOR_LOAD_RVA = 0x002F99D7
FIXED_FRAMING_RVA = 0x002FB800
CAPTURE_VIEWPORT_RVA = 0x002AE774
BASE_Y_RVA, LIVE_HIDE_X_RVA = 0x002A8364, 0x002A8368
K512_RVA, KHALF_RVA, KBIAS_RVA = 0x002F9BD0, 0x002F9BD4, 0x002F9BD8
VPBUF_RVA = 0x002F9BE8

CONTEXT = (
    (0x002F995D, bytes.fromhex("66837c24420c752f"),
     "type-0x28 payload-size discriminator (kept)"),
    (S3BR_THUNK_RVA, bytes.fromhex("4883ec28"), "accepted 3BR thunk"),
    (S3BR_SCALE_TAIL_RVA, bytes.fromhex("f30f101daf000000"), "3BR scale tail"),
    (0x002F9BBD, bytes.fromhex("4883c428c3"), "3BR return"),
    (K512_RVA, bytes.fromhex("000000440000003fb9379e3d"), "3BR constants"),
    (0x00053E04, bytes.fromhex("f30f11355c452500"), "live hide publisher"),
    (0x000199B3, bytes.fromhex("b201"), "3BJ absent"),
)


def main():
    src, out = Path(sys.argv[1]), Path(sys.argv[2])
    blob = bytearray(src.read_bytes())
    sha = hashlib.sha256(blob).hexdigest()
    print("input", sha)
    if sha != EXPECTED_INPUT_SHA256:
        raise SystemExit("wrong Stage 3CB input: " + sha)
    pe = parse_pe(blob)
    if pe["n"] != 12:
        raise SystemExit("unexpected PE geometry")
    for rva, expect, label in CONTEXT:
        o = rva_off(pe, rva)
        if bytes(blob[o:o+len(expect)]) != expect:
            raise SystemExit(f"{label}: got {bytes(blob[o:o+len(expect)]).hex()}")
    # baseY atomic identity, decoded from the report's own load (any r32)
    o = rva_off(pe, 0x515D0)
    if blob[o] != 0x8B or (blob[o+1] & 0xC7) != 0x05 or             0x515D0 + 6 + struct.unpack_from("<i", blob, o+2)[0] != BASE_Y_RVA:
        raise SystemExit("report baseY load targets another field")
    so = rva_off(pe, SELECTOR_LOAD_RVA)
    if bytes(blob[so:so+4]) != bytes.fromhex("f30f100d") or \
            SELECTOR_LOAD_RVA + 8 + struct.unpack_from("<i", blob, so+4)[0] \
            != FIXED_FRAMING_RVA:
        raise SystemExit("selector un-hide load unexpected")
    go = rva_off(pe, GATE_CALL_RVA)
    if bytes(blob[go:go+5]) != GATE_OLD:
        raise SystemExit("gate call unexpected")
    po, pl = rva_off(pe, PAYLOAD_RVA), rva_off(pe, PAYLOAD_LIMIT-1)+1
    if hashlib.sha256(bytes(blob[po:pl])).hexdigest() != DEAD_3BN_SHA256:
        raise SystemExit("payload region is not the exact dead 3BN thunk")
    blob[po:pl] = bytes(pl - po)

    code, symbols = build_payload(
        Path(__file__).parent / "stage3cq_h4_per_axis_framing.S", PAYLOAD_RVA,
        defs=dict(CAPTURE_VIEWPORT=CAPTURE_VIEWPORT_RVA, BASE_Y=BASE_Y_RVA,
                  LIVE_HIDE_X=LIVE_HIDE_X_RVA, K512=K512_RVA,
                  KHALF=KHALF_RVA, KBIAS=KBIAS_RVA, VPBUF=VPBUF_RVA,
                  S3BR_SCALE_TAIL=S3BR_SCALE_TAIL_RVA,
                  S3BR_THUNK=S3BR_THUNK_RVA),
        want_symbols=("s3cq_per_axis_viewport",))
    if PAYLOAD_RVA + len(code) > PAYLOAD_LIMIT:
        raise SystemExit(f"payload too large: {len(code)}")
    thunk = symbols["s3cq_per_axis_viewport"]
    blob[po:po+len(code)] = code
    blob[go:go+5] = b"\xE8" + struct.pack("<i", thunk - (GATE_CALL_RVA + 5))
    struct.pack_into("<i", blob, so + 4,
                     LIVE_HIDE_X_RVA - (SELECTOR_LOAD_RVA + 8))
    print(f"payload {len(code)}B at 0x{PAYLOAD_RVA:X}; gate -> 0x{thunk:X}; "
          f"selector un-hide -> live 0x{LIVE_HIDE_X_RVA:X}")
    out.write_bytes(bytes(blob))
    print("output", hashlib.sha256(bytes(blob)).hexdigest(), len(blob))


if __name__ == "__main__":
    main()
