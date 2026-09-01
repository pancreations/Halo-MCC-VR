"""Stage 3AP - enable Halo 4's authored CUI reticle capture.

Stage 3AL already ships the whole authored-reticle path for Halo 4: the gameplay
CUI dispatcher replays the command stream into the private centred capture
target, the visible pass moves the native flat copy offscreen, and the shared
OpenXR reticle quad presents whatever art the capture holds. Halo 3, ODST and
Reach use the same path.

C-H4-50 disabled the Halo 4 half of it with one early return in
VR_ShouldCaptureAuthoredReticleThisFrame:

    if (TitleAdapter_GetActiveTitle() == GameTitle::Halo4)
        return false;

so Halo 4 hid its native reticle and then had nothing to present, leaving the
procedural bullet-ray marker on the VR quad. That is the reported symptom: the
game's crosshair disappears and the VR crosshair never carries the real art.

This stage removes exactly that early return. Halo 4 then falls into the same
cadence logic every other title uses, whose switch already carries a Halo 4 case
driving g_config.crosshair_animation_frames. No other title's path is touched:
every instruction below the patch is shared and unchanged, and the compare that
selects a title is the one being removed.

The Stage 3X title-query wrapper called immediately above the patch (the Halo 4
effect/HUD heartbeat living in .s3qd) is deliberately left in place; it has
already run by the time the removed branch would have been taken.
"""

from pathlib import Path
import hashlib
import struct
import sys

EXPECTED_STAGE3AL_SHA256 = \
    "fb1e6b5d7a584930303b2f6aed6696c4012e24f007538d7c7955ad75ca583da2"

# VR_ShouldCaptureAuthoredReticleThisFrame
#   0x030890  sub  rsp, 0x28
#   0x030894  call 0x2F438D            ; Stage 3X title-query wrapper
#   0x030899  cmp  al, 4               ; GameTitle::Halo4
#   0x03089B  je   0x030943            ; -> "return false"     <- removed here
#   0x0308A1  cmp  byte [g_reticleContainsAuthored], 0
C_H4_50_EARLY_RETURN_RVA = 0x0003089B
C_H4_50_EARLY_RETURN_EXPECT = bytes.fromhex("0f84a2000000")
C_H4_50_EARLY_RETURN_PATCH = b"\x90" * len(C_H4_50_EARLY_RETURN_EXPECT)

# Guards proving the patch sits in the function this stage means to change.
CONTEXT = (
    (0x00030890, bytes.fromhex("4883ec28"), "function prologue"),
    (0x00030899, bytes.fromhex("3c04"), "cmp al, GameTitle::Halo4"),
    (0x000308A1, bytes.fromhex("803d97db270000"), "g_reticleContainsAuthored test"),
    (0x00030943, bytes.fromhex("32c04883c428c3"), "return-false epilogue"),
)


def parse_pe(blob):
    if blob[:2] != b"MZ":
        raise SystemExit("input is not MZ")
    p = struct.unpack_from("<I", blob, 0x3C)[0]
    if blob[p:p + 4] != b"PE\0\0":
        raise SystemExit("input is not PE")
    coff = p + 4
    n = struct.unpack_from("<H", blob, coff + 2)[0]
    osz = struct.unpack_from("<H", blob, coff + 16)[0]
    opt = coff + 20
    if struct.unpack_from("<H", blob, opt)[0] != 0x20B:
        raise SystemExit("not PE32+")
    st = opt + osz
    secs = []
    for i in range(n):
        o = st + i * 40
        name = bytes(blob[o:o + 8]).split(b"\0", 1)[0].decode("ascii")
        vs, va, rs, rp = struct.unpack_from("<IIII", blob, o + 8)
        secs.append(dict(name=name, vs=vs, va=va, rs=rs, rp=rp))
    return dict(opt=opt, n=n, sections=secs)


def rva_off(pe, rva):
    for s in pe["sections"]:
        if s["va"] <= rva < s["va"] + max(s["vs"], s["rs"]):
            return s["rp"] + rva - s["va"]
    raise KeyError(hex(rva))


def guard(blob, pe, rva, expected, label):
    o = rva_off(pe, rva)
    actual = bytes(blob[o:o + len(expected)])
    if actual != expected:
        raise SystemExit(
            f"{label}: expected {expected.hex()} at 0x{rva:X}, got {actual.hex()}")
    return o


def main():
    if len(sys.argv) != 3:
        raise SystemExit(
            "usage: build_stage3ap_h4_authored_reticle.py "
            "<Stage3AL-HaloMCCVR.dll> <output.dll>")
    src, out = Path(sys.argv[1]), Path(sys.argv[2])
    blob = bytearray(src.read_bytes())
    sha = hashlib.sha256(blob).hexdigest()
    print("input", sha)
    if sha != EXPECTED_STAGE3AL_SHA256:
        raise SystemExit("wrong Stage3AL input DLL: " + sha)

    pe = parse_pe(blob)
    if pe["n"] != 12:
        raise SystemExit(f"unexpected Stage3AL PE geometry: n={pe['n']}")
    for rva, expect, label in CONTEXT:
        guard(blob, pe, rva, expect, label)

    o = guard(blob, pe, C_H4_50_EARLY_RETURN_RVA,
              C_H4_50_EARLY_RETURN_EXPECT, "C-H4-50 Halo 4 early return")
    blob[o:o + len(C_H4_50_EARLY_RETURN_PATCH)] = C_H4_50_EARLY_RETURN_PATCH
    print(f"C-H4-50 early return: 0x{C_H4_50_EARLY_RETURN_RVA:X} "
          f"{C_H4_50_EARLY_RETURN_EXPECT.hex()} -> "
          f"{C_H4_50_EARLY_RETURN_PATCH.hex()}")

    # The whole accepted lineage ships an unset PE checksum. Introducing one
    # here would be a second, unrelated difference from Stage 3AL, so the field
    # is left exactly as the input carried it and the diff stays at six bytes.
    if struct.unpack_from("<I", blob, pe["opt"] + 0x40)[0] != 0:
        raise SystemExit("unexpected non-zero Stage3AL PE checksum")

    if len(blob) != len(src.read_bytes()):
        raise SystemExit("Stage 3AP must not change the file size")
    out.write_bytes(bytes(blob))
    print("output", hashlib.sha256(bytes(blob)).hexdigest(), len(blob))


if __name__ == "__main__":
    main()
