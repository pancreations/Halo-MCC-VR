"""Stage 3AS - Halo 4 pause-resume freeze fix, visor filter v2 with stream
trace, and the authored-reticle alpha/rgb probe.

Input is the exact Stage 3AR DLL. Three changes, no PE geometry change:

  1. The Stage 3AQ page at 0x2F9000 is rewritten: the admitted
     VR_RedirectRenderTargets path now clamps an out-of-range raster eye in
     r9d AND its spill slot [rsp+0x44], closing the teardown race that read
     g_eyeCacheRtvs[-1] and froze the game on pause-resume (2026-08-28 log).

  2. The Stage 3AR page at 0x2FA000 is rewritten: the visor polyart drop now
     also covers parallax brackets of NON-gameplay CUI roots, counts both
     contexts, and logs the counters every ~4s. Config key and splice entry
     offsets (+0x10 filter, +0x88 config) are unchanged and asserted.

  3. One new 5-byte splice at 0x1DD8B redirects MeasureAuthoredReticleCoverage's
     8x8 probe loop into the page, which sums alpha AND rgb ink and logs both
     every 64th measure, then resumes at 0x1DDD8 with the original contract
     (edi = alpha ink, r8 = 0).

See stage3as_eye_gate.S, stage3as_page.S, STAGE3AS-H4-FIXES-NOTES.md.
"""

from pathlib import Path
import hashlib
import struct
import sys

sys.path.insert(0, str(Path(__file__).parent))
from postlink import build_payload   # noqa: E402

EXPECTED_STAGE3AR_SHA256 = \
    "bd6a8eb33bbb9ca82686b41730c2949a5b1e5fbccef0ad3a8441e698aff15496"

EYE_GATE_RVA = 0x002F9000
PAGE_RVA = 0x002FA000

EYE_GATE_DEFS = {
    "TITLE_BYTE_RVA": 0x002BA6C8,
    "CAPTURE_ACTIVE_RVA": 0x002AE770,
    "REDIRECT_CONTINUE_RVA": 0x0002FF18,
    "REDIRECT_REFUSE_RVA": 0x0002FE92,
}

PAGE_DEFS = {
    "TLS_INDEX_RVA": 0x002D5E48,
    "ORIGINAL_LOG_RVA": 0x00001D90,
    "DETOUR_CONTINUE_RVA": 0x0005378A,
    "CONFIG_CONSUMED_RVA": 0x00005CE9,
    "CONFIG_UNKNOWN_RVA": 0x00005CA5,
    "PROBE_RESUME_RVA": 0x0001DDD8,
}

# MeasureAuthoredReticleCoverage, after the successful Map (js at 0x1DD89):
#   0x1DD8B  48 8B 54 24 58     mov rdx, [rsp+0x58]   <- splice (5 bytes)
#   0x1DD90  41 B8 08 00 00 00  mov r8d, 8
PROBE_SPLICE_RVA = 0x0001DD8B
PROBE_SPLICE_EXPECT = bytes.fromhex("488b542458")

