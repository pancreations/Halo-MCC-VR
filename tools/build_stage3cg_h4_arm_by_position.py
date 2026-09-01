"""Stage 3CG - position-based capture arming on Stage 3CF (log probe becomes
dead; see stage3cg_h4_arm_by_position.S)."""
from pathlib import Path
import hashlib, struct, sys
sys.path.insert(0, str(Path(__file__).parent))
from postlink import build_payload
from build_stage3br_h4_capture_scale import parse_pe, rva_off

EXPECTED_INPUT_SHA256 = \
    "804252810430aed84dd3e9cb9812f232d49395a2445c2b07e7a9be0c193c2324"
PAYLOAD_RVA = 0x002F9C70
PAYLOAD_LIMIT = 0x002F9CD0
SPLICE_RVA = 0x002F995D            # 3CF: jmp 0x2F9F90 + 3 nops
S3CF_PROBE_RVA = 0x002F9F90
ARM_PATH_RVA = 0x002F9965
SKIP_PATH_RVA = 0x002F9994
LIVE_HIDE_X_RVA = 0x002A8368
KHALF_RVA = 0x002F9BD4

CONTEXT = (
    (0x002F9958, bytes.fromhex("83f828751e"), "selector cmp eax,0x28 / jne"),
    (ARM_PATH_RVA, bytes.fromhex("c705ad000000"), "arm path"),
    (SKIP_PATH_RVA, bytes.fromhex("e811000000"), "skip path call helper"),
    (KHALF_RVA, bytes.fromhex("0000003f"), "3BR 0.5f constant"),
    (0x000515C3, bytes.fromhex("8b0d9f6d2500"), "live hide reporter read"),
    # the shipped rescue helper uses the same container addressing
    (0x002F99AF, bytes.fromhex("8b8e70080000"), "helper reads [rsi+0x870]"),
    (0x002F99C1, bytes.fromhex("486bc034"), "helper imul rax,rax,0x34"),
    (0x000199B3, bytes.fromhex("b201"), "3BJ NOT included"),
)


def main():
    src, out = Path(sys.argv[1]), Path(sys.argv[2])
    blob = bytearray(src.read_bytes())
    sha = hashlib.sha256(blob).hexdigest()
    print("input", sha)
    if sha != EXPECTED_INPUT_SHA256:
        raise SystemExit("wrong Stage 3CF input DLL: " + sha)
    pe = parse_pe(blob)
    if pe["n"] != 12:
        raise SystemExit("unexpected PE geometry")
    for rva, expect, label in CONTEXT:
        o = rva_off(pe, rva)
        if bytes(blob[o:o+len(expect)]) != expect:
            raise SystemExit(f"{label}: got {bytes(blob[o:o+len(expect)]).hex()}")
    so = rva_off(pe, SPLICE_RVA)
    if blob[so] != 0xE9 or bytes(blob[so+5:so+8]) != b"\x90\x90\x90":
        raise SystemExit("splice site unexpected: " + bytes(blob[so:so+8]).hex())
    if SPLICE_RVA + 5 + struct.unpack_from("<i", blob, so+1)[0] \
            != S3CF_PROBE_RVA:
        raise SystemExit("splice does not target the 3CF probe")
    po, pl = rva_off(pe, PAYLOAD_RVA), rva_off(pe, PAYLOAD_LIMIT-1)+1
    if any(blob[po:pl]):
        raise SystemExit("payload region not free")
    code, symbols = build_payload(
        Path(__file__).parent / "stage3cg_h4_arm_by_position.S", PAYLOAD_RVA,
        defs=dict(LIVE_HIDE_X=LIVE_HIDE_X_RVA, KHALF=KHALF_RVA,
                  ARM_PATH=ARM_PATH_RVA, SKIP_PATH=SKIP_PATH_RVA),
        want_symbols=("s3cg_arm_by_position",))
    if PAYLOAD_RVA + len(code) > PAYLOAD_LIMIT:
        raise SystemExit(f"payload too large: {len(code)}")
    thunk = symbols["s3cg_arm_by_position"]
    print(f"payload {len(code)} bytes at 0x{PAYLOAD_RVA:X}")
    blob[po:po+len(code)] = code
    struct.pack_into("<i", blob, so+1, thunk - (SPLICE_RVA + 5))
    print(f"splice 0x{SPLICE_RVA:X}: probe 0x{S3CF_PROBE_RVA:X} -> "
          f"position arm 0x{thunk:X} (probe now dead)")
    out.write_bytes(bytes(blob))
    print("output", hashlib.sha256(bytes(blob)).hexdigest(), len(blob))


if __name__ == "__main__":
    main()
