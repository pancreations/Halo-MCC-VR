"""Stage 3CO - reconcile the proven Halo 4 reticle with the 3CB theatre.

Input is the exact Stage 3CB theatre DLL.  The complete Stage 3BT/3BU
authored-reticle chain stays intact.  Two layout inputs that 3BT froze to the
accepted -1033.578/336.549 session instead follow the transform published by
the current type-0x28 container:

* the 3BP selector subtracts the live hide displacement; and
* the private capture viewport derives its width and vertical bias from the
  live hide/baseY pair before entering the unchanged 3BR 2.5x scale tail.

Invalid live values fall back to the complete accepted 3BR viewport thunk.
The unproven Stage 3CN removal of the 0x0c payload-size discriminator is not
included.
"""
from pathlib import Path
import hashlib
import struct
import sys

sys.path.insert(0, str(Path(__file__).parent))
from postlink import build_payload
from build_stage3br_h4_capture_scale import parse_pe, rva_off

EXPECTED_INPUT_SHA256 = (
    "9ee60ca1b97934002473a0f970c4af8e94a79aa34cfc92116f1c06b4f5690885")
EXPECTED_OUTPUT_SHA256 = (
    "7ebfc2e137e81d34d913489a776cc244bade74cd9f019bec2d05a94c613e0ef9")
ARCHIVED_PAYLOAD_SOURCE_SHA256 = (
    "4e23974c08697779153b98782f7ee5d4b79cf5ef7ffc922ad71213ee29b9df1b")
ARCHIVED_PAYLOAD_SIZE = 228

PAYLOAD_RVA, PAYLOAD_LIMIT = 0x002F93E0, 0x002F94D0
DEAD_3BN_SHA256 = (
    "e4e61358967df60898ca79c2795be6a53db55028f32913368a428b7d8ba2a868")
GATE_CALL_RVA = 0x002FB992
GATE_OLD = bytes.fromhex("e8f9e0ffff")  # call accepted 3BR thunk
S3BR_THUNK_RVA, S3BR_SCALE_TAIL_RVA = 0x002F9A90, 0x002F9B29

SELECTOR_LOAD_RVA = 0x002F99D7
SELECTOR_LOAD_OLD_TARGET_RVA = 0x002FB800
CAPTURE_VIEWPORT_RVA = 0x002AE774
BASE_Y_RVA, LIVE_HIDE_X_RVA = 0x002A8364, 0x002A8368
BBW_RVA, BBH_RVA = 0x002AEB58, 0x002AEB5C
K512_RVA, KHALF_RVA = 0x002F9BD0, 0x002F9BD4
VPBUF_RVA = 0x002F9BE8

CONTEXT = (
    (0x002F995D, bytes.fromhex("66837c24420c752f"),
     "proven type-0x28 payload-size discriminator"),
    (0x002F9A90, bytes.fromhex("4883ec28"), "accepted 3BR thunk"),
    (0x002F9B29, bytes.fromhex("f30f101daf000000"), "3BR scale tail"),
    (0x002F9BBD, bytes.fromhex("4883c428c3"), "3BR return"),
    (0x002F9BD0, bytes.fromhex("000000440000003fb9379e3d"),
     "3BR constants"),
    (0x00053E04, bytes.fromhex("f30f11355c452500"),
     "live hide publisher"),
    (0x000199B3, bytes.fromhex("b201"), "3BJ absent"),
)


def rel32(target, next_rva):
    return struct.pack("<i", target - next_rva)


def archived_fixed_payload(source_dir):
    """Return the independently verified encoding when clang is unavailable.

    The reference is hash-pinned and only the 228-byte payload authored by the
    checked-in assembly is read.  The output's whole-image hash is checked
    again after every splice, so this cannot silently import another stage.
    """
    reference_path = source_dir / "Stage3CL-HaloMCCVR.dll"
    reference = bytearray(reference_path.read_bytes())
    if hashlib.sha256(reference).hexdigest() != ARCHIVED_PAYLOAD_SOURCE_SHA256:
        raise SystemExit("archived fixed-encoding reference changed")
    reference_pe = parse_pe(reference)
    start = rva_off(reference_pe, PAYLOAD_RVA)
    code = bytes(reference[start:start + ARCHIVED_PAYLOAD_SIZE])
    if len(code) != ARCHIVED_PAYLOAD_SIZE or code[:4] != bytes.fromhex(
            "4883ec28"):
        raise SystemExit("archived fixed-encoding payload is malformed")
    return code, {"s3cl_layout_viewport": PAYLOAD_RVA}


