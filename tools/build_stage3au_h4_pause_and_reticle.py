"""Stage 3AU - Halo 4 pause black screen + authored-reticle capture framing.

Input is the exact Stage 3AT DLL. Three changes; PE geometry is untouched (the
new code lands in the free tail of the existing 0x2FA000 page).

  1. Capture framing (the crosshair). The Halo 4 branch of
     BeginAuthoredReticleCaptureInternal loads a 16-byte viewport constant at
     .rdata 0x1BF0C0 = {2048, 2048, 0, 1} (Width, Height, MinDepth, MaxDepth;
     the code shufps-extracts lane 1 as Height and derives
     TopLeft = (512 - extent) * 0.5). Halo 4's CUI draws in raster-pixel space
     whose measured half-extents are 1893 x 1064.517, so a 2048-wide capture
     viewport MINIFIES the reticle by 0.54x - and the file's own C-H4-47 note
     records that minifying a thin/hollow reticle outline into the capture
     produces a totally blank result. The 08:38 headset log proves the capture
     is blank, not merely alpha-less: the Stage 3AS probe measured
     `alpha 0 rgb 0` on 1792 consecutive samples.

     The constant becomes {7572, 4258.068, 0, 1} = 4x the measured half-extents,
     i.e. the full CUI extent at 2x MAGNIFICATION. Lines get thicker, not
     thinner, the CUI centre still maps to the texture centre
     (TopLeft = (512 - 7572) * 0.5 = -3530 puts CUI x=0 at texture x=256), and
     the authored reticle (nominal height 81.92 CUI units) occupies ~32% of the
     512x512 capture - the same order as Halo 3/ODST's accepted 4x occupancy.

     This constant has exactly ONE reference in the image (0x11C77), asserted
     below, so nothing else can be affected.

  2. Pause black screen. Input_RequestPauseToggle (0x86A10) is spliced through
     a grace gate; see stage3au_pause_gate.S for the measured trace.

  3. The Stage 3AS reticle probe throttle drops from every 64th measure to
     every 16th, so the next log shows the ink moving without waiting on the
     post-publish sampling cadence.
"""

from pathlib import Path
import hashlib
import struct
import sys

sys.path.insert(0, str(Path(__file__).parent))
from postlink import build_payload   # noqa: E402

EXPECTED_STAGE3AT_SHA256 = \
    "250da86094b9eb35d295f73b850ff84165a900a77c249c4539477caccca77671"

# --- 1. capture viewport constant -----------------------------------------
VIEWPORT_CONST_RVA = 0x001BF0C0
VIEWPORT_CONST_EXPECT = struct.pack("<ffff", 2048.0, 2048.0, 0.0, 1.0)
# 4x the measured CUI half-extents (-1893.000 / 1064.517), i.e. the full CUI
# extent at 2x magnification.
VIEWPORT_CONST_PATCH = struct.pack("<ffff", 7572.0, 4258.068, 0.0, 1.0)
VIEWPORT_LOAD_RVA = 0x00011C77
VIEWPORT_LOAD_EXPECT = bytes.fromhex("660f6f0d41d41a00")   # movdqa xmm1,[rip]

# --- 2. pause toggle gate --------------------------------------------------
#   0x086A10  48 83 EC 28        sub  rsp, 0x28
#   0x086A14  E8 47 C2 FB FF     call 0x42C60   (Game_AllowsPauseToggleInput)
#   0x086A19  84 C0              test al, al          <- continue here
#   ...
#   0x086AA5  48 83 C4 28 C3     add  rsp, 0x28 / ret <- refusal epilogue
TOGGLE_RVA = 0x00086A10
TOGGLE_EXPECT = bytes.fromhex("4883ec28e847c2fbff")
TOGGLE_CONTINUE_RVA = 0x00086A19
TOGGLE_REFUSE_RVA = 0x00086AA5
ALLOWS_TOGGLE_RVA = 0x00042C60

GATE_RVA = 0x002FA400          # free tail of the Stage 3AS page (877 bytes used)

