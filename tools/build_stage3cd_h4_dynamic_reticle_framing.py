"""Replace Halo 4's stale fixed reticle hide cancellation with its live value."""
from pathlib import Path
import hashlib
import struct
import sys

sys.path.insert(0, str(Path(__file__).parent))
from build_stage3bp_h4_capture_weapon_only import parse_pe, rva_off

EXPECTED_INPUT_SHA256 = (
    "4c60a1f71a07255c11bf542d87b9ce2db0bc0a81410bd39afd2e73e2bfbaf0d9")

# Stage 3BP's selector used 3BH's one-session 4*halfWidth constant here.
LOAD_RVA = 0x002F99D7
LOAD_OLD = bytes.fromhex("f30f100d211e0000")
FIXED_FRAMING_RVA = 0x002FB800

# The C-H4-48 report loads six contiguous atomics in reverse argument order:
# baseX/baseY/aimX/aimY/stockScale/writtenScale. aimX is the exact live
# offscreen hide displacement written by the native-reticle suppression path.
LIVE_HIDE_X_RVA = 0x002A8368
LIVE_HIDE_X_REFERENCE_RVA = 0x000515C3
LIVE_HIDE_X_REFERENCE = bytes.fromhex("8b0d9f6d2500")
LIVE_HIDE_X_STORE_RVA = 0x00053E04
LIVE_HIDE_X_STORE = bytes.fromhex("f30f11355c452500")


def rel32(target, next_rva):
    value = target - next_rva
    if not -0x80000000 <= value <= 0x7fffffff:
        raise SystemExit("RIP displacement out of range")
    return struct.pack("<i", value)


def main():
    source, output = Path(sys.argv[1]), Path(sys.argv[2])
    blob = bytearray(source.read_bytes())
    digest = hashlib.sha256(blob).hexdigest()
    print("input", digest)
    if digest != EXPECTED_INPUT_SHA256:
        raise SystemExit("wrong Stage 3CC input DLL: " + digest)
    pe = parse_pe(blob)
    load_off = rva_off(pe, LOAD_RVA)
    if bytes(blob[load_off:load_off + len(LOAD_OLD)]) != LOAD_OLD:
        raise SystemExit("Stage 3BP framing load changed")
    if LOAD_RVA + 8 + struct.unpack_from("<i", LOAD_OLD, 4)[0] != \
            FIXED_FRAMING_RVA:
        raise SystemExit("old load does not target the fixed framing value")
    reference_off = rva_off(pe, LIVE_HIDE_X_REFERENCE_RVA)
    if bytes(blob[reference_off:reference_off + 6]) != LIVE_HIDE_X_REFERENCE:
        raise SystemExit("live hide-X reference changed")
    if LIVE_HIDE_X_REFERENCE_RVA + 6 + struct.unpack_from(
            "<i", LIVE_HIDE_X_REFERENCE, 2)[0] != LIVE_HIDE_X_RVA:
        raise SystemExit("live hide-X reference targets another field")
    store_off = rva_off(pe, LIVE_HIDE_X_STORE_RVA)
    if bytes(blob[store_off:store_off + 8]) != LIVE_HIDE_X_STORE:
        raise SystemExit("live hide-X publisher changed")
    if LIVE_HIDE_X_STORE_RVA + 8 + struct.unpack_from(
            "<i", LIVE_HIDE_X_STORE, 4)[0] != LIVE_HIDE_X_RVA:
        raise SystemExit("live hide-X publisher targets another field")

    blob[load_off + 4:load_off + 8] = rel32(
        LIVE_HIDE_X_RVA, LOAD_RVA + 8)
    output.write_bytes(blob)
    print(f"reticle selector {LOAD_RVA:#x}: fixed {FIXED_FRAMING_RVA:#x} -> "
          f"live hide X {LIVE_HIDE_X_RVA:#x}")
    print("output", hashlib.sha256(blob).hexdigest(), len(blob))


if __name__ == "__main__":
    main()
