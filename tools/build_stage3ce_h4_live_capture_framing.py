"""Stage 3CE - live Halo 4 capture framing on Stage 3CB.
Input: exact Stage 3CB (= accepted 3BX + the proven theatre-camera delta).
One behavioral change (the capture framing follows the live CUI layout) in
three byte-regions: the gate call at 0x2FB992 re-pointed to s3ce_live_viewport,
the new payload in the 3BU scratch tail, and the 3BP selector's un-hide load
displacement re-pointed from the fixed constant to the live hide value."""
from pathlib import Path
import hashlib, struct, sys
sys.path.insert(0, str(Path(__file__).parent))
from postlink import build_payload
from build_stage3br_h4_capture_scale import parse_pe, rva_off

EXPECTED_INPUT_SHA256 = \
    "9ee60ca1b97934002473a0f970c4af8e94a79aa34cfc92116f1c06b4f5690885"
# The payload replaces the DEAD 3BN thunk: unreachable since 3BQ re-pointed
# the gate to 3BR (verified: gate targets 0x2F9A90 and no rel32 call/jmp in
# the whole image lands in 0x2F93E0..0x2F94D0). Its prior bytes are pinned by
# hash below so only that exact dead artifact can be replaced.
PAYLOAD_RVA = 0x002F93E0
PAYLOAD_LIMIT = 0x002F94D0
DEAD_3BN_SHA256 =     "e4e61358967df60898ca79c2795be6a53db55028f32913368a428b7d8ba2a868"

GATE_CALL_RVA = 0x002FB992
GATE_OLD = bytes.fromhex("e8f9e0ffff")   # call 0x2F9A90 (3BR thunk)
S3BR_THUNK_RVA = 0x002F9A90
S3BR_SCALE_TAIL_RVA = 0x002F9B29
CAPTURE_VIEWPORT_RVA = 0x002AE774
LIVE_HIDE_X_RVA = 0x002A8368
BBW_RVA = 0x002AEB58
BBH_RVA = 0x002AEB5C
K512_RVA = 0x002F9BD0
KHALF_RVA = 0x002F9BD4
KBIAS_RVA = 0x002F9BD8
VPBUF_RVA = 0x002F9BE8

SELECTOR_LOAD_RVA = 0x002F99D7
SELECTOR_LOAD_OLD = bytes.fromhex("f30f100d211e0000")   # movss xmm1,[0x2FB800]
FIXED_FRAMING_RVA = 0x002FB800

CONTEXT = (
    # 3BR thunk internals this stage reuses, all verified in the input
    (S3BR_THUNK_RVA, bytes.fromhex("4883ec28"), "3BR thunk head"),
    (S3BR_SCALE_TAIL_RVA, bytes.fromhex("f30f10 1d af000000".replace(" ", "")),
     "3BR .Lscale: movss xmm3,[kscale]"),
    (0x002F9BBD, bytes.fromhex("4883c428c3"), "3BR tail add rsp,0x28; ret"),
    (K512_RVA, bytes.fromhex("00000044" "0000003f" "b9379e3d"),
     "3BR k512/khalf/kbias constants"),
    (0x002F9BE0, struct.pack("<ff", 2.5, 256.0), "3BR kscale/k256"),
    # the hide path publishes the live float this stage consumes
    (0x00053E04, bytes.fromhex("f30f11355c452500"),
     "native-hide path stores live hide X at 0x2A8368"),
    (0x000515C3, bytes.fromhex("8b0d9f6d2500"),
     "C-H4-48 reporter reads 0x2A8368"),
    # 3BP selector: the fixed framing value it still loads
    (FIXED_FRAMING_RVA, struct.pack("<ff", 4134.312, 1346.196),
     "3BH fixed framing constant"),
    (0x002F99CD, bytes.fromhex("f30f104028"), "selector reads container X"),
    # theatre-camera delta (3CB) still present
    (0x00056EBF, bytes.fromhex("e8"), "snapshot call spliced by 3CB"),
    (0x000199B3, bytes.fromhex("b201"), "3BJ NOT included"),
)


def main():
    src, out = Path(sys.argv[1]), Path(sys.argv[2])
    blob = bytearray(src.read_bytes())
    sha = hashlib.sha256(blob).hexdigest()
    print("input", sha)
    if sha != EXPECTED_INPUT_SHA256:
        raise SystemExit("wrong Stage 3CB input DLL: " + sha)
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
    if GATE_CALL_RVA + 5 + struct.unpack_from("<i", GATE_OLD, 1)[0] \
            != S3BR_THUNK_RVA:
        raise SystemExit("gate call does not target the 3BR thunk")
    so = rva_off(pe, SELECTOR_LOAD_RVA)
    if bytes(blob[so:so+8]) != SELECTOR_LOAD_OLD:
        raise SystemExit("selector load unexpected: " +
                         bytes(blob[so:so+8]).hex())
    if SELECTOR_LOAD_RVA + 8 + struct.unpack_from(
            "<i", SELECTOR_LOAD_OLD, 4)[0] != FIXED_FRAMING_RVA:
        raise SystemExit("selector load does not target the fixed constant")
    po, pl = rva_off(pe, PAYLOAD_RVA), rva_off(pe, PAYLOAD_LIMIT-1)+1
    if hashlib.sha256(bytes(blob[po:pl])).hexdigest() != DEAD_3BN_SHA256:
        raise SystemExit("payload region is not the exact dead 3BN thunk")
    blob[po:pl] = bytes(pl - po)

    code, symbols = build_payload(
        Path(__file__).parent / "stage3ce_h4_live_capture_framing.S",
        PAYLOAD_RVA,
        defs=dict(CAPTURE_VIEWPORT=CAPTURE_VIEWPORT_RVA,
                  LIVE_HIDE_X=LIVE_HIDE_X_RVA,
                  BBW=BBW_RVA, BBH=BBH_RVA,
                  K512=K512_RVA, KHALF=KHALF_RVA, KBIAS=KBIAS_RVA,
                  VPBUF=VPBUF_RVA,
                  S3BR_SCALE_TAIL=S3BR_SCALE_TAIL_RVA,
                  S3BR_THUNK=S3BR_THUNK_RVA),
        want_symbols=("s3ce_live_viewport",))
    if PAYLOAD_RVA + len(code) > PAYLOAD_LIMIT:
        raise SystemExit(f"payload too large: {len(code)}")
    thunk = symbols["s3ce_live_viewport"]
    print(f"payload {len(code)} bytes at 0x{PAYLOAD_RVA:X}")
    blob[po:po+len(code)] = code
    blob[go:go+5] = b"\xE8" + struct.pack("<i", thunk - (GATE_CALL_RVA + 5))
    print(f"gate 0x{GATE_CALL_RVA:X}: 3BR 0x{S3BR_THUNK_RVA:X} -> "
          f"live 0x{thunk:X}")
    struct.pack_into("<i", blob, so + 4,
                     LIVE_HIDE_X_RVA - (SELECTOR_LOAD_RVA + 8))
    print(f"selector 0x{SELECTOR_LOAD_RVA:X}: fixed 0x{FIXED_FRAMING_RVA:X} "
          f"-> live hide 0x{LIVE_HIDE_X_RVA:X}")
    out.write_bytes(bytes(blob))
    print("output", hashlib.sha256(bytes(blob)).hexdigest(), len(blob))


if __name__ == "__main__":
    main()
