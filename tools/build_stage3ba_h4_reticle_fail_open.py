"""Stage 3BA: disable the headset-failed optional Halo 4 CUI reticle hook."""

from pathlib import Path
import hashlib
import struct
import sys

EXPECTED_STAGE3AZ = (
    "76727356f3ce4b053b09eccde6783a82ea5249d7ebfb9df9bc949b8822affb10"
)


def sections(blob):
    pe = struct.unpack_from("<I", blob, 0x3C)[0]
    count = struct.unpack_from("<H", blob, pe + 6)[0]
    optional_size = struct.unpack_from("<H", blob, pe + 20)[0]
    table = pe + 24 + optional_size
    out = []
    for index in range(count):
        header = table + index * 40
        vs, va, rs, rp = struct.unpack_from("<IIII", blob, header + 8)
        out.append((vs, va, rs, rp))
    return out


def offset(image_sections, rva):
    for vs, va, rs, rp in image_sections:
        if va <= rva < va + max(vs, rs):
            return rp + rva - va
    raise KeyError(hex(rva))


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: build_stage3ba_h4_reticle_fail_open.py <Stage3AZ.dll> <output.dll>")
    source, output = Path(sys.argv[1]), Path(sys.argv[2])
    blob = bytearray(source.read_bytes())
    digest = hashlib.sha256(blob).hexdigest()
    if digest != EXPECTED_STAGE3AZ:
        raise SystemExit("wrong Stage3AZ input: " + digest)
    image_sections = sections(blob)
    patches = (
        (0x599C2, bytes.fromhex("e8 49 01 00 00"), "initial optional install"),
        (0x843A8, bytes.fromhex("e8 63 57 fd ff"), "optional retry install"),
    )
    for rva, expected, label in patches:
        start = offset(image_sections, rva)
        actual = bytes(blob[start:start + 5])
        if actual != expected:
            raise SystemExit(f"{label}: expected {expected.hex()} at 0x{rva:X}, got {actual.hex()}")
        blob[start:start + 5] = b"\x90" * 5
        print(f"{label} disabled at 0x{rva:X}")
    output.write_bytes(blob)
    print("output", hashlib.sha256(blob).hexdigest(), len(blob))


if __name__ == "__main__":
    main()
