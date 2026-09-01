"""Stage 3BS - Halo 4 uploads are always treated as a fresh crosshair
identity, so the half-ink hold never freezes the animation.
Input: exact Stage 3BR DLL.  One 13-byte change at 0x2A701 in
UploadAuthoredReticle -> call s3bs_h4_identity + 8 nops."""
from pathlib import Path
import hashlib, struct, sys
sys.path.insert(0, str(Path(__file__).parent))
from postlink import build_payload
from build_stage3br_h4_capture_scale import parse_pe, rva_off

PAYLOAD_RVA = 0x002F9C50
PAYLOAD_LIMIT = 0x002F9CD0
SPLICE_RVA = 0x0002A701
SPLICE_OLD = bytes.fromhex("0fb6f2" "488b05853d2800" "0fb6e9")
ACTIVE_TITLE_RVA = 0x002BA6C8
UPLOAD_GLOB_RVA = 0x002AE490        # 0x2A704 + 7 + 0x283d85

CONTEXT = (
    (0x0002A6F9, bytes.fromhex("440fb6053b3d2800"), "prologue: r8d = flag byte"),
    (0x0002A70E, bytes.fromhex("4584c0"), "test r8b,r8b follows the splice"),
    (0x000199B3, bytes.fromhex("b201"), "3BJ NOT included"),
)


def main():
    src, out = Path(sys.argv[1]), Path(sys.argv[2])
    blob = bytearray(src.read_bytes())
    expected = sys.argv[3]
    sha = hashlib.sha256(blob).hexdigest()
    print("input", sha)
    if sha != expected:
        raise SystemExit("wrong input DLL: " + sha)
    pe = parse_pe(blob)
    if pe["n"] != 12:
        raise SystemExit("unexpected PE geometry")
    for rva, expect, label in CONTEXT:
        o = rva_off(pe, rva)
        if bytes(blob[o:o+len(expect)]) != expect:
            raise SystemExit(f"{label}: got {bytes(blob[o:o+len(expect)]).hex()}")
    so = rva_off(pe, SPLICE_RVA)
    if bytes(blob[so:so+13]) != SPLICE_OLD:
        raise SystemExit("splice site unexpected: " + bytes(blob[so:so+13]).hex())
    if 0x2A704 + 7 + struct.unpack_from("<i", blob, so + 6)[0] != UPLOAD_GLOB_RVA:
        raise SystemExit("displaced global RVA mismatch")
    po, pl = rva_off(pe, PAYLOAD_RVA), rva_off(pe, PAYLOAD_LIMIT-1)+1
    if any(blob[po:pl]):
        raise SystemExit("payload region not free")
    code, symbols = build_payload(
        Path(__file__).parent / "stage3bs_h4_identity_per_upload.S", PAYLOAD_RVA,
        defs=dict(ACTIVE_TITLE=ACTIVE_TITLE_RVA, UPLOAD_GLOB=UPLOAD_GLOB_RVA),
        want_symbols=("s3bs_h4_identity",))
    if PAYLOAD_RVA + len(code) > PAYLOAD_LIMIT:
        raise SystemExit(f"payload too large: {len(code)}")
    thunk = symbols["s3bs_h4_identity"]
    print(f"payload {len(code)} bytes at 0x{PAYLOAD_RVA:X}")
    blob[po:po+len(code)] = code
    blob[so:so+13] = b"\xE8" + struct.pack("<i", thunk - (SPLICE_RVA + 5)) + b"\x90" * 8
    print(f"splice 0x{SPLICE_RVA:X}: -> call 0x{thunk:X} + 8 nops")
    out.write_bytes(bytes(blob))
    print("output", hashlib.sha256(bytes(blob)).hexdigest(), len(blob))


if __name__ == "__main__":
    main()
