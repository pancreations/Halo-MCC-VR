"""Stage 3BI - capture only the hidden reticle container.

Input is the exact Stage 3BH DLL.  One splice:

  0x53921  the capture-replay per-command original() call inside
           Halo4CuiRenderCommandBody (`mov rcx,rsi` + `call [rsp+0x30]`)
           -> s3bi_capture_select: forwards the call, then applies the
           PROVEN native-hide transform shift (x += 4*halfWidth from the
           0x2FB800 constant) to the stack-top transform after every
           command executed OUTSIDE a type-0x28/payload-0xC container,
           and cancels any inherited shift INSIDE one.  Only while
           scope.redirectActive - a failed Begin leaves the stock pass
           untouched.

The result: the capture texture receives exactly the content the visible
pass hides - the authored reticle - and every other HUD element lands one
viewport-width right of the 512x512 scissor, exactly as the reticle does
on the visible HUD.  What disappears from the flat HUD is what appears on
the VR crosshair quad.
"""

from pathlib import Path
import hashlib
import struct
import sys

sys.path.insert(0, str(Path(__file__).parent))
from postlink import build_payload   # noqa: E402

EXPECTED_STAGE3BH_SHA256 = \
    "80573ed9fffd3a557dea96b80823e490ecdaea7c0063ee6a5e1bacc4486d48c4"

PAYLOAD_RVA = 0x002FBE40          # 16-aligned, after the 3BH payload
PAYLOAD_LIMIT = 0x002FC000

SPLICE_RVA = 0x00053921           # mov rcx,rsi ; call qword ptr [rsp+0x30]
SPLICE_OLD = bytes.fromhex("488bceff542430")

G_ORIG_CUI_RENDER_RVA = 0x002B9B18
HALO4_SAFE_READ_RVA = 0x00056C10
CAPTURE_FRAMING_CONST_RVA = 0x002FB800

CONTEXT = (
    # the capture branch this splice lives in
    (0x000538C8, bytes.fromhex("43385c0704"),
     "captureReplay test byte [r15+r8+4] guards the branch"),
    (0x000538D3, bytes.fromhex("e9b86e2a00"),
     "existing attempted-check splice jmp 0x2fa790 (kept)"),
    (0x00053914, bytes.fromhex("4c8b4c2438498bd54c8b442440"),
     "arg reload straight before the spliced pair"),
    (0x00053928, bytes.fromhex("440fb6f8"),
     "movzx r15d, al consumes the preserved result"),
    (0x00053934, bytes.fromhex("385c2421"),
     "beginReadable test byte [rsp+0x21] follows"),
    # the facts the payload depends on
    (0x00053791, bytes.fromhex("488b0580632600"),
     "g_halo4OrigCuiRenderCommand load -> 0x2B9B18"),
    (0x000538EE, bytes.fromhex("6641c74407020101"),
     "Begin success sets redirectActive/captureAuthored word at scope+2"),
    (0x00053876, bytes.fromhex("4c8b3cc8"),
     "r15 = TLS slot base held through the capture branch"),
    (HALO4_SAFE_READ_RVA, bytes.fromhex("4883ec28488bc2488bd1488bc8e8"),
     "Halo4SafeRead SEH thunk at 0x56C10"),
    # the proven hide this stage mirrors (visible-pass shift write)
    (0x00053B01, bytes.fromhex("83fa28"),
     "visible pass tests command 0x28"),
    # Stage 3BH artifacts stay
    (CAPTURE_FRAMING_CONST_RVA, struct.pack("<ffff",
     4134.312, 1346.196, 0.0, 1.0),
     "Stage 3BH engine-framing constant at 0x2FB800"),
    (0x0000D0C5, bytes.fromhex("e8"), "3BH draw splice intact"),
    (0x0002763C, bytes.fromhex("e8"), "3BH upload tap intact"),
    # accepted-base carried bytes
    (0x002C0589, None, "CREDIT bytes present"),
)


