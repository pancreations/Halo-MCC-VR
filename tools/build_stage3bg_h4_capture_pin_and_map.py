"""Stage 3BG - draw-time framing pin + telemetry + one-shot 32x32 capture map.

Input is the exact Stage 3BD DLL (pre-3BE base): the whole payload region is
rewritten.  Three splices:

  0xD0C5 / 0xD229  census-hook TitleAdapter calls -> s3bg_draw_gate
                   (byte-for-byte the Stage 3BF pin + viewport telemetry)
  0x2763C          the `call LOG` printing "%s reticle upload: ..." ->
                   s3bg_upload_tap: forwards the call (five stack varargs
                   copied into a fresh frame), then once per session, on the
                   third Halo 4 stats window, copies the published capture
                   texture to a CPU staging texture and logs a 32x32 ASCII
                   intensity map ('.'=blank, 0-9).  That picture is the
                   measurement the container/framing guesses never had.

New disassembly-proven addresses (this session):
  g_device 0x2AE290; g_authoredReticleTexture 0x2AE440 (CreateTexture2D out
  at 0x18641-0x18666, SRV/RTV source at 0x18451/0x1867B); upload stats LOG
  site 0x2763C with format string 0x1BD550 (lea rcx at 0x27635); vtable:
  CreateTexture2D dev+0x28, GetDesc tex+0x50, Map ctx+0x70, Unmap ctx+0x78,
  CopyResource ctx+0x178.
"""

from pathlib import Path
import hashlib
import struct
import sys

sys.path.insert(0, str(Path(__file__).parent))
from postlink import build_payload   # noqa: E402

EXPECTED_STAGE3BD_SHA256 = \
    "2e19f93af12f8538f37bb7375a5faa8bf0210b74697a87cccc5d29c637068422"

PAYLOAD_RVA = 0x002FB810
PAYLOAD_LIMIT = 0x002FC000

LOG_RVA = 0x00001D90
TITLE_ADAPTER_RVA = 0x000879C0
CAPTURE_ACTIVE_RVA = 0x002AE770
PUBLISHES_AUTHORED_RVA = 0x002AE771
CAPTURE_VIEWPORT_RVA = 0x002AE774
CAPTURE_SCISSOR_RVA = 0x002AE78C
FRAMING_CAPTURED_RVA = 0x002AE79C
G_CONTEXT_RVA = 0x002AE298
G_DEVICE_RVA = 0x002AE290
AUTHORED_RTV_RVA = 0x002AE448
DISCARD_RTV_RVA = 0x002AE458
AUTHORED_TEX_RVA = 0x002AE440
ACTIVE_TITLE_RVA = 0x002BA6C8

DRAW_CALL_RVA = 0x0000D0C5
DRAWIDX_CALL_RVA = 0x0000D229
UPLOAD_CALL_RVA = 0x0002763C
CALL_EXPECT = {
    DRAW_CALL_RVA: (bytes.fromhex("e8f6a80700"), TITLE_ADAPTER_RVA),
    DRAWIDX_CALL_RVA: (bytes.fromhex("e892a70700"), TITLE_ADAPTER_RVA),
    UPLOAD_CALL_RVA: (bytes.fromhex("e84fa7fdff"), LOG_RVA),
}

