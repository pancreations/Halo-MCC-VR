"""Static acceptance for Stage 3AX."""

from pathlib import Path
import hashlib
import struct
import sys

from capstone import Cs, CS_ARCH_X86, CS_MODE_64


AW = "ad0c6bbca337f2436a258cb4a0cb9da5884b20270bc2f9dadf7a06daba1ed676"
AX = "0d32751585670c28bb7b98110a35b04817ec4f683fcc3ab3301c0941a4613053"


def pe(blob):
    p = struct.unpack_from("<I", blob, 0x3C)[0]
    n = struct.unpack_from("<H", blob, p + 6)[0]
    osz = struct.unpack_from("<H", blob, p + 20)[0]
    st = p + 24 + osz
    sections = []
    for i in range(n):
        h = st + i * 40
        vs, va, rs, rp = struct.unpack_from("<IIII", blob, h + 8)
        sections.append((vs, va, rs, rp))
    return sections


def off(sections, rva):
    for vs, va, rs, rp in sections:
        if va <= rva < va + max(vs, rs):
            return rp + rva - va
    raise KeyError(hex(rva))


def main():
    root = Path(__file__).parent.parent
    before = Path(sys.argv[1]) if len(sys.argv) > 1 else \
        root / "built/Stage3AW-HaloMCCVR.dll"
    after = Path(sys.argv[2]) if len(sys.argv) > 2 else \
        root / "built/Stage3AX-HaloMCCVR.dll"
    a, b = before.read_bytes(), after.read_bytes()
    assert hashlib.sha256(a).hexdigest() == AW
    assert hashlib.sha256(b).hexdigest() == AX
    assert len(a) == len(b) == 2919424
    sections = pe(b)

    diffs = {i for i, (x, y) in enumerate(zip(a, b)) if x != y}
    allowed = set()
    for lo, hi in ((0x539C0, 0x539CA), (0x67CE5, 0x67CE6),
                   (0x2FA9A0, 0x2FAA9E)):
        allowed.update(range(off(sections, lo), off(sections, hi - 1) + 1))
    assert diffs and diffs <= allowed

    # Pause helper, native reason-3 latch, and stock-wrapper routing are exact.
    pause_lo, pause_hi = off(sections, 0x2FA890), off(sections, 0x2FA991)
    assert a[pause_lo:pause_hi] == b[pause_lo:pause_hi]

    call = b[off(sections, 0x539C0):off(sections, 0x539C0) + 5]
    assert call[0] == 0xE8
    assert 0x539C5 + struct.unpack_from("<i", call, 1)[0] == 0x2FA9A0
    assert b[off(sections, 0x67CE3):off(sections, 0x67CE3) + 4] == \
        bytes.fromhex("84 c0 eb 32")

    code = b[off(sections, 0x2FA9A0):off(sections, 0x2FAA9E)]
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    insns = list(md.disasm(code, 0x2FA9A0))
    assert insns[-1].address == 0x2FAA9D and insns[-1].mnemonic == "ret"
    text = [(i.mnemonic, i.op_str) for i in insns]
    assert text.count(("addss", "xmm2, xmm2")) == 3
    assert text.count(("addss", "xmm3, xmm3")) == 3
    assert text.count(("addss", "xmm0, xmm0")) == 2
    assert text.count(("addss", "xmm1, xmm1")) == 2
    assert ("movss", "dword ptr [rip - 0x4c2ef], xmm2") in text
    assert ("movss", "dword ptr [rip - 0x4c2f3], xmm3") in text
    print("Stage3AX static acceptance: PASS")
    print("changed bytes", len(diffs))


if __name__ == "__main__":
    main()
