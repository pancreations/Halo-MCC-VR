#!/usr/bin/env python3
"""Locate the retail x64 homolog of H2EK's central aim-assist routine.

This is an evidence helper, not a runtime patcher.  It uses the PE exception
directory to walk compiler-defined x64 functions, identifies the H2EK-proven
targeting-result initializer by its field writes, and then lists direct callers
whose entry logic also clears the three-float aim-assist control result.
"""

from __future__ import annotations

import argparse
import bisect
import struct
from dataclasses import dataclass
from pathlib import Path

from capstone import CS_ARCH_X86, CS_MODE_64, Cs
from capstone.x86 import (
    X86_INS_CALL,
    X86_INS_MOV,
    X86_INS_MOVD,
    X86_INS_MOVQ,
    X86_INS_MOVSS,
    X86_INS_MOVUPS,
    X86_INS_MOVAPS,
    X86_INS_XOR,
    X86_INS_XORPD,
    X86_INS_XORPS,
    X86_OP_IMM,
    X86_OP_MEM,
    X86_OP_REG,
)


@dataclass(frozen=True)
class Section:
    name: str
    virtual_size: int
    rva: int
    raw_size: int
    raw_offset: int


class Pe64:
    def __init__(self, path: Path):
        self.path = path
        self.data = path.read_bytes()
        pe = struct.unpack_from("<I", self.data, 0x3C)[0]
        if self.data[pe : pe + 4] != b"PE\0\0":
            raise ValueError(f"{path} is not a PE image")
        coff = pe + 4
        count = struct.unpack_from("<H", self.data, coff + 2)[0]
        optional_size = struct.unpack_from("<H", self.data, coff + 16)[0]
        optional = coff + 20
        if struct.unpack_from("<H", self.data, optional)[0] != 0x20B:
            raise ValueError(f"{path} is not PE32+")
        self.image_base = struct.unpack_from("<Q", self.data, optional + 24)[0]
        table = optional + optional_size
        sections = []
        for index in range(count):
            off = table + index * 40
            name = self.data[off : off + 8].split(b"\0", 1)[0].decode("ascii")
            virtual_size, rva, raw_size, raw_offset = struct.unpack_from(
                "<IIII", self.data, off + 8
            )
            sections.append(Section(name, virtual_size, rva, raw_size, raw_offset))
        self.sections = sections

    def section(self, name: str) -> Section:
        return next(section for section in self.sections if section.name == name)

    def file_offset(self, rva: int) -> int:
        for section in self.sections:
            extent = max(section.virtual_size, section.raw_size)
            if section.rva <= rva < section.rva + extent:
                return section.raw_offset + rva - section.rva
        raise KeyError(hex(rva))

    def bytes_at(self, rva: int, size: int) -> bytes:
        off = self.file_offset(rva)
        return self.data[off : off + size]

    def loaded_image(self) -> bytes:
        optional = struct.unpack_from("<I", self.data, 0x3C)[0] + 24
        image_size = struct.unpack_from("<I", self.data, optional + 56)[0]
        result = bytearray(image_size)
        for section in self.sections:
            chunk = self.data[
                section.raw_offset : section.raw_offset + section.raw_size
            ]
            result[section.rva : section.rva + len(chunk)] = chunk
        return bytes(result)

    def runtime_functions(self) -> list[tuple[int, int]]:
        pdata = self.section(".pdata")
        result = []
        for off in range(pdata.raw_offset, pdata.raw_offset + pdata.raw_size - 11, 12):
            begin, end, _unwind = struct.unpack_from("<III", self.data, off)
            if begin and begin < end:
                result.append((begin, end))
        return sorted(set(result))


def direct_calls(disassembler: Cs, image: Pe64, begin: int, end: int) -> list[int]:
    calls = []
    for instruction in disassembler.disasm(image.bytes_at(begin, end - begin), begin):
        if (
            instruction.id == X86_INS_CALL
            and instruction.operands
            and instruction.operands[0].type == X86_OP_IMM
        ):
            calls.append(instruction.operands[0].imm)
    return calls


