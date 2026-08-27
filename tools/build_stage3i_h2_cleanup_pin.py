from pathlib import Path
import hashlib
import shutil
import struct
import subprocess
import sys
import tempfile

EXPECTED_STAGE3H_SHA256 = "0d0338b7c0b749ce8c7f956914eace9812bcc2d5d26bda14c4024043416f6249"
IMAGE_BASE = 0x180000000
NEW_CODE_RVA = 0x002F1000
CLEANUP_PIN_RVA = 0x002F0018
MODULE_REFERENCE_RVA = 0x002BAB38
IAT_GET_MODULE_HANDLE_EX_RVA = 0x00180160
IAT_FREE_LIBRARY_RVA = 0x00180230
RESTORE_FP_RVA = 0x000304D0

# Exact Stage 3H patch sites, independently disassembled from the accepted input.
RESTORE_CALL_RVA = 0x00090AC4
EXPECTED_RESTORE_CALL = bytes.fromhex("E8 07 FA F9 FF")
CLEAR_REFERENCE_RVA = 0x00090CE2
EXPECTED_CLEAR_REFERENCE = bytes.fromhex("4C 89 35 4F 9E 22 00")


def align(value, alignment):
    return (value + alignment - 1) // alignment * alignment


def run(command):
    subprocess.run(command, check=True)


def parse_pe(blob):
    if blob[:2] != b"MZ":
        raise SystemExit("input is not MZ")
    pe_offset = struct.unpack_from("<I", blob, 0x3C)[0]
    if blob[pe_offset:pe_offset + 4] != b"PE\0\0":
        raise SystemExit("input is not PE")
    coff = pe_offset + 4
    section_count = struct.unpack_from("<H", blob, coff + 2)[0]
    optional_size = struct.unpack_from("<H", blob, coff + 16)[0]
    optional = coff + 20
    if struct.unpack_from("<H", blob, optional)[0] != 0x20B:
        raise SystemExit("input is not PE32+")
    section_table = optional + optional_size
    sections = []
    for index in range(section_count):
        offset = section_table + index * 40
        name = bytes(blob[offset:offset + 8]).split(b"\0", 1)[0].decode("ascii")
        virtual_size, virtual_address, raw_size, raw_pointer = struct.unpack_from(
            "<IIII", blob, offset + 8)
        sections.append({
            "name": name, "virtual_size": virtual_size,
            "virtual_address": virtual_address, "raw_size": raw_size,
            "raw_pointer": raw_pointer, "header": offset,
        })
    return {
        "coff": coff, "optional": optional, "section_table": section_table,
        "section_count": section_count, "sections": sections,
        "file_alignment": struct.unpack_from("<I", blob, optional + 0x24)[0],
        "section_alignment": struct.unpack_from("<I", blob, optional + 0x20)[0],
        "size_headers": struct.unpack_from("<I", blob, optional + 0x3C)[0],
    }


def rva_offset(pe, rva):
    for section in pe["sections"]:
        start = section["virtual_address"]
        extent = max(section["virtual_size"], section["raw_size"])
        if start <= rva < start + extent:
            return section["raw_pointer"] + (rva - start)
    raise KeyError(hex(rva))


def guarded_patch(blob, pe, rva, expected, replacement, label):
    offset = rva_offset(pe, rva)
    actual = bytes(blob[offset:offset + len(expected)])
    if actual != expected:
        raise SystemExit(
            f"{label}: expected {expected.hex()} at RVA 0x{rva:X}, got {actual.hex()}"
        )
    if len(replacement) != len(expected):
        raise SystemExit(f"{label}: replacement length mismatch")
    blob[offset:offset + len(expected)] = replacement
    print(f"{label}: RVA 0x{rva:X}")


def rel_call(source_rva, target_rva):
    return b"\xE8" + struct.pack("<i", target_rva - (source_rva + 5))


