"""Stage 3BY - add authored-camera ownership to the accepted Stage 3BX DLL.

The only executable change is Halo4StereoTransaction's existing call to
VR_Halo4GetRenderSnapshot.  The replacement calls the original helper first
and is a strict pass-through unless VR_IsCutsceneTheaterActive reports that
the shared compositor has completed its full-black authored-theatre handoff.
"""
from pathlib import Path
import hashlib
import struct
import sys

sys.path.insert(0, str(Path(__file__).parent))
from postlink import build_payload

EXPECTED_INPUT_SHA256 = (
    "54130fd5a37b2d5e19e610aad712fa8595501849cc27ead7753b132de7dfbd9e")

# Halo4StereoTransaction's first operation after zeroing its local snapshot.
CALL_SITE_RVA = 0x00056EBF
CALL_SITE_OLD = bytes.fromhex("e89c7bfdff")  # call 0x2EA60
SNAPSHOT_FN_RVA = 0x0002EA60

# Verified by the existing UpdateCinematicFovPolicy callsite at 0x8359C and
# the target's complete load/ret body in the Stage 3BX input.
THEATRE_ACTIVE_RVA = 0x0002EE60
THEATRE_ACTIVE_BODY = bytes.fromhex("0fb60515fb270090c3")

# A 0x90-byte verified-zero gap in BX's already allocated .s3qd scratch
# section.  The nominal tail page is occupied by an earlier accepted stage.
PAYLOAD_RVA = 0x002FA370
PAYLOAD_LIMIT = 0x002FA400


def parse_pe(blob):
    pe = struct.unpack_from("<I", blob, 0x3C)[0]
    if blob[pe:pe + 4] != b"PE\0\0":
        raise SystemExit("not a PE image")
    count = struct.unpack_from("<H", blob, pe + 6)[0]
    opt_size = struct.unpack_from("<H", blob, pe + 20)[0]
    sections = []
    for i in range(count):
        at = pe + 24 + opt_size + i * 40
        virtual_size, virtual_address, raw_size, raw_offset = \
            struct.unpack_from("<IIII", blob, at + 8)
        sections.append((virtual_address, virtual_size, raw_offset, raw_size))
    return {"n": count, "sections": sections}


def rva_off(pe, rva):
    for va, virtual_size, raw_offset, raw_size in pe["sections"]:
        if va <= rva < va + max(virtual_size, raw_size):
            return raw_offset + rva - va
    raise SystemExit(f"RVA is outside the PE sections: {rva:#x}")


def rel32(target, next_rva):
    value = target - next_rva
    if not -0x80000000 <= value <= 0x7fffffff:
        raise SystemExit("call target is out of rel32 range")
    return struct.pack("<i", value)


def build_wrapper():
    """Assemble the fixed 57-byte thunk without requiring a host clang.

    Other stages use postlink.build_payload and therefore need clang's ELF
    assembler.  This wrapper has no data, labels exposed to callers, or
    relocations other than its two fixed rel32 calls, so encoding it here keeps
    the accepted post-link workflow available on the normal MSVC-only shell.
    The canonical instruction source remains beside this builder in the .S
    file; test_stage3by decodes every emitted instruction below.
    """
    code = bytearray(bytes.fromhex(
        "4883ec38"          # sub rsp, 0x38
        "48894c2420"        # mov [rsp+0x20], rcx
        "e800000000"        # call VR_Halo4GetRenderSnapshot
        "84c0"              # test al, al
        "7422"              # jz .Ldone
        "88442428"          # mov [rsp+0x28], al
        "e800000000"        # call VR_IsCutsceneTheaterActive
        "84c0"              # test al, al
        "7411"              # jz .Lrestore
        "488b4c2420"        # mov rcx, [rsp+0x20]
        "c6412c00"          # eyes[0].fovValid = false
        "c6415c00"          # eyes[1].fovValid = false
        "c6417c00"          # headPoseValid = false
        "8a442428"          # mov al, [rsp+0x28]
        "4883c438"          # add rsp, 0x38
        "c3"                # ret
    ))
    assert len(code) == 57
    code[10:14] = rel32(SNAPSHOT_FN_RVA, PAYLOAD_RVA + 14)
    code[23:27] = rel32(THEATRE_ACTIVE_RVA, PAYLOAD_RVA + 27)
    return bytes(code)


def main():
    src, out = Path(sys.argv[1]), Path(sys.argv[2])
    blob = bytearray(src.read_bytes())
    digest = hashlib.sha256(blob).hexdigest()
    print("input", digest)
    if digest != EXPECTED_INPUT_SHA256:
        raise SystemExit("wrong input DLL: " + digest)

    pe = parse_pe(blob)
    if pe["n"] != 12:
        raise SystemExit("unexpected PE geometry")
    call_off = rva_off(pe, CALL_SITE_RVA)
    if bytes(blob[call_off:call_off + 5]) != CALL_SITE_OLD:
        raise SystemExit("snapshot callsite unexpected: " +
                         bytes(blob[call_off:call_off + 5]).hex())
    old_target = CALL_SITE_RVA + 5 + struct.unpack_from(
        "<i", CALL_SITE_OLD, 1)[0]
    if old_target != SNAPSHOT_FN_RVA:
        raise SystemExit("snapshot callsite has the wrong original target")
    active_off = rva_off(pe, THEATRE_ACTIVE_RVA)
    if bytes(blob[active_off:active_off + len(THEATRE_ACTIVE_BODY)]) != \
            THEATRE_ACTIVE_BODY:
        raise SystemExit("theatre-active helper identity changed")

    payload_off = rva_off(pe, PAYLOAD_RVA)
    payload_end = rva_off(pe, PAYLOAD_LIMIT - 1) + 1
    if any(blob[payload_off:payload_end]):
        raise SystemExit("payload region is not free")
    # Keep using the regular stage assembler when it is installed.  Codex's
    # MSVC-only shell intentionally has no clang, so this small fixed thunk
    # has a byte-for-byte local encoder as a narrow fallback.
    try:
        code, symbols = build_payload(
            Path(__file__).parent / "stage3by_h4_theatre_snapshot.S", PAYLOAD_RVA,
            defs={"SNAPSHOT_FN": SNAPSHOT_FN_RVA,
                  "THEATRE_ACTIVE": THEATRE_ACTIVE_RVA},
            want_symbols=("s3by_get_render_snapshot",))
        thunk = symbols["s3by_get_render_snapshot"]
    except SystemExit as error:
        if str(error) != "clang is required to assemble post-link stage payloads":
            raise
        code = build_wrapper()
        thunk = PAYLOAD_RVA
        print("clang unavailable; used fixed local encoding for this 57-byte thunk")
    if PAYLOAD_RVA + len(code) > PAYLOAD_LIMIT:
        raise SystemExit(f"payload too large: {len(code)} bytes")

    blob[payload_off:payload_off + len(code)] = code
    blob[call_off:call_off + 5] = b"\xE8" + rel32(thunk, CALL_SITE_RVA + 5)
    out.write_bytes(blob)
    print(f"payload {len(code)} bytes at {PAYLOAD_RVA:#x}")
    print(f"call {CALL_SITE_RVA:#x}: {SNAPSHOT_FN_RVA:#x} -> {thunk:#x}")
    print("output", hashlib.sha256(blob).hexdigest(), len(blob))


if __name__ == "__main__":
    main()
