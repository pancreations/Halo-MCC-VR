"""Stage 3AY: H4 native-reticle differential capture + ODST core isolation."""

from pathlib import Path
import hashlib
import struct
import sys

sys.path.insert(0, str(Path(__file__).parent))
from postlink import build_payload  # noqa: E402


EXPECTED_STAGE3AX_SHA256 = (
    "0d32751585670c28bb7b98110a35b04817ec4f683fcc3ab3301c0941a4613053"
)
CODE_RVA = 0x2FB000
PAGE_SIZE = 0x1000

DEFS = {
    "BEGIN_CAPTURE_INTERNAL_RVA": 0x11A70,
    "BEGIN_AUTHORED_CAPTURE_RVA": 0x2B7A0,
    "END_CUI_REDIRECT_RVA": 0x544C0,
    "HALO4_SAFE_WRITE_RVA": 0x56C40,
    "CAPTURE_FINISH_CONTINUE_RVA": 0x53E9A,
    "LOGGER_RVA": 0x1D90,
    "G_CONTEXT_RVA": 0x2AE298,
    "G_DEVICE_RVA": 0x2AF290,
    "G_DISCARD_TEXTURE_RVA": 0x2AE450,
    "G_DISCARD_RTV_RVA": 0x2AE458,
    "G_ACTIVE_TITLE_RVA": 0x2BA6C8,
    "G_TLS_INDEX_RVA": 0x2D5E48,
    "D3D_BACKUP_RESTORE_RVA": 0x24400,
}

PATCHES = (
    (0x53636, bytes.fromhex(
        "66 c7 43 04 01 00 c6 43 06 00 44 88 64 24 28"),
     "stage3ay_prepare_full_replay", 15, "full replay pass kind"),
    (0x536A9, bytes.fromhex("8b d7 8b ce 41 ff d6 90"),
     "stage3ay_second_replay", 8, "hidden-reticle replay"),
    (0x538E0, bytes.fromhex("e8 bb 7e fd ff"),
     "stage3ay_capture_begin", 5, "capture kind chooser"),
    (0x53A70, bytes.fromhex("41 0f b6 c7 e9 21 04 00 00"),
     "stage3ay_capture_finish", 9, "suppression transform write"),
    (0x12121, bytes.fromhex(
        "48 8b 0d 70 c1 29 00 45 33 c9 45 33 c0 48 8b d3 48 8b 01 ff 50 48"),
     "stage3ay_select_difference_shader", 22, "two-source reticle shader"),
    (0x12182, bytes.fromhex(
        "48 8b 15 0f c1 29 00 48 8d 4d 80 e8 6e 22 01 00"),
     "stage3ay_restore_difference_resource", 16,
     "restore title pixel-resource slot 1"),
    (0x5C368, bytes.fromhex("e8 83 2c 00 00"),
     "stage3ay_skip_odst_cinematic_scan", 5, "ODST optional scan isolation"),
)

DIRECT_PATCHES = (
    (0x538EE, bytes.fromhex("66 41 c7 44 07 02 01 01"),
     bytes.fromhex("43 c6 44 07 02 01 90 90"),
     "preserve requested capture kind"),
    (0x539C0, bytes.fromhex("e8 db 6f 2a 00 90 90 90 90 90"),
     bytes.fromhex("e8 e5 6d 2a 00 90 90 90 90 90"),
     "disable rejected AX zoom; restore AW framing"),
    (0x67CE5, b"\xEB", b"\x75",
     "restore proven ODST readiness edge"),
    (0x2F370, b"\x20", b"\x28",
     "discard texture RTV plus SRV binding"),
)