def parse_pe(blob):
    if blob[:2] != b"MZ":
        raise SystemExit("input is not MZ")
    p = struct.unpack_from("<I", blob, 0x3C)[0]
    if blob[p:p + 4] != b"PE\0\0":
        raise SystemExit("input is not PE")
    coff = p + 4
    n = struct.unpack_from("<H", blob, coff + 2)[0]
    osz = struct.unpack_from("<H", blob, coff + 16)[0]
    st = coff + 20 + osz
    secs = []
    for i in range(n):
        o = st + i * 40
        name = bytes(blob[o:o + 8]).split(b"\0", 1)[0].decode("ascii")
        vs, va, rs, rp = struct.unpack_from("<IIII", blob, o + 8)
        secs.append(dict(name=name, vs=vs, va=va, rs=rs, rp=rp))
    return dict(opt=coff + 20, n=n, sections=secs)


def rva_off(pe, rva):
    for s in pe["sections"]:
        if s["va"] <= rva < s["va"] + max(s["vs"], s["rs"]):
            return s["rp"] + rva - s["va"]
    raise KeyError(hex(rva))


def main():
    if len(sys.argv) != 3:
        raise SystemExit(
            "usage: build_stage3bi_h4_capture_reticle_only.py "
            "<Stage3BH-HaloMCCVR.dll> <output.dll>")
    src, out = Path(sys.argv[1]), Path(sys.argv[2])

    blob = bytearray(src.read_bytes())
    original_len = len(blob)
    sha = hashlib.sha256(blob).hexdigest()
    print("input", sha)
    if sha != EXPECTED_STAGE3BH_SHA256:
        raise SystemExit("wrong Stage3BH input DLL: " + sha)

    pe = parse_pe(blob)
    if pe["n"] != 12:
        raise SystemExit(f"unexpected PE geometry: n={pe['n']}")
    s3qd = next(s for s in pe["sections"] if s["name"] == ".s3qd")
    if not (s3qd["va"] <= PAYLOAD_RVA and
            PAYLOAD_LIMIT <= s3qd["va"] + s3qd["rs"]):
        raise SystemExit("payload region is not inside .s3qd raw data")

    for rva, expect, label in CONTEXT:
        if expect is None:
            continue
        o = rva_off(pe, rva)
        actual = bytes(blob[o:o + len(expect)])
        if actual != expect:
            raise SystemExit(f"{label}: expected {expect.hex()} at "
                             f"0x{rva:X}, got {actual.hex()}")

    so = rva_off(pe, SPLICE_RVA)
    if bytes(blob[so:so + 7]) != SPLICE_OLD:
        raise SystemExit("splice site bytes unexpected: " +
                         bytes(blob[so:so + 7]).hex())

    po = rva_off(pe, PAYLOAD_RVA)
    pl = rva_off(pe, PAYLOAD_LIMIT - 1) + 1
    if any(blob[po:pl]):
        raise SystemExit("Stage3BI payload region is not free")

    code, symbols = build_payload(
        Path(__file__).parent / "stage3bi_h4_capture_reticle_only.S",
        PAYLOAD_RVA,
        defs=dict(
            G_ORIG_CUI_RENDER=G_ORIG_CUI_RENDER_RVA,
            HALO4_SAFE_READ=HALO4_SAFE_READ_RVA,
            CAPTURE_FRAMING_CONST=CAPTURE_FRAMING_CONST_RVA,
        ),
        want_symbols=("s3bi_capture_select", "s3bi_data"))
    if PAYLOAD_RVA + len(code) > PAYLOAD_LIMIT:
        raise SystemExit(f"payload too large: {len(code)} bytes")
    select_rva = symbols["s3bi_capture_select"]
    print(f"payload {len(code)} bytes at 0x{PAYLOAD_RVA:X}; "
          f"select 0x{select_rva:X}, data 0x{symbols['s3bi_data']:X}")

    blob[po:po + len(code)] = code

    disp = select_rva - (SPLICE_RVA + 5)
    blob[so:so + 7] = b"\xE8" + struct.pack("<i", disp) + b"\x90\x90"
    print(f"splice 0x{SPLICE_RVA:X}: mov rcx,rsi + call [rsp+0x30] -> "
          f"call 0x{select_rva:X} + 2 nops")

    if struct.unpack_from("<I", blob, pe["opt"] + 0x40)[0] != 0:
        raise SystemExit("unexpected non-zero PE checksum")
    if len(blob) != original_len:
        raise SystemExit("output size changed unexpectedly")

    out.write_bytes(bytes(blob))
    print("output", hashlib.sha256(bytes(blob)).hexdigest(), len(blob))


if __name__ == "__main__":
    main()
