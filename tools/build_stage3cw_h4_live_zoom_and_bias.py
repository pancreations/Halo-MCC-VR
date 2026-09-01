"""Stage 3CW - measured live zoom + re-anchored vertical bias, on Stage 3CR.

Headset 14:28 session (3CU, layout -1635.814/731.878): the FULL reticle is on
the VR crosshair (user-confirmed working) with two measured residuals:
  * SIZE: rest envelope 286px vs the accepted 185px (dumps, both layouts).
    The art scales with the layout (286/185 = 1.55 ~ |x| ratio 1.58) while the
    zoom stayed frozen at 2.5x - so on fire the bloomed arcs (2.34x rest,
    calibrated bloom dump 432px) exceed the 512 frame: the reported cut-off
    corners.
  * PLACEMENT: centroid (256, 218) - horizontally perfect, 58px high of the
    accepted look (centroid 15px below centre at accepted size).

Both laws are two-point fits through the SAME two measured layouts, exact at
both anchors, refreshed live per capture, structure identical to 3CT/3CU:

    zoom  = ZA + ZB/hide          ZA=0.101563, ZB=9915.93
        zoom(4134.312) = 2.5 (accepted, bit-near)   zoom(6543.256) = 1.6170
        (1.617 = 2.5 x 185/286: the accepted apparent size on this layout)
    kbias = max(0, B/|baseY| - A) B=46.09455, A=0.0597075
        f(336.549) = 0.0772547 (accepted, bit-exact)
        f(731.878) = 0.003274  (centres the 185px-equivalent reticle 15px
        below centre at the corrected zoom: (271-256)/1.617 = +9.28px pre-zoom
        from the measured -15.2px -> bias 39.95-24.5 = 15.45px of H=4719.3)

The payload refreshes KBIAS (0x2F9BD8) and KSCALE (0x2F9BE0) then runs the
byte-identical accepted 3BR thunk. Invalid live values skip that refresh
(frozen accepted constants). Selector stays live (3CR). Nothing else moves.
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
KBIAS_RVA, KSCALE_RVA = 0x002F9BD8, 0x002F9BE0
BASE_Y_RVA, LIVE_HIDE_X_RVA = 0x002A8364, 0x002A8368
SELECTOR_LOAD_RVA = 0x002F99D7

# measured anchors: (layout value, required constant)
CAL_Y, CAL_F = 336.549, struct.unpack("<f", bytes.fromhex("b9379e3d"))[0]
NEW_Y, NEW_F = 731.878, 0.003274
CAL_H, CAL_Z = 4134.312, 2.5
NEW_H, NEW_Z = 6543.256, 2.5 * 185.0 / 286.0
B = (CAL_F - NEW_F) / (1.0 / CAL_Y - 1.0 / NEW_Y)
A = B / CAL_Y - CAL_F
ZA = (CAL_Z * CAL_H - NEW_Z * NEW_H) / (CAL_H - NEW_H)
ZB = (CAL_Z - ZA) * CAL_H

CONTEXT = (
    (S3BR_THUNK_RVA, bytes.fromhex("4883ec28"), "accepted 3BR thunk head"),
    (0x002F9B29, bytes.fromhex("f30f101daf000000"), "3BR scale tail"),
    (0x002F9BBD, bytes.fromhex("4883c428c3"), "3BR return"),
    (0x002F9BD0, bytes.fromhex("000000440000003fb9379e3d"), "3BR constants"),
    (KSCALE_RVA, bytes.fromhex("0000204000008043"), "kscale 2.5 / k256"),
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
    print("bias: B=%.5f A=%.7f | zoom: ZA=%.6f ZB=%.5f" % (B, A, ZA, ZB))
    for y, want in ((CAL_Y, CAL_F), (NEW_Y, NEW_F)):
        if abs(B / y - A - want) > 1e-6:
            raise SystemExit("bias law misses anchor")
    for h, want in ((CAL_H, CAL_Z), (NEW_H, NEW_Z)):
        if abs(ZA + ZB / h - want) > 1e-4:
            raise SystemExit("zoom law misses anchor")

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

    code = bytearray()
    fixups = []          # (offset_of_disp, target_slot_index)
    def rip(opcode: bytes, target=None, slot=None):
        here = PAYLOAD_RVA + len(code)
        code.extend(opcode)
        if slot is not None:
            fixups.append((len(code), slot))
            code.extend(b"\0\0\0\0")
        else:
            code.extend(struct.pack("<i", target - (here + len(opcode) + 4)))

    # -- bias: KBIAS = max(0, B/baseY - A) --
    rip(bytes.fromhex("f30f1005"), BASE_Y_RVA)     # movss xmm0,[baseY]
    code += bytes.fromhex("0f2ec0")                # ucomiss xmm0,xmm0
    jp1 = len(code); code += b"\x7A\x00"           # jp  .zoom
    code += bytes.fromhex("0f57d2")                # xorps xmm2,xmm2
    code += bytes.fromhex("0f2fc2")                # comiss xmm0,xmm2
    jbe1 = len(code); code += b"\x76\x00"          # jbe .zoom
    rip(bytes.fromhex("f30f100d"), slot=0)         # movss xmm1,[B]
    code += bytes.fromhex("f30f5ec8")              # divss xmm1,xmm0
    rip(bytes.fromhex("f30f5c0d"), slot=1)         # subss xmm1,[A]
    code += bytes.fromhex("f30f5fca")              # maxss xmm1,xmm2
    rip(bytes.fromhex("f30f110d"), KBIAS_RVA)      # movss [KBIAS],xmm1
    zoom_at = len(code)
    code[jp1+1] = zoom_at - (jp1 + 2)
    code[jbe1+1] = zoom_at - (jbe1 + 2)
    # -- zoom: KSCALE = ZA + ZB/hide --
    rip(bytes.fromhex("f30f1005"), LIVE_HIDE_X_RVA)  # movss xmm0,[hide]
    code += bytes.fromhex("0f2ec0")                # ucomiss xmm0,xmm0
    jp2 = len(code); code += b"\x7A\x00"           # jp  .done
    code += bytes.fromhex("0f57d2")                # xorps xmm2,xmm2
    code += bytes.fromhex("0f2fc2")                # comiss xmm0,xmm2
    jbe2 = len(code); code += b"\x76\x00"          # jbe .done
    rip(bytes.fromhex("f30f100d"), slot=2)         # movss xmm1,[ZB]
    code += bytes.fromhex("f30f5ec8")              # divss xmm1,xmm0
    rip(bytes.fromhex("f30f580d"), slot=3)         # addss xmm1,[ZA]
    rip(bytes.fromhex("f30f110d"), KSCALE_RVA)     # movss [KSCALE],xmm1
    done_at = len(code)
    code[jp2+1] = done_at - (jp2 + 2)
    code[jbe2+1] = done_at - (jbe2 + 2)
    jmp_at = len(code)
    code += b"\xE9" + struct.pack("<i",
                                  S3BR_THUNK_RVA - (PAYLOAD_RVA + jmp_at + 5))
    while len(code) % 4:
        code += b"\x90"
    slots = []
    for v in (B, A, ZB, ZA):
        slots.append(PAYLOAD_RVA + len(code))
        code += struct.pack("<f", v)
    for disp_off, slot in fixups:
        struct.pack_into("<i", code, disp_off,
                         slots[slot] - (PAYLOAD_RVA + disp_off + 4))
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
    eb = [struct.unpack_from("<f", code, s - PAYLOAD_RVA)[0] for s in slots]
    for y, want in ((CAL_Y, CAL_F), (NEW_Y, NEW_F)):
        got = max(0.0, eb[0] / y - eb[1])
        if abs(got - want) > 1e-5:
            raise SystemExit("embedded bias constants miss anchor")
        print(f"    verify bias f({y}) = {got:.7f}")
    for h, want in ((CAL_H, CAL_Z), (NEW_H, NEW_Z)):
        got = eb[3] + eb[2] / h
        if abs(got - want) > 1e-4:
            raise SystemExit("embedded zoom constants miss anchor")
        print(f"    verify zoom z({h}) = {got:.5f}")

    blob[po:po+len(code)] = code
    blob[go:go+5] = b"\xE8" + struct.pack("<i", PAYLOAD_RVA - (GATE_CALL_RVA+5))
    print(f"payload {len(code)}B; gate -> 0x{PAYLOAD_RVA:X} -> accepted 3BR; "
          f"live KBIAS and live KSCALE; selector live")
    out.write_bytes(bytes(blob))
    print("output", hashlib.sha256(bytes(blob)).hexdigest(), len(blob))


if __name__ == "__main__":
    main()
