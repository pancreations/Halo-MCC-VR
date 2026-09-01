"""Stage 3AQ - let Halo 4's authored-reticle capture reach the redirect block.

Input is the exact Stage 3AP DLL (Stage 3AL plus the six-byte C-H4-50 removal).
Appends one 0x1000 page to the existing final .s3qd section and redirects the
eye-range refusal in VR_RedirectRenderTargets through it. See
`stage3aq_h4_capture_eye_gate.S` for the payload in source form and
STAGE3AQ-H4-CAPTURE-EYE-GATE-NOTES.md for the evidence.
"""

from pathlib import Path
import hashlib
import struct
import sys

EXPECTED_STAGE3AP_SHA256 = \
    "3f6cceddf290305886adb37129eb1ab9b3d04a19d91ecfca6c37b40d103858ac"

CODE_RVA = 0x002F9000

# VR_RedirectRenderTargets, the eye-range half of the entry refusal:
#   0x02FF0E  41 83 F9 01              cmp r9d, 1
#   0x02FF12  0F 87 7A FF FF FF        ja  0x02FE92     ; return false
#   0x02FF18                           test rbx, rbx    ; continue
EYE_GATE_RVA = 0x0002FF0E
EYE_GATE_EXPECT = bytes.fromhex("4183f9010f877affffff")
REDIRECT_CONTINUE_RVA = 0x0002FF18
REDIRECT_REFUSE_RVA = 0x0002FE92

DEFS = {
    # TitleAdapter_GetActiveTitle is `movzx eax, byte ptr [rip+X]; nop; ret`.
    "TITLE_ACTIVE_BYTE_RVA": 0x002BA6C8,
    # g_reticleCaptureState.active, written 1 at the end of
    # BeginAuthoredReticleCaptureInternal and cleared by the End path.
    "RETICLE_CAPTURE_ACTIVE_RVA": 0x002AE770,
}

# Guards proving the surrounding function is the one this stage means to change.
CONTEXT = (
    (0x0002FE92, bytes.fromhex("32c0"), "return-false stub"),
    (0x0002FF18, bytes.fromhex("4885db"), "continue edge (test rbx, rbx)"),
    (0x0002FF2A, bytes.fromhex("48833db6eb270000"), "g_gameSwapchain test"),
    (0x0002FF38, bytes.fromhex("803d31e8270000"),
     "g_reticleCaptureState.active test"),
    (0x0002FF52, bytes.fromhex("3c04"), "Halo 4 title compare"),
    (0x000879C0, bytes.fromhex("0fb605012d230090c3"),
     "TitleAdapter_GetActiveTitle body"),
)


def parse_pe(blob):
    if blob[:2] != b"MZ":
        raise SystemExit("input is not MZ")
    p = struct.unpack_from("<I", blob, 0x3C)[0]
    if blob[p:p + 4] != b"PE\0\0":
        raise SystemExit("input is not PE")
    coff = p + 4
    n = struct.unpack_from("<H", blob, coff + 2)[0]
    osz = struct.unpack_from("<H", blob, coff + 16)[0]
    opt = coff + 20
    if struct.unpack_from("<H", blob, opt)[0] != 0x20B:
        raise SystemExit("not PE32+")
    st = opt + osz
    secs = []
    for i in range(n):
        o = st + i * 40
        name = bytes(blob[o:o + 8]).split(b"\0", 1)[0].decode("ascii")
        vs, va, rs, rp = struct.unpack_from("<IIII", blob, o + 8)
        secs.append(dict(name=name, vs=vs, va=va, rs=rs, rp=rp, h=o))
    return dict(opt=opt, n=n, sections=secs)


def rva_off(pe, rva):
    for s in pe["sections"]:
        if s["va"] <= rva < s["va"] + max(s["vs"], s["rs"]):
            return s["rp"] + rva - s["va"]
    raise KeyError(hex(rva))


def guard(blob, pe, rva, expected, label):
    o = rva_off(pe, rva)
    actual = bytes(blob[o:o + len(expected)])
    if actual != expected:
        raise SystemExit(
            f"{label}: expected {expected.hex()} at 0x{rva:X}, "
            f"got {actual.hex()}")
    return o


def assemble():
    """Emit the payload of stage3aq_h4_capture_eye_gate.S.

    Eight instructions, no external relocations, so the bytes are produced here
    instead of shelling out to a linker. Every displacement is derived from the
    recorded RVAs, and the result is decoded back and checked against the
    intended program before it is allowed near the DLL.
    """
    title = DEFS["TITLE_ACTIVE_BYTE_RVA"]
    active = DEFS["RETICLE_CAPTURE_ACTIVE_RVA"]

    a = CODE_RVA          # cmp byte ptr [rip+d], 4        7 bytes
    b = a + 7             # jne .Lstock                    2
    c = b + 2             # cmp byte ptr [rip+d], 0        7
    d = c + 7             # jne .Ladmit                    2
    stock = d + 2         # cmp r9d, 1                     4
    f = stock + 4         # ja  REDIRECT_REFUSE_RVA        6
    admit = f + 6         # jmp REDIRECT_CONTINUE_RVA      5
    end = admit + 5

    code = b"".join((
        bytes([0x80, 0x3D]) + struct.pack("<i", title - (a + 7)) + bytes([0x04]),
        bytes([0x75]) + struct.pack("<b", stock - (b + 2)),
        bytes([0x80, 0x3D]) + struct.pack("<i", active - (c + 7)) + bytes([0x00]),
        bytes([0x75]) + struct.pack("<b", admit - (d + 2)),
        bytes([0x41, 0x83, 0xF9, 0x01]),
        bytes([0x0F, 0x87]) + struct.pack("<i", REDIRECT_REFUSE_RVA - (f + 6)),
        bytes([0xE9]) + struct.pack("<i", REDIRECT_CONTINUE_RVA - (admit + 5)),
    ))
    if len(code) != end - CODE_RVA:
        raise SystemExit("Stage3AQ layout mismatch")
    verify(code)
    return code


