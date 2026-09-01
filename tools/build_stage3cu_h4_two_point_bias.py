"""Stage 3CU - two-point measured vertical bias for the H4 capture, on 3CR.

Stage 3CT put the reticle back on the VR crosshair (headset-confirmed, layout
-1635.814/731.878) but clipped: the capture dumps measure the reticle's bottom
sliver at y 0..15, horizontally PERFECT (bbox x 184..327, centre 256).

Direct measurements, both from on-disk capture dumps:
  accepted 3BT session (|y|=336.549): reticle 185x174 px, centred (254,271)
      -> bias there = 0.0772547 * H  (52.0 layout units)   [the accepted value]
  3CT today          (|y|=731.878): reticle 144 wide -> 135.4 tall at the same
      art aspect; bottom edge y=15 -> centre at -52.7; target centre 266.5
      (the accepted 13.5px-below-centre placement scaled by reticle size)
      -> needed shift +319.2 post-zoom px = +127.7 pre-zoom
      -> bias there = 167.65 - 127.7 = 39.95px of H=4719.3 -> fraction 0.008465

Both single-constant models are disproven BY these measurements: fixed fraction
(0.0772547 everywhere) missed by ~500px; fixed layout units (52 = Stage 3CT)
missed by ~319px. Two measured layouts anchor the two-parameter law

    fraction(|y|) = B/|y| - A,   B = 42.863, A = 0.050107

which is BIT-EXACT on the accepted layout, exact on today's, monotone between
them (every gameplay |y| observed to date lies inside [336.549, 731.878]), and
clamped at zero beyond. The payload refreshes KBIAS(0x2F9BD8) with this value
per capture, then runs the byte-identical accepted 3BR thunk - identical
structure to Stage 3CT, two instructions longer.
"""
from pathlib import Path
import hashlib, struct, sys
sys.path.insert(0, str(Path(__file__).parent))
from build_stage3br_h4_capture_scale import parse_pe, rva_off

EXPECTED_INPUT_SHA256 = \
    "7d8e3ee647689daac18ff25ca61d3794b63e6229a198296932181014c4c93878"   # 3CR
PAYLOAD_RVA, PAYLOAD_LIMIT = 0x002F93E0, 0x002F94D0
DEAD_3BN_SHA256 = \
    "e4e61358967df60898ca79c2795be6a53db55028f32913368a428b7d8ba2a868"
GATE_CALL_RVA = 0x002FB992
GATE_OLD = bytes.fromhex("e8f9e0ffff")
S3BR_THUNK_RVA = 0x002F9A90
KBIAS_RVA = 0x002F9BD8
BASE_Y_RVA = 0x002A8364
SELECTOR_LOAD_RVA = 0x002F99D7
LIVE_HIDE_X_RVA = 0x002A8368

# the two measured anchors
CAL_Y, CAL_F = 336.549, struct.unpack("<f", bytes.fromhex("b9379e3d"))[0]
NEW_Y, NEW_F = 731.878, 0.008465
B = (CAL_F - NEW_F) / (1.0 / CAL_Y - 1.0 / NEW_Y)
A = B / CAL_Y - CAL_F

CONTEXT = (
    (S3BR_THUNK_RVA, bytes.fromhex("4883ec28"), "accepted 3BR thunk head"),
    (0x002F9B29, bytes.fromhex("f30f101daf000000"), "3BR scale tail"),
    (0x002F9BBD, bytes.fromhex("4883c428c3"), "3BR return"),
    (0x002F9BD0, bytes.fromhex("000000440000003fb9379e3d"), "3BR constants"),
    (0x002F995D, bytes.fromhex("66837c24420c752f"), "type-0x28 discriminator"),
    (0x00053E04, bytes.fromhex("f30f11355c452500"), "live hide publisher"),
    (0x000199B3, bytes.fromhex("b201"), "3BJ absent"),
)