def main():
    source, output = Path(sys.argv[1]), Path(sys.argv[2])
    blob = bytearray(source.read_bytes())
    digest = hashlib.sha256(blob).hexdigest()
    print("input", digest)
    if digest != EXPECTED_INPUT_SHA256:
        raise SystemExit("wrong Stage 3CB input DLL: " + digest)
    pe = parse_pe(blob)
    if pe["n"] != 12:
        raise SystemExit("unexpected PE geometry")

    for rva, expected, label in CONTEXT:
        at = rva_off(pe, rva)
        if bytes(blob[at:at + len(expected)]) != expected:
            raise SystemExit(f"{label} changed")

    selector = rva_off(pe, SELECTOR_LOAD_RVA)
    if bytes(blob[selector:selector + 4]) != bytes.fromhex("f30f100d"):
        raise SystemExit("selector un-hide load opcode changed")
    old_target = SELECTOR_LOAD_RVA + 8 + struct.unpack_from(
        "<i", blob, selector + 4)[0]
    if old_target != SELECTOR_LOAD_OLD_TARGET_RVA:
        raise SystemExit(f"selector load targets {old_target:#x}")

    gate = rva_off(pe, GATE_CALL_RVA)
    if bytes(blob[gate:gate + 5]) != GATE_OLD:
        raise SystemExit("capture gate changed")
    payload, payload_end = rva_off(pe, PAYLOAD_RVA), rva_off(
        pe, PAYLOAD_LIMIT - 1) + 1
    if hashlib.sha256(bytes(blob[payload:payload_end])).hexdigest() != \
            DEAD_3BN_SHA256:
        raise SystemExit("payload region is not the exact dead 3BN thunk")

    blob[payload:payload_end] = bytes(payload_end - payload)
    try:
        code, symbols = build_payload(
            Path(__file__).parent / "stage3cl_h4_layout_relative_bias.S",
            PAYLOAD_RVA,
            defs=dict(CAPTURE_VIEWPORT=CAPTURE_VIEWPORT_RVA,
                      BASE_Y=BASE_Y_RVA, LIVE_HIDE_X=LIVE_HIDE_X_RVA,
                      BBW=BBW_RVA, BBH=BBH_RVA, K512=K512_RVA,
                      KHALF=KHALF_RVA, VPBUF=VPBUF_RVA,
                      S3BR_SCALE_TAIL=S3BR_SCALE_TAIL_RVA,
                      S3BR_THUNK=S3BR_THUNK_RVA),
            want_symbols=("s3cl_layout_viewport",))
    except OSError:
        code, symbols = archived_fixed_payload(source.parent)
        print("assembler temporary directory unavailable; used hash-pinned "
              "fixed encoding")
    if PAYLOAD_RVA + len(code) > PAYLOAD_LIMIT:
        raise SystemExit(f"payload too large: {len(code)} bytes")
    thunk = symbols["s3cl_layout_viewport"]
    blob[payload:payload + len(code)] = code
    blob[gate:gate + 5] = b"\xE8" + rel32(thunk, GATE_CALL_RVA + 5)
    blob[selector + 4:selector + 8] = rel32(
        LIVE_HIDE_X_RVA, SELECTOR_LOAD_RVA + 8)

    result = hashlib.sha256(blob).hexdigest()
    if result != EXPECTED_OUTPUT_SHA256:
        raise SystemExit("unexpected output DLL: " + result)
    output.write_bytes(blob)
    print(f"payload {len(code)} bytes at {PAYLOAD_RVA:#x}; gate -> {thunk:#x}")
    print("output", result, len(blob))


if __name__ == "__main__":
    main()
