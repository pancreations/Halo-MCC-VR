"""Stage 3BV - Halo 4 cinematic-globals probe (log-only, fail-open).
Input: exact Stage 3BU DLL.  One 6-byte change: the `jne` at 0x2C2BC (which
Stage 3BU pointed at its write-back thunk 0x2F9D10) is re-pointed to
s3bv_probe, which samples halo4.dll's H4EK-proven cinematic-globals byte at
most every 250 ms, LOGs only state transitions, and ends with
`jmp 0x2F9D10` so the accepted 3BU behavior is unchanged."""
from pathlib import Path
import hashlib, struct, sys
sys.path.insert(0, str(Path(__file__).parent))
from postlink import build_payload
from build_stage3br_h4_capture_scale import parse_pe, rva_off

EXPECTED_INPUT_SHA256 = \
    "5905afce6cc06996dd0c379d9cb1c6731091fe2d7f7a74051f6ae82004f3ac03"
PAYLOAD_RVA = 0x002F9E10            # after the 3BU region (ends 0x2F9E10)
PAYLOAD_LIMIT = 0x002FA000          # end of the 3AS scratch page
SPLICE_RVA = 0x0002C2BC
SPLICE_OLD = bytes.fromhex("0f854eda2c00")   # jne 0x2F9D10 (Stage 3BU)
S3BU_THUNK_RVA = 0x002F9D10

LOG_RVA = 0x00001D90
IAT_GETTICKCOUNT64_RVA = 0x00180150
IAT_GETMODULEHANDLEW_RVA = 0x001800F8

CONTEXT = (
    (S3BU_THUNK_RVA, bytes.fromhex("4883ec40"), "3BU thunk head sub rsp,0x40"),
    (LOG_RVA, bytes.fromhex("48894c2408488954"), "LOG prologue"),
    # IAT thunk file bytes: import-name-table RVAs (rebound at load)
    (IAT_GETTICKCOUNT64_RVA, bytes.fromhex("ce10240000000000"),
     "IAT GetTickCount64"),
    (IAT_GETMODULEHANDLEW_RVA, bytes.fromhex("f80f240000000000"),
     "IAT GetModuleHandleW"),
    (0x0002C2A8, bytes.fromhex("e813b70500"), "call TitleAdapter_GetActiveTitle"),
    (0x0002C2AD, bytes.fromhex("3c04" "0f8530010000"), "cmp al,4 (Halo 4 gate)"),
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
    if SPLICE_RVA + 6 + struct.unpack_from("<i", SPLICE_OLD, 2)[0] \
            != S3BU_THUNK_RVA:
        raise SystemExit("original jne does not target the 3BU thunk")
    po, pl = rva_off(pe, PAYLOAD_RVA), rva_off(pe, PAYLOAD_LIMIT-1)+1
    if any(blob[po:pl]):
        raise SystemExit("payload region not free")
    code, symbols = build_payload(
        Path(__file__).parent / "stage3bv_h4_cine_probe.S", PAYLOAD_RVA,
        defs=dict(LOGFN=LOG_RVA,
                  IAT_TICK64=IAT_GETTICKCOUNT64_RVA,
                  IAT_GETMODW=IAT_GETMODULEHANDLEW_RVA,
                  S3BU_THUNK=S3BU_THUNK_RVA),
        want_symbols=("s3bv_probe",))
    if PAYLOAD_RVA + len(code) > PAYLOAD_LIMIT:
        raise SystemExit(f"payload too large: {len(code)}")
    thunk = symbols["s3bv_probe"]
    print(f"payload {len(code)} bytes at 0x{PAYLOAD_RVA:X}")
    blob[po:po+len(code)] = code
    blob[so:so+6] = b"\x0F\x85" + struct.pack("<i", thunk - (SPLICE_RVA + 6))
    print(f"splice 0x{SPLICE_RVA:X}: jne 0x{S3BU_THUNK_RVA:X} -> "
          f"jne 0x{thunk:X} (chains back)")
    out.write_bytes(bytes(blob))
    print("output", hashlib.sha256(bytes(blob)).hexdigest(), len(blob))


if __name__ == "__main__":
    main()
