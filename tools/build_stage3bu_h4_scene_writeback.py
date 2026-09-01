"""Stage 3BU - Halo 4 scene-target write-back (desktop mirror + damage black).
Input: exact Stage 3BT DLL.  One 6-byte change: the steady-state `jne 0x2C3E5`
at 0x2C2BC inside VR_EndRasterEye's inlined Halo4ResolveSceneTargetAtEyeEnd
(taken when eye>=0 && title==Halo4 && g_sceneColorRtv latched) is re-pointed
to s3bu_writeback, which CopyResources the eye cache into the real scene
texture and jumps to the original destination."""
from pathlib import Path
import hashlib, struct, sys
sys.path.insert(0, str(Path(__file__).parent))
from postlink import build_payload
from build_stage3br_h4_capture_scale import parse_pe, rva_off

EXPECTED_INPUT_SHA256 = \
    "4a1970734c5266688d2c61490fc485f74cbd3d39a3ffc568d0832791ada2279a"
PAYLOAD_RVA = 0x002F9D10            # after the 3BT thunk (limit page 0x2FA000)
PAYLOAD_LIMIT = 0x002F9E10
SPLICE_RVA = 0x0002C2BC
SPLICE_OLD = bytes.fromhex("0f8523010000")   # jne 0x2C3E5
RESUME_RVA = 0x0002C3E5

RASTER_EYE_RVA = 0x002420A8
EYE_CACHE_RVA = 0x002AE808
SCENE_RTV_RVA = 0x002AEAF0
G_CONTEXT_RVA = 0x002AE298

CONTEXT = (
    (0x0002C2A8, bytes.fromhex("e813b70500"), "call TitleAdapter_GetActiveTitle 0x879C0"),
    (0x0002C2AD, bytes.fromhex("3c04" "0f8530010000"), "cmp al,4 / jne (Halo 4 gate)"),
    (0x0002C2B5, bytes.fromhex("48393d34282800"), "cmp [g_sceneColorRtv 0x2AEAF0], rdi"),
    (0x00018B95, bytes.fromhex("48833d6b5c290000"), "EnsureEyeCaches checks g_eyeCache[0] 0x2AE808"),
    (0x0002C4EF, bytes.fromhex("8705b35b2100"), "epilogue xchg [g_rasterEye 0x2420A8], eax"),
    (0x000199B3, bytes.fromhex("b201"), "3BJ NOT included"),
)


def main():
    src, out = Path(sys.argv[1]), Path(sys.argv[2])
    blob = bytearray(src.read_bytes())
    sha = hashlib.sha256(blob).hexdigest()
    print("input", sha)
    if sha != EXPECTED_INPUT_SHA256:
        raise SystemExit("wrong input DLL: " + sha)
    pe = parse_pe(blob)
    if pe["n"] != 12:
        raise SystemExit("unexpected PE geometry")
    for rva, expect, label in CONTEXT:
        o = rva_off(pe, rva)
        if bytes(blob[o:o+len(expect)]) != expect:
            raise SystemExit(f"{label}: got {bytes(blob[o:o+len(expect)]).hex()}")
    so = rva_off(pe, SPLICE_RVA)
    if bytes(blob[so:so+6]) != SPLICE_OLD:
        raise SystemExit("splice site unexpected: " + bytes(blob[so:so+6]).hex())
    if SPLICE_RVA + 6 + struct.unpack_from("<i", SPLICE_OLD, 2)[0] != RESUME_RVA:
        raise SystemExit("original jne does not target RESUME_RVA")
    po, pl = rva_off(pe, PAYLOAD_RVA), rva_off(pe, PAYLOAD_LIMIT-1)+1
    if any(blob[po:pl]):
        raise SystemExit("payload region not free")
    code, symbols = build_payload(
        Path(__file__).parent / "stage3bu_h4_scene_writeback.S", PAYLOAD_RVA,
        defs=dict(RASTER_EYE=RASTER_EYE_RVA, EYE_CACHE=EYE_CACHE_RVA,
                  SCENE_RTV=SCENE_RTV_RVA, G_CONTEXT=G_CONTEXT_RVA,
                  RESUME=RESUME_RVA),
        want_symbols=("s3bu_writeback",))
    if PAYLOAD_RVA + len(code) > PAYLOAD_LIMIT:
        raise SystemExit(f"payload too large: {len(code)}")
    thunk = symbols["s3bu_writeback"]
    print(f"payload {len(code)} bytes at 0x{PAYLOAD_RVA:X}")
    blob[po:po+len(code)] = code
    blob[so:so+6] = b"\x0F\x85" + struct.pack("<i", thunk - (SPLICE_RVA + 6))
    print(f"splice 0x{SPLICE_RVA:X}: jne 0x{RESUME_RVA:X} -> jne 0x{thunk:X}")
    out.write_bytes(bytes(blob))
    print("output", hashlib.sha256(bytes(blob)).hexdigest(), len(blob))


if __name__ == "__main__":
    main()
