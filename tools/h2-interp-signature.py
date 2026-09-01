"""Build a signature for the retail interpolated first-person frame getter
(0x7226F0) and count matches the way the RUNTIME scanner does.

The first attempt failed in the headset because uniqueness was checked
against the raw FILE while `CountPatternMatches` walks the LOADED IMAGE
(sections at their virtual addresses, with alignment gaps). A 24-byte
generic prologue matched twice there and the hook correctly refused.

This lays the sections out at their RVAs, exactly as the loader does, and
counts matches of increasing prefix lengths so the shortest UNIQUE pattern
can be chosen with its distinctive constants (0x38 bank stride, the
0x1A39D44 / 0x1A39D48 bases) included.
"""
from pathlib import Path
import struct
import sys

DLL = Path(r"N:\SteamLibrary\steamapps\common\Halo The Master Chief Collection"
           r"\halo2\halo2.dll")
GETTER_RVA = 0x7226F0


def parse(blob):
    pe = struct.unpack_from("<I", blob, 0x3C)[0]
    coff = pe + 4
    count = struct.unpack_from("<H", blob, coff + 2)[0]
    optional_size = struct.unpack_from("<H", blob, coff + 16)[0]
    optional = coff + 20
    image_size = struct.unpack_from("<I", blob, optional + 56)[0]
    table = optional + optional_size
    sections = []
    for index in range(count):
        entry = table + index * 40
        virtual_size, virtual_address, raw_size, raw_pointer = \
            struct.unpack_from("<IIII", blob, entry + 8)
        sections.append((virtual_address, virtual_size, raw_size, raw_pointer))
    return image_size, sections


def build_image(blob, image_size, sections):
    image = bytearray(image_size)
    for virtual_address, virtual_size, raw_size, raw_pointer in sections:
        size = min(raw_size, max(virtual_size, raw_size))
        chunk = blob[raw_pointer:raw_pointer + size]
        image[virtual_address:virtual_address + len(chunk)] = chunk
    return bytes(image)


def main():
    blob = DLL.read_bytes()
    image_size, sections = parse(blob)
    image = build_image(blob, image_size, sections)
    print(f"loaded-image layout: {image_size:#x} bytes")

    body = image[GETTER_RVA:GETTER_RVA + 64]
    print("bytes at 0x%X:" % GETTER_RVA)
    print("  " + " ".join(f"{b:02X}" for b in body))

    print("\nmatch counts over the LOADED IMAGE by prefix length:")
    chosen = None
    for length in range(16, 57, 4):
        pattern = body[:length]
        count = image.count(pattern)
        flag = ""
        if count == 1 and chosen is None:
            chosen = length
            flag = "   <-- shortest unique"
        print(f"  {length:2d} bytes -> {count} match(es){flag}")

    if chosen is None:
        print("\nNo unique prefix within 56 bytes.")
        return 1

    pattern = body[:chosen]
    where = image.find(pattern)
    print(f"\nchosen {chosen}-byte signature, unique, at RVA 0x{where:X} "
          f"({'matches the verified getter' if where == GETTER_RVA else 'MISMATCH'})")
    text = " ".join(f"{b:02X}" for b in pattern)
    print("\npattern string for the runtime scanner:")
    for start in range(0, len(text), 48):
        print(f'    "{text[start:start + 48].strip()} "')
    print("\nC array:")
    rows = [", ".join(f"0x{b:02X}" for b in pattern[i:i + 10])
            for i in range(0, len(pattern), 10)]
    print("        " + ",\n        ".join(rows))
    return 0


if __name__ == "__main__":
    sys.exit(main())
