"""Stage 3BX - Halo 4 theatre obeys the ODST look-constraint rule.
Input: exact Stage 3BW DLL.  One behavioral change in two byte-regions:
  1. jne 0x2C2BC re-pointed from the 3BW thunk to s3bx_theatre.
  2. Payload at 0x2FACB0 (free tail of the same scratch page, verified zero).
The capability grants and every other 3BW byte are untouched; the new thunk
still chains to the 3BU write-back at 0x2F9D10."""
from pathlib import Path
import hashlib, struct, sys
sys.path.insert(0, str(Path(__file__).parent))
from postlink import build_payload
from build_stage3br_h4_capture_scale import parse_pe, rva_off

EXPECTED_INPUT_SHA256 = \
    "9d6bba764e93dd6cd7b2482e0131765057196f92da725836277497f4996620f1"
PAYLOAD_RVA = 0x002FACB0            # after the 3BW thunk (ends 0x2FACA9)
PAYLOAD_LIMIT = 0x002FB000
SPLICE_RVA = 0x0002C2BC
SPLICE_OLD = bytes.fromhex("0f85dee72c00")   # jne 0x2FAAA0 (Stage 3BW)
S3BW_THUNK_RVA = 0x002FAAA0
S3BU_THUNK_RVA = 0x002F9D10

LOG_RVA = 0x00001D90
IAT_GETTICKCOUNT64_RVA = 0x00180150
IAT_GETMODULEHANDLEW_RVA = 0x001800F8
PUB_CTRL_RVA = 0x00088400
PUB_PROJ_RVA = 0x000884C0
GEN_FN_RVA = 0x0000A5A0
GTR_RVA = 0x002BA538
BBW_RVA = 0x002AEB58
BBH_RVA = 0x002AEB5C

RUNTIME_CAPS_IMM_RVA = 0x00068111
REGISTRY_CAPS_RVA = 0x001890D4

CONTEXT = (
    (S3BW_THUNK_RVA, bytes.fromhex("4883ec50"), "3BW thunk head"),
    (S3BU_THUNK_RVA, bytes.fromhex("4883ec40"), "3BU thunk head"),
    (LOG_RVA, bytes.fromhex("48894c2408488954"), "LOG prologue"),
    (IAT_GETTICKCOUNT64_RVA, bytes.fromhex("ce10240000000000"),
     "IAT GetTickCount64"),
    (IAT_GETMODULEHANDLEW_RVA, bytes.fromhex("f80f240000000000"),
     "IAT GetModuleHandleW"),
    (PUB_CTRL_RVA, bytes.fromhex("48895c2408"), "PublishCinematicControl"),
    (PUB_PROJ_RVA, bytes.fromhex("48895c2408"),
     "PublishCutsceneTheaterProjection"),
    (GEN_FN_RVA, bytes.fromhex("40534883ec20488bd9"),
     "TitleRuntime::Generation"),
    # the 3BW capability grants must already be in place and stay untouched
    (RUNTIME_CAPS_IMM_RVA, bytes.fromhex("baf3010000"),
     "kHalo4RuntimeCapabilities = 0x1F3"),
    (REGISTRY_CAPS_RVA, bytes.fromhex("f3010000"), "registry caps = 0x1F3"),
    (0x000199B3, bytes.fromhex("b201"), "3BJ NOT included"),
)

# halo4.dll facts this stage newly depends on, byte-verified at build time
# against the pinned retail module.
HALO4_DLL = Path(r"N:/SteamLibrary/steamapps/common"
                 r"/Halo The Master Chief Collection/halo4/halo4.dll")
HALO4_SHA256 = \
    "7c53e7d5bc9848545a1b70e2768242479336fba1b7630d7ab955f7fd0c34fa84"
# (file offset, bytes, label) - retail .text is mapped at RVA-0x1000+0x400,
# so these are checked through the module's own section table below.
HALO4_CONTEXT = (
    # script external -> implementation -> live writer
    (0x0015EC5B, "488d15b6deba00",
     "hs registration lea of cinematic_scripting_set_user_input_constraints"),
    (0x002C9314, "40534883ec204963", "constraints implementation head"),
    # writer 0x28D18C: TLS index, camera at +0x58, type word 6
    (0x0028D18C, "448b0585a0dc00", "constraint writer reads TLS index"),
    (0x0028D19F, "41b958000000", "camera member slot +0x58"),
    (0x0028D1AD, "b80600000066413b41020f", "camera type word == 6"),
    (0x0028D1F7, "f3410f1181d8000000", "writes rate +0xD8"),
    (0x0028D281, "458981f0000000", "stores remaining ticks +0xF0"),
    # reset 0x28D00B zeroes limits and rates (all-zero = no look freedom)
    (0x0028D05F, "f30f7f81c8000000", "reset zeroes limits +0xC8"),
    (0x0028D06E, "f30f7f89d8000000", "reset zeroes rates +0xD8"),
)