CONTEXT = (
    (0x0000D0BD, bytes.fromhex("418bf08bfa488bd9"),
     "Draw hook saves r8d/edx/rcx(->rbx) before the title call"),
    (0x0000D21E, bytes.fromhex("418be9418bf08bfa488bd9"),
     "DrawIndexed hook saves r9d/r8d/edx/rcx(->rbx) before the title call"),
    (TITLE_ADAPTER_RVA, bytes.fromhex("0fb605012d230090c3"),
     "TitleAdapter_GetActiveTitle reads the active-title byte"),
    # the upload stats call site: lea rcx,[fmt] directly before the call,
    # and the five stack varargs stores directly before that
    (0x00027635, bytes.fromhex("488d0d145f1900"),
     "lea rcx, [\"%s reticle upload: ...\"] before the spliced call"),
    (0x0002761E, bytes.fromhex("8974242848897c2420"),
     "pieces/key stack vararg stores before the stats call"),
    (0x00027641, bytes.fromhex("44893d446e2800"),
     "post-call counter reset directly after the spliced call"),
    # capture state layout (Stage 3BE/3BF proofs)
    (0x00011DBF, bytes.fromhex("0f1105aec92900c605cfc92900010f10442440"),
     "Begin stores captureViewport at 0x2AE774 and framingCaptured=1"),
    (0x00011DE1, bytes.fromhex("0f1105a4c92900"),
     "Begin stores captureScissor at 0x2AE78C"),
    (0x00011C1E, bytes.fromhex("ff90f8020000"),
     "Begin calls RSGetViewports via vtable +0x2F8"),
    (0x00011C46, bytes.fromhex("ff9000030000"),
     "Begin calls RSGetScissorRects via vtable +0x300"),
    (0x0002FFDE, bytes.fromhex("4c8d058fe72700ba01000000ff9060010000"),
     "reroute re-asserts RSSetViewports(1, 0x2AE774)"),
    (0x0002FF38, bytes.fromhex("803d31e8270000"),
     "reroute tests g_reticleCaptureState.active at 0x2AE770"),
    (0x0002FFCF, bytes.fromhex("488b0dc2e227004c3bf1"),
     "reroute compares the context against g_context at 0x2AE298"),
    (0x0002FF5A, bytes.fromhex(
        "803d10e8270000488b0df0e42700480f450dd8e42700"),
     "reroute selects authored/discard RTV from 0x2AE448/0x2AE458"),
    # texture/device globals used by the dump
    (0x00018641, bytes.fromhex("488b0d485c29004c8d0df15d2900"),
     "Ensure creates the authored texture: rcx=g_device 0x2AE290, "
     "r9=&g_authoredReticleTexture 0x2AE440"),
    (LOG_RVA, bytes.fromhex("48894c2408488954"), "LOG prologue"),
    (0x00011C77, bytes.fromhex("660f6f0d819b2e00"),
     "Stage 3BD movdqa load of the private viewport constant"),
    (0x002FB800, struct.pack("<ffff", 2048.0, 2048.0, 0.0, 1.0),
     "Stage 3BD private {2048,2048,0,1} constant"),
    (0x00053634, bytes.fromhex("7456"), "Stage 3AX capture edge restored"),
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
    opt = coff + 20
    st = opt + osz
    secs = []
    for i in range(n):
        o = st + i * 40
        name = bytes(blob[o:o + 8]).split(b"\0", 1)[0].decode("ascii")
        vs, va, rs, rp = struct.unpack_from("<IIII", blob, o + 8)
        secs.append(dict(name=name, vs=vs, va=va, rs=rs, rp=rp))
    return dict(opt=opt, n=n, sections=secs)


def rva_off(pe, rva):
    for s in pe["sections"]:
        if s["va"] <= rva < s["va"] + max(s["vs"], s["rs"]):
            return s["rp"] + rva - s["va"]
    raise KeyError(hex(rva))


def guard(blob, pe, rva, expected, label):
    o = rva_off(pe, rva)
    actual = bytes(blob[o:o + len(expected)])
    if actual != expected:
        raise SystemExit(
            f"{label}: expected {expected.hex()} at 0x{rva:X}, "
            f"got {actual.hex()}")
    return o


def main():
    if len(sys.argv) != 3:
        raise SystemExit(
            "usage: build_stage3bg_h4_capture_pin_and_map.py "
            "<Stage3BD-HaloMCCVR.dll> <output.dll>")
    src, out = Path(sys.argv[1]), Path(sys.argv[2])

    blob = bytearray(src.read_bytes())
    original_len = len(blob)
    sha = hashlib.sha256(blob).hexdigest()
    print("input", sha)
    if sha != EXPECTED_STAGE3BD_SHA256:
        raise SystemExit("wrong Stage3BD input DLL: " + sha)

    pe = parse_pe(blob)
    if pe["n"] != 12:
        raise SystemExit(f"unexpected PE geometry: n={pe['n']}")
    s3qd = next(s for s in pe["sections"] if s["name"] == ".s3qd")
    if not (s3qd["va"] <= PAYLOAD_RVA and
            PAYLOAD_LIMIT <= s3qd["va"] + s3qd["rs"]):
        raise SystemExit("payload region is not inside .s3qd raw data")

    for rva, expect, label in CONTEXT:
        guard(blob, pe, rva, expect, label)
    for rva, (expect, target) in CALL_EXPECT.items():
        guard(blob, pe, rva, expect, f"spliced call at 0x{rva:X}")
        disp = struct.unpack_from("<i", blob, rva_off(pe, rva) + 1)[0]
        if rva + 5 + disp != target:
            raise SystemExit(
                f"call at 0x{rva:X} does not target 0x{target:X}")

    o = rva_off(pe, PAYLOAD_RVA)
    if any(blob[o:rva_off(pe, PAYLOAD_LIMIT - 1) + 1]):
        raise SystemExit("Stage3BG payload region is not free")

    code, symbols = build_payload(
        Path(__file__).parent / "stage3bg_h4_capture_pin_and_map.S",
        PAYLOAD_RVA,
        defs=dict(
            LOG_RVA=LOG_RVA,
            TITLE_ADAPTER_RVA=TITLE_ADAPTER_RVA,
            CAPTURE_ACTIVE_RVA=CAPTURE_ACTIVE_RVA,
            PUBLISHES_AUTHORED_RVA=PUBLISHES_AUTHORED_RVA,
            CAPTURE_VIEWPORT_RVA=CAPTURE_VIEWPORT_RVA,
            CAPTURE_SCISSOR_RVA=CAPTURE_SCISSOR_RVA,
            FRAMING_CAPTURED_RVA=FRAMING_CAPTURED_RVA,
            G_CONTEXT_RVA=G_CONTEXT_RVA,
            G_DEVICE_RVA=G_DEVICE_RVA,
            AUTHORED_RTV_RVA=AUTHORED_RTV_RVA,
            DISCARD_RTV_RVA=DISCARD_RTV_RVA,
            AUTHORED_TEX_RVA=AUTHORED_TEX_RVA,
            ACTIVE_TITLE_RVA=ACTIVE_TITLE_RVA,
        ),
        want_symbols=("stage3bg_data", "s3bg_draw_gate", "s3bg_upload_tap"))
    if PAYLOAD_RVA + len(code) > PAYLOAD_LIMIT:
        raise SystemExit(f"payload too large: {len(code)} bytes")
    gate_rva = symbols["s3bg_draw_gate"]
    tap_rva = symbols["s3bg_upload_tap"]
    print(f"payload {len(code)} bytes at 0x{PAYLOAD_RVA:X}; "
          f"gate 0x{gate_rva:X}, tap 0x{tap_rva:X}")

    blob[o:o + len(code)] = code

    for rva, target in ((DRAW_CALL_RVA, gate_rva),
                        (DRAWIDX_CALL_RVA, gate_rva),
                        (UPLOAD_CALL_RVA, tap_rva)):
        struct.pack_into("<i", blob, rva_off(pe, rva) + 1, target - (rva + 5))
        print(f"call 0x{rva:X} -> 0x{target:X}")

    if struct.unpack_from("<I", blob, pe["opt"] + 0x40)[0] != 0:
        raise SystemExit("unexpected non-zero PE checksum")
    if len(blob) != original_len:
        raise SystemExit("output size changed unexpectedly")

    out.write_bytes(bytes(blob))
    print("output", hashlib.sha256(bytes(blob)).hexdigest(), len(blob))


if __name__ == "__main__":
    main()
