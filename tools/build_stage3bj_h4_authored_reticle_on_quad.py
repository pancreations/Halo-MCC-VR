"""Stage 3BJ - let Halo 4's captured authored reticle actually reach the quad.

Input is the exact Stage 3BI DLL.  ONE two-byte change, inside the Halo-4-only
branch of EnsureReticleChain (0x19920).

The C-H4-50 policy in src/common/halo4_cui_reticle_logic.h says:

    // Until the authored H4 capture has a narrower, independently proven
    // widget boundary, keep that procedural art as the only H4 quad content.
    constexpr bool Halo4CuiReticleUsesProceduralFallback(
        bool authoredCaptureLive, bool crosshairEnabled,
        bool killNativeReticle) noexcept
    { return authoredCaptureLive && crosshairEnabled && killNativeReticle; }

which is unconditionally TRUE for Halo 4 whenever the capture is live.  That
is why every stage since 3BB has looped back to the placeholder crosshair:
however good the capture got, this gate refused to show it.

Stage 3BI supplied the narrower boundary the policy was waiting for.  The
20:31 log proves it: the one-shot capture map lost every full-width HUD band
(rows 1-14 now blank; only one small element remains), and `art` fell from
~1700-2500 to 407-457 while `blankHeld` reached 0 and uploads succeeded.

Compiled shape at 0x19987 (EnsureReticleChain):

    019987  call  0x879C0            ; TitleAdapter_GetActiveTitle()
    01998C  movzx ebx, al            ; reticleTitle
    01998F  call  0x48070            ; Game_TitleCapturesAuthoredCrosshair()
    019994  movzx r8d, al            ; titleHasAuthoredCapture
    019998  cmp   bl, 4              ; == GameTitle::Halo4 ?
    01999B  jne   0x199B7            ;   no  -> dl = 0
    01999D  test  al, al             ; titleHasAuthoredCapture
    01999F  je    0x199B7
    0199A1  cmp   byte [0x2ADBB1], 0 ; g_config.crosshair
    0199A8  je    0x199B7
    0199AA  cmp   byte [0x2ADC04], 0 ; g_config.kill_reticle
    0199B1  je    0x199B7
    0199B3  mov   dl, 1              ; halo4ProceduralBootstrap = TRUE  <-- HERE
    0199B5  jmp   0x199B9
    0199B7  xor   dl, dl             ; = false (every other title lands here)

`dl` then drives exactly the two things the user is seeing:

    0199F0  test dl, dl / je 0x19B3F   ; authoredThisFrame && !bootstrap
                                       ;   -> return true, authored art kept
    019A72  test dl, dl / jne 0x19A33  ; kProceduralOpacity
                                       ;   -> 1.0f OPAQUE procedural painted
                                       ;      over the authored swapchain

Patch: 0x199B3  `mov dl,1` (B2 01) -> `xor dl,dl` (30 D2).

Both encodings are 2 bytes, so nothing moves.  `xor` writes flags where `mov`
did not; the only consumer is `jmp 0x199B9` -> `cmp byte [0x2AE43E],0`, which
sets its own flags first, so the change is flag-safe.

Scope: the site is dominated by `cmp bl,4`, so ONLY Halo 4 reaches it.  Halo 3,
ODST, Reach and Halo 2 already fall through to `xor dl,dl` at 0x199B7 and are
bit-for-bit unaffected.

Result for Halo 4 - the same two behaviours the other three titles ship:
  * authored art in the swapchain is returned early and never repainted, and
  * the procedural fallback becomes fully transparent (0.0f) instead of
    opaque, so it can no longer cover the authored reticle.
"""

from pathlib import Path
import hashlib
import struct
import sys

EXPECTED_STAGE3BI_SHA256 = \
    "dad8a373ed30f3e31f42350fa85b575e2f7b146b8ba83d7f4cdf7333da653d22"

PATCH_RVA = 0x000199B3
PATCH_OLD = bytes.fromhex("b201")     # mov dl, 1
PATCH_NEW = bytes.fromhex("30d2")     # xor dl, dl