def immediate_writes(disassembler: Cs, image: Pe64, begin: int, end: int):
    writes = []
    for instruction in disassembler.disasm(image.bytes_at(begin, end - begin), begin):
        if instruction.id != X86_INS_MOV or len(instruction.operands) != 2:
            continue
        destination, source = instruction.operands
        if destination.type != X86_OP_MEM or source.type != X86_OP_IMM:
            continue
        writes.append(
            (
                instruction.address,
                instruction.reg_name(destination.mem.base),
                destination.mem.disp,
                destination.size,
                source.imm,
                instruction.mnemonic + " " + instruction.op_str,
            )
        )
    return writes


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("image", type=Path)
    parser.add_argument("--show", type=lambda value: int(value, 0))
    parser.add_argument("--callers", type=lambda value: int(value, 0))
    parser.add_argument("--output-initializers", action="store_true")
    parser.add_argument("--target-writers", action="store_true")
    parser.add_argument(
        "--pattern",
        help="count a space-separated byte pattern; ?? bytes are wildcards",
    )
    args = parser.parse_args()

    image = Pe64(args.image)
    functions = image.runtime_functions()
    starts = [begin for begin, _end in functions]
    disassembler = Cs(CS_ARCH_X86, CS_MODE_64)
    disassembler.detail = True

    if args.pattern is not None:
        tokens = args.pattern.split()
        values = [None if token in {"?", "??"} else int(token, 16)
                  for token in tokens]
        loaded = image.loaded_image()
        matches = []
        for offset in range(0, len(loaded) - len(values) + 1):
            if all(value is None or loaded[offset + index] == value
                   for index, value in enumerate(values)):
                matches.append(offset)
        print(f"loaded-image pattern bytes={len(values)} matches={len(matches)}")
        for match in matches[:32]:
            print(f"  RVA=0x{match:X}")
        return 0 if len(matches) == 1 else 1

    if args.show is not None:
        index = bisect.bisect_right(starts, args.show) - 1
        begin, end = functions[index]
        print(f"function RVA 0x{begin:X}..0x{end:X}")
        for instruction in disassembler.disasm(image.bytes_at(begin, end - begin), begin):
            print(f"  {instruction.address:08X}  {instruction.mnemonic:<8} {instruction.op_str}")
        return 0

    if args.callers is not None:
        target = args.callers
        counts: dict[int, int] = {}
        for begin, end in functions:
            if end - begin > 0x5000:
                continue
            count = direct_calls(disassembler, image, begin, end).count(target)
            if count:
                counts[begin] = count
        for begin, count in sorted(counts.items(), key=lambda item: (-item[1], item[0])):
            index = bisect.bisect_left(starts, begin)
            end = functions[index][1]
            print(
                f"RVA=0x{begin:X}..0x{end:X} size=0x{end - begin:X} "
                f"direct_calls={count}"
            )
        return 0

    if args.output_initializers:
        results = []
        move_ids = {
            X86_INS_MOV,
            X86_INS_MOVD,
            X86_INS_MOVQ,
            X86_INS_MOVSS,
            X86_INS_MOVUPS,
            X86_INS_MOVAPS,
        }
        xor_ids = {X86_INS_XOR, X86_INS_XORPD, X86_INS_XORPS}
        for begin, end in functions:
            if end - begin > 0x5000:
                continue
            constants: dict[int, int] = {}
            stores: dict[str, list[tuple[int, int, int, str]]] = {}
            code_end = min(end, begin + 0x180)
            for instruction in disassembler.disasm(
                image.bytes_at(begin, code_end - begin), begin
            ):
                operands = instruction.operands
                if (
                    instruction.id in xor_ids
                    and len(operands) == 2
                    and operands[0].type == X86_OP_REG
                    and operands[1].type == X86_OP_REG
                    and operands[0].reg == operands[1].reg
                ):
                    constants[operands[0].reg] = 0
                    continue
                if (
                    instruction.id == X86_INS_MOV
                    and len(operands) == 2
                    and operands[0].type == X86_OP_REG
                    and operands[1].type == X86_OP_IMM
                ):
                    constants[operands[0].reg] = operands[1].imm
                    continue
                if instruction.id not in move_ids or len(operands) != 2:
                    continue
                destination, source = operands
                if (
                    destination.type != X86_OP_MEM
                    or destination.mem.base == 0
                    or destination.mem.index != 0
                ):
                    continue
                value = None
                if source.type == X86_OP_IMM:
                    value = source.imm
                elif source.type == X86_OP_REG:
                    value = constants.get(source.reg)
                if value is None:
                    continue
                base_name = instruction.reg_name(destination.mem.base)
                stores.setdefault(base_name, []).append(
                    (
                        destination.mem.disp,
                        destination.size,
                        value,
                        f"0x{instruction.address:X}: {instruction.mnemonic} {instruction.op_str}",
                    )
                )
            for base_name, writes in stores.items():
                zero_control = any(d == 0 and v == 0 and s >= 8 for d, s, v, _t in writes) and any(
                    d == 8 and v == 0 and s >= 4 for d, s, v, _t in writes
                )
                target_sentinels = sum(
                    1
                    for displacement in (0, 4, 8)
                    if any(
                        d == displacement and v in (-1, 0xFFFFFFFF, 0xFFFFFFFFFFFFFFFF)
                        for d, _s, v, _t in writes
                    )
                )
                target_zeros = sum(
                    1
                    for displacement in (0x18, 0x1C, 0x20)
                    if any(d == displacement and v == 0 for d, _s, v, _t in writes)
                )
                if zero_control or (target_sentinels >= 2 and target_zeros >= 1):
                    results.append(
                        (zero_control, target_sentinels + target_zeros, begin, end, base_name, writes)
                    )
        for zero_control, target_score, begin, end, base_name, writes in sorted(
            results, reverse=True
        )[:120]:
            print(
                f"RVA=0x{begin:X}..0x{end:X} size=0x{end - begin:X} "
                f"base={base_name} control={int(zero_control)} target_score={target_score}"
            )
            for displacement, size, value, description in writes:
                if displacement in (0, 4, 8, 0x18, 0x1C, 0x20):
                    print(
                        f"  off=0x{displacement:X} size={size} value={value} {description}"
                    )
        return 0

    if args.target_writers:
        results = []
        write_ids = {
            X86_INS_MOV,
            X86_INS_MOVD,
            X86_INS_MOVQ,
            X86_INS_MOVSS,
            X86_INS_MOVUPS,
            X86_INS_MOVAPS,
        }
        desired = {0, 4, 8, 0x18, 0x1C, 0x20}
        for begin, end in functions:
            if end - begin > 0x3000:
                continue
            by_base: dict[str, list[tuple[int, str]]] = {}
            calls: list[int] = []
            for instruction in disassembler.disasm(
                image.bytes_at(begin, end - begin), begin
            ):
                if (
                    instruction.id == X86_INS_CALL
                    and instruction.operands
                    and instruction.operands[0].type == X86_OP_IMM
                ):
                    calls.append(instruction.operands[0].imm)
                if instruction.id not in write_ids or len(instruction.operands) != 2:
                    continue
                destination = instruction.operands[0]
                if (
                    destination.type != X86_OP_MEM
                    or destination.mem.base == 0
                    or destination.mem.index != 0
                    or destination.mem.disp not in desired
                ):
                    continue
                base_name = instruction.reg_name(destination.mem.base)
                by_base.setdefault(base_name, []).append(
                    (
                        destination.mem.disp,
                        f"0x{instruction.address:X}: {instruction.mnemonic} {instruction.op_str}",
                    )
                )
            for base_name, writes in by_base.items():
                offsets = {displacement for displacement, _text in writes}
                score = len(offsets & desired)
                if score >= 4:
                    results.append((score, begin, end, base_name, writes, calls.count(0x8D7000)))
        for score, begin, end, base_name, writes, accessor_calls in sorted(
            results, key=lambda item: (item[5], item[0], -item[1]), reverse=True
        )[:160]:
            print(
                f"score={score} RVA=0x{begin:X}..0x{end:X} size=0x{end - begin:X} "
                f"base={base_name} object_accessors={accessor_calls}"
            )
            for _displacement, description in writes:
                print(" ", description)
        return 0

    # A direct immediate -1 store encodes four consecutive FF bytes. Seed the
    # much smaller set of containing functions before asking Capstone for
    # semantic field writes; disassembling every retail function is needlessly
    # expensive and obscures the evidence.
    seeded: set[tuple[int, int]] = set()
    text_section = image.section(".text")
    text_data = image.data[
        text_section.raw_offset : text_section.raw_offset + text_section.raw_size
    ]
    needle = b"\xFF\xFF\xFF\xFF"
    position = 0
    while True:
        position = text_data.find(needle, position)
        if position < 0:
            break
        rva = text_section.rva + position
        index = bisect.bisect_right(starts, rva) - 1
        if index >= 0 and functions[index][0] <= rva < functions[index][1]:
            seeded.add(functions[index])
        position += 4

    candidates = []
    for begin, end in seeded:
        if end - begin > 0x5000:
            continue
        writes = immediate_writes(disassembler, image, begin, end)
        by_register: dict[str, set[tuple[int, int, int]]] = {}
        for _address, register, displacement, size, value, _text in writes:
            by_register.setdefault(register, set()).add((displacement, size, value))
        best_score = 0
        best_register = ""
        for register, fields in by_register.items():
            score = 0
            for displacement in (0, 4, 8):
                if any(d == displacement and v in (-1, 0xFFFFFFFF) for d, _s, v in fields):
                    score += 2
            for displacement in (0x18, 0x1C, 0x20):
                if any(d == displacement and v == 0 for d, _s, v in fields):
                    score += 1
            if score > best_score:
                best_score = score
                best_register = register
        if best_score >= 4:
            candidates.append((best_score, begin, end, best_register, writes))

    candidates.sort(reverse=True)
    candidate_starts = {begin for _score, begin, _end, _register, _writes in candidates}
    callers_by_target: dict[int, list[int]] = {}
    # Every direct x64 near call is E8 + rel32.  Decode those five bytes once,
    # then map matching calls back to the compiler function that owns them.
    for position in range(0, len(text_data) - 4):
        if text_data[position] != 0xE8:
            continue
        call_rva = text_section.rva + position
        displacement = struct.unpack_from("<i", text_data, position + 1)[0]
        target = call_rva + 5 + displacement
        if target not in candidate_starts:
            continue
        index = bisect.bisect_right(starts, call_rva) - 1
        if index >= 0 and functions[index][0] <= call_rva < functions[index][1]:
            callers_by_target.setdefault(target, []).append(functions[index][0])
    print(f"image_base=0x{image.image_base:X} runtime_functions={len(functions)}")
    for score, begin, end, register, writes in candidates[:40]:
        print(
            f"candidate score={score} RVA=0x{begin:X}..0x{end:X} "
            f"size=0x{end - begin:X} base={register}"
        )
        for address, reg, displacement, size, value, text in writes:
            if reg == register and displacement in (0, 4, 8, 0x18, 0x1C, 0x20):
                print(
                    f"  0x{address:X}: off=0x{displacement:X} size={size} "
                    f"value={value}  {text}"
                )
        callers = callers_by_target.get(begin, [])
        print("  direct callers:", " ".join(f"0x{x:X}" for x in callers) or "none")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