CONTEXT = (
    # around the probe splice
    (0x0001DD84, bytes.fromhex("ff5070"), "Map vtable call before the loop"),
    (0x0001DD90, bytes.fromhex("41b808000000"), "probe loop row count"),
    (0x0001DDD8, bytes.fromhex("488b0d"), "Unmap tail the probe resumes at"),
    # the redirect gate this build's eye-gate page is entered from
    (0x0002FF0E, bytes.fromhex("e9ed902c0090"), "Stage 3AQ splice at 0x2FF0E"),
    (0x0002FEE4, bytes.fromhex("44894c2444"), "eye spill mov [rsp+0x44], r9d"),
    (0x00030046, bytes.fromhex("448b4c2444"), "eye reload from [rsp+0x44]"),
    (0x0003007A, bytes.fromhex("4898"), "cdqe before g_eyeCacheRtvs index"),
    # the two page entry splices (unchanged, must still target this page)
    (0x00053780, bytes.fromhex("e98b682a00"), "CUI detour splice -> 0x2FA010"),
    (0x00005CA0, bytes.fromhex("e9e3432f00"), "config splice -> 0x2FA088"),
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


def verify(code, code_rva, defs, checks):
    """Disassemble a payload and assert every rip/branch target it uses."""
    from capstone import Cs, CS_ARCH_X86, CS_MODE_64
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True
    targets = set()
    for ins in md.disasm(bytes(code), code_rva):
        for op in ins.operands:
            if op.type == 3 and op.mem.base == 41:      # X86_OP_MEM, RIP
                targets.add(ins.address + ins.size + op.mem.disp)
            elif op.type == 2:                           # X86_OP_IMM (branch)
                t = op.imm
                if not (code_rva <= t < code_rva + len(code) + 0x40):
                    targets.add(t)
    for name, rva in checks.items():
        if rva not in targets:
            raise SystemExit(
                f"payload verify: expected reference to {name} 0x{rva:X} "
                f"not found in emitted code")
    print(f"verify ok: {len(targets)} external targets, "
          f"{len(checks)} asserted")


def main():
    if len(sys.argv) != 3:
        raise SystemExit(
            "usage: build_stage3as_h4_fixes.py "
            "<Stage3AR-HaloMCCVR.dll> <output.dll>")
    src, out = Path(sys.argv[1]), Path(sys.argv[2])

    blob = bytearray(src.read_bytes())
    sha = hashlib.sha256(blob).hexdigest()
    print("input", sha)
    if sha != EXPECTED_STAGE3AR_SHA256:
        raise SystemExit("wrong Stage3AR input DLL: " + sha)

    pe = parse_pe(blob)
    if pe["n"] != 12:
        raise SystemExit(f"unexpected PE geometry: n={pe['n']}")
    qd = next((s for s in pe["sections"] if s["name"] == ".s3qd"), None)
    if not qd or qd["va"] != 0x2F3000 or qd["vs"] != 0x8000:
        raise SystemExit("unexpected .s3qd geometry")

    for rva, expect, label in CONTEXT:
        guard(blob, pe, rva, expect, label)

    here = Path(__file__).parent

    gate_code, gate_syms = build_payload(
        here / "stage3as_eye_gate.S", EYE_GATE_RVA, EYE_GATE_DEFS,
        ("stage3as_eye_gate",))
    print("eye gate", hashlib.sha256(gate_code).hexdigest(), len(gate_code))
    if gate_syms["stage3as_eye_gate"] != EYE_GATE_RVA:
        raise SystemExit("eye gate entry moved")
    if len(gate_code) > 0x1000:
        raise SystemExit("eye gate payload exceeds one page")
    verify(gate_code, EYE_GATE_RVA, EYE_GATE_DEFS, {
        "title byte": EYE_GATE_DEFS["TITLE_BYTE_RVA"],
        "capture active": EYE_GATE_DEFS["CAPTURE_ACTIVE_RVA"],
        "continue edge": EYE_GATE_DEFS["REDIRECT_CONTINUE_RVA"],
        "refuse edge": EYE_GATE_DEFS["REDIRECT_REFUSE_RVA"],
    })

    page_code, page_syms = build_payload(
        here / "stage3as_page.S", PAGE_RVA, PAGE_DEFS,
        ("stage3as_filter_entry", "stage3as_config_key", "stage3as_probe"))
    print("page", hashlib.sha256(page_code).hexdigest(), len(page_code))
    print("symbols", {k: hex(v) for k, v in page_syms.items()})
    if page_syms["stage3as_filter_entry"] != PAGE_RVA + 0x10:
        raise SystemExit("filter entry is not at +0x10")
    if page_syms["stage3as_config_key"] != PAGE_RVA + 0x88:
        raise SystemExit("config entry is not at +0x88")
    if len(page_code) > 0x1000:
        raise SystemExit("page payload exceeds one page")
    verify(page_code, PAGE_RVA, PAGE_DEFS, {
        "TLS index": PAGE_DEFS["TLS_INDEX_RVA"],
        "LOG": PAGE_DEFS["ORIGINAL_LOG_RVA"],
        "detour continue": PAGE_DEFS["DETOUR_CONTINUE_RVA"],
        "config consumed": PAGE_DEFS["CONFIG_CONSUMED_RVA"],
        "config unknown": PAGE_DEFS["CONFIG_UNKNOWN_RVA"],
        "probe resume": PAGE_DEFS["PROBE_RESUME_RVA"],
    })

    # 1+2: rewrite both pages in place (each is one committed 0x1000 page).
    for rva, code in ((EYE_GATE_RVA, gate_code), (PAGE_RVA, page_code)):
        o = rva_off(pe, rva)
        blob[o:o + 0x1000] = bytes(code).ljust(0x1000, b"\0")
        print(f"page 0x{rva:X}: {len(code)} bytes written")

    # 3: probe splice.
    o = guard(blob, pe, PROBE_SPLICE_RVA, PROBE_SPLICE_EXPECT, "probe splice")
    jump = bytes([0xE9]) + struct.pack(
        "<i", page_syms["stage3as_probe"] - (PROBE_SPLICE_RVA + 5))
    blob[o:o + 5] = jump
    print(f"probe splice: 0x{PROBE_SPLICE_RVA:X} "
          f"{PROBE_SPLICE_EXPECT.hex()} -> {jump.hex()}")

    if struct.unpack_from("<I", blob, pe["opt"] + 0x40)[0] != 0:
        raise SystemExit("unexpected non-zero PE checksum")
    if len(blob) != 2919424:
        raise SystemExit("output size changed unexpectedly")

    out.write_bytes(bytes(blob))
    print("output", hashlib.sha256(bytes(blob)).hexdigest(), len(blob))


if __name__ == "__main__":
    main()