GATE_DEFS = {
    "PAUSE_GRACE_RVA": 0x002F31C8,
    "IAT_GET_TICK_COUNT64_RVA": 0x00180150,
    "ALLOWS_TOGGLE_RVA": ALLOWS_TOGGLE_RVA,
    "TOGGLE_CONTINUE_RVA": TOGGLE_CONTINUE_RVA,
    "TOGGLE_REFUSE_RVA": TOGGLE_REFUSE_RVA,
}

# --- 3. probe throttle -----------------------------------------------------
PROBE_THROTTLE_RVA = 0x002FA2B4
PROBE_THROTTLE_EXPECT = bytes.fromhex("a93f000000")        # test eax, 0x3F
PROBE_THROTTLE_PATCH = bytes.fromhex("a90f000000")         # test eax, 0x0F

CONTEXT = (
    (0x00011C7F, bytes.fromhex("0f28d9"), "movaps xmm3,xmm1 after the load"),
    (0x00011C82, bytes.fromhex("0fc6d955"), "shufps lane 1 -> Height"),
    (0x00011C73, bytes.fromhex("3c04"), "cmp al,4 (GameTitle::Halo4)"),
    (0x00086AA5, bytes.fromhex("4883c428c3"), "pause-toggle refusal epilogue"),
    (0x00086A19, bytes.fromhex("84c0"), "pause-toggle continue edge"),
    (0x002FA2AE, bytes.fromhex("8b054c000000"), "probe throttle load"),
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
        secs.append(dict(name=name, vs=vs, va=va, rs=rs, rp=rp, h=o))
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


def sole_reference(blob, pe, const_rva, allowed_rva):
    """Assert the 16-byte constant is read from exactly one instruction."""
    import re
    text = next(s for s in pe["sections"] if s["name"] == ".text")
    data = bytes(blob[text["rp"]:text["rp"] + text["rs"]])
    base = text["va"]
    pats = ((rb"\x66\x0f\x6f[\x05\x0d\x15\x1d\x25\x2d\x35\x3d]", 4),
            (rb"\x0f\x28[\x05\x0d\x15\x1d\x25\x2d\x35\x3d]", 3),
            (rb"\x0f\x10[\x05\x0d\x15\x1d\x25\x2d\x35\x3d]", 3),
            (rb"\xf3\x0f\x10[\x05\x0d\x15\x1d\x25\x2d\x35\x3d]", 4),
            (rb"\x66\x0f\xd6[\x05\x0d\x15\x1d\x25\x2d\x35\x3d]", 4))
    hits = set()
    for pat, dl in pats:
        for m in re.finditer(pat, data):
            o = m.start()
            disp = struct.unpack_from("<i", data, o + dl)[0]
            target = base + o + dl + 4 + disp
            if const_rva <= target < const_rva + 16:
                hits.add(base + o)
    if hits != {allowed_rva}:
        raise SystemExit(
            "viewport constant is not solely referenced by "
            f"0x{allowed_rva:X}: {[hex(h) for h in sorted(hits)]}")
    print(f"viewport constant sole reference: 0x{allowed_rva:X}")


def verify(code, code_rva, checks):
    from capstone import Cs, CS_ARCH_X86, CS_MODE_64
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True
    targets = set()
    for ins in md.disasm(bytes(code), code_rva):
        for op in ins.operands:
            if op.type == 3 and op.mem.base == 41:
                targets.add(ins.address + ins.size + op.mem.disp)
            elif op.type == 2:
                t = op.imm
                if not (code_rva <= t < code_rva + len(code) + 0x40):
                    targets.add(t)
    for name, rva in checks.items():
        if rva not in targets:
            raise SystemExit(
                f"payload verify: no reference to {name} 0x{rva:X}")
    print(f"verify ok: {len(targets)} external targets, "
          f"{len(checks)} asserted")


def main():
    if len(sys.argv) != 3:
        raise SystemExit(
            "usage: build_stage3au_h4_pause_and_reticle.py "
            "<Stage3AT-HaloMCCVR.dll> <output.dll>")
    src, out = Path(sys.argv[1]), Path(sys.argv[2])

    blob = bytearray(src.read_bytes())
    sha = hashlib.sha256(blob).hexdigest()
    print("input", sha)
    if sha != EXPECTED_STAGE3AT_SHA256:
        raise SystemExit("wrong Stage3AT input DLL: " + sha)

    pe = parse_pe(blob)
    if pe["n"] != 12:
        raise SystemExit(f"unexpected PE geometry: n={pe['n']}")
    qd = next((s for s in pe["sections"] if s["name"] == ".s3qd"), None)
    if not qd or qd["va"] != 0x2F3000 or qd["vs"] != 0x8000:
        raise SystemExit("unexpected .s3qd geometry")

    for rva, expect, label in CONTEXT:
        guard(blob, pe, rva, expect, label)
    guard(blob, pe, VIEWPORT_LOAD_RVA, VIEWPORT_LOAD_EXPECT,
          "capture viewport constant load")
    sole_reference(blob, pe, VIEWPORT_CONST_RVA, VIEWPORT_LOAD_RVA)

    code, syms = build_payload(
        Path(__file__).with_name("stage3au_pause_gate.S"),
        GATE_RVA, GATE_DEFS, ("stage3au_pause_gate",))
    print("gate", hashlib.sha256(code).hexdigest(), len(code))
    if syms["stage3au_pause_gate"] != GATE_RVA:
        raise SystemExit("gate entry moved")
    verify(code, GATE_RVA, {
        "pause grace": GATE_DEFS["PAUSE_GRACE_RVA"],
        "GetTickCount64 IAT": GATE_DEFS["IAT_GET_TICK_COUNT64_RVA"],
        "allows-toggle": ALLOWS_TOGGLE_RVA,
        "continue edge": TOGGLE_CONTINUE_RVA,
        "refuse edge": TOGGLE_REFUSE_RVA,
    })

    # the gate must land in genuinely unused page tail
    gate_off = rva_off(pe, GATE_RVA)
    if any(blob[gate_off:gate_off + len(code) + 16]):
        raise SystemExit("Stage3AU gate region is not free")

    # 1. viewport constant
    o = guard(blob, pe, VIEWPORT_CONST_RVA, VIEWPORT_CONST_EXPECT,
              "capture viewport constant")
    blob[o:o + 16] = VIEWPORT_CONST_PATCH
    print("viewport constant: {2048,2048,0,1} -> "
          f"{struct.unpack('<ffff', VIEWPORT_CONST_PATCH)}")

    # 2. pause gate: payload, then splice
    blob[gate_off:gate_off + len(code)] = code
    o = guard(blob, pe, TOGGLE_RVA, TOGGLE_EXPECT, "pause toggle prologue")
    jump = bytes([0xE9]) + struct.pack("<i", GATE_RVA - (TOGGLE_RVA + 5))
    blob[o:o + len(TOGGLE_EXPECT)] = \
        jump + bytes([0x90]) * (len(TOGGLE_EXPECT) - len(jump))
    print(f"pause toggle: 0x{TOGGLE_RVA:X} -> 0x{GATE_RVA:X}")

    # 3. probe throttle
    o = guard(blob, pe, PROBE_THROTTLE_RVA, PROBE_THROTTLE_EXPECT,
              "probe throttle")
    blob[o:o + len(PROBE_THROTTLE_PATCH)] = PROBE_THROTTLE_PATCH
    print("probe throttle: every 64th -> every 16th measure")

    if struct.unpack_from("<I", blob, pe["opt"] + 0x40)[0] != 0:
        raise SystemExit("unexpected non-zero PE checksum")
    if len(blob) != 2919424:
        raise SystemExit("output size changed unexpectedly")

    out.write_bytes(bytes(blob))
    print("output", hashlib.sha256(bytes(blob)).hexdigest(), len(blob))


if __name__ == "__main__":
    main()