def main():
    src, out = Path(sys.argv[1]), Path(sys.argv[2])
    blob = bytearray(src.read_bytes())
    sha = hashlib.sha256(blob).hexdigest()
    print("input", sha)
    if sha != EXPECTED_INPUT_SHA256:
        raise SystemExit("wrong Stage 3CR input: " + sha)
    print("anchors: f(%.3f)=%.7f  f(%.3f)=%.7f  ->  B=%.5f  A=%.7f"
          % (CAL_Y, CAL_F, NEW_Y, NEW_F, B, A))
    # the law must reproduce both measured anchors
    if abs(B / CAL_Y - A - CAL_F) > 1e-6 or abs(B / NEW_Y - A - NEW_F) > 1e-6:
        raise SystemExit("two-point law does not reproduce its anchors")

    pe = parse_pe(blob)
    if pe["n"] != 12:
        raise SystemExit("unexpected PE geometry")
    for rva, expect, label in CONTEXT:
        o = rva_off(pe, rva)
        if bytes(blob[o:o+len(expect)]) != expect:
            raise SystemExit(f"{label}: got {bytes(blob[o:o+len(expect)]).hex()}")
    so = rva_off(pe, SELECTOR_LOAD_RVA)
    if SELECTOR_LOAD_RVA + 8 + struct.unpack_from("<i", blob, so+4)[0] \
            != LIVE_HIDE_X_RVA:
        raise SystemExit("3CR live selector missing")
    go = rva_off(pe, GATE_CALL_RVA)
    if bytes(blob[go:go+5]) != GATE_OLD:
        raise SystemExit("gate call unexpected")
    po, pl = rva_off(pe, PAYLOAD_RVA), rva_off(pe, PAYLOAD_LIMIT-1)+1
    if hashlib.sha256(bytes(blob[po:pl])).hexdigest() != DEAD_3BN_SHA256:
        raise SystemExit("payload region is not the exact dead 3BN thunk")
    blob[po:pl] = bytes(pl - po)

    # ---- payload: KBIAS = max(0, B/baseY - A), then the accepted thunk ----
    code = bytearray()
    def rip(opcode: bytes, target: int):
        here = PAYLOAD_RVA + len(code)
        code.extend(opcode)
        code.extend(struct.pack("<i", target - (here + len(opcode) + 4)))

    kB_slot, kA_slot = [0], [0]
    rip(bytes.fromhex("f30f1005"), BASE_Y_RVA)         # movss xmm0,[baseY]
    code += bytes.fromhex("0f2ec0")                    # ucomiss xmm0,xmm0
    jp_at = len(code); code += b"\x7A\x00"             # jp  .skip
    code += bytes.fromhex("0f57d2")                    # xorps xmm2,xmm2
    code += bytes.fromhex("0f2fc2")                    # comiss xmm0,xmm2
    jbe_at = len(code); code += b"\x76\x00"            # jbe .skip
    b_at = len(code)
    rip(bytes.fromhex("f30f100d"), 0)                  # movss xmm1,[B]
    code += bytes.fromhex("f30f5ec8")                  # divss xmm1,xmm0
    a_at = len(code)
    rip(bytes.fromhex("f30f5c0d"), 0)                  # subss xmm1,[A]
    code += bytes.fromhex("f30f5fca")                  # maxss xmm1,xmm2 (>=0)
    rip(bytes.fromhex("f30f110d"), KBIAS_RVA)          # movss [KBIAS],xmm1
    skip_at = len(code)
    code[jp_at+1] = skip_at - (jp_at + 2)
    code[jbe_at+1] = skip_at - (jbe_at + 2)
    jmp_at = len(code)
    code += b"\xE9" + struct.pack("<i",
                                  S3BR_THUNK_RVA - (PAYLOAD_RVA + jmp_at + 5))
    while len(code) % 4:
        code += b"\x90"
    kB_slot[0] = PAYLOAD_RVA + len(code); code += struct.pack("<f", B)
    kA_slot[0] = PAYLOAD_RVA + len(code); code += struct.pack("<f", A)
    struct.pack_into("<i", code, b_at + 4,
                     kB_slot[0] - (PAYLOAD_RVA + b_at + 8))
    struct.pack_into("<i", code, a_at + 4,
                     kA_slot[0] - (PAYLOAD_RVA + a_at + 8))
    code = bytes(code)
    if PAYLOAD_RVA + len(code) > PAYLOAD_LIMIT:
        raise SystemExit(f"payload too large: {len(code)}")

    from capstone import Cs, CS_ARCH_X86, CS_MODE_64
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    ops = []
    for ins in md.disasm(code[:jmp_at+5], PAYLOAD_RVA):
        ops.append(f"{ins.mnemonic} {ins.op_str}")
        print(f"    0x{ins.address:06X}  {ins.mnemonic:<8} {ins.op_str}")
    if f"jmp 0x{S3BR_THUNK_RVA:x}" not in ops:
        raise SystemExit("payload does not chain to the accepted 3BR thunk")
    fB = struct.unpack_from("<f", code, kB_slot[0]-PAYLOAD_RVA)[0]
    fA = struct.unpack_from("<f", code, kA_slot[0]-PAYLOAD_RVA)[0]
    for y, want in ((CAL_Y, CAL_F), (NEW_Y, NEW_F)):
        got = max(0.0, fB / y - fA)
        if abs(got - want) > 1e-5:
            raise SystemExit(f"embedded constants miss anchor y={y}")
        print(f"    verify f({y}) = {got:.7f} (target {want:.7f})")

    blob[po:po+len(code)] = code
    blob[go:go+5] = b"\xE8" + struct.pack("<i", PAYLOAD_RVA - (GATE_CALL_RVA+5))
    print(f"payload {len(code)}B; gate -> 0x{PAYLOAD_RVA:X} -> accepted 3BR; "
          f"KBIAS = max(0, {fB:.5f}/baseY - {fA:.7f}); selector live")
    out.write_bytes(bytes(blob))
    print("output", hashlib.sha256(bytes(blob)).hexdigest(), len(blob))


if __name__ == "__main__":
    main()
