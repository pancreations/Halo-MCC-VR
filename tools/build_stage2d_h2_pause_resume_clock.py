"""Build Stage 2D from the exact Stage 2C HaloMCCVR.dll.

C-H2-73 is a narrowly-scoped post-link equivalent of the matching source:
while Halo 2 is already displaying the head-locked pause presentation, sample
its H2EK-proven game_time_globals clock.  The helper first requires a genuine
freeze, then waits for two distinct native ticks to advance before requesting
stereo again.  Menu buttons are never interpreted as Resume.
"""
from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import struct

EXPECTED_STAGE2C_SHA256 = (
    "a2460f279783df5d9e30feac23cc9fb22a6b66a79487a707fb17ef9c05d05f0f"
)
IMAGE_BASE = 0x180000000
PATCH_RVA = 0x00042CB3
RETURN_RVA = 0x00042CC4
EXPECTED_PATCH = bytes.fromhex(
    "45 84 ED 75 09 84 C0 75 05 40 32 FF EB 03 40 B7 01"
)
READ_GAME_TIME_VA = 0x180088FB0
SET_PRESENTATION_READY_VA = 0x18008D880
REQUEST_PAUSE_PRESENTATION_VA = 0x18002FAB0
LOG_VA = 0x180001D90
FREEZE_CONFIRM_SAMPLES = 4
RESUME_TICK_CHANGES = 2
MESSAGE = (
    b"Halo 2 pause presentation: native game-time clock resumed; "
    b"restoring stereo 3D\0"
)


def align_up(v: int, a: int) -> int:
    return (v + a - 1) & ~(a - 1)


def parse_pe(b: bytearray):
    pe = struct.unpack_from("<I", b, 0x3C)[0]
    assert b[pe:pe + 4] == b"PE\0\0"
    coff = pe + 4
    nsec = struct.unpack_from("<H", b, coff + 2)[0]
    optsz = struct.unpack_from("<H", b, coff + 16)[0]
    opt = coff + 20
    assert struct.unpack_from("<H", b, opt)[0] == 0x20B
    image_base = struct.unpack_from("<Q", b, opt + 24)[0]
    assert image_base == IMAGE_BASE
    section_alignment = struct.unpack_from("<I", b, opt + 32)[0]
    file_alignment = struct.unpack_from("<I", b, opt + 36)[0]
    size_of_headers = struct.unpack_from("<I", b, opt + 60)[0]
    sect = opt + optsz
    sections = []
    for i in range(nsec):
        h = sect + i * 40
        name = bytes(b[h:h + 8]).rstrip(b"\0").decode("ascii")
        vsize, rva, rawsize, raw = struct.unpack_from("<IIII", b, h + 8)
        chars = struct.unpack_from("<I", b, h + 36)[0]
        sections.append(
            dict(name=name, vsize=vsize, rva=rva, rawsize=rawsize,
                 raw=raw, hdr=h, chars=chars)
        )
    return pe, coff, opt, sect, sections, section_alignment, file_alignment, size_of_headers


def rva_to_raw(rva: int, sections) -> int:
    for s in sections:
        if s["rva"] <= rva < s["rva"] + max(s["vsize"], s["rawsize"]):
            return s["raw"] + (rva - s["rva"])
    raise AssertionError(f"RVA not mapped: 0x{rva:X}")


