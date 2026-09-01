"""Stage 3BT - the 3BM capture dumps roll for the whole session (wrap 4->1).
Input: exact Stage 3BS DLL.  One 9-byte change at 0x2F9066 inside the 3BM
payload (`cmp eax,4` + `ja 0x2F92C5`) -> call s3bt_wrap + 4 nops."""
from pathlib import Path
import hashlib, struct, sys
sys.path.insert(0, str(Path(__file__).parent))
from postlink import build_payload
from build_stage3br_h4_capture_scale import parse_pe, rva_off

PAYLOAD_RVA = 0x002F9CD0
PAYLOAD_LIMIT = 0x002F9D10
SPLICE_RVA = 0x002F9066
SPLICE_OLD = bytes.fromhex("83f804" "0f8756020000")
DUMP_COUNTER_RVA = 0x002F9040

CONTEXT = (
    (0x002F905A, bytes.fromhex("ff05e0ffffff"), "3BM tick increments the counter"),
    (0x002F906F, bytes.fromhex("c705a3270000feffffff"), "windows=-2 store follows"),
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
    if bytes(blob[so:so+9]) != SPLICE_OLD:
        raise SystemExit("splice site unexpected: " + bytes(blob[so:so+9]).hex())
    po, pl = rva_off(pe, PAYLOAD_RVA), rva_off(pe, PAYLOAD_LIMIT-1)+1
    if any(blob[po:pl]):
        raise SystemExit("payload region not free")
    code, symbols = build_payload(
        Path(__file__).parent / "stage3bt_h4_rolling_dumps.S", PAYLOAD_RVA,
        defs=dict(DUMP_COUNTER=DUMP_COUNTER_RVA),
        want_symbols=("s3bt_wrap",))
    if PAYLOAD_RVA + len(code) > PAYLOAD_LIMIT:
        raise SystemExit(f"payload too large: {len(code)}")
    thunk = symbols["s3bt_wrap"]
    print(f"payload {len(code)} bytes at 0x{PAYLOAD_RVA:X}")
    blob[po:po+len(code)] = code
    blob[so:so+9] = b"\xE8" + struct.pack("<i", thunk - (SPLICE_RVA + 5)) + b"\x90" * 4
    print(f"splice 0x{SPLICE_RVA:X}: -> call 0x{thunk:X} + 4 nops")
    out.write_bytes(bytes(blob))
    print("output", hashlib.sha256(bytes(blob)).hexdigest(), len(blob))


if __name__ == "__main__":
    main()
