"""Stage 3BD - give Halo 4's authored capture the 4x centre zoom it never had.

Derived from the official H4EK tags, not from another runtime guess.

`tools/export_h4_kit.ps1` exports `ui\hud\weapons\human\ar\assault_rifle`
(cui_screen). Its reticle subtree is
`reticule_offset_container > reticule_art_container > reticule_art_color`, and
the authored geometry of the eight bitmap leaves is:

    quarter_circle_top_left      left=-37 top=-37  32x32   ar_corner.bitm
    quarter_circle_top_right     left=+5  top=-37  32x32   ar_corner.bitm
    quarter_circle_bottom_left   left=-37 top=+5   32x32   ar_corner.bitm
    quarter_circle_bottom_right  left=+5  top=+5   32x32   ar_corner.bitm
    reticule_tick_top/bottom     +/-9 in y         2.2x8.2 white_pix.bitm
    reticule_tick_left/right     +/-9 in x         8.2x2.2 white_pix.bitm

So the reticle spans -37..+37 - about **74 authored units, centred on the
origin** of its container, on a virtual screen whose own widgets sit near
(516, 275), i.e. roughly 1030x550 units.

Now the shipped capture framing, read out of the binary at 0x11C77:

    cmp  al, 4                      ; TitleAdapter_GetActiveTitle() == Halo4
    jne  0x11C8D
    movdqa xmm1, [0x1BF0C0]         ; {512.0, 512.0, 0.0, 1.0}
    ...
    mulss  xmm1, xmm4               ; Width  *= authoredCaptureScale (1.0)
    mulss  xmm3, xmm4               ; Height *= authoredCaptureScale
    subss  xmm0, xmm1               ; 512 - Width
    mulss  xmm0, 0.5                ; TopLeftX = (512 - Width) / 2

For Halo 4 that yields viewport {0, 0, 512, 512}: offset zero, exactly the
size of the capture texture. A viewport maps the FULL NDC range onto its rect,
so this does not crop anything - it scales Halo 4's entire HUD down into the
512x512 reticle texture. That is precisely the reported symptom, and it is why
selecting a different CUI container could never change the captured content.

Halo 3 and ODST zoom instead: an oversized viewport with a matching negative
offset, so only the central fraction lands inside the texture. At 4x their
viewport is 2048 wide at offset -768.

Halo 4 needs the same treatment, and the H4EK measurement says 4x is the right
amount: a 74-unit reticle on a ~550-unit-tall screen maps to
`74 / 550 * 2048 ~ 275` pixels of the 512 texture - a little over half, which
is the same occupancy Halo 3 and ODST already ship. `src/dll/vr.cpp` even
documents this intent ("Halo 4 now uses that SAME proven 4x ratio"); the
shipped bytes simply do not implement it.

The constant at 0x1BF0C0 has TWO referents (0x11C7B here and 0x152C1F
elsewhere), so it must not be edited in place. This candidate instead writes a
private {2048, 2048, 0, 1} constant into the free tail of the .s3qd page and
retargets ONLY the Halo-4 branch's rip-relative displacement at 0x11C7B.
Elements 2 and 3 stay 0.0 and 1.0 because the same `movups` seeds the
viewport's MinDepth/MaxDepth.

Also restores the Stage 3AX capture edge at 0x53634 (`EB` -> `74`), which
Stage 3BB had forced to the normal pass; without it no capture runs at all.

Not included: Stage 3BC's payload-X container discriminator. It was shown to
be irrelevant to the captured content and is deliberately left out.
"""

from pathlib import Path
import hashlib
import struct
import sys

EXPECTED_STAGE3BB = \
    "10e39cf66862f4e88eba245fc22da750c0817c4684a1af114c466703722a8192"

# 16-byte aligned slot in the free tail of .s3qd (movdqa requires alignment).
CONST_RVA = 0x002FB800
CONST_LIMIT = 0x002FC000
NEW_VIEWPORT = struct.pack("<ffff", 2048.0, 2048.0, 0.0, 1.0)

# movdqa xmm1, [rip+disp32]  =  66 0F 6F 0D <disp32>, next instruction +8
H4_LOAD_RVA = 0x00011C77
H4_LOAD_DISP_RVA = 0x00011C7B
H4_LOAD_NEXT_RVA = 0x00011C7F
H4_LOAD_EXPECT = bytes.fromhex("660f6f0d41d41a00")
OLD_CONST_RVA = 0x001BF0C0

CAPTURE_EDGE_RVA = 0x00053634
CAPTURE_EDGE_EXPECT = bytes.fromhex("eb56")
CAPTURE_EDGE_PATCH = bytes.fromhex("7456")

