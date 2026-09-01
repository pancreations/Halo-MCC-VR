"""Stage 3CX - three-point vertical bias + the accepted live zoom, on 3CR.

Third measured layout (14:46 post-restart session, base -1308.806/582.373,
hide 5235.225): the live zoom law delivered the ACCEPTED apparent size (user
screenshot: correct reticle, slight top shave) and the dumps measure the
placement miss exactly - ink bbox y 0..326 (top-clipped), bloomed envelope
~419x342, so the content centre sat at ~155 and needs ~+101px (post-zoom,
z=1.9957 here) to centre: bias 73.4 -> ~22.8 pre-zoom px of H=3775.2
-> f(582.373) ~= 0.006040.

Three measured anchors now pin the bias curve (u = 1/|baseY|):

    f(u) = clamp(P + Q*u + R*u^2, 0, f_cal)
    f(336.549) = 0.0772547   (accepted layout, bit-exact)
    f(731.878) = 0.003274    (3CU session, measured rest centring)
    f(582.373) = 0.006040    (this session, measured bloom centring)

monotone decreasing across the observed layout range (sampled check below),
clamped to [0, f_cal] outside it. The zoom law is unchanged from 3CW
(z = ZA + ZB/hide, exact at both size anchors; this session interpolated
z=1.996 and the user confirmed the size). Payload structure otherwise
identical: refresh KBIAS and KSCALE, run the byte-identical accepted 3BR
thunk; invalid live values skip their refresh; selector stays live.
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

CAL_F = struct.unpack("<f", bytes.fromhex("b9379e3d"))[0]
ANCHORS = ((336.549, CAL_F), (731.878, 0.003274), (582.373, 0.006040))
CAL_H, CAL_Z = 4134.312, 2.5
NEW_H, NEW_Z = 6543.256, 2.5 * 185.0 / 286.0
ZA = (CAL_Z * CAL_H - NEW_Z * NEW_H) / (CAL_H - NEW_H)
ZB = (CAL_Z - ZA) * CAL_H

# exact quadratic in u = 1/y through the three anchors
(u1, f1), (u2, f2), (u3, f3) = [(1.0 / y, f) for y, f in ANCHORS]
den12, den32 = u1 - u2, u3 - u2
r_num = (f3 - f2) / den32 - (f1 - f2) / den12
R = r_num / (u3 - u1)
Q = (f1 - f2) / den12 - R * (u1 + u2)
P = f2 - Q * u2 - R * u2 * u2

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


def law(y):
    u = 1.0 / y
    return min(max(P + Q * u + R * u * u, 0.0), CAL_F)


def main():
    src, out = Path(sys.argv[1]), Path(sys.argv[2])
    blob = bytearray(src.read_bytes())
    sha = hashlib.sha256(blob).hexdigest()
    print("input", sha)
    if sha != EXPECTED_INPUT_SHA256:
        raise SystemExit("wrong Stage 3CR input: " + sha)
    print("bias quad: P=%.7f Q=%.5f R=%.3f | zoom: ZA=%.6f ZB=%.5f"
          % (P, Q, R, ZA, ZB))
    for y, want in ANCHORS:
        if abs(law(y) - want) > 1e-6:
            raise SystemExit(f"bias law misses anchor y={y}")
        print(f"    anchor f({y}) = {law(y):.7f}")
    prev = None
    for i in range(80):
        y = 336.549 + (731.878 - 336.549) * i / 79.0
        f = law(y)
        # sub-pixel wiggle allowance: 1e-4 of H (~0.4px pre-zoom) is noise
        if prev is not None and f > prev + 1e-4:
            raise SystemExit(f"bias law not monotone at y={y:.1f}")
        prev = f
    print("    monotone decreasing (sub-pixel tolerance) 336.549..731.878: OK")
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
    fixups = []
    def rip(opcode: bytes, target=None, slot=None):
        here = PAYLOAD_RVA + len(code)
        code.extend(opcode)
        if slot is not None:
            fixups.append((len(code), slot))
            code.extend(b"\0\0\0\0")
        else:
            code.extend(struct.pack("<i", target - (here + len(opcode) + 4)))

    # -- bias: KBIAS = clamp(P + Q*u + R*u^2, 0, CAL_F), u = 1/baseY --
    rip(bytes.fromhex("f30f1005"), BASE_Y_RVA)     # movss xmm0,[baseY]
    code += bytes.fromhex("0f2ec0")                # ucomiss xmm0,xmm0
    jp1 = len(code); code += b"\x7A\x00"           # jp  .zoom
    code += bytes.fromhex("0f57d2")                # xorps xmm2,xmm2
    code += bytes.fromhex("0f2fc2")                # comiss xmm0,xmm2
    jbe1 = len(code); code += b"\x76\x00"          # jbe .zoom
    rip(bytes.fromhex("f30f100d"), slot=0)         # movss xmm1,[ONE]
    code += bytes.fromhex("f30f5ec8")              # divss xmm1,xmm0  -> u
    rip(bytes.fromhex("f30f101d"), slot=1)         # movss xmm3,[R]
    code += bytes.fromhex("f30f59d9")              # mulss xmm3,xmm1
    rip(bytes.fromhex("f30f581d"), slot=2)         # addss xmm3,[Q]
    code += bytes.fromhex("f30f59d9")              # mulss xmm3,xmm1
    rip(bytes.fromhex("f30f581d"), slot=3)         # addss xmm3,[P]
    code += bytes.fromhex("f30f5fda")              # maxss xmm3,xmm2 (>=0)
    rip(bytes.fromhex("f30f5d1d"), slot=4)         # minss xmm3,[CAL_F]
    rip(bytes.fromhex("f30f111d"), KBIAS_RVA)      # movss [KBIAS],xmm3
    zoom_at = len(code)
    code[jp1+1] = zoom_at - (jp1 + 2)
    code[jbe1+1] = zoom_at - (jbe1 + 2)
    # -- zoom: KSCALE = ZA + ZB/hide (unchanged from 3CW) --
    rip(bytes.fromhex("f30f1005"), LIVE_HIDE_X_RVA)
    code += bytes.fromhex("0f2ec0")
    jp2 = len(code); code += b"\x7A\x00"
    code += bytes.fromhex("0f57d2")
    code += bytes.fromhex("0f2fc2")
    jbe2 = len(code); code += b"\x76\x00"
    rip(bytes.fromhex("f30f100d"), slot=5)         # movss xmm1,[ZB]
    code += bytes.fromhex("f30f5ec8")              # divss xmm1,xmm0
    rip(bytes.fromhex("f30f580d"), slot=6)         # addss xmm1,[ZA]
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
    for v in (1.0, R, Q, P, CAL_F, ZB, ZA):
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
    ev = [struct.unpack_from("<f", code, s - PAYLOAD_RVA)[0] for s in slots]
    for y, want in ANCHORS:
        u = ev[0] / y
        got = min(max(ev[3] + ev[2] * u + ev[1] * u * u, 0.0), ev[4])
        if abs(got - want) > 2e-5:
            raise SystemExit(f"embedded bias misses anchor y={y}: {got}")
        print(f"    verify bias f({y}) = {got:.7f}")
    for h, want in ((CAL_H, CAL_Z), (NEW_H, NEW_Z)):
        got = ev[6] + ev[5] / h
        if abs(got - want) > 1e-4:
            raise SystemExit("embedded zoom misses anchor")
        print(f"    verify zoom z({h}) = {got:.5f}")

    blob[po:po+len(code)] = code
    blob[go:go+5] = b"\xE8" + struct.pack("<i", PAYLOAD_RVA - (GATE_CALL_RVA+5))
    print(f"payload {len(code)}B; three-point live KBIAS + live KSCALE; "
          f"selector live; accepted 3BR chain untouched")
    out.write_bytes(bytes(blob))
    print("output", hashlib.sha256(bytes(blob)).hexdigest(), len(blob))


if __name__ == "__main__":
    main()
