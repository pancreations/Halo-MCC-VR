"""Stage 3BM - raw dump of the published Halo 4 capture (instrument only).

Input: exact Stage 3BL DLL.  One 7-byte splice inside the Stage 3BH upload
tap (0x2FBA58, `mov byte ptr [s3bh_dumped],1`) -> `call s3bm_tick` + 2 NOPs.
The payload lives in the free tail of the Stage 3AS eye-gate page
(0x2F9040..0x2FA000).  See stage3bm_h4_capture_raw_dump.S.
"""
from pathlib import Path
import hashlib, struct, sys
sys.path.insert(0, str(Path(__file__).parent))
from postlink import build_payload

EXPECTED_STAGE3BL_SHA256 = \
    "1fd640f3151d9602951774e722681ec45ce54a5bb26e6c5b44d2c2e88583e930"
PAYLOAD_RVA = 0x002F9040
PAYLOAD_LIMIT = 0x002FA000
SPLICE_RVA = 0x002FBA58
SPLICE_OLD = bytes.fromhex("c605c1fdffff01")      # mov byte [0x2FB820], 1
S3BH_WINDOWS_RVA = 0x002FB81C
S3BH_DUMPED_RVA = 0x002FB820
LOG_RVA = 0x00001D90
LOGDIR_RVA = 0x00001830
G_CONTEXT_RVA = 0x002AE298
G_DEVICE_RVA = 0x002AE290
AUTHORED_TEX_RVA = 0x002AE440
IAT_CREATEFILEW_RVA = 0x00180220
IAT_WRITEFILE_RVA = 0x00180350
IAT_CLOSEHANDLE_RVA = 0x001800B8

CONTEXT = (
    # the 3BH tap around the splice
    (0x002FBA49, bytes.fromhex("ff05cdfdffff833dc6fdffff03720c"),
     "tap: inc windows / cmp windows,3 / jb"),
    (0x002FBA5F, bytes.fromhex("e805000000"), "tap: call s3bh_dump follows"),
    (0x002FBA37, bytes.fromhex("803d8aecfbff04"), "tap: title == Halo 4"),
    # LogDirectory leaf
    (LOGDIR_RVA, bytes.fromhex("48833de007240007488d05c1072400"),
     "LogDirectory() SSO leaf"),
    # 3AS eye gate occupies only 0x2F9000..0x2F9034
    (0x002F9000, bytes.fromhex("803d"), "3AS eye gate head"),
    # 3BL / 3BK / 3BI artifacts
    (0x002FA2D7, bytes.fromhex("e9"), "3BL splice"),
    (0x002FB992, bytes.fromhex("e8"), "3BK gate splice"),
    (0x00053921, bytes.fromhex("e8"), "3BI capture-selection splice"),
    (0x000199B3, bytes.fromhex("b201"), "3BJ NOT included"),
)


def parse_pe(blob):
    p = struct.unpack_from("<I", blob, 0x3C)[0]; coff = p + 4
    n = struct.unpack_from("<H", blob, coff + 2)[0]
    osz = struct.unpack_from("<H", blob, coff + 16)[0]
    st = coff + 20 + osz; secs = []
    for i in range(n):
        o = st + i * 40
        name = bytes(blob[o:o+8]).split(b"\0", 1)[0].decode()
        vs, va, rs, rp = struct.unpack_from("<IIII", blob, o+8)
        secs.append(dict(name=name, vs=vs, va=va, rs=rs, rp=rp))
    return dict(opt=coff+20, n=n, sections=secs)


def rva_off(pe, rva):
    for s in pe["sections"]:
        if s["va"] <= rva < s["va"] + max(s["vs"], s["rs"]):
            return s["rp"] + rva - s["va"]
    raise KeyError(hex(rva))


def main():
    src, out = Path(sys.argv[1]), Path(sys.argv[2])
    blob = bytearray(src.read_bytes())
    original_len = len(blob)
    sha = hashlib.sha256(blob).hexdigest()
    print("input", sha)
    if sha != EXPECTED_STAGE3BL_SHA256:
        raise SystemExit("wrong Stage3BL input DLL: " + sha)
    pe = parse_pe(blob)
    if pe["n"] != 12:
        raise SystemExit("unexpected PE geometry")
    for rva, expect, label in CONTEXT:
        o = rva_off(pe, rva)
        if bytes(blob[o:o+len(expect)]) != expect:
            raise SystemExit(f"{label}: expected {expect.hex()} at 0x{rva:X}, "
                             f"got {bytes(blob[o:o+len(expect)]).hex()}")
    so = rva_off(pe, SPLICE_RVA)
    if bytes(blob[so:so+7]) != SPLICE_OLD:
        raise SystemExit("splice bytes unexpected: " + bytes(blob[so:so+7]).hex())
    if SPLICE_RVA + 7 - 0x23F != S3BH_DUMPED_RVA:
        raise SystemExit("s3bh_dumped RVA mismatch")
    po = rva_off(pe, PAYLOAD_RVA)
    pl = rva_off(pe, PAYLOAD_LIMIT-1)+1
    if any(blob[po:pl]):
        raise SystemExit("payload region not free")
    code, symbols = build_payload(
        Path(__file__).parent / "stage3bm_h4_capture_raw_dump.S",
        PAYLOAD_RVA,
        defs=dict(S3BH_WINDOWS_RVA=S3BH_WINDOWS_RVA,
                  S3BH_DUMPED_RVA=S3BH_DUMPED_RVA,
                  LOG_RVA=LOG_RVA, LOGDIR_RVA=LOGDIR_RVA,
                  G_CONTEXT_RVA=G_CONTEXT_RVA, G_DEVICE_RVA=G_DEVICE_RVA,
                  AUTHORED_TEX_RVA=AUTHORED_TEX_RVA,
                  IAT_CREATEFILEW_RVA=IAT_CREATEFILEW_RVA,
                  IAT_WRITEFILE_RVA=IAT_WRITEFILE_RVA,
                  IAT_CLOSEHANDLE_RVA=IAT_CLOSEHANDLE_RVA),
        want_symbols=("s3bm_tick", "stage3bm_data"))
    if PAYLOAD_RVA + len(code) > PAYLOAD_LIMIT:
        raise SystemExit(f"payload too large: {len(code)}")
    tick = symbols["s3bm_tick"]
    print(f"payload {len(code)} bytes at 0x{PAYLOAD_RVA:X}; tick 0x{tick:X}")
    blob[po:po+len(code)] = code
    blob[so:so+7] = b"\xE8" + struct.pack("<i", tick - (SPLICE_RVA + 5)) + b"\x90\x90"
    print(f"splice 0x{SPLICE_RVA:X}: mov byte [s3bh_dumped],1 -> call 0x{tick:X} + 2 nops")
    if len(blob) != original_len:
        raise SystemExit("size changed")
    out.write_bytes(bytes(blob))
    print("output", hashlib.sha256(bytes(blob)).hexdigest(), len(blob))


if __name__ == "__main__":
    main()