def parse_pe(blob):
    pe = struct.unpack_from("<I", blob, 0x3C)[0]
    if blob[:2] != b"MZ" or blob[pe:pe + 4] != b"PE\0\0":
        raise SystemExit("input is not PE")
    coff = pe + 4
    count = struct.unpack_from("<H", blob, coff + 2)[0]
    optional_size = struct.unpack_from("<H", blob, coff + 16)[0]
    optional = coff + 20
    table = optional + optional_size
    sections = []
    for index in range(count):
        header = table + index * 40
        name = bytes(blob[header:header + 8]).split(b"\0", 1)[0].decode("ascii")
        virtual_size, virtual_address, raw_size, raw_pointer = struct.unpack_from(
            "<IIII", blob, header + 8)
        sections.append(dict(
            name=name, vs=virtual_size, va=virtual_address,
            rs=raw_size, rp=raw_pointer, header=header))
    return dict(optional=optional, sections=sections)


def rva_offset(pe, rva):
    for section in pe["sections"]:
        if section["va"] <= rva < section["va"] + max(section["vs"], section["rs"]):
            return section["rp"] + rva - section["va"]
    raise KeyError(hex(rva))


def guard(blob, pe, rva, expected, label):
    offset = rva_offset(pe, rva)
    actual = bytes(blob[offset:offset + len(expected)])
    if actual != expected:
        raise SystemExit(
            f"{label}: expected {expected.hex()} at 0x{rva:X}, got {actual.hex()}")
    return offset


def rel_call(source, target):
    return b"\xE8" + struct.pack("<i", target - (source + 5))


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: build_stage3ay_h4_reticle_difference_odst_isolation.py <Stage3AX.dll> <output.dll>")
    source, output = Path(sys.argv[1]), Path(sys.argv[2])
    blob = bytearray(source.read_bytes())
    digest = hashlib.sha256(blob).hexdigest()
    print("input", digest)
    if digest != EXPECTED_STAGE3AX_SHA256:
        raise SystemExit("wrong Stage3AX input: " + digest)
    pe = parse_pe(blob)
    shader = Path(__file__).with_name("stage3ay_h4_reticle_difference.cso")
    if shader.stat().st_size != 1000:
        raise SystemExit("unexpected Stage3AY shader bytecode size")
    qd = next((s for s in pe["sections"] if s["name"] == ".s3qd"), None)
    if not qd or qd["va"] + qd["vs"] != CODE_RVA or \
            qd["rp"] + qd["rs"] != len(blob):
        raise SystemExit("Stage3AX .s3qd is not the expected final section")

    code, symbols = build_payload(
        Path(__file__).with_name(
            "stage3ay_h4_reticle_difference_odst_isolation.S"),
        CODE_RVA, DEFS,
        tuple(symbol for _, _, symbol, _, _ in PATCHES))
    if len(code) > PAGE_SIZE:
        raise SystemExit(f"Stage3AY payload exceeds one page: {len(code)}")
    print("payload", len(code), hashlib.sha256(code).hexdigest())

    for rva, expected, symbol, size, label in PATCHES:
        offset = guard(blob, pe, rva, expected, label)
        replacement = rel_call(rva, symbols[symbol])
        if len(replacement) > size:
            raise SystemExit(label + ": replacement does not fit")
        blob[offset:offset + size] = replacement + b"\x90" * (size - 5)
        print(label, f"0x{rva:X} -> 0x{symbols[symbol]:X}")
    for rva, expected, replacement, label in DIRECT_PATCHES:
        offset = guard(blob, pe, rva, expected, label)
        blob[offset:offset + len(replacement)] = replacement
        print(label, f"0x{rva:X}")

    blob.extend(code.ljust(PAGE_SIZE, b"\0"))
    struct.pack_into("<I", blob, qd["header"] + 8, qd["vs"] + PAGE_SIZE)
    struct.pack_into("<I", blob, qd["header"] + 16, qd["rs"] + PAGE_SIZE)
    struct.pack_into("<I", blob, pe["optional"] + 0x38, CODE_RVA + PAGE_SIZE)
    size_of_code = struct.unpack_from("<I", blob, pe["optional"] + 4)[0]
    struct.pack_into("<I", blob, pe["optional"] + 4, size_of_code + PAGE_SIZE)
    struct.pack_into("<I", blob, pe["optional"] + 0x40, 0)
    output.write_bytes(blob)
    print("output", hashlib.sha256(blob).hexdigest(), len(blob))


if __name__ == "__main__":
    main()