CONTEXT = (
    (0x00011C73, bytes.fromhex("3c047516"),
     "Halo 4 title compare and branch guarding this load"),
    (0x00011C7F, bytes.fromhex("0f28d90fc6d955"),
     "movaps/shufps that broadcast element 1 as the viewport height"),
    (0x00011D21, bytes.fromhex("f30f59cc"),
     "mulss xmm1, authoredCaptureScale"),
    (0x00011D45, bytes.fromhex("f30f590587711700"),
     "mulss xmm0, 0.5 forming TopLeftX"),
    (0x00053632, bytes.fromhex("84c0"),
     "capture predicate test feeding the restored edge"),
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
            f"{label}: expected {expected.hex()} at 0x{rva:X}, "
            f"got {actual.hex()}")
    return o


def main():
    if len(sys.argv) != 3:
        raise SystemExit(
            "usage: build_stage3bd_h4_capture_zoom.py "
            "<Stage3BB-HaloMCCVR.dll> <output.dll>")
    src, out = Path(sys.argv[1]), Path(sys.argv[2])

    blob = bytearray(src.read_bytes())
    original_len = len(blob)
    sha = hashlib.sha256(blob).hexdigest()
    print("input", sha)
    if sha != EXPECTED_STAGE3BB:
        raise SystemExit("wrong Stage3BB input DLL: " + sha)

    pe = parse_pe(blob)
    if pe["n"] != 12:
        raise SystemExit(f"unexpected PE geometry: n={pe['n']}")

    for rva, expect, label in CONTEXT:
        guard(blob, pe, rva, expect, label)
    guard(blob, pe, H4_LOAD_RVA, H4_LOAD_EXPECT, "Halo 4 viewport load")
    guard(blob, pe, CAPTURE_EDGE_RVA, CAPTURE_EDGE_EXPECT, "capture edge")

    # The displaced load must currently resolve to the shared constant.
    disp = struct.unpack_from("<i", blob, rva_off(pe, H4_LOAD_DISP_RVA))[0]
    if H4_LOAD_NEXT_RVA + disp != OLD_CONST_RVA:
        raise SystemExit("Halo 4 load does not resolve to the known constant")
    old = bytes(blob[rva_off(pe, OLD_CONST_RVA):
                     rva_off(pe, OLD_CONST_RVA) + 16])
    if struct.unpack("<ffff", old) != (512.0, 512.0, 0.0, 1.0):
        raise SystemExit(f"unexpected shared constant: {old.hex()}")

    if CONST_RVA % 16:
        raise SystemExit("movdqa target must be 16-byte aligned")
    if CONST_RVA + 16 > CONST_LIMIT:
        raise SystemExit("constant does not fit the page tail")
    o = rva_off(pe, CONST_RVA)
    if any(blob[o:rva_off(pe, CONST_LIMIT - 1) + 1]):
        raise SystemExit("Stage3BD constant region is not free")
    blob[o:o + 16] = NEW_VIEWPORT
    print(f"new viewport constant at 0x{CONST_RVA:X}: "
          f"{struct.unpack('<ffff', NEW_VIEWPORT)}")

    # Retarget only the Halo 4 branch's displacement; the shared constant and
    # every other title's framing are untouched.
    struct.pack_into("<i", blob, rva_off(pe, H4_LOAD_DISP_RVA),
                     CONST_RVA - H4_LOAD_NEXT_RVA)
    print(f"H4 viewport load: 0x{H4_LOAD_RVA:X} "
          f"0x{OLD_CONST_RVA:X} -> 0x{CONST_RVA:X}")

    # Restore the Stage 3AX capture edge so a capture actually runs.
    o = rva_off(pe, CAPTURE_EDGE_RVA)
    blob[o:o + len(CAPTURE_EDGE_PATCH)] = CAPTURE_EDGE_PATCH
    print(f"capture edge: 0x{CAPTURE_EDGE_RVA:X} "
          f"{CAPTURE_EDGE_EXPECT.hex()} -> {CAPTURE_EDGE_PATCH.hex()}")

    if struct.unpack_from("<I", blob, pe["opt"] + 0x40)[0] != 0:
        raise SystemExit("unexpected non-zero PE checksum")
    if len(blob) != original_len:
        raise SystemExit("output size changed unexpectedly")

    out.write_bytes(bytes(blob))
    print("output", hashlib.sha256(bytes(blob)).hexdigest(), len(blob))


if __name__ == "__main__":
    main()
