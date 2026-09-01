"""Stage 3CF - log-only id probe on Stage 3CE (see stage3cf_h4_id_probe.S)."""
from pathlib import Path
import hashlib, struct, sys
sys.path.insert(0, str(Path(__file__).parent))
from postlink import build_payload
from build_stage3br_h4_capture_scale import parse_pe, rva_off

EXPECTED_INPUT_SHA256 = \
    "f23fa778c3755c5f16e1a321ec9e70af488a484f2a8fd1acb4984ee08832dc98"
PAYLOAD_RVA = 0x002F9F90
PAYLOAD_LIMIT = 0x002FA000
SPLICE_RVA = 0x002F995D
SPLICE_OLD = bytes.fromhex("66837c24420c" "752f")  # cmp word,0xC / jne +0x2F
ARM_PATH_RVA = 0x002F9965
SKIP_PATH_RVA = 0x002F9994
LOG_RVA = 0x00001D90

CONTEXT = (
    (0x002F9958, bytes.fromhex("83f828751e"), "selector cmp eax,0x28 / jne"),
    (ARM_PATH_RVA, bytes.fromhex("c705ad000000"), "arm path mov dword,1"),
    (SKIP_PATH_RVA, bytes.fromhex("e811000000"), "skip path call helper"),
    (LOG_RVA, bytes.fromhex("48894c2408488954"), "LOG prologue"),
    (0x000199B3, bytes.fromhex("b201"), "3BJ NOT included"),
)


def main():
    src, out = Path(sys.argv[1]), Path(sys.argv[2])
    blob = bytearray(src.read_bytes())
    sha = hashlib.sha256(blob).hexdigest()
    print("input", sha)
    if sha != EXPECTED_INPUT_SHA256:
        raise SystemExit("wrong Stage 3CE input DLL: " + sha)
    pe = parse_pe(blob)
    if pe["n"] != 12:
        raise SystemExit("unexpected PE geometry")
    for rva, expect, label in CONTEXT:
        o = rva_off(pe, rva)
        if bytes(blob[o:o+len(expect)]) != expect:
            raise SystemExit(f"{label}: got {bytes(blob[o:o+len(expect)]).hex()}")
    so = rva_off(pe, SPLICE_RVA)
    if bytes(blob[so:so+8]) != SPLICE_OLD:
        raise SystemExit("splice site unexpected: " + bytes(blob[so:so+8]).hex())
    if SPLICE_RVA + 8 + 0x2F != SKIP_PATH_RVA:
        raise SystemExit("jne target mismatch")
    po, pl = rva_off(pe, PAYLOAD_RVA), rva_off(pe, PAYLOAD_LIMIT-1)+1
    if any(blob[po:pl]):
        raise SystemExit("payload region not free")
    code, symbols = build_payload(
        Path(__file__).parent / "stage3cf_h4_id_probe.S", PAYLOAD_RVA,
        defs=dict(LOGFN=LOG_RVA, ARM_PATH=ARM_PATH_RVA,
                  SKIP_PATH=SKIP_PATH_RVA),
        want_symbols=("s3cf_id_probe",))
    if PAYLOAD_RVA + len(code) > PAYLOAD_LIMIT:
        raise SystemExit(f"payload too large: {len(code)}")
    thunk = symbols["s3cf_id_probe"]
    print(f"payload {len(code)} bytes at 0x{PAYLOAD_RVA:X}")
    blob[po:po+len(code)] = code
    blob[so:so+8] = b"\xE9" + struct.pack("<i", thunk - (SPLICE_RVA + 5)) + \
        b"\x90\x90\x90"
    print(f"splice 0x{SPLICE_RVA:X}: cmp/jne -> jmp 0x{thunk:X} + 3 nops")
    out.write_bytes(bytes(blob))
    print("output", hashlib.sha256(bytes(blob)).hexdigest(), len(blob))


if __name__ == "__main__":
    main()
