"""Stage 3AT - restore the accepted CREDIT + ODST-unpin bytes onto Stage 3AS.

On 2026-08-27 evening the accepted lineage advanced past plain Stage 3AL:

  Stage3AL-CREDIT     56 bytes  .s3ic 0x2F2189..0x2F21C1  F1 welcome line ->
                                "Maintained by pancreations and @MeWhenINameMyself."
  Stage3AN-ODST-UNPIN  2 bytes  .s3qd 0x2F8016 (mov ecx,4 -> 6: GetModuleHandleEx
                                gains UNCHANGED_REFCOUNT - the teardown pin no
                                longer holds halo3odst.dll) and 0x2F806B
                                (je -> jmp: the matching release is never made)
  Stage3AO-ODST-FIX    1 byte   .s3ic 0x2F1B0C (je -> jmp: skip one saved-pointer
                                release in the pin helper)

The Halo 4 chain (3AP..3AS) was mistakenly rebuilt from plain Stage 3AL,
dropping all three. The 2026-08-28 08:25 ODST session showed the exact
regression: runtime mode never left Loading and stereo never armed. The
separate Stage3AM title-thrash patch was a DISPROVEN hypothesis and is
deliberately NOT applied.

This builder takes the exact Stage 3AS DLL, extracts the accepted byte
differences directly from built/Stage3AO-ODST-FIX.dll vs
built/Stage3AL-HaloMCCVR.dll (it never re-encodes them by hand), asserts that
Stage 3AS still carries the original 3AL bytes at every one of those offsets,
and applies them. Output: Stage 3AT.
"""

from pathlib import Path
import hashlib
import sys

EXPECTED_STAGE3AS_SHA256 = \
    "311036c85a491fb9b65f3fe4ad17e1b331d03bb2bf7450e641c3142d5ffd1058"
EXPECTED_STAGE3AL_SHA256 = \
    "fb1e6b5d7a584930303b2f6aed6696c4012e24f007538d7c7955ad75ca583da2"
EXPECTED_STAGE3AO_SHA256 = \
    "37487a5bc7b08da1a5543aa81fac2ebed9428bf3fc30527fa08554f34cf5c28d"

# The three accepted change regions, as file-offset ranges. Section layout
# below .s3qd's extension is identical between 3AL and 3AS, so file offsets
# transfer directly; every byte is still guarded against 3AL's value first.
EXPECTED_RANGES = (
    (0x2BFF0C, 0x2BFF0D, "3AO pin-helper release skip"),
    (0x2C0589, 0x2C05C2, "CREDIT welcome message"),
    (0x2C5C16, 0x2C5C17, "3AN GetModuleHandleEx flags"),
    (0x2C5C6B, 0x2C5C6C, "3AN release skip"),
)


def main():
    if len(sys.argv) != 3:
        raise SystemExit(
            "usage: build_stage3at_restore_credit_odst.py "
            "<Stage3AS-HaloMCCVR.dll> <output.dll>")
    src, out = Path(sys.argv[1]), Path(sys.argv[2])
    here = Path(__file__).parent.parent / "built"

    base3al = (here / "Stage3AL-HaloMCCVR.dll").read_bytes()
    if hashlib.sha256(base3al).hexdigest() != EXPECTED_STAGE3AL_SHA256:
        raise SystemExit("Stage3AL reference DLL is not the expected one")
    acc3ao = (here / "Stage3AO-ODST-FIX.dll").read_bytes()
    sha3ao = hashlib.sha256(acc3ao).hexdigest()
    print("Stage3AO reference", sha3ao)
    if sha3ao != EXPECTED_STAGE3AO_SHA256:
        raise SystemExit("Stage3AO reference DLL is not the expected one")
    if len(acc3ao) != len(base3al):
        raise SystemExit("reference size mismatch")

    diffs = [i for i in range(len(base3al)) if base3al[i] != acc3ao[i]]
    print(f"accepted diff: {len(diffs)} bytes")
    covered = set()
    for lo, hi, _ in EXPECTED_RANGES:
        covered.update(range(lo, hi))
    if not set(diffs) <= covered:
        raise SystemExit("unexpected extra accepted-diff offsets: " +
                         ", ".join(hex(d) for d in sorted(set(diffs) - covered)))
    if not diffs:
        raise SystemExit("no diff between references")

    blob = bytearray(src.read_bytes())
    sha = hashlib.sha256(blob).hexdigest()
    print("input", sha)
    if sha != EXPECTED_STAGE3AS_SHA256:
        raise SystemExit("wrong Stage3AS input DLL: " + sha)

    for off in diffs:
        if blob[off] != base3al[off]:
            raise SystemExit(
                f"Stage3AS byte at 0x{off:X} is 0x{blob[off]:02X}, expected "
                f"3AL's 0x{base3al[off]:02X}; refusing to overwrite")
        blob[off] = acc3ao[off]
    for lo, hi, label in EXPECTED_RANGES:
        changed = sum(1 for i in range(lo, hi) if i in set(diffs))
        print(f"{label}: {changed} byte(s) applied at 0x{lo:X}")

    out.write_bytes(bytes(blob))
    print("output", hashlib.sha256(bytes(blob)).hexdigest(), len(blob))


if __name__ == "__main__":
    main()