class Code:
    def __init__(self, base_va: int):
        self.base_va = base_va
        self.b = bytearray()
        self.labels: dict[str, int] = {}
        self.fixups: list[tuple[int, str | int, str, int]] = []

    def emit(self, x: bytes):
        self.b.extend(x)

    def label(self, name: str):
        assert name not in self.labels
        self.labels[name] = len(self.b)

    def rel32_label(self, op: bytes, name: str):
        pos = len(self.b)
        self.emit(op + b"\0\0\0\0")
        self.fixups.append((pos + len(op), name, "label", 0))

    def rel32_va(self, op: bytes, va: int):
        pos = len(self.b)
        self.emit(op + b"\0\0\0\0")
        self.fixups.append((pos + len(op), va, "va", 0))

    def rip(self, prefix: bytes, va: int, suffix: bytes = b""):
        pos = len(self.b)
        self.emit(prefix + b"\0\0\0\0" + suffix)
        self.fixups.append((pos + len(prefix), va, "rip", len(suffix)))

    def resolve(self):
        for off, target, kind, suffix_len in self.fixups:
            if kind == "label":
                target_va = self.base_va + self.labels[str(target)]
            else:
                target_va = int(target)
            next_va = self.base_va + off + 4 + suffix_len
            rel = target_va - next_va
            assert -(1 << 31) <= rel < (1 << 31), (hex(next_va), hex(target_va))
            struct.pack_into("<i", self.b, off, rel)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("input_dll", type=Path)
    ap.add_argument("output_dll", type=Path)
    args = ap.parse_args()

    original = args.input_dll.read_bytes()
    actual = hashlib.sha256(original).hexdigest()
    assert actual == EXPECTED_STAGE2C_SHA256, (
        "Stage 2D requires exact Stage 2C DLL; got " + actual
    )
    b = bytearray(original)
    pe, coff, opt, sect, sections, sec_align, file_align, size_headers = parse_pe(b)
    assert len(sections) == 9, len(sections)
    assert sec_align == 0x1000 and file_align == 0x200
    assert sections[-1]["name"] == ".h2pm"

    patch_raw = rva_to_raw(PATCH_RVA, sections)
    assert bytes(b[patch_raw:patch_raw + len(EXPECTED_PATCH)]) == EXPECTED_PATCH

    last = sections[-1]
    code_rva = align_up(last["rva"] + max(last["vsize"], last["rawsize"]), sec_align)
    data_rva = code_rva + sec_align
    code_raw = align_up(len(b), file_align)
    code_va = IMAGE_BASE + code_rva
    data_va = IMAGE_BASE + data_rva
    state_va = data_va

    code = Code(code_va)
    # r15b is the already-displayed g_pausePresentation local captured by the
    # stock H2 Game_AutoVrTick immediately before this interposition.
    code.emit(bytes.fromhex("45 84 FF"))                 # test r15b,r15b
    code.rel32_label(bytes.fromhex("0F 85"), "paused") # jne paused
    code.rip(bytes.fromhex("48 C7 05"), state_va, bytes.fromhex("00 00 00 00"))
    code.rel32_label(bytes.fromhex("E9"), "displaced")

    code.label("paused")
    code.emit(bytes.fromhex("48 83 EC 40"))              # shadow + locals
    code.emit(bytes.fromhex("48 8D 4C 24 20"))           # rcx=&object
    code.emit(bytes.fromhex("48 8D 54 24 28"))           # rdx=&initialized
    code.emit(bytes.fromhex("4C 8D 44 24 2C"))           # r8=&tick
    code.rel32_va(bytes.fromhex("E8"), READ_GAME_TIME_VA)
    code.emit(bytes.fromhex("84 C0"))                    # test al,al
    code.rel32_label(bytes.fromhex("0F 84"), "invalid")
    code.emit(bytes.fromhex("80 7C 24 28 00"))           # initialized?
    code.rel32_label(bytes.fromhex("0F 84"), "invalid")
    code.emit(bytes.fromhex("8B 44 24 2C"))              # eax=tick

    # state = {lastTick:u32, have:u8, frozen:u8, stable:u8, changes:u8}
    code.rip(bytes.fromhex("80 3D"), state_va + 4, bytes.fromhex("00"))
    code.rel32_label(bytes.fromhex("0F 85"), "have_tick")
    code.rip(bytes.fromhex("89 05"), state_va)            # last=eax
    code.rip(bytes.fromhex("C7 05"), state_va + 4, bytes.fromhex("01 00 00 00"))
    code.rel32_label(bytes.fromhex("E9"), "finish_stack")

    code.label("have_tick")
    code.rip(bytes.fromhex("3B 05"), state_va)            # cmp eax,last
    code.rel32_label(bytes.fromhex("0F 85"), "tick_changed")
    code.rip(bytes.fromhex("80 3D"), state_va + 5, bytes.fromhex("00"))
    code.rel32_label(bytes.fromhex("0F 85"), "finish_stack")
    code.rip(bytes.fromhex("FE 05"), state_va + 6)        # ++stable
    code.rip(bytes.fromhex("80 3D"), state_va + 6, bytes([FREEZE_CONFIRM_SAMPLES]))
    code.rel32_label(bytes.fromhex("0F 82"), "finish_stack")  # jb
    code.rip(bytes.fromhex("C6 05"), state_va + 5, bytes.fromhex("01"))
    code.rel32_label(bytes.fromhex("E9"), "finish_stack")

    code.label("tick_changed")
    code.rip(bytes.fromhex("89 05"), state_va)            # last=eax
    code.rip(bytes.fromhex("C6 05"), state_va + 6, bytes.fromhex("00"))
    code.rip(bytes.fromhex("80 3D"), state_va + 5, bytes.fromhex("00"))
    code.rel32_label(bytes.fromhex("0F 84"), "not_frozen")
    code.rip(bytes.fromhex("FE 05"), state_va + 7)        # ++changes
    code.rip(bytes.fromhex("80 3D"), state_va + 7, bytes([RESUME_TICK_CHANGES]))
    code.rel32_label(bytes.fromhex("0F 82"), "finish_stack")
    code.rip(bytes.fromhex("48 C7 05"), state_va, bytes.fromhex("00 00 00 00"))
    code.emit(bytes.fromhex("33 C9"))                     # ecx=false
    code.rel32_va(bytes.fromhex("E8"), SET_PRESENTATION_READY_VA)
    code.emit(bytes.fromhex("33 C9"))
    code.rel32_va(bytes.fromhex("E8"), REQUEST_PAUSE_PRESENTATION_VA)
    # Message is appended after code; target fixed after label creation below.
    code.rel32_label(bytes.fromhex("E9"), "log_resume")

    code.label("not_frozen")
    code.rip(bytes.fromhex("C6 05"), state_va + 7, bytes.fromhex("00"))
    code.rel32_label(bytes.fromhex("E9"), "finish_stack")

    code.label("invalid")
    code.rip(bytes.fromhex("48 C7 05"), state_va, bytes.fromhex("00 00 00 00"))
    code.rel32_label(bytes.fromhex("E9"), "finish_stack")

    code.label("log_resume")
    # Keep calls within our 0x40-byte shadow/local allocation.
    msg_fixup_pos = len(code.b)
    code.emit(bytes.fromhex("48 8D 0D") + b"\0\0\0\0")
    code.rel32_va(bytes.fromhex("E8"), LOG_VA)
    code.rel32_label(bytes.fromhex("E9"), "finish_stack")

    code.label("finish_stack")
    code.emit(bytes.fromhex("48 83 C4 40"))

    code.label("displaced")
    # Recompute source's mustClearForeignPause boolean in DIL.  The original
    # block used AL for pausePresentation, but our helper made calls, so use the
    # preserved r15b local instead (same value, ABI-safe).
    code.emit(bytes.fromhex("45 84 ED"))                  # test r13b,target
    code.rel32_label(bytes.fromhex("0F 85"), "set_true")
    code.emit(bytes.fromhex("45 84 FF"))                  # test r15b,current
    code.rel32_label(bytes.fromhex("0F 85"), "set_true")
    code.emit(bytes.fromhex("40 32 FF"))                  # xor dil,dil
    code.rel32_label(bytes.fromhex("E9"), "return")
    code.label("set_true")
    code.emit(bytes.fromhex("40 B7 01"))                  # mov dil,1
    code.label("return")
    code.rel32_va(bytes.fromhex("E9"), IMAGE_BASE + RETURN_RVA)

    code.label("message")
    code.emit(MESSAGE)
    # Resolve all normal fixups first, then fill LEA-message displacement.
    code.resolve()
    lea_disp_off = msg_fixup_pos + 3
    lea_next_va = code_va + lea_disp_off + 4
    msg_va = code_va + code.labels["message"]
    struct.pack_into("<i", code.b, lea_disp_off, msg_va - lea_next_va)

    code_raw_size = align_up(len(code.b), file_align)
    data_raw_size = file_align
    data_raw = code_raw + code_raw_size
    if len(b) < code_raw:
        b.extend(b"\0" * (code_raw - len(b)))
    b.extend(code.b)
    b.extend(b"\xCC" * (code_raw_size - len(code.b)))
    b.extend(b"\0" * data_raw_size)

    # Add two section headers. There is room in the existing 0x400 header.
    new_hdr = sect + len(sections) * 40
    assert new_hdr + 80 <= size_headers, (hex(new_hdr), hex(size_headers))
    b[new_hdr:new_hdr + 8] = b".h2pr\0\0\0"
    struct.pack_into(
        "<IIIIIIHHI", b, new_hdr + 8,
        len(code.b), code_rva, code_raw_size, code_raw,
        0, 0, 0, 0, 0x60000020,
    )
    data_hdr = new_hdr + 40
    b[data_hdr:data_hdr + 8] = b".h2pd\0\0\0"
    struct.pack_into(
        "<IIIIIIHHI", b, data_hdr + 8,
        8, data_rva, data_raw_size, data_raw,
        0, 0, 0, 0, 0xC0000040,
    )
    struct.pack_into("<H", b, coff + 2, len(sections) + 2)
    struct.pack_into("<I", b, opt + 56, align_up(data_rva + 8, sec_align))
    struct.pack_into("<I", b, opt + 4,
                     struct.unpack_from("<I", b, opt + 4)[0] + code_raw_size)
    struct.pack_into("<I", b, opt + 8,
                     struct.unpack_from("<I", b, opt + 8)[0] + data_raw_size)

    # Interpose only the H2 pause boolean block.
    rel = code_va - (IMAGE_BASE + PATCH_RVA + 5)
    patch = b"\xE9" + struct.pack("<i", rel) + b"\x90" * (len(EXPECTED_PATCH) - 5)
    b[patch_raw:patch_raw + len(EXPECTED_PATCH)] = patch

    args.output_dll.write_bytes(b)
    print("input SHA256 ", actual)
    print("output SHA256", hashlib.sha256(b).hexdigest())
    print("patch RVA     ", hex(PATCH_RVA), "bytes", len(EXPECTED_PATCH))
    print("helper RVA    ", hex(code_rva), "bytes", len(code.b))
    print("state RVA     ", hex(data_rva), "bytes 8")
    print("message offset", hex(code.labels["message"]))


if __name__ == "__main__":
    main()
