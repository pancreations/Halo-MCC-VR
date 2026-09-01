"""Stage 3CA - one-shot Halo 4 cutscene eye-image validation.

Built directly from exact Stage 3BX.  The probe chains into BX unchanged and
only clears the existing M2 validation-done byte once after BX state 4 is live.
"""
from pathlib import Path
import hashlib
import struct
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).parent))
from postlink import build_payload

EXPECTED_INPUT_SHA256 = (
    "54130fd5a37b2d5e19e610aad712fa8595501849cc27ead7753b132de7dfbd9e")
SPLICE_RVA = 0x0002C2BC
SPLICE_OLD = bytes.fromhex("0f85eee92c00")  # jne 0x2FACB0
S3BX_THUNK_RVA = 0x002FACB0
S3BX_STATE_RVA = 0x002FAFE0
VALIDATION_DONE_RVA = 0x002AE85F
VALIDATION_FN_RVA = 0x00032400
VALIDATION_FN_HEAD = bytes.fromhex("4055488d6c24a94881ecc0000000")
VALIDATION_GUARD = bytes.fromhex("803d3cc4270000")
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
        raise SystemExit("relative target is out of range")
    return struct.pack("<i", value)


def build_fixed_probe():
    code = bytearray(bytes.fromhex(
        "0fb60500000000"    # movzx eax, byte ptr [rip+S3BX_STATE]
        "83f804"            # cmp eax, 4
        "7519"              # jne .Lnot_locked
        "803d0000000000"    # cmp byte ptr [rip+latch], 0
        "7517"              # jne .Lchain
        "c6050000000001"    # mov byte ptr [rip+latch], 1
        "c6050000000000"    # mov byte ptr [rip+VALIDATION_DONE], 0
        "eb07"              # jmp .Lchain
        "c6050000000000"    # mov byte ptr [rip+latch], 0
        "e900000000"        # jmp S3BX_THUNK
        "00"                # latch
    ))
    if len(code) != 50:
        raise SystemExit("fixed probe encoding has the wrong length")
    latch = PAYLOAD_RVA + 49
    code[3:7] = rel32(S3BX_STATE_RVA, PAYLOAD_RVA + 7)
    code[14:18] = rel32(latch, PAYLOAD_RVA + 19)
    code[23:27] = rel32(latch, PAYLOAD_RVA + 28)
    code[30:34] = rel32(VALIDATION_DONE_RVA, PAYLOAD_RVA + 35)
    code[39:43] = rel32(latch, PAYLOAD_RVA + 44)
    code[45:49] = rel32(S3BX_THUNK_RVA, PAYLOAD_RVA + 49)
    return bytes(code), {"s3ca_probe": PAYLOAD_RVA}


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

    splice_off = rva_off(pe, SPLICE_RVA)
    if bytes(blob[splice_off:splice_off + 6]) != SPLICE_OLD:
        raise SystemExit("BX splice identity changed")
    if SPLICE_RVA + 6 + struct.unpack_from("<i", SPLICE_OLD, 2)[0] != \
            S3BX_THUNK_RVA:
        raise SystemExit("BX splice has the wrong target")
    fn_off = rva_off(pe, VALIDATION_FN_RVA)
    if bytes(blob[fn_off:fn_off + len(VALIDATION_FN_HEAD)]) != \
            VALIDATION_FN_HEAD:
        raise SystemExit("eye validator function identity changed")
    guard_off = fn_off + 0x1C
    if bytes(blob[guard_off:guard_off + len(VALIDATION_GUARD)]) != \
            VALIDATION_GUARD:
        raise SystemExit("eye validator done-flag guard changed")
    guard_target = VALIDATION_FN_RVA + 0x1C + 7 + struct.unpack_from(
        "<i", VALIDATION_GUARD, 2)[0]
    if guard_target != VALIDATION_DONE_RVA:
        raise SystemExit("eye validator flag decode changed")

    payload_off = rva_off(pe, PAYLOAD_RVA)
    payload_end = rva_off(pe, PAYLOAD_LIMIT - 1) + 1
    if any(blob[payload_off:payload_end]):
        raise SystemExit("payload region is not free")
    try:
        code, symbols = build_payload(
            Path(__file__).parent / "stage3ca_h4_cutscene_eye_probe.S",
            PAYLOAD_RVA,
            defs={"S3BX_STATE": S3BX_STATE_RVA,
                  "VALIDATION_DONE": VALIDATION_DONE_RVA,
                  "S3BX_THUNK": S3BX_THUNK_RVA},
            want_symbols=("s3ca_probe",))
    except subprocess.CalledProcessError:
        code, symbols = build_fixed_probe()
        print("archived clang unavailable; used verified fixed 50-byte encoding")
    if PAYLOAD_RVA + len(code) > PAYLOAD_LIMIT:
        raise SystemExit(f"payload too large: {len(code)} bytes")
    thunk = symbols["s3ca_probe"]
    blob[payload_off:payload_off + len(code)] = code
    blob[splice_off:splice_off + 6] = b"\x0F\x85" + rel32(
        thunk, SPLICE_RVA + 6)
    output.write_bytes(blob)
    print(f"payload {len(code)} bytes at {PAYLOAD_RVA:#x}")
    print(f"splice {SPLICE_RVA:#x}: {S3BX_THUNK_RVA:#x} -> {thunk:#x}")
    print("output", hashlib.sha256(blob).hexdigest(), len(blob))


if __name__ == "__main__":
    main()
