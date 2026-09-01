"""Verify Stage 3AW's H4EK-derived pause-reason binding in retail modules."""

from pathlib import Path
import hashlib
import struct
import sys


EXPECTED_HASHES = {
    "7c53e7d5bc9848545a1b70e2768242479336fba1b7630d7ab955f7fd0c34fa84",
    "5767cd564c1e8e8d012d002a8de8e92960a3de46442399ed054e3c4ef44aa496",
}
EXPECTED_TIMESTAMP = 0x68A0E7BF
EXPECTED_IMAGE_SIZE = 0x04A3F000
GETTER_RVA = 0x000A0AE4
GETTER_BODY_SHA256 = \
    "4a7ba392baaf1ac6fc0aad577ba8ecba13d43d875602a17195a65459e868b5a2"

SIGNATURE = bytes.fromhex(
    "8B 15 00 00 00 00 65 48 8B 04 25 58 00 00 00 "
    "41 B8 90 00 00 00 48 8B 04 D0 49 8B 14 00 "
    "32 C0 38 02 74 0F B8 01 00 00 00 66 D3 E0 "
    "66 85 42 02 0F 95 C0 C3")
MASK = bytes([1, 1, 0, 0, 0, 0] + [1] * (len(SIGNATURE) - 6))


def pe_identity(blob):
    pe = struct.unpack_from("<I", blob, 0x3C)[0]
    assert blob[:2] == b"MZ" and blob[pe:pe + 4] == b"PE\0\0"
    coff = pe + 4
    count = struct.unpack_from("<H", blob, coff + 2)[0]
    timestamp = struct.unpack_from("<I", blob, coff + 4)[0]
    optional_size = struct.unpack_from("<H", blob, coff + 16)[0]
    optional = coff + 20
    image_size = struct.unpack_from("<I", blob, optional + 0x38)[0]
    section_table = optional + optional_size
    sections = []
    for index in range(count):
        offset = section_table + index * 40
        name = bytes(blob[offset:offset + 8]).split(b"\0", 1)[0].decode()
        virtual_size, virtual_address, raw_size, raw_pointer = \
            struct.unpack_from("<IIII", blob, offset + 8)
        sections.append((name, virtual_size, virtual_address,
                         raw_size, raw_pointer))
    return timestamp, image_size, sections


def verify(path):
    blob = path.read_bytes()
    digest = hashlib.sha256(blob).hexdigest()
    assert digest in EXPECTED_HASHES, f"unexpected halo4.dll hash: {digest}"
    timestamp, image_size, sections = pe_identity(blob)
    assert timestamp == EXPECTED_TIMESTAMP
    assert image_size == EXPECTED_IMAGE_SIZE
    text = next(section for section in sections if section[0] == ".text")
    _name, virtual_size, virtual_address, raw_size, raw_pointer = text
    data = blob[raw_pointer:raw_pointer + min(virtual_size, raw_size)]
    hits = []
    for offset in range(0, len(data) - len(SIGNATURE) + 1):
        candidate = data[offset:offset + len(SIGNATURE)]
        if all(not check or candidate[index] == SIGNATURE[index]
               for index, check in enumerate(MASK)):
            hits.append(virtual_address + offset)
    assert hits == [GETTER_RVA], \
        "pause-reason getter hits: " + ", ".join(hex(hit) for hit in hits)
    body = data[GETTER_RVA - virtual_address:
                GETTER_RVA - virtual_address + len(SIGNATURE)]
    assert hashlib.sha256(body).hexdigest() == GETTER_BODY_SHA256
    print(path)
    print("  sha256", digest)
    print("  unique pause-reason getter RVA 0xA0AE4: PASS")
    return digest


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: verify_stage3aw_h4_pause_binding.py "
                         "<Steam halo4.dll> <Store halo4.dll>")
    found = {verify(Path(argument)) for argument in sys.argv[1:]}
    assert found == EXPECTED_HASHES, "both pinned retail editions are required"


if __name__ == "__main__":
    main()