def verify(code):
    """Decode the emitted bytes and require exactly the intended program."""
    try:
        from capstone import CS_ARCH_X86, CS_MODE_64, Cs
    except ImportError:
        raise SystemExit("capstone is required to verify the Stage3AQ payload")

    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True
    decoded = list(md.disasm(code, CODE_RVA))
    for i in decoded:
        print(f"  0x{i.address:06X}  {i.bytes.hex():<14} {i.mnemonic} {i.op_str}")
    if [i.mnemonic for i in decoded] != \
            ["cmp", "jne", "cmp", "jne", "cmp", "ja", "jmp"]:
        raise SystemExit(
            "Stage3AQ decode mismatch: " + str([i.mnemonic for i in decoded]))

    def rip_target(insn):
        for operand in insn.operands:
            if operand.type == 3 and operand.mem.base == 41:   # X86_OP_MEM, RIP
                return insn.address + insn.size + operand.mem.disp
        raise SystemExit("expected a rip-relative operand")

    checks = (
        (rip_target(decoded[0]), DEFS["TITLE_ACTIVE_BYTE_RVA"], "active title"),
        (rip_target(decoded[2]), DEFS["RETICLE_CAPTURE_ACTIVE_RVA"],
         "capture-active flag"),
        (decoded[1].operands[0].imm, decoded[4].address, "jne .Lstock"),
        (decoded[3].operands[0].imm, decoded[6].address, "jne .Ladmit"),
        (decoded[5].operands[0].imm, REDIRECT_REFUSE_RVA, "ja refuse"),
        (decoded[6].operands[0].imm, REDIRECT_CONTINUE_RVA, "jmp continue"),
    )
    for got, want, label in checks:
        if got != want:
            raise SystemExit(
                f"Stage3AQ {label}: resolved 0x{got:X}, expected 0x{want:X}")
    # The stock path must be byte-identical to the instruction pair it replaces.
    stock_pair = code[decoded[4].address - CODE_RVA:
                      decoded[5].address - CODE_RVA + decoded[5].size]
    if stock_pair[:4] != EYE_GATE_EXPECT[:4]:
        raise SystemExit("Stage3AQ stock path does not match the original cmp")


def main():
    if len(sys.argv) != 3:
        raise SystemExit(
            "usage: build_stage3aq_h4_capture_eye_gate.py "
            "<Stage3AP-HaloMCCVR.dll> <output.dll>")
    src, out = Path(sys.argv[1]), Path(sys.argv[2])

    blob = bytearray(src.read_bytes())
    sha = hashlib.sha256(blob).hexdigest()
    print("input", sha)
    if sha != EXPECTED_STAGE3AP_SHA256:
        raise SystemExit("wrong Stage3AP input DLL: " + sha)

    code = assemble()
    print("code", hashlib.sha256(code).hexdigest(), len(code))
    if len(code) > 0x1000:
        raise SystemExit("Stage3AQ payload exceeds one page")

    pe = parse_pe(blob)
    if pe["n"] != 12:
        raise SystemExit(f"unexpected PE geometry: n={pe['n']}")
    qd = next((s for s in pe["sections"] if s["name"] == ".s3qd"), None)
    if not qd or qd["va"] != 0x2F3000 or qd["vs"] != 0x6000 or \
            qd["rs"] != 0x6000:
        raise SystemExit("unexpected .s3qd geometry")
    if qd["rp"] + qd["rs"] != len(blob):
        raise SystemExit("unexpected overlay after .s3qd")

    for rva, expect, label in CONTEXT:
        guard(blob, pe, rva, expect, label)
    gate_off = guard(blob, pe, EYE_GATE_RVA, EYE_GATE_EXPECT,
                     "eye-range refusal")

    jump = bytes([0xE9]) + struct.pack("<i", CODE_RVA - (EYE_GATE_RVA + 5))
    replacement = jump + bytes([0x90]) * (len(EYE_GATE_EXPECT) - len(jump))
    blob[gate_off:gate_off + len(EYE_GATE_EXPECT)] = replacement
    print(f"eye-range refusal: 0x{EYE_GATE_RVA:X} {EYE_GATE_EXPECT.hex()} -> "
          f"{replacement.hex()}")

    blob.extend(code.ljust(0x1000, b"\0"))
    struct.pack_into("<I", blob, qd["h"] + 8, 0x7000)          # VirtualSize
    struct.pack_into("<I", blob, qd["h"] + 16, 0x7000)         # SizeOfRawData
    struct.pack_into("<I", blob, pe["opt"] + 0x38, 0x2FA000)   # SizeOfImage
    struct.pack_into("<I", blob, pe["opt"] + 8,
                     struct.unpack_from("<I", blob, pe["opt"] + 8)[0] + 0x1000)
    if struct.unpack_from("<I", blob, pe["opt"] + 0x40)[0] != 0:
        raise SystemExit("unexpected non-zero PE checksum")

    out.write_bytes(bytes(blob))
    print("output", hashlib.sha256(bytes(blob)).hexdigest(), len(blob))


if __name__ == "__main__":
    main()
