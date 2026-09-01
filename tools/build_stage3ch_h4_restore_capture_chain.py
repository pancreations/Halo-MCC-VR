"""Stage 3CH - restore the EXACT Stage 3BT capture chain onto the cutscene work.

Input: exact Stage 3CF.  Output: every byte of the Halo 4 reticle-capture
chain restored to Stage 3BT (the build whose crosshair the user accepted,
art 609), while all cutscene/theatre work (3BU write-back, 3BV/3BW/3BX
detector + publications + capability, 3CB theatre camera) is retained
untouched.

Reverted regions (the only capture-chain bytes that differ from 3BT):
  0x2F93E0..0x2F94D0  the dead 3BN thunk 3CE overwrote with live framing
  0x2F995D..0x2F9965  the 3BP selector id check (3CF probe splice)
  0x2F99DB..0x2F99DF  the 3BP selector un-hide source (3CD/3CE live value)
  0x2FB993..0x2FB995  the capture gate call target (3CE live framing thunk)
Every restored byte is copied from the Stage 3BT image itself, and the
result is proven byte-identical to 3BT across the whole capture chain.
"""
from pathlib import Path
import hashlib, struct, sys
sys.path.insert(0, str(Path(__file__).parent))
from build_stage3br_h4_capture_scale import parse_pe, rva_off

EXPECTED_INPUT_SHA256 = \
    "804252810430aed84dd3e9cb9812f232d49395a2445c2b07e7a9be0c193c2324"
STAGE3BT_SHA256 = \
    "4a1970734c5266688d2c61490fc485f74cbd3d39a3ffc568d0832791ada2279a"

REVERT = (
    (0x002F93E0, 0x002F94D0, "dead 3BN thunk (3CE live-framing payload)"),
    (0x002F995D, 0x002F9965, "3BP selector id check (3CF probe splice)"),
    (0x002F99DB, 0x002F99DF, "3BP selector un-hide source (3CD/3CE)"),
    (0x002FB993, 0x002FB995, "capture gate call target (3CE)"),
)

# Cutscene/theatre work that must survive untouched.
KEEP = (
    (0x0002C2BC, 6, "detector jne -> 3BX chain"),
    (0x00056EBF, 5, "3CB theatre-camera snapshot call"),
    (0x00068111, 5, "runtime capability mask (+CutsceneTheater)"),
    (0x001890D4, 4, "registry capability dword"),
    (0x002F9D10, 0x80, "3BU scene write-back"),
    (0x002F9E10, 0x80, "3BV probe"),
    (0x002FA36C, 0x94, "3CB theatre camera payload"),
    (0x002FAAA0, 0x200, "3BW publications"),
    (0x002FACB0, 0x300, "3BX look constraints"),
)


def main():
    src, out = Path(sys.argv[1]), Path(sys.argv[2])
    blob = bytearray(src.read_bytes())
    sha = hashlib.sha256(blob).hexdigest()
    print("input", sha)
    if sha != EXPECTED_INPUT_SHA256:
        raise SystemExit("wrong Stage 3CF input DLL: " + sha)
    bt = bytearray((src.parent / "Stage3BT-HaloMCCVR.dll").read_bytes())
    if hashlib.sha256(bt).hexdigest() != STAGE3BT_SHA256:
        raise SystemExit("Stage 3BT reference DLL is not the accepted build")
    pe = parse_pe(blob)
    if pe["n"] != 12 or len(bt) != len(blob):
        raise SystemExit("unexpected PE geometry")

    keep_before = {rva: bytes(blob[rva_off(pe, rva):rva_off(pe, rva)+n])
                   for rva, n, _ in KEEP}
    for lo, hi, label in REVERT:
        o, e = rva_off(pe, lo), rva_off(pe, hi)
        if bytes(blob[o:e]) == bytes(bt[o:e]):
            print(f"  already 3BT: {label}")
            continue
        blob[o:e] = bt[o:e]
        print(f"  restored 0x{lo:X}..0x{hi:X}  {label}")
    for rva, n, label in KEEP:
        o = rva_off(pe, rva)
        if bytes(blob[o:o+n]) != keep_before[rva]:
            raise SystemExit(f"cutscene work disturbed: {label}")
    out.write_bytes(bytes(blob))
    print("output", hashlib.sha256(bytes(blob)).hexdigest(), len(blob))


if __name__ == "__main__":
    main()