def parse_module(blob):
    p = struct.unpack_from("<I", blob, 0x3C)[0]
    n = struct.unpack_from("<H", blob, p + 6)[0]
    osz = struct.unpack_from("<H", blob, p + 20)[0]
    st = p + 24 + osz
    secs = []
    for i in range(n):
        o = st + i * 40
        vs, va, rs, rp = struct.unpack_from("<IIII", blob, o + 8)
        secs.append((va, vs, rp, rs))
    return secs


def module_off(secs, rva):
    for va, vs, rp, rs in secs:
        if va <= rva < va + max(vs, rs):
            return rp + rva - va
    raise KeyError(hex(rva))


def verify_halo4():
    blob = HALO4_DLL.read_bytes()
    sha = hashlib.sha256(blob).hexdigest()
    if sha != HALO4_SHA256:
        raise SystemExit("unexpected halo4.dll identity: " + sha)
    secs = parse_module(blob)
    for rva, hexbytes, label in HALO4_CONTEXT:
        want = bytes.fromhex(hexbytes)
        o = module_off(secs, rva)
        got = blob[o:o+len(want)]
        if got != want:
            raise SystemExit(f"halo4.dll {label} @ {rva:#x}: got {got.hex()}")
    print("halo4.dll look-constraint layout verified "
          "(camera TLS+0x58 type 6; limits +0xC8..+0xD4, rates +0xD8..+0xE4, "
          "ticks +0xF0)")


def main():
    src, out = Path(sys.argv[1]), Path(sys.argv[2])
    blob = bytearray(src.read_bytes())
    sha = hashlib.sha256(blob).hexdigest()
    print("input", sha)
    if sha != EXPECTED_INPUT_SHA256:
        raise SystemExit("wrong input DLL: " + sha)
    verify_halo4()
    pe = parse_pe(blob)
    if pe["n"] != 12:
        raise SystemExit("unexpected PE geometry")
    for rva, expect, label in CONTEXT:
        o = rva_off(pe, rva)
        if bytes(blob[o:o+len(expect)]) != expect:
            raise SystemExit(f"{label}: got {bytes(blob[o:o+len(expect)]).hex()}")
    so = rva_off(pe, SPLICE_RVA)
    if bytes(blob[so:so+6]) != SPLICE_OLD:
        raise SystemExit("splice site unexpected: " + bytes(blob[so:so+6]).hex())
    if SPLICE_RVA + 6 + struct.unpack_from("<i", SPLICE_OLD, 2)[0] \
            != S3BW_THUNK_RVA:
        raise SystemExit("original jne does not target the 3BW thunk")
    po, pl = rva_off(pe, PAYLOAD_RVA), rva_off(pe, PAYLOAD_LIMIT-1)+1
    if any(blob[po:pl]):
        raise SystemExit("payload region not free")
    code, symbols = build_payload(
        Path(__file__).parent / "stage3bx_h4_look_constraints.S", PAYLOAD_RVA,
        defs=dict(LOGFN=LOG_RVA,
                  IAT_TICK64=IAT_GETTICKCOUNT64_RVA,
                  IAT_GETMODW=IAT_GETMODULEHANDLEW_RVA,
                  S3BU_THUNK=S3BU_THUNK_RVA,
                  PUB_CTRL=PUB_CTRL_RVA,
                  PUB_PROJ=PUB_PROJ_RVA,
                  GEN_FN=GEN_FN_RVA,
                  GTR=GTR_RVA,
                  BBW=BBW_RVA,
                  BBH=BBH_RVA),
        want_symbols=("s3bx_theatre",))
    if PAYLOAD_RVA + len(code) > PAYLOAD_LIMIT:
        raise SystemExit(f"payload too large: {len(code)}")
    thunk = symbols["s3bx_theatre"]
    print(f"payload {len(code)} bytes at 0x{PAYLOAD_RVA:X}")
    blob[po:po+len(code)] = code
    blob[so:so+6] = b"\x0F\x85" + struct.pack("<i", thunk - (SPLICE_RVA + 6))
    print(f"splice 0x{SPLICE_RVA:X}: jne 0x{S3BW_THUNK_RVA:X} -> "
          f"jne 0x{thunk:X} (chains to 3BU)")
    out.write_bytes(bytes(blob))
    print("output", hashlib.sha256(bytes(blob)).hexdigest(), len(blob))


if __name__ == "__main__":
    main()
