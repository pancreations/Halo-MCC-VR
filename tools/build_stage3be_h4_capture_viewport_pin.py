"""Stage 3BE - pin the Halo 4 capture framing at every captured draw.

Input is the exact Stage 3BD DLL.  PE geometry is untouched; the payload
lands in the free tail of the .s3qd page at 0x2FB810, immediately after the
Stage 3BD viewport constant (0x2FB800..0x2FB80F).

Why (2026-08-28 headset result): Stage 3BD quadrupled the capture viewport
and the captured art did not change size ("IT DIDNT SHRINK") - proof that the
framing this mod sets is not the framing the captured draws execute with.
The shipped reroute already re-asserts viewport+scissor after every
mid-capture scene rebind (0x2FFDE..0x30010), so the engine must be setting
its own viewport AFTER that re-assert and before the batched draws flush -
exactly what SCENEPROBE measured (947x683 at one moment, full-raster at
another, on the same bound RTV).

Fix: the only un-raceable point is the draw itself.  The DLL already detours
ID3D11DeviceContext::Draw (Halo2DrawCensusHook, 0xD0A0) and ::DrawIndexed
(Halo2DrawIndexedCensusHook, 0xD200) in production; both open with a 5-byte
`call TitleAdapter_GetActiveTitle` (0x879C0) once the context is saved in
rbx.  Both call sites are retargeted to stage3be_draw_gate, which - only for
Halo 4, only on the game's immediate context, only while an authored capture
is active, and only when the CURRENTLY BOUND render target is the capture
target - sets the saved capture viewport+scissor back immediately before the
draw, then tail-jumps into the real title adapter so the Halo 2 census logic
is untouched.

Every referenced global was read out of the Stage 3BD disassembly this
session, not copied from source or another title:
  0x2AE770 g_reticleCaptureState.active        (byte,  set 0x11DD2)
  0x2AE771 g_reticleCaptureState.publishesAuthored (byte, set 0x11DB3)
  0x2AE774 g_reticleCaptureState.captureViewport   (24B, stored 0x11DBF/0x11DD9)
  0x2AE78C g_reticleCaptureState.captureScissor    (16B, stored 0x11DE1)
  0x2AE79C g_reticleCaptureState.framingCaptured   (byte, set 0x11DC6)
  0x2AE298 g_context                (compared by the reroute at 0x2FFCF)
  0x2AE448 g_authoredReticleRtv     (selected at 0x2FF61/0x11B6F)
  0x2AE458 g_authoredReticleDiscardRtv
  0x2BA6C8 active-title byte        (TitleAdapter_GetActiveTitle body 0x879C0)
Vtable offsets confirmed against the compiled Begin/reroute code:
  OMGetRenderTargets +0x2C8 (0x11BF6), RSSetViewports +0x160 (0x11D8C/0x2FFEA),
  RSSetScissorRects +0x168 (0x11DA6/0x2FFFF), IUnknown::Release +0x10.
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
PAYLOAD_LIMIT = 0x002FC000       # end of .s3qd

# --- DLL functions / globals the payload references ------------------------
LOG_RVA = 0x00001D90
TITLE_ADAPTER_RVA = 0x000879C0
CAPTURE_ACTIVE_RVA = 0x002AE770
PUBLISHES_AUTHORED_RVA = 0x002AE771
CAPTURE_VIEWPORT_RVA = 0x002AE774
CAPTURE_SCISSOR_RVA = 0x002AE78C
FRAMING_CAPTURED_RVA = 0x002AE79C
G_CONTEXT_RVA = 0x002AE298
AUTHORED_RTV_RVA = 0x002AE448
DISCARD_RTV_RVA = 0x002AE458
ACTIVE_TITLE_RVA = 0x002BA6C8

# --- splice sites: the two census hooks' TitleAdapter calls -----------------
DRAW_CALL_RVA = 0x0000D0C5           # inside Halo2DrawCensusHook (0xD0A0)
DRAWIDX_CALL_RVA = 0x0000D229        # inside Halo2DrawIndexedCensusHook (0xD200)
CALL_EXPECT = {
    DRAW_CALL_RVA: bytes.fromhex("e8f6a80700"),
    DRAWIDX_CALL_RVA: bytes.fromhex("e892a70700"),
}

CONTEXT = (
    # both hooks' argument-save prologues directly before the splices: the
    # context must already be in rbx when the gate runs
    (0x0000D0BD, bytes.fromhex("418bf08bfa488bd9"),
     "Draw hook saves r8d/edx/rcx(->rbx) before the title call"),
    (0x0000D21E, bytes.fromhex("418be9418bf08bfa488bd9"),
     "DrawIndexed hook saves r9d/r8d/edx/rcx(->rbx) before the title call"),
    # the title adapter the gate tail-jumps into, reading the exact byte the
    # gate also tests
    (TITLE_ADAPTER_RVA, bytes.fromhex("0fb605012d230090c3"),
     "TitleAdapter_GetActiveTitle reads the active-title byte"),
    # capture state layout: Begin stores viewport/flags exactly where the
    # gate reads them
    (0x00011DBF, bytes.fromhex("0f1105aec92900c605cfc92900010f10442440"),
     "Begin stores captureViewport at 0x2AE774 and framingCaptured=1"),
    (0x00011DE1, bytes.fromhex("0f1105a4c92900"),
     "Begin stores captureScissor at 0x2AE78C"),
    # the reroute's own re-assert: proves viewport/scissor addresses and the
    # +0x160 vtable offset in shipped code
    (0x0002FFDE, bytes.fromhex("4c8d058fe72700ba01000000ff9060010000"),
     "reroute re-asserts RSSetViewports(1, 0x2AE774)"),
    (0x0002FF38, bytes.fromhex("803d31e8270000"),
     "reroute tests g_reticleCaptureState.active at 0x2AE770"),
    (0x0002FFCF, bytes.fromhex("488b0dc2e227004c3bf1"),
     "reroute compares the context against g_context at 0x2AE298"),
    (0x0002FF5A, bytes.fromhex(
        "803d10e8270000488b0df0e42700480f450dd8e42700"),
     "reroute selects authored/discard RTV from 0x2AE448/0x2AE458"),
    (LOG_RVA, bytes.fromhex("48894c2408488954"), "LOG prologue"),
    # Stage 3BD identity: the retargeted movdqa and its constant
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
            "usage: build_stage3be_h4_capture_viewport_pin.py "
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
    for rva, expect in CALL_EXPECT.items():
        guard(blob, pe, rva, expect, f"census title call at 0x{rva:X}")
        disp = struct.unpack_from("<i", blob, rva_off(pe, rva) + 1)[0]
        if rva + 5 + disp != TITLE_ADAPTER_RVA:
            raise SystemExit(
                f"call at 0x{rva:X} does not target the title adapter")

    # The payload region must be genuinely free.
    o = rva_off(pe, PAYLOAD_RVA)
    if any(blob[o:rva_off(pe, PAYLOAD_LIMIT - 1) + 1]):
        raise SystemExit("Stage3BE payload region is not free")

    code, symbols = build_payload(
        Path(__file__).parent / "stage3be_h4_capture_viewport_pin.S",
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
            AUTHORED_RTV_RVA=AUTHORED_RTV_RVA,
            DISCARD_RTV_RVA=DISCARD_RTV_RVA,
            ACTIVE_TITLE_RVA=ACTIVE_TITLE_RVA,
        ),
        want_symbols=("stage3be_data", "stage3be_draw_gate"))
    if PAYLOAD_RVA + len(code) > PAYLOAD_LIMIT:
        raise SystemExit(f"payload too large: {len(code)} bytes")
    gate_rva = symbols["stage3be_draw_gate"]
    print(f"payload {len(code)} bytes at 0x{PAYLOAD_RVA:X}; "
          f"gate at 0x{gate_rva:X}")

    blob[o:o + len(code)] = code

    # Retarget both census title calls into the gate.
    for rva in CALL_EXPECT:
        struct.pack_into("<i", blob, rva_off(pe, rva) + 1,
                         gate_rva - (rva + 5))
        print(f"census call 0x{rva:X}: 0x{TITLE_ADAPTER_RVA:X} "
              f"-> 0x{gate_rva:X}")

    if struct.unpack_from("<I", blob, pe["opt"] + 0x40)[0] != 0:
        raise SystemExit("unexpected non-zero PE checksum")
    if len(blob) != original_len:
        raise SystemExit("output size changed unexpectedly")

    out.write_bytes(bytes(blob))
    print("output", hashlib.sha256(bytes(blob)).hexdigest(), len(blob))


if __name__ == "__main__":
    main()