def assemble(tool_dir, temporary):
    required = ("as", "ld", "objcopy", "nm")
    missing = [name for name in required if shutil.which(name) is None]
    if missing:
        raise SystemExit("Stage 3I helper assembly requires GNU binutils: " + ", ".join(missing))
    obj = temporary / "stage3i.o"
    elf = temporary / "stage3i.elf"
    raw = temporary / "stage3i.bin"
    run(["as", "--64", "-o", str(obj), str(tool_dir / "stage3i_h2_cleanup_pin.S")])
    defs = {
        "CLEANUP_PIN_RVA": CLEANUP_PIN_RVA,
        "MODULE_REFERENCE_RVA": MODULE_REFERENCE_RVA,
        "IAT_GET_MODULE_HANDLE_EX_RVA": IAT_GET_MODULE_HANDLE_EX_RVA,
        "IAT_FREE_LIBRARY_RVA": IAT_FREE_LIBRARY_RVA,
        "RESTORE_FP_RVA": RESTORE_FP_RVA,
    }
    link = ["ld", "-m", "elf_x86_64", f"-Ttext=0x{NEW_CODE_RVA:x}",
            "-e", "stage3i_restore_fp_and_pin"]
    for key, value in defs.items():
        link.extend(("--defsym", f"{key}=0x{value:x}"))
    link.extend(("-o", str(elf), str(obj)))
    run(link)
    run(["objcopy", "-O", "binary", "-j", ".text", str(elf), str(raw)])
    symbols = {}
    for line in subprocess.check_output(["nm", "-n", str(elf)], text=True).splitlines():
        fields = line.split()
        if len(fields) == 3 and fields[2].startswith("stage3i_"):
            symbols[fields[2]] = int(fields[0], 16)
    required_symbols = {"stage3i_restore_fp_and_pin", "stage3i_release_pin_and_clear_ref"}
    if not required_symbols.issubset(symbols):
        raise SystemExit("Stage 3I helper symbol extraction failed")
    return raw.read_bytes(), symbols


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: build_stage3i_h2_cleanup_pin.py <Stage3H-HaloMCCVR.dll> <output.dll>")
    source = Path(sys.argv[1])
    output = Path(sys.argv[2])
    input_bytes = source.read_bytes()
    input_sha = hashlib.sha256(input_bytes).hexdigest()
    if input_sha != EXPECTED_STAGE3H_SHA256:
        raise SystemExit("wrong Stage 3H input DLL: " + input_sha)

    tool_dir = Path(__file__).resolve().parent
    with tempfile.TemporaryDirectory(prefix="halomccvr-stage3i-") as name:
        helper, symbols = assemble(tool_dir, Path(name))
    if len(helper) > 0x200:
        raise SystemExit("Stage 3I helper exceeds one file-alignment block")

    blob = bytearray(input_bytes)
    pe = parse_pe(blob)
    if pe["section_count"] != 10:
        raise SystemExit("Stage 3H section count mismatch")
    by_name = {s["name"]: s for s in pe["sections"]}
    data = by_name.get(".s3hd")
    if not data or data["virtual_address"] != 0x002F0000 or data["raw_size"] != 0x200:
        raise SystemExit("Stage 3H .s3hd layout mismatch")
    if data["virtual_size"] != 0x18:
        raise SystemExit("Stage 3H .s3hd virtual size mismatch")

    # The pin lives in the zero-filled tail of Stage 3H's writable helper data.
    pin_offset = data["raw_pointer"] + (CLEANUP_PIN_RVA - data["virtual_address"])
    if bytes(blob[pin_offset:pin_offset + 8]) != b"\0" * 8:
        raise SystemExit("Stage 3I cleanup-pin slot is not zero")
    struct.pack_into("<I", blob, data["header"] + 8, 0x20)

    file_alignment = pe["file_alignment"]
    section_alignment = pe["section_alignment"]
    header = pe["section_table"] + pe["section_count"] * 40
    if header + 40 > pe["size_headers"]:
        raise SystemExit("no room for Stage 3I section header")
    expected_rva = align(
        max(s["virtual_address"] + max(s["virtual_size"], s["raw_size"])
            for s in pe["sections"]), section_alignment)
    if expected_rva != NEW_CODE_RVA:
        raise SystemExit(f"unexpected Stage 3I code RVA 0x{expected_rva:X}")
    raw_pointer = align(
        max(s["raw_pointer"] + s["raw_size"] for s in pe["sections"]),
        file_alignment)
    if raw_pointer < len(blob):
        raise SystemExit("Stage 3I section would overwrite overlay data")
    raw_size = align(len(helper), file_alignment)
    blob.extend(b"\0" * (raw_pointer - len(blob)))
    blob.extend(helper.ljust(raw_size, b"\0"))

    blob[header:header + 8] = b".s3ic\0\0\0"
    struct.pack_into("<IIIIIIHHI", blob, header + 8,
                     len(helper), NEW_CODE_RVA, raw_size, raw_pointer,
                     0, 0, 0, 0, 0x60000020)
    struct.pack_into("<H", blob, pe["coff"] + 2, pe["section_count"] + 1)
    size_of_image = align(NEW_CODE_RVA + len(helper), section_alignment)
    struct.pack_into("<I", blob, pe["optional"] + 0x38, size_of_image)

    guarded_patch(
        blob, pe, RESTORE_CALL_RVA, EXPECTED_RESTORE_CALL,
        rel_call(RESTORE_CALL_RVA, symbols["stage3i_restore_fp_and_pin"]),
        "Classic cleanup pin acquire wrapper")
    guarded_patch(
        blob, pe, CLEAR_REFERENCE_RVA, EXPECTED_CLEAR_REFERENCE,
        rel_call(CLEAR_REFERENCE_RVA, symbols["stage3i_release_pin_and_clear_ref"]) + b"\x90\x90",
        "Classic cleanup pin release + reference clear")

    # PE checksum is intentionally cleared, matching the Stage 3H builder.
    struct.pack_into("<I", blob, pe["optional"] + 0x40, 0)
    output.write_bytes(blob)
    print("Stage 3I output SHA-256:", hashlib.sha256(blob).hexdigest())
    print("Stage 3I output size:", len(blob))
    print("helper bytes:", len(helper))


if __name__ == "__main__":
    main()
