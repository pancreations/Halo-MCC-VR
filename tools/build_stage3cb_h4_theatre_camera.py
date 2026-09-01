"""Build Stage 3CB directly from the accepted Stage 3BX DLL."""
from pathlib import Path
import hashlib
import struct
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).parent))
from postlink import build_payload
from build_stage3bz_h4_authored_camera import parse_pe, rva_off, rel32

EXPECTED_INPUT_SHA256 = (
    "54130fd5a37b2d5e19e610aad712fa8595501849cc27ead7753b132de7dfbd9e")
CALL_SITE_RVA = 0x00056EBF
CALL_SITE_OLD = bytes.fromhex("e89c7bfdff")
SNAPSHOT_FN_RVA = 0x0002EA60
THEATRE_ACTIVE_RVA = 0x0002EE60
THEATRE_ACTIVE_BODY = bytes.fromhex("0fb60515fb270090c3")
THEATRE_DEPTH_RVA = 0x002AB1FC
THEATRE_DEPTH_REFERENCE_RVA = 0x0006DBEA
THEATRE_DEPTH_REFERENCE = bytes.fromhex("f30f10350ad62300")
PAYLOAD_RVA = 0x002FA36C
PAYLOAD_LIMIT = 0x002FA400


class Encoder:
    def __init__(self):
        self.code = bytearray()
        self.labels = {}
        self.rel8_fixups = []

    def emit(self, value):
        self.code += bytes.fromhex(value)

    def label(self, name):
        self.labels[name] = len(self.code)

    def rel8(self, opcode, label):
        self.emit(opcode + " 00")
        self.rel8_fixups.append((len(self.code) - 1, label))

    def rel32(self, opcode, target):
        self.emit(opcode)
        next_rva = PAYLOAD_RVA + len(self.code) + 4
        self.code += rel32(target, next_rva)

    def rip32(self, prefix, target):
        self.emit(prefix)
        next_rva = PAYLOAD_RVA + len(self.code) + 4
        self.code += rel32(target, next_rva)

    def finish(self):
        for at, label in self.rel8_fixups:
            displacement = self.labels[label] - (at + 1)
            if not -128 <= displacement <= 127:
                raise SystemExit(f"short branch to {label} is out of range")
            self.code[at] = displacement & 0xFF
        return bytes(self.code)


def build_fixed_payload():
    e = Encoder()
    e.emit("48 83 EC 28")
    e.emit("48 89 4C 24 20")
    e.rel32("E8", SNAPSHOT_FN_RVA)
    e.emit("84 C0")
    e.rel8("74", "done")
    e.rel32("E8", THEATRE_ACTIVE_RVA)
    e.emit("84 C0")
    e.rel8("74", "success")
    e.emit("48 8B 4C 24 20")
    e.rip32("F3 0F 10 05", THEATRE_DEPTH_RVA)
    e.emit("0F 2E C0")
    e.rel8("7B", "finite")
    e.emit("B8 00 00 80 3F")
    e.emit("66 0F 6E C0")
    e.rel8("EB", "scale")
    e.label("finite")
    e.emit("0F 57 C9")
    e.emit("F3 0F 5F C1")
    two_fixup = len(e.code)
    e.emit("F3 0F 5D 05 00 00 00 00")
    e.label("scale")
    e.emit("0F C6 C0 00")
    e.emit("0F 10 49 08 0F 59 C8 0F 11 49 08")
    e.emit("0F 10 49 38 0F 59 C8 0F 11 49 38")
    e.emit("31 D2 48 89 51 14 89 51 1C")
    e.emit("B8 00 00 80 3F 89 41 20")
    e.emit("48 89 51 44 89 51 4C 89 41 50")
    e.emit("88 51 34 88 51 64 88 91 84 00 00 00")
    e.label("success")
    e.emit("B0 01")
    e.label("done")
    e.emit("48 83 C4 28 C3")
    while len(e.code) & 3:
        e.emit("90")
    two_rva = PAYLOAD_RVA + len(e.code)
    e.emit("00 00 00 40")
    struct.pack_into(
        "<i", e.code, two_fixup + 4,
        two_rva - (PAYLOAD_RVA + two_fixup + 8))
    return e.finish(), {"s3cb_get_render_snapshot": PAYLOAD_RVA}


def main():
    source, output = Path(sys.argv[1]), Path(sys.argv[2])
    blob = bytearray(source.read_bytes())
    digest = hashlib.sha256(blob).hexdigest()
    print("input", digest)
    if digest != EXPECTED_INPUT_SHA256:
        raise SystemExit("wrong input DLL: " + digest)
    pe = parse_pe(blob)
    call_off = rva_off(pe, CALL_SITE_RVA)
    if bytes(blob[call_off:call_off + 5]) != CALL_SITE_OLD:
        raise SystemExit("snapshot callsite changed")
    if CALL_SITE_RVA + 5 + struct.unpack_from(
            "<i", CALL_SITE_OLD, 1)[0] != SNAPSHOT_FN_RVA:
        raise SystemExit("snapshot call target changed")
    active_off = rva_off(pe, THEATRE_ACTIVE_RVA)
    if bytes(blob[active_off:active_off + len(THEATRE_ACTIVE_BODY)]) != \
            THEATRE_ACTIVE_BODY:
        raise SystemExit("theatre-active helper changed")
    depth_ref_off = rva_off(pe, THEATRE_DEPTH_REFERENCE_RVA)
    if bytes(blob[depth_ref_off:depth_ref_off + 8]) != THEATRE_DEPTH_REFERENCE:
        raise SystemExit("existing theatre-depth load changed")
    if (THEATRE_DEPTH_REFERENCE_RVA + 8 + struct.unpack_from(
            "<i", THEATRE_DEPTH_REFERENCE, 4)[0]) != THEATRE_DEPTH_RVA:
        raise SystemExit("existing theatre-depth load targets another field")
    payload_off = rva_off(pe, PAYLOAD_RVA)
    payload_end = rva_off(pe, PAYLOAD_LIMIT - 1) + 1
    if any(blob[payload_off:payload_end]):
        raise SystemExit("payload region is not free")

    try:
        code, symbols = build_payload(
            Path(__file__).parent / "stage3cb_h4_theatre_camera.S",
            PAYLOAD_RVA,
            defs={"SNAPSHOT_FN": SNAPSHOT_FN_RVA,
                  "THEATRE_ACTIVE": THEATRE_ACTIVE_RVA,
                  "THEATRE_DEPTH": THEATRE_DEPTH_RVA},
            want_symbols=("s3cb_get_render_snapshot",))
    except subprocess.CalledProcessError:
        code, symbols = build_fixed_payload()
        print("archived clang unavailable; used verified fixed encoding")
    if PAYLOAD_RVA + len(code) > PAYLOAD_LIMIT:
        raise SystemExit(f"payload too large: {len(code)} bytes")
    thunk = symbols["s3cb_get_render_snapshot"]
    blob[payload_off:payload_off + len(code)] = code
    blob[call_off:call_off + 5] = b"\xE8" + rel32(
        thunk, CALL_SITE_RVA + 5)
    output.write_bytes(blob)
    print(f"payload {len(code)} bytes at {PAYLOAD_RVA:#x}")
    print("output", hashlib.sha256(blob).hexdigest(), len(blob))


if __name__ == "__main__":
    main()
