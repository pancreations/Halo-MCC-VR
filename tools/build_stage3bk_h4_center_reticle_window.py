"""Stage 3BK - centre the reticle in the capture window.

Input is the exact Stage 3BI DLL (NOT the rejected 3BJ: the procedural
bootstrap paint stays exactly as shipped, so the no-crosshair/layer-invalid
failure cannot recur).  One behavioural change:

  Both capture maps put Halo 4's reticle at texel (256, ~360) - x-centred,
  104 px below centre - because H4 anchors the reticle at the visible screen
  centre, which is CUI canvas (0, +52 units), not the canvas origin the
  engine's framing centres.  The quad shows the texture centre at the aim
  point, so the art rode low and small ("still the placeholder").

  The 3BH draw-time gate re-asserts the stored capture viewport (0x2AE774)
  on every captured draw via RSSetViewports (vtable +0x160).  This stage
  redirects that one call through a thunk that passes a private copy of the
  struct with TopLeftY -= 104.0.  Begin rewrites the stored struct every
  capture and the thunk biases a copy, so nothing accumulates.  Scissor and
  everything else stay as shipped.

Patch: the 24-byte call sequence inside the gate at 0x2FB992
  (mov rcx,rbx / mov edx,1 / lea r8,[0x2AE774] / mov rax,[rbx] /
   call [rax+0x160])
becomes `call s3bk_biased_viewport` + 19 NOPs; the thunk performs the same
call with the biased copy and returns to the scissor re-assert at 0x2FB9AA.
"""

from pathlib import Path
import hashlib
import struct
import sys

sys.path.insert(0, str(Path(__file__).parent))
from postlink import build_payload   # noqa: E402

EXPECTED_STAGE3BI_SHA256 = \
    "dad8a373ed30f3e31f42350fa85b575e2f7b146b8ba83d7f4cdf7333da653d22"

PAYLOAD_RVA = 0x002FBF64          # directly after the 3BI payload
PAYLOAD_LIMIT = 0x002FC000

GATE_CALL_RVA = 0x002FB992
GATE_OLD = bytes.fromhex(
    "4889d9"            # mov rcx, rbx
    "ba01000000"        # mov edx, 1
    "4c8d05d32dfbff"    # lea r8, [rip -> 0x2AE774]
    "488b03"            # mov rax, [rbx]
    "ff9060010000")     # call [rax+0x160]
GATE_RETURN_RVA = GATE_CALL_RVA + len(GATE_OLD)   # 0x2FB9AA scissor re-assert

CAPTURE_VIEWPORT_RVA = 0x002AE774

CONTEXT = (
    # the gate this splices into is the 3BH gate, still intact
    (0x002FB850, bytes.fromhex("803d192ffbff00"),
     "gate entry tests g_reticleCaptureState.active (0x2AE770)"),
    (GATE_RETURN_RVA, bytes.fromhex("4889d9ba010000004c8d05d32dfbff"),
     "scissor re-assert follows the patched sequence"),
    (0x002FB9BC, bytes.fromhex("ff9068010000"),
     "scissor call RSSetScissorRects (+0x168)"),
    # Begin still computes the stored viewport from the 3BH constant
    (0x002FB800, struct.pack("<ffff", 4134.312, 1346.196, 0.0, 1.0),
     "3BH engine-framing constant"),
    (0x00011DBF, bytes.fromhex("0f1105aec92900"),
     "Begin stores captureViewport at 0x2AE774"),
    # 3BI artifacts intact
    (0x00053921, bytes.fromhex("e8"), "3BI capture-selection splice"),
    (0x0000D0C5, bytes.fromhex("e8"), "3BH draw splice"),
    (0x0000D229, bytes.fromhex("e8"), "3BH drawIndexed splice"),
    (0x0002763C, bytes.fromhex("e8"), "3BH upload tap"),
    # the rejected 3BJ patch must NOT be present: 0x199B3 is stock mov dl,1
    (0x000199B3, bytes.fromhex("b201"),
     "0x199B3 is stock `mov dl,1` (3BJ NOT included)"),
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
            "usage: build_stage3bk_h4_center_reticle_window.py "
            "<Stage3BI-HaloMCCVR.dll> <output.dll>")
    src, out = Path(sys.argv[1]), Path(sys.argv[2])

    blob = bytearray(src.read_bytes())
    original_len = len(blob)
    sha = hashlib.sha256(blob).hexdigest()
    print("input", sha)
    if sha != EXPECTED_STAGE3BI_SHA256:
        raise SystemExit("wrong Stage3BI input DLL: " + sha)

    pe = parse_pe(blob)
    if pe["n"] != 12:
        raise SystemExit(f"unexpected PE geometry: n={pe['n']}")

    for rva, expect, label in CONTEXT:
        o = rva_off(pe, rva)
        actual = bytes(blob[o:o + len(expect)])
        if actual != expect:
            raise SystemExit(f"{label}: expected {expect.hex()} at "
                             f"0x{rva:X}, got {actual.hex()}")

    go = rva_off(pe, GATE_CALL_RVA)
    if bytes(blob[go:go + len(GATE_OLD)]) != GATE_OLD:
        raise SystemExit("gate call sequence unexpected: " +
                         bytes(blob[go:go + len(GATE_OLD)]).hex())

    po = rva_off(pe, PAYLOAD_RVA)
    pl = rva_off(pe, PAYLOAD_LIMIT - 1) + 1
    if any(blob[po:pl]):
        raise SystemExit("Stage3BK payload region is not free")

    code, symbols = build_payload(
        Path(__file__).parent / "stage3bk_h4_center_reticle_window.S",
        PAYLOAD_RVA,
        defs=dict(CAPTURE_VIEWPORT=CAPTURE_VIEWPORT_RVA),
        want_symbols=("s3bk_biased_viewport", "s3bk_data"))
    if PAYLOAD_RVA + len(code) > PAYLOAD_LIMIT:
        raise SystemExit(f"payload too large: {len(code)} bytes")
    thunk_rva = symbols["s3bk_biased_viewport"]
    print(f"payload {len(code)} bytes at 0x{PAYLOAD_RVA:X}; "
          f"thunk 0x{thunk_rva:X}, data 0x{symbols['s3bk_data']:X}")

    blob[po:po + len(code)] = code

    disp = thunk_rva - (GATE_CALL_RVA + 5)
    patch = b"\xE8" + struct.pack("<i", disp) + b"\x90" * (len(GATE_OLD) - 5)
    blob[go:go + len(GATE_OLD)] = patch
    print(f"gate 0x{GATE_CALL_RVA:X}: RSSetViewports(stored) -> "
          f"call 0x{thunk_rva:X} (TopLeftY - 104.0) + {len(GATE_OLD)-5} nops")

    if struct.unpack_from("<I", blob, pe["opt"] + 0x40)[0] != 0:
        raise SystemExit("unexpected non-zero PE checksum")
    if len(blob) != original_len:
        raise SystemExit("output size changed unexpectedly")

    out.write_bytes(bytes(blob))
    print("output", hashlib.sha256(bytes(blob)).hexdigest(), len(blob))


if __name__ == "__main__":
    main()
