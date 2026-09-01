"""Assemble one post-link stage payload without a linker.

The stage builders in this directory historically shelled out to GNU
`as` + `ld` + `objcopy` + `nm`. This machine has no GNU binutils, and the LLVM
`ld.lld` that stood in for it stopped loading (STATUS_DLL_NOT_FOUND) partway
through the work, so the link step is done here instead.

Only what a stage payload actually needs is implemented:

  * assemble a `.S` with clang's integrated assembler to an ELF64 object,
  * read `.text`, `.symtab` and `.rela.text`,
  * resolve R_X86_64_PC32 / R_X86_64_PLT32 against absolute symbol values,
  * return the flat `.text` bytes plus the addresses of the payload's own
    globals.

Addresses of DLL and game code are left undefined in the source and resolved
here from `defs`, because Intel-syntax `jmp SYMBOL` rejects an absolute `.set`
constant as an operand but accepts an undefined symbol. Anything unusual - an
unsupported relocation type, an undefined symbol, a non-empty `.data`, or a
section the payload did not ask for - raises instead of silently producing
bytes that would be welded into a shipping DLL.
"""

from pathlib import Path
import os
import shutil
import struct
import subprocess
import tempfile

R_X86_64_PC32 = 2
R_X86_64_PLT32 = 4

SHT_SYMTAB = 2
SHT_STRTAB = 3
SHT_RELA = 4


def _clang():
    configured = os.environ.get("HALOMCCVR_CLANG")
    if configured:
        configured = Path(configured)
        if configured.is_file():
            return str(configured)
        raise SystemExit(
            "HALOMCCVR_CLANG does not name a file: " + str(configured))
    for name in ("clang", "clang.exe", "clang-23", "clang-23.exe"):
        found = shutil.which(name)
        if found:
            return found
    raise SystemExit("clang is required to assemble post-link stage payloads")


class _Elf:
    def __init__(self, blob):
        if blob[:4] != b"\x7fELF" or blob[4] != 2 or blob[5] != 1:
            raise SystemExit("not a little-endian ELF64 object")
        e_shoff = struct.unpack_from("<Q", blob, 0x28)[0]
        e_shentsize = struct.unpack_from("<H", blob, 0x3A)[0]
        e_shnum = struct.unpack_from("<H", blob, 0x3C)[0]
        e_shstrndx = struct.unpack_from("<H", blob, 0x3E)[0]
        self.blob = blob
        self.sections = []
        for i in range(e_shnum):
            o = e_shoff + i * e_shentsize
            name, kind, _flags, _addr, offset, size, link, info, _align, entsize = \
                struct.unpack_from("<IIQQQQIIQQ", blob, o)
            self.sections.append(dict(
                name_off=name, type=kind, offset=offset, size=size,
                link=link, info=info, entsize=entsize))
        shstr = self.sections[e_shstrndx]
        for s in self.sections:
            s["name"] = self._cstr(shstr["offset"] + s["name_off"])

    def _cstr(self, offset):
        end = self.blob.index(b"\0", offset)
        return self.blob[offset:end].decode("ascii")

    def section(self, name):
        for s in self.sections:
            if s["name"] == name:
                return s
        return None

    def data(self, section):
        return self.blob[section["offset"]:section["offset"] + section["size"]]

    def symbols(self):
        symtab = next(s for s in self.sections if s["type"] == SHT_SYMTAB)
        strtab = self.sections[symtab["link"]]
        out = []
        count = symtab["size"] // symtab["entsize"]
        for i in range(count):
            o = symtab["offset"] + i * symtab["entsize"]
            name_off, info, _other, shndx, value, size = \
                struct.unpack_from("<IBBHQQ", self.blob, o)
            out.append(dict(
                name=self._cstr(strtab["offset"] + name_off) if name_off else "",
                info=info, shndx=shndx, value=value, size=size))
        return out


def build_payload(source, code_rva, defs, want_symbols=()):
    """Return (code_bytes, {symbol: rva}) for one stage payload.

    `source`      path to the stage's `.S`
    `code_rva`    RVA the payload will be placed at inside the DLL
    `defs`        absolute symbol name -> value (RVAs of DLL/game globals)
    `want_symbols` payload symbols whose resolved RVA the caller needs
    """
    source = Path(source)
    with tempfile.TemporaryDirectory(prefix="halomccvr-postlink-") as td:
        td = Path(td)
        merged = td / source.name
        merged.write_text(source.read_text(encoding="utf-8"),
                          encoding="utf-8")
        obj = td / "payload.o"
        clang_env = None
        clang_dll_dirs = os.environ.get("HALOMCCVR_CLANG_DLL_DIRS")
        if clang_dll_dirs:
            clang_env = os.environ.copy()
            clang_env["PATH"] = clang_dll_dirs + os.pathsep + \
                clang_env.get("PATH", "")
        subprocess.run(
            [_clang(), "--target=x86_64-unknown-linux-gnu", "-c",
             "-o", str(obj), str(merged)],
            check=True, env=clang_env)
        elf = _Elf(obj.read_bytes())

    text = elf.section(".text")
    if text is None:
        raise SystemExit("payload has no .text")
    for s in elf.sections:
        if s["name"] in (".data", ".bss") and s["size"]:
            raise SystemExit(f"payload must be code-only; {s['name']} is "
                             f"{s['size']} bytes")
    code = bytearray(elf.data(text))
    text_index = elf.sections.index(text)
    symbols = elf.symbols()

    rela = elf.section(".rela.text")
    if rela is not None:
        for i in range(rela["size"] // rela["entsize"]):
            o = rela["offset"] + i * rela["entsize"]
            r_offset, r_info, r_addend = struct.unpack_from(
                "<QQq", elf.blob, o)
            kind = r_info & 0xFFFFFFFF
            sym = symbols[r_info >> 32]
            if kind not in (R_X86_64_PC32, R_X86_64_PLT32):
                raise SystemExit(
                    f"unsupported relocation type {kind} for {sym['name']!r}")
            if sym["shndx"] == 0:                 # SHN_UNDEF: a supplied RVA
                if sym["name"] not in defs:
                    raise SystemExit(
                        f"payload references unknown symbol {sym['name']!r}")
                target = defs[sym["name"]]
            elif sym["shndx"] == 0xFFF1:          # SHN_ABS
                target = sym["value"]
            elif sym["shndx"] == text_index:      # local label in this payload
                target = code_rva + sym["value"]
            else:
                raise SystemExit(
                    f"unresolvable symbol {sym['name']!r} in payload")
            value = target + r_addend - (code_rva + r_offset)
            if not -0x80000000 <= value <= 0x7FFFFFFF:
                raise SystemExit(
                    f"relocation for {sym['name']!r} does not fit in 32 bits")
            struct.pack_into("<i", code, r_offset, value)

    resolved = {}
    for name in want_symbols:
        match = next((s for s in symbols
                      if s["name"] == name and s["shndx"] == text_index), None)
        if match is None:
            raise SystemExit(f"payload does not define {name!r}")
        resolved[name] = code_rva + match["value"]
    return bytes(code), resolved
