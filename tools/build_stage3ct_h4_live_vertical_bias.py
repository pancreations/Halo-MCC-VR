"""Stage 3CT - make the H4 capture's vertical bias live, on Stage 3CR.

THE DEFECT, in one line of arithmetic:

    0.0772547 (the frozen bias fraction, unchanged since 3BK)
        x 336.549 (the canvas half-height it was measured on)
        = 26.000 canvas units

The bias is a fixed distance in CANVAS units - the authored reticle sits 26
units (x2 = 52 in the pre-zoom window) above the canvas centre - but the chain
stores it as a FRACTION OF THE VIEWPORT HEIGHT. Those are the same number only
on the canvas it was measured on:

    canvas 336.549 (working) : 0.0772547 * H  ->  52 units   -> reticle centred
    canvas 731.878 (today)   : 0.0772547 * H  -> 113 units   -> window 61 units low

Post-zoom vertical coverage is only about +/-32 canvas units, so a 61-unit
error puts the reticle completely outside the frame: the byte-empty captures.
When a capture does land it frames the dark HUD art below the reticle - the
"black square" the headset reported mid-level.

FIX: the bias fraction is 26.0 / halfHeight, evaluated live from the published
canvas half-height (BASE_Y, 0x2A8364). On the calibrated canvas this is
26.0/336.549 = 0.0772547 EXACTLY, so the accepted behaviour is reproduced
bit-for-bit; on any other canvas it tracks.

The whole accepted 3BR framing thunk then runs UNCHANGED (slot width, aspect
height, 2.5x zoom, RSSetViewports) - this stage only refreshes the constant it
reads. 3CR's live selector, the 3BU crosshair chain and the entire cutscene /
theatre stack are untouched.
"""
from pathlib import Path
import hashlib, struct, sys
sys.path.insert(0, str(Path(__file__).parent))
from build_stage3br_h4_capture_scale import parse_pe, rva_off

EXPECTED_INPUT_SHA256 = \
    "7d8e3ee647689daac18ff25ca61d3794b63e6229a198296932181014c4c93878"
PAYLOAD_RVA, PAYLOAD_LIMIT = 0x002F93E0, 0x002F94D0
DEAD_3BN_SHA256 = \
    "e4e61358967df60898ca79c2795be6a53db55028f32913368a428b7d8ba2a868"
GATE_CALL_RVA = 0x002FB992
GATE_OLD = bytes.fromhex("e8f9e0ffff")          # call the accepted 3BR thunk
S3BR_THUNK_RVA = 0x002F9A90
KBIAS_RVA = 0x002F9BD8
BASE_Y_RVA = 0x002A8364
SELECTOR_LOAD_RVA = 0x002F99D7
LIVE_HIDE_X_RVA = 0x002A8368

# 0.0772547 * 336.549 == 26.0 : the bias is 26 canvas units, not a fraction.
ACCEPTED_BIAS_FRACTION = struct.unpack("<f", bytes.fromhex("b9379e3d"))[0]
CALIBRATED_HALF_HEIGHT = 336.549
BIAS_UNITS = 26.0

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

    # the whole premise, checked before a byte is written
    derived = BIAS_UNITS / CALIBRATED_HALF_HEIGHT
    if abs(derived - ACCEPTED_BIAS_FRACTION) > 1e-7:
        raise SystemExit(f"bias premise fails: {derived} vs {ACCEPTED_BIAS_FRACTION}")
    print(f"premise: {BIAS_UNITS}/{CALIBRATED_HALF_HEIGHT} = {derived:.7f} "
          f"== accepted {ACCEPTED_BIAS_FRACTION:.7f}")

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
    # the bias constant the 3BR tail reads must be the accepted fraction
    ko = rva_off(pe, KBIAS_RVA)
    if bytes(blob[ko:ko+4]) != bytes.fromhex("b9379e3d"):
        raise SystemExit("KBIAS is not the accepted fraction")
    go = rva_off(pe, GATE_CALL_RVA)
    if bytes(blob[go:go+5]) != GATE_OLD:
        raise SystemExit("gate call unexpected: " + bytes(blob[go:go+5]).hex())
    po, pl = rva_off(pe, PAYLOAD_RVA), rva_off(pe, PAYLOAD_LIMIT-1)+1
    if hashlib.sha256(bytes(blob[po:pl])).hexdigest() != DEAD_3BN_SHA256:
        raise SystemExit("payload region is not the exact dead 3BN thunk")
    blob[po:pl] = bytes(pl - po)

    # ---- payload: refresh KBIAS = 26.0/baseY, then run the accepted thunk ----
    code = bytearray()

    def rip(opcode: bytes, target_fn):
        here = PAYLOAD_RVA + len(code)
        code.extend(opcode)
        code.extend(struct.pack("<i", target_fn() - (here + len(opcode) + 4)))

    k26_rva = [0]
    rip(bytes.fromhex("f30f1005"), lambda: BASE_Y_RVA)     # movss xmm0,[baseY]
    code += bytes.fromhex("0f2ec0")                        # ucomiss xmm0,xmm0
    jp_at = len(code); code += b"\x7A\x00"                 # jp  .skip
    code += bytes.fromhex("0f57c9")                        # xorps xmm1,xmm1
    code += bytes.fromhex("0f2fc1")                        # comiss xmm0,xmm1
    jbe_at = len(code); code += b"\x76\x00"                # jbe .skip
    rip(bytes.fromhex("f30f100d"), lambda: k26_rva[0])     # movss xmm1,[26.0]
    code += bytes.fromhex("f30f5ec8")                      # divss xmm1,xmm0
    rip(bytes.fromhex("f30f110d"), lambda: KBIAS_RVA)      # movss [KBIAS],xmm1
    skip_at = len(code)
    code[jp_at+1] = skip_at - (jp_at + 2)
    code[jbe_at+1] = skip_at - (jbe_at + 2)
    jmp_at = len(code)
    code += b"\xE9" + struct.pack(
        "<i", S3BR_THUNK_RVA - (PAYLOAD_RVA + jmp_at + 5))  # jmp accepted 3BR
    while len(code) % 4:
        code += b"\x90"
    k26_rva[0] = PAYLOAD_RVA + len(code)
    code += struct.pack("<f", BIAS_UNITS)
    # re-emit the one displacement that pointed at the constant
    for i in range(len(code) - 8):
        if bytes(code[i:i+4]) == bytes.fromhex("f30f100d"):
            struct.pack_into("<i", code, i+4,
                             k26_rva[0] - (PAYLOAD_RVA + i + 8))
            break
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

    blob[po:po+len(code)] = code
    blob[go:go+5] = b"\xE8" + struct.pack("<i", PAYLOAD_RVA - (GATE_CALL_RVA + 5))
    print(f"payload {len(code)}B at 0x{PAYLOAD_RVA:X}; gate -> 0x{PAYLOAD_RVA:X} "
          f"-> accepted 3BR 0x{S3BR_THUNK_RVA:X}; KBIAS refreshed live "
          f"= 26.0/baseY; selector live -> 0x{LIVE_HIDE_X_RVA:X}")
    out.write_bytes(bytes(blob))
    print("output", hashlib.sha256(bytes(blob)).hexdigest(), len(blob))


if __name__ == "__main__":
    main()