CONTEXT = (
    (0x00019920, bytes.fromhex("4883ec78"),
     "EnsureReticleChain prologue"),
    (0x0001993A, bytes.fromhex("488d05ff821600"),
     'EnsureReticleChain lea rax, ["crosshair"] (0x181C40)'),
    (0x00019987, bytes.fromhex("e834e00600"),
     "call TitleAdapter_GetActiveTitle (0x879C0)"),
    (0x0001998F, bytes.fromhex("e8dce60200"),
     "call Game_TitleCapturesAuthoredCrosshair (0x48070)"),
    (0x00019998, bytes.fromhex("80fb04"),
     "cmp bl, 4 dominates the patched site (Halo 4 only)"),
    (0x0001999B, bytes.fromhex("751a"),
     "jne 0x199B7 -> the shared false path"),
    (0x0001999D, bytes.fromhex("84c07416"),
     "test al,al / je 0x199B7 (titleHasAuthoredCapture)"),
    (0x000199A1, bytes.fromhex("803d09422900"),
     "cmp g_config.crosshair (0x2ADBB1), 0"),
    (0x000199AA, bytes.fromhex("803d5342290000"),
     "cmp g_config.kill_reticle (0x2ADC04), 0"),
    (0x000199B5, bytes.fromhex("eb02"),
     "jmp 0x199B9 straight after the patched instruction"),
    (0x000199B7, bytes.fromhex("32d2"),
     "xor dl,dl - the false path every other title takes"),
    (0x000199F0, bytes.fromhex("84d2"),
     "test dl,dl gating the authored early return"),
    (0x00019A72, bytes.fromhex("84d2"),
     "test dl,dl gating the opaque procedural opacity"),
    # Stage 3BI / 3BH artifacts must survive untouched
    (0x00053921, bytes.fromhex("e8"), "3BI capture-selection splice intact"),
    (0x0000D0C5, bytes.fromhex("e8"), "3BH draw splice intact"),
    (0x0002763C, bytes.fromhex("e8"), "3BH upload tap intact"),
    (0x002FB800, struct.pack("<ffff", 4134.312, 1346.196, 0.0, 1.0),
     "3BH engine-framing constant"),
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
    st = coff + 20 + osz
    secs = []
    for i in range(n):
        o = st + i * 40
        name = bytes(blob[o:o + 8]).split(b"\0", 1)[0].decode("ascii")
        vs, va, rs, rp = struct.unpack_from("<IIII", blob, o + 8)
        secs.append(dict(name=name, vs=vs, va=va, rs=rs, rp=rp))
    return dict(opt=coff + 20, n=n, sections=secs)


def rva_off(pe, rva):
    for s in pe["sections"]:
        if s["va"] <= rva < s["va"] + max(s["vs"], s["rs"]):
            return s["rp"] + rva - s["va"]
    raise KeyError(hex(rva))


def main():
    if len(sys.argv) != 3:
        raise SystemExit(
            "usage: build_stage3bj_h4_authored_reticle_on_quad.py "
            "<Stage3BI-HaloMCCVR.dll> <output.dll>")
    src, out = Path(sys.argv[1]), Path(sys.argv[2])

    blob = bytearray(src.read_bytes())
    original_len = len(blob)
    sha = hashlib.sha256(blob).hexdigest()
    print("input", sha)
    if sha != EXPECTED_STAGE3BI_SHA256:
        raise SystemExit("wrong Stage3BI input DLL: " + sha)

    pe = parse_pe(blob)
    if pe["n"] != 12:
        raise SystemExit(f"unexpected PE geometry: n={pe['n']}")

    for rva, expect, label in CONTEXT:
        o = rva_off(pe, rva)
        actual = bytes(blob[o:o + len(expect)])
        if actual != expect:
            raise SystemExit(f"{label}: expected {expect.hex()} at "
                             f"0x{rva:X}, got {actual.hex()}")

    po = rva_off(pe, PATCH_RVA)
    if bytes(blob[po:po + 2]) != PATCH_OLD:
        raise SystemExit("patch site is not `mov dl,1`: " +
                         bytes(blob[po:po + 2]).hex())
    blob[po:po + 2] = PATCH_NEW
    print(f"patched 0x{PATCH_RVA:X}: mov dl,1 -> xor dl,dl "
          f"(halo4ProceduralBootstrap forced false)")

    if struct.unpack_from("<I", blob, pe["opt"] + 0x40)[0] != 0:
        raise SystemExit("unexpected non-zero PE checksum")
    if len(blob) != original_len:
        raise SystemExit("output size changed unexpectedly")

    out.write_bytes(bytes(blob))
    print("output", hashlib.sha256(bytes(blob)).hexdigest(), len(blob))


if __name__ == "__main__":
    main()
