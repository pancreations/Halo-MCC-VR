"""Stage 3BZ - lock Halo 4's camera/FOV on the proven 3BX theatre state.

Input is exact Stage 3BX, not failed Stage 3BY.  Only the existing Halo 4
snapshot call and one verified-zero scratch gap change.
"""
from pathlib import Path
import hashlib
import subprocess
import struct
import sys

sys.path.insert(0, str(Path(__file__).parent))
from postlink import build_payload

EXPECTED_INPUT_SHA256 = (
    "54130fd5a37b2d5e19e610aad712fa8595501849cc27ead7753b132de7dfbd9e")

CALL_SITE_RVA = 0x00056EBF
CALL_SITE_OLD = bytes.fromhex("e89c7bfdff")
SNAPSHOT_FN_RVA = 0x0002EA60

# Final byte in 3BX's payload: the logged classifier state.  The layout is
# verified below from the payload's format string, UTF-16 module name, aligned
# next-ms/base qwords, and this trailing state byte.
S3BX_STATE_RVA = 0x002FAFE0
S3BX_FORMAT_RVA = 0x002FAF15
S3BX_FORMAT = b"H4CINE: state %d -> %d"
S3BX_WSTR_RVA = 0x002FAFB6
S3BX_WSTR = "halo4.dll\0".encode("utf-16le")

PAYLOAD_RVA = 0x002FA370
PAYLOAD_LIMIT = 0x002FA400


def parse_pe(blob):
    pe = struct.unpack_from("<I", blob, 0x3C)[0]
    if blob[pe:pe + 4] != b"PE\0\0":
        raise SystemExit("not a PE image")
    count = struct.unpack_from("<H", blob, pe + 6)[0]
    opt_size = struct.unpack_from("<H", blob, pe + 20)[0]
    sections = []
    for index in range(count):
        at = pe + 24 + opt_size + index * 40
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
    if not -0x80000000 <= value <= 0x7FFFFFFF:
        raise SystemExit("call target is out of rel32 range")
    return struct.pack("<i", value)


def build_fixed_wrapper():
    """Encode the canonical 49-byte .S wrapper if the archived clang is dead."""
    code = bytearray(bytes.fromhex(
        "4883ec28"          # sub rsp, 0x28
        "48894c2420"        # mov [rsp+0x20], rcx
        "e800000000"        # call VR_Halo4GetRenderSnapshot
        "84c0"              # test al, al
        "741a"              # jz .Ldone
        "803d0000000004"    # cmp byte ptr [rip+S3BX_STATE], 4
        "7511"              # jne .Ldone
        "488b4c2420"        # mov rcx, [rsp+0x20]
        "c6412c00"          # eyes[0].fovValid = false
        "c6415c00"          # eyes[1].fovValid = false
        "c6417c00"          # headPoseValid = false
        "4883c428"          # add rsp, 0x28
        "c3"                # ret
    ))
    if len(code) != 49:
        raise SystemExit("fixed wrapper encoding has the wrong length")
    code[10:14] = rel32(SNAPSHOT_FN_RVA, PAYLOAD_RVA + 14)
    code[20:24] = rel32(S3BX_STATE_RVA, PAYLOAD_RVA + 25)
    return bytes(code), {"s3bz_get_render_snapshot": PAYLOAD_RVA}


def main():
    source, output = Path(sys.argv[1]), Path(sys.argv[2])
    blob = bytearray(source.read_bytes())
    digest = hashlib.sha256(blob).hexdigest()
    print("input", digest)
    if digest != EXPECTED_INPUT_SHA256:
        raise SystemExit("wrong input DLL: " + digest)
    pe = parse_pe(blob)
    if pe["n"] != 12:
        raise SystemExit("unexpected PE geometry")

    call_off = rva_off(pe, CALL_SITE_RVA)
    if bytes(blob[call_off:call_off + 5]) != CALL_SITE_OLD:
        raise SystemExit("snapshot callsite unexpected")
    original_target = CALL_SITE_RVA + 5 + struct.unpack_from(
        "<i", CALL_SITE_OLD, 1)[0]
    if original_target != SNAPSHOT_FN_RVA:
        raise SystemExit("snapshot callsite has the wrong original target")

    format_off = rva_off(pe, S3BX_FORMAT_RVA)
    if bytes(blob[format_off:format_off + len(S3BX_FORMAT)]) != S3BX_FORMAT:
        raise SystemExit("3BX state format identity changed")
    wstr_off = rva_off(pe, S3BX_WSTR_RVA)
    if bytes(blob[wstr_off:wstr_off + len(S3BX_WSTR)]) != S3BX_WSTR:
        raise SystemExit("3BX payload layout changed")
    if S3BX_STATE_RVA != ((S3BX_WSTR_RVA + len(S3BX_WSTR) + 7) & ~7) + 16:
        raise SystemExit("3BX state layout proof is inconsistent")
    if blob[rva_off(pe, S3BX_STATE_RVA)] != 0:
        raise SystemExit("3BX initial state byte changed")

    payload_off = rva_off(pe, PAYLOAD_RVA)
    payload_end = rva_off(pe, PAYLOAD_LIMIT - 1) + 1
    if any(blob[payload_off:payload_end]):
        raise SystemExit("payload region is not free")
    try:
        code, symbols = build_payload(
            Path(__file__).parent / "stage3bz_h4_authored_camera.S", PAYLOAD_RVA,
            defs={"SNAPSHOT_FN": SNAPSHOT_FN_RVA,
                  "S3BX_STATE": S3BX_STATE_RVA},
            want_symbols=("s3bz_get_render_snapshot",))
    except subprocess.CalledProcessError:
        # The archived conda clang currently exits STATUS_DLL_NOT_FOUND on this
        # machine.  This narrow fallback emits exactly the .S above; the stage
        # verifier disassembles and proves every call, gate, branch and write.
        code, symbols = build_fixed_wrapper()
        print("archived clang unavailable; used verified fixed 49-byte encoding")
    if PAYLOAD_RVA + len(code) > PAYLOAD_LIMIT:
        raise SystemExit(f"payload too large: {len(code)} bytes")
    thunk = symbols["s3bz_get_render_snapshot"]

    blob[payload_off:payload_off + len(code)] = code
    blob[call_off:call_off + 5] = b"\xE8" + rel32(
        thunk, CALL_SITE_RVA + 5)
    output.write_bytes(blob)
    print(f"payload {len(code)} bytes at {PAYLOAD_RVA:#x}")
    print(f"call {CALL_SITE_RVA:#x}: {SNAPSHOT_FN_RVA:#x} -> {thunk:#x}")
    print("output", hashlib.sha256(blob).hexdigest(), len(blob))


if __name__ == "__main__":
    main()
