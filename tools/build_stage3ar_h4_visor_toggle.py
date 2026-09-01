"""Stage 3AR - Halo 4 Mjolnir visor toggle.

Input is the exact Stage 3AQ DLL. Appends one 0x1000 page to the final .s3qd
section and redirects two edges through it:

  * HaloMCCVR.dll+0x53780 - Halo4CuiRenderCommandDetour's entry, so visor
    polyart draws inside a CUI parallax bracket can be dropped;
  * HaloMCCVR.dll+0x5CA0  - the config parser's unknown-key edge, so
    halomccvr.cfg gains `halo4_visor = 0|1`.

See stage3ar_h4_visor_toggle.S and STAGE3AR-H4-VISOR-TOGGLE-NOTES.md.
"""

from pathlib import Path
import hashlib
import struct
import sys

sys.path.insert(0, str(Path(__file__).parent))
from postlink import build_payload   # noqa: E402

EXPECTED_STAGE3AQ_SHA256 = \
    "e68b39b1e1f054d1b3db69bf9fa111943a40eaf2709f0395699d09f17d7f186a"

CODE_RVA = 0x002FA000

# Halo4CuiRenderCommandDetour prologue.
#   0x053780  40 55              push rbp
#   0x053782  56                 push rsi
#   0x053783  41 55              push r13
#   0x053785  48 8D 6C 24 E0     lea  rbp, [rsp - 0x20]
#   0x05378A  48 81 EC 20 01..   sub  rsp, 0x120     <- continue here
DETOUR_RVA = 0x00053780
DETOUR_EXPECT = bytes.fromhex("4055564155488d6c24e0")
DETOUR_CONTINUE_RVA = 0x0005378A

# Config parser, unknown-key edge.
#   0x005CA0  44 38 33           cmp byte ptr [rbx], r14b
#   0x005CA3  74 44              je   0x005CE9       <- key consumed
#   0x005CA5                     mov  rdx, rbx       <- "unknown key" log
CONFIG_RVA = 0x00005CA0
CONFIG_EXPECT = bytes.fromhex("4438337444")
CONFIG_CONSUMED_RVA = 0x00005CE9
CONFIG_UNKNOWN_RVA = 0x00005CA5

DEFS = {
    "TLS_INDEX_RVA": 0x002D5E48,
    "DETOUR_CONTINUE_RVA": DETOUR_CONTINUE_RVA,
    "CONFIG_CONSUMED_RVA": CONFIG_CONSUMED_RVA,
    "CONFIG_UNKNOWN_RVA": CONFIG_UNKNOWN_RVA,
}

CONTEXT = (
    (0x000537BE, bytes.fromhex("48899c24"), "detour body after the prologue"),
    (0x00005CA5, bytes.fromhex("488bd3"), "unknown-key log edge"),
    (0x00005CE9, bytes.fromhex("4c8b4424"), "config loop continue edge"),
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


def splice(blob, offset, expect, target, rva, label):
    jump = bytes([0xE9]) + struct.pack("<i", target - (rva + 5))
    replacement = jump + bytes([0x90]) * (len(expect) - len(jump))
    blob[offset:offset + len(expect)] = replacement
    print(f"{label}: 0x{rva:X} {expect.hex()} -> {replacement.hex()}")


def main():
    if len(sys.argv) != 3:
        raise SystemExit(
            "usage: build_stage3ar_h4_visor_toggle.py "
            "<Stage3AQ-HaloMCCVR.dll> <output.dll>")
    src, out = Path(sys.argv[1]), Path(sys.argv[2])

    blob = bytearray(src.read_bytes())
    sha = hashlib.sha256(blob).hexdigest()
    print("input", sha)
    if sha != EXPECTED_STAGE3AQ_SHA256:
        raise SystemExit("wrong Stage3AQ input DLL: " + sha)

    code, symbols = build_payload(
        Path(__file__).with_name("stage3ar_h4_visor_toggle.S"),
        CODE_RVA, DEFS,
        ("stage3ar_cui_visor_filter", "stage3ar_config_key"))
    print("code", hashlib.sha256(code).hexdigest(), len(code))
    print("symbols", {k: hex(v) for k, v in symbols.items()})
    if len(code) > 0x1000:
        raise SystemExit("Stage3AR payload exceeds one page")

    pe = parse_pe(blob)
    if pe["n"] != 12:
        raise SystemExit(f"unexpected PE geometry: n={pe['n']}")
    qd = next((s for s in pe["sections"] if s["name"] == ".s3qd"), None)
    if not qd or qd["va"] != 0x2F3000 or qd["vs"] != 0x7000 or \
            qd["rs"] != 0x7000:
        raise SystemExit("unexpected .s3qd geometry")
    if qd["rp"] + qd["rs"] != len(blob):
        raise SystemExit("unexpected overlay after .s3qd")

    for rva, expect, label in CONTEXT:
        guard(blob, pe, rva, expect, label)

    detour_off = guard(blob, pe, DETOUR_RVA, DETOUR_EXPECT,
                       "CUI dispatcher detour prologue")
    config_off = guard(blob, pe, CONFIG_RVA, CONFIG_EXPECT,
                       "config unknown-key edge")

    splice(blob, detour_off, DETOUR_EXPECT,
           symbols["stage3ar_cui_visor_filter"], DETOUR_RVA, "CUI detour")
    splice(blob, config_off, CONFIG_EXPECT,
           symbols["stage3ar_config_key"], CONFIG_RVA, "config key")

    blob.extend(code.ljust(0x1000, b"\0"))
    struct.pack_into("<I", blob, qd["h"] + 8, 0x8000)          # VirtualSize
    struct.pack_into("<I", blob, qd["h"] + 16, 0x8000)         # SizeOfRawData
    struct.pack_into("<I", blob, pe["opt"] + 0x38, 0x2FB000)   # SizeOfImage
    struct.pack_into("<I", blob, pe["opt"] + 8,
                     struct.unpack_from("<I", blob, pe["opt"] + 8)[0] + 0x1000)
    if struct.unpack_from("<I", blob, pe["opt"] + 0x40)[0] != 0:
        raise SystemExit("unexpected non-zero PE checksum")

    out.write_bytes(bytes(blob))
    print("output", hashlib.sha256(bytes(blob)).hexdigest(), len(blob))


if __name__ == "__main__":
    main()
