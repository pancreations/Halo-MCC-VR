from pathlib import Path
import hashlib
import shutil
import struct
import subprocess
import sys
import tempfile


EXPECTED_STAGE3G_SHA256 = (
    "40291c25af302046675d55f680d9f656105b766b388c72078ea4a15832adcd6e"
)
H2_CODE_RVA = 0x002EE000
H4_CODE_RVA = 0x002EF000
H4_DATA_RVA = 0x002F0000


def align(value, alignment):
    return (value + alignment - 1) // alignment * alignment


def run(command):
    subprocess.run(command, check=True)


def assemble_helpers(tool_dir, temporary):
    required = ("as", "ld", "objcopy", "nm")
    missing = [name for name in required if shutil.which(name) is None]
    if missing:
        raise SystemExit(
            "Stage 3H helper assembly requires GNU binutils on PATH: "
            + ", ".join(missing)
        )

    h2_object = temporary / "stage3h_h2_viewport.o"
    h2_elf = temporary / "stage3h_h2_viewport.elf"
    h2_binary = temporary / "stage3h_h2_viewport.bin"
    run(["as", "--64", "-o", str(h2_object),
         str(tool_dir / "stage3h_h2_viewport.S")])
    run([
        "ld", "-m", "elf_x86_64", "-Ttext=0x2ee000",
        "-e", "stage3h_h2_viewport",
        "--defsym", "HUD_SIZE_RVA=0x2adc28",
        "--defsym", "HUD_ASPECT_RVA=0x2adc2c",
        "--defsym", "HUD_VERTICAL_RVA=0x2adc34",
        "--defsym", "STATE_FAILURES_RVA=0x2ae0a8",
        "-o", str(h2_elf), str(h2_object),
    ])
    run(["objcopy", "-O", "binary", "-j", ".text",
         str(h2_elf), str(h2_binary)])

    h4_object = temporary / "stage3h_h4_native_reticle.o"
    h4_elf = temporary / "stage3h_h4_native_reticle.elf"
    h4_binary = temporary / "stage3h_h4_native_reticle.bin"
    run(["as", "--64", "-o", str(h4_object),
         str(tool_dir / "stage3h_h4_native_reticle.S")])
    definitions = {
        "CORE_BASE_PTR_RVA": 0x2A71F0,
        "CONFIG_CROSSHAIR_RVA": 0x2ADBB1,
        "CONFIG_DISTANCE_RVA": 0x2ADBB4,
        "CONFIG_SIZE_RVA": 0x2ADBB8,
        "STEREO_EYE_RVA": 0x2A67EC,
        "WORLD_SCALE_RVA": 0x2A67F0,
        "ENGINE_AIM_RVA": 0x2A83D8,
        "CUI_SERIAL_RVA": 0x2A8378,
        "HALF_FOV_X_RVA": 0x2B9B28,
        "HALF_FOV_Y_RVA": 0x2B9B30,
        "RENDER_SERIAL_RVA": 0x2B9B38,
        "CENTER_ROOT_RVA": 0x2B9B50,
        "EYE_ROOT_RVA": 0x2B9BB8,
        "SAFE_READ_RVA": 0x056C10,
        "TITLE_ACTIVE_RVA": 0x0879C0,
        "TANF_RVA": 0x156EE0,
        "IAT_GET_CURRENT_PROCESS_RVA": 0x180140,
        "IAT_FLUSH_INSTRUCTION_CACHE_RVA": 0x1801E8,
        "IAT_VIRTUAL_PROTECT_RVA": 0x180200,
        "VIEWPORT_HALF_WIDTH_RVA": H4_DATA_RVA,
        "VIEWPORT_HALF_HEIGHT_RVA": H4_DATA_RVA + 4,
        "EFFECT_INSTALL_LOCK_RVA": H4_DATA_RVA + 8,
        "EFFECT_MODULE_BASE_RVA": H4_DATA_RVA + 0x10,
    }
    link = [
        "ld", "-m", "elf_x86_64", "-Ttext=0x2ef000",
        "-e", "stage3h_measure_viewport",
    ]
    for name, value in definitions.items():
        link.extend(("--defsym", f"{name}=0x{value:x}"))
    link.extend(("-o", str(h4_elf), str(h4_object)))
    run(link)
    run(["objcopy", "-O", "binary", "-j", ".text",
         str(h4_elf), str(h4_binary)])

    symbols = {}
    nm_output = subprocess.check_output(
        ["nm", "-n", str(h4_elf)], text=True
    )
    for line in nm_output.splitlines():
        fields = line.split()
        if len(fields) == 3 and fields[2].startswith("stage3h_"):
            symbols[fields[2]] = int(fields[0], 16)
    required_symbols = {
        "stage3h_measure_viewport",
        "stage3h_reticle_delta",
        "stage3h_publish_native_serial",
        "stage3h_native_pair_proven",
        "stage3h_effect_wrapper",
    }
    if not required_symbols.issubset(symbols):
        raise SystemExit("Stage 3H helper symbol extraction failed")
    return h2_binary.read_bytes(), h4_binary.read_bytes(), symbols


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
        name = bytes(blob[offset:offset + 8]).split(b"\0", 1)[0].decode(
            "ascii", errors="strict")
        virtual_size, virtual_address, raw_size, raw_pointer = \
            struct.unpack_from("<IIII", blob, offset + 8)
        sections.append({
            "name": name,
            "virtual_size": virtual_size,
            "virtual_address": virtual_address,
            "raw_size": raw_size,
            "raw_pointer": raw_pointer,
            "header": offset,
        })
    return {
        "coff": coff,
        "optional": optional,
        "section_table": section_table,
        "section_count": section_count,
        "sections": sections,
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


def guarded_patch(blob, pe, rva, expected_hex, replacement, label):
    expected = bytes.fromhex(expected_hex)
    offset = rva_offset(pe, rva)
    actual = bytes(blob[offset:offset + len(expected)])
    if actual != expected:
        raise SystemExit(
            f"{label}: expected {expected.hex()} at RVA 0x{rva:X}, "
            f"got {actual.hex()}"
        )
    if len(replacement) != len(expected):
        raise SystemExit(f"{label}: replacement length mismatch")
    blob[offset:offset + len(replacement)] = replacement
    print(f"{label}: RVA 0x{rva:X}")


def relative_branch(opcode, source_rva, target_rva):
    return bytes((opcode,)) + struct.pack("<i", target_rva - (source_rva + 5))


def main():
    if len(sys.argv) != 3:
        raise SystemExit(
            "usage: python build_stage3h_h2_sliders_h4_native_reticle.py "
            "<Stage3G-HaloMCCVR.dll> <output.dll>"
        )
    source = Path(sys.argv[1])
    output = Path(sys.argv[2])
    input_bytes = source.read_bytes()
    input_sha = hashlib.sha256(input_bytes).hexdigest()
    if input_sha != EXPECTED_STAGE3G_SHA256:
        raise SystemExit("wrong Stage 3G input DLL: " + input_sha)

    tool_dir = Path(__file__).resolve().parent
    with tempfile.TemporaryDirectory(prefix="halomccvr-stage3h-") as name:
        h2_helper, h4_helper, symbols = assemble_helpers(
            tool_dir, Path(name)
        )
    if len(h2_helper) > 0x200:
        raise SystemExit("Halo 2 helper no longer fits .h2sf")
    if len(h4_helper) > 0x1000:
        raise SystemExit("Halo 4 helper no longer fits .s3hc")

    blob = bytearray(input_bytes)
    pe = parse_pe(blob)
    section_by_name = {item["name"]: item for item in pe["sections"]}
    h2_section = section_by_name.get(".h2sf")
    if not h2_section or h2_section["virtual_address"] != H2_CODE_RVA or \
            h2_section["raw_size"] != 0x200:
        raise SystemExit("Stage 3G .h2sf section layout mismatch")
    h2_raw = h2_section["raw_pointer"]
    blob[h2_raw:h2_raw + 0x200] = h2_helper.ljust(0x200, b"\0")
    struct.pack_into("<I", blob, h2_section["header"] + 8, len(h2_helper))

    file_alignment = pe["file_alignment"]
    section_alignment = pe["section_alignment"]
    header = pe["section_table"] + pe["section_count"] * 40
    if header + 80 > pe["size_headers"]:
        raise SystemExit("no room for Stage 3H section headers")
    expected_code_rva = align(
        max(item["virtual_address"] + max(
            item["virtual_size"], item["raw_size"])
            for item in pe["sections"]),
        section_alignment,
    )
    if expected_code_rva != H4_CODE_RVA:
        raise SystemExit(
            f"unexpected Stage 3H code RVA 0x{expected_code_rva:X}"
        )
    code_raw = align(
        max(item["raw_pointer"] + item["raw_size"]
            for item in pe["sections"]),
        file_alignment,
    )
    if code_raw < len(blob):
        raise SystemExit("Stage 3H code would overwrite an existing overlay")
    code_raw_size = align(len(h4_helper), file_alignment)
    data_raw = code_raw + code_raw_size
    data_virtual_size = 0x18
    data_raw_size = align(data_virtual_size, file_alignment)
    blob.extend(b"\0" * (code_raw - len(blob)))
    blob.extend(h4_helper.ljust(code_raw_size, b"\0"))
    blob.extend(b"\0" * data_raw_size)

    blob[header:header + 8] = b".s3hc\0\0\0"
    struct.pack_into(
        "<IIIIIIHHI", blob, header + 8,
        len(h4_helper), H4_CODE_RVA, code_raw_size, code_raw,
        0, 0, 0, 0, 0x60000020,
    )
    data_header = header + 40
    blob[data_header:data_header + 8] = b".s3hd\0\0\0"
    struct.pack_into(
        "<IIIIIIHHI", blob, data_header + 8,
        data_virtual_size, H4_DATA_RVA, data_raw_size, data_raw,
        0, 0, 0, 0, 0xC0000040,
    )
    struct.pack_into("<H", blob, pe["coff"] + 2, pe["section_count"] + 2)
    struct.pack_into(
        "<I", blob, pe["optional"] + 0x38,
        align(H4_DATA_RVA + data_virtual_size, section_alignment),
    )
    size_code = struct.unpack_from("<I", blob, pe["optional"] + 4)[0]
    struct.pack_into(
        "<I", blob, pe["optional"] + 4, size_code + code_raw_size
    )
    size_data = struct.unpack_from("<I", blob, pe["optional"] + 8)[0]
    struct.pack_into(
        "<I", blob, pe["optional"] + 8, size_data + data_raw_size
    )

    # The RVA mapper now needs the two appended sections.
    pe["sections"].extend((
        {
            "name": ".s3hc", "virtual_size": len(h4_helper),
            "virtual_address": H4_CODE_RVA, "raw_size": code_raw_size,
            "raw_pointer": code_raw, "header": header,
        },
        {
            "name": ".s3hd", "virtual_size": data_virtual_size,
            "virtual_address": H4_DATA_RVA, "raw_size": data_raw_size,
            "raw_pointer": data_raw, "header": data_header,
        },
    ))

    capture_expected = (
        "3c04752b0fb60557f125009084c074180fb6059bf125009084c0740c"
        "0fb60541f125009084c0744e32c04883c428c3"
    )
    capture_replacement = (
        bytes.fromhex("3c04752b32c04883c428c3")
        + b"\x90" * (0x2F - 11)
    )
    guarded_patch(
        blob, pe, 0x48086, capture_expected, capture_replacement,
        "disable Halo 4 whole-CUI authored capture",
    )

    proof_expected = "4883ec28e8a7f8030032c04883c428c3"
    proof_replacement = relative_branch(
        0xE9, 0x48110, symbols["stage3h_native_pair_proven"]
    ) + b"\x90" * 11
    guarded_patch(
        blob, pe, 0x48110, proof_expected, proof_replacement,
        "install current-pair native reticle proof",
    )

    gameplay_expected = (
        "33c038054aa625000f95c03943087571803d8fa52500007468803dd9a5"
        "250000745fe85ed2fdff84c0745666c743040100c6430600448864242844"
        "897c24204c8bac24900000004d8bcd4c8b8424880000008bd78bce41ffd6"
        "c6430400807b0200740733c9e84b0e0000807b06007519488d156b000000"
        "488b4c2438e8fc1d0f00"
    )
    gameplay_replacement = (
        bytes.fromhex("488b8c2488000000")
        + relative_branch(
            0xE8, 0x53613, symbols["stage3h_measure_viewport"]
        )
        + relative_branch(0xE9, 0x53618, 0x5368C)
    )
    gameplay_replacement += b"\x90" * (0x81 - len(gameplay_replacement))
    guarded_patch(
        blob, pe, 0x5360B, gameplay_expected, gameplay_replacement,
        "measure live viewport and keep one normal CUI pass",
    )

    reticle_expected = (
        "410f28c0450f57c90f57f632dbe88d0e10006685c07f620f28c7e8800e"
        "10006685c07f55f30f101d77511300410f28c80f540d885213000f28d70f"
        "2fd90f54157b52130077330f2fda772ef30f1005284518000f2fc877210f2f"
        "d0771c0f28f1f30f5935bf511300660f7ef0250000807f3d0000807f0f95"
        "c384db0f848f000000"
    )
    reticle_replacement = (
        bytes.fromhex("488d4c2448")
        + relative_branch(
            0xE8, 0x53D46, symbols["stage3h_reticle_delta"]
        )
        + bytes.fromhex("84c00f84")
        + struct.pack("<i", 0x53E51 - 0x53D53)
        + bytes.fromhex("0f28f0440f28c9")
        + relative_branch(0xE9, 0x53D5A, 0x53DC2)
    )
    reticle_replacement += b"\x90" * (0x81 - len(reticle_replacement))
    guarded_patch(
        blob, pe, 0x53D41, reticle_expected, reticle_replacement,
        "position exact pushed native Halo 4 reticle matrix",
    )

    publish_expected = (
        "b8b803000049634c0708498b44071890488984cf90110000eb08"
    )
    publish_replacement = (
        bytes.fromhex("4c89f94889fa")
        + relative_branch(
            0xE8, 0x53E3D, symbols["stage3h_publish_native_serial"]
        )
        + relative_branch(0xE9, 0x53E42, 0x53E59)
    )
    publish_replacement += b"\x90" * (
        len(bytes.fromhex(publish_expected)) - len(publish_replacement)
    )
    guarded_patch(
        blob, pe, 0x53E37, publish_expected, publish_replacement,
        "publish native proof only after a positioned reticle",
    )

    output.write_bytes(blob)
    print("input sha256 ", input_sha)
    print("h2 helper sha256", hashlib.sha256(h2_helper).hexdigest())
    print("h4 helper sha256", hashlib.sha256(h4_helper).hexdigest())
    print("output sha256", hashlib.sha256(blob).hexdigest())
    print("output size  ", len(blob))


if __name__ == "__main__":
    main()
