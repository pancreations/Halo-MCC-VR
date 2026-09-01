"""Stage 3BW - Halo 4 joins the shared 3D cutscene theatre.
Input: exact Stage 3BV DLL.  One behavioral change (theatre enablement) in
four byte-regions:
  1. jne 0x2C2BC re-pointed from the 3BV probe to s3bw_theatre (probe +
     the two title publications), which chains to the 3BU thunk 0x2F9D10.
  2. Payload at 0x2FAAA0 (free scratch, verified zero).
  3. kHalo4RuntimeCapabilities immediate 0xF3 -> 0x1F3 in the compiled
     PublishHalo4Lifecycle (mov edx,0xF3 at 0x68111; +CutsceneTheater 1<<8).
  4. Registry kTitles Halo4 capabilities dword 0xF3 -> 0x1F3 at 0x1890D4."""
from pathlib import Path
import hashlib, struct, sys
sys.path.insert(0, str(Path(__file__).parent))
from postlink import build_payload
from build_stage3br_h4_capture_scale import parse_pe, rva_off

EXPECTED_INPUT_SHA256 = \
    "e98e9502516ea0166c7bcb528fca5e10ba2ec1db1275f8a62bae449001fe44d9"
PAYLOAD_RVA = 0x002FAAA0            # free tail of the 3BH scratch page
PAYLOAD_LIMIT = 0x002FB000
SPLICE_RVA = 0x0002C2BC
SPLICE_OLD = bytes.fromhex("0f854edb2c00")   # jne 0x2F9E10 (Stage 3BV)
S3BU_THUNK_RVA = 0x002F9D10
S3BV_THUNK_RVA = 0x002F9E10

LOG_RVA = 0x00001D90
IAT_GETTICKCOUNT64_RVA = 0x00180150
IAT_GETMODULEHANDLEW_RVA = 0x001800F8
PUB_CTRL_RVA = 0x00088400           # TitleAdapter_PublishCinematicControl
PUB_PROJ_RVA = 0x000884C0           # TitleAdapter_PublishCutsceneTheaterProjection
GEN_FN_RVA = 0x0000A5A0             # TitleRuntime::Generation(GameTitle)
GTR_RVA = 0x002BA538                # g_titleRuntime
BBW_RVA = 0x002AEB58                # backbuffer width  (3BN/3BR-proven)
BBH_RVA = 0x002AEB5C                # backbuffer height

RUNTIME_CAPS_IMM_RVA = 0x00068111   # mov edx, 0xF3 (imm at +1)
REGISTRY_CAPS_RVA = 0x001890D4      # kTitles[Halo4].capabilities

CONTEXT = (
    (S3BU_THUNK_RVA, bytes.fromhex("4883ec40"), "3BU thunk head"),
    (S3BV_THUNK_RVA, bytes.fromhex("4883ec40"), "3BV thunk head"),
    (LOG_RVA, bytes.fromhex("48894c2408488954"), "LOG prologue"),
    (IAT_GETTICKCOUNT64_RVA, bytes.fromhex("ce10240000000000"),
     "IAT GetTickCount64"),
    (IAT_GETMODULEHANDLEW_RVA, bytes.fromhex("f80f240000000000"),
     "IAT GetModuleHandleW"),
    # publish/generation functions, verified prologues
    (PUB_CTRL_RVA, bytes.fromhex("48895c2408"), "PublishCinematicControl"),
    (PUB_CTRL_RVA + 0x23, bytes.fromhex("0fb6e9"), "PubCtrl movzx ebp,cl"),
    (PUB_PROJ_RVA, bytes.fromhex("48895c2408"), "PublishCutsceneTheaterProjection"),
    (PUB_PROJ_RVA + 0x0F, bytes.fromhex("f30f11542418"),
     "PubProj movss [rsp+0x18],xmm2"),
    (GEN_FN_RVA, bytes.fromhex("40534883ec20488bd9"),
     "TitleRuntime::Generation"),
    # the two capability masks, still stock 0xF3
    (RUNTIME_CAPS_IMM_RVA, bytes.fromhex("baf3000000"),
     "kHalo4RuntimeCapabilities mov edx,0xF3"),
    (REGISTRY_CAPS_RVA - 4, bytes.fromhex("00000000" "f3000000" "40000000"),
     "registry row pad/caps/admission"),
    (REGISTRY_CAPS_RVA - 0x1C, bytes.fromhex("0400000000000000"),
     "registry row title = Halo4"),
    (0x000199B3, bytes.fromhex("b201"), "3BJ NOT included"),
)


def main():
    src, out = Path(sys.argv[1]), Path(sys.argv[2])
    blob = bytearray(src.read_bytes())
    sha = hashlib.sha256(blob).hexdigest()
    print("input", sha)
    if sha != EXPECTED_INPUT_SHA256:
        raise SystemExit("wrong input DLL: " + sha)
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
            != S3BV_THUNK_RVA:
        raise SystemExit("original jne does not target the 3BV thunk")
    po, pl = rva_off(pe, PAYLOAD_RVA), rva_off(pe, PAYLOAD_LIMIT-1)+1
    if any(blob[po:pl]):
        raise SystemExit("payload region not free")

    code, symbols = build_payload(
        Path(__file__).parent / "stage3bw_h4_cutscene_theatre.S", PAYLOAD_RVA,
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
        want_symbols=("s3bw_theatre",))
    if PAYLOAD_RVA + len(code) > PAYLOAD_LIMIT:
        raise SystemExit(f"payload too large: {len(code)}")
    thunk = symbols["s3bw_theatre"]
    print(f"payload {len(code)} bytes at 0x{PAYLOAD_RVA:X}")
    blob[po:po+len(code)] = code
    blob[so:so+6] = b"\x0F\x85" + struct.pack("<i", thunk - (SPLICE_RVA + 6))
    print(f"splice 0x{SPLICE_RVA:X}: jne 0x{S3BV_THUNK_RVA:X} -> "
          f"jne 0x{thunk:X} (chains to 3BU)")
    # capability grants: 0xF3 -> 0x1F3 (TitleCapability_CutsceneTheater = 1<<8)
    io = rva_off(pe, RUNTIME_CAPS_IMM_RVA)
    blob[io+1:io+5] = struct.pack("<I", 0x1F3)
    ro = rva_off(pe, REGISTRY_CAPS_RVA)
    blob[ro:ro+4] = struct.pack("<I", 0x1F3)
    print("capability grants: runtime imm 0x68112 and registry dword "
          "0x1890D4 -> 0x1F3")
    out.write_bytes(bytes(blob))
    print("output", hashlib.sha256(bytes(blob)).hexdigest(), len(blob))


if __name__ == "__main__":
    main()
