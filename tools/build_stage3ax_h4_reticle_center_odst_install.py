"""Stage 3AX - isolate H4 centre reticle and restore deterministic ODST install.

Input is the exact headset-tested Stage 3AW DLL.  The accepted Halo 4 pause
stock-wrapper path is byte-identical.  Two bounded changes are made:

* Relocate Stage 3AW's validated selected-transform framing helper into unused
  .s3qd space and add one more factor-of-two in each viewport dimension and
  centring offset.  The official H4EK reticle is centred under this transform;
  the tighter 4x authored crop excludes outer visor/HUD pixels.
* Once ODST's frozen-then-ticking level-liveness proof has passed, stop making
  hook installation depend on sampling the engine's 10 Hz tail boolean in its
  zero phase.  The original camera-mode check still executes for diagnostics;
  the existing post-install debounce remains the authority that waits for a
  valid redirectable camera before arming stereo.
"""

from pathlib import Path
import hashlib
import struct
import sys

from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86_const import X86_GRP_CALL, X86_GRP_JUMP, X86_OP_MEM, X86_REG_RIP


EXPECTED_STAGE3AW_SHA256 = \
    "ad0c6bbca337f2436a258cb4a0cb9da5884b20270bc2f9dadf7a06daba1ed676"

OLD_HELPER_START = 0x002FA7AA
OLD_HELPER_END = 0x002FA890
NEW_HELPER_START = 0x002FA9A0
NEW_HELPER_LIMIT = 0x002FAB00
RETICLE_CALL_RVA = 0x000539C0
RETICLE_CALL_EXPECT = bytes.fromhex("e8 e5 6d 2a 00 90 90 90 90 90")

# Stage 3AW calls OdstCameraArraySupportsMode(cameraArray, false), then only
# installs on AL!=0.  Liveness has already been proven at this point.  Preserve
# the call/test and change only the conditional install edge to unconditional.
ODST_READY_BRANCH_RVA = 0x00067CE5
ODST_READY_CONTEXT = bytes.fromhex("33 d2 49 8b c9 e8 6d 96 ff ff 84 c0 75 32")

# Insert an additional doubling after Stage 3AW's existing 4x extent and 2x
# half-extent calculations: width/height become 8*halfExtent and top-left uses
# 256-4*halfExtent.  Since the source raster spans 2*halfExtent, this is a 4x
# centre magnification.
INSERT_AFTER = {
    0x002FA811: bytes.fromhex("f3 0f 58 d2"),  # addss xmm2,xmm2
    0x002FA81C: bytes.fromhex("f3 0f 58 db"),  # addss xmm3,xmm3
    0x002FA820: bytes.fromhex("f3 0f 58 c0"),  # addss xmm0,xmm0
    0x002FA824: bytes.fromhex("f3 0f 58 c9"),  # addss xmm1,xmm1
}


def parse_pe(blob):
    p = struct.unpack_from("<I", blob, 0x3C)[0]
    if blob[:2] != b"MZ" or blob[p:p + 4] != b"PE\0\0":
        raise SystemExit("input is not PE")
    coff = p + 4
    n = struct.unpack_from("<H", blob, coff + 2)[0]
    opt_size = struct.unpack_from("<H", blob, coff + 16)[0]
    opt = coff + 20
    table = opt + opt_size
    sections = []
    for i in range(n):
        h = table + i * 40
        name = bytes(blob[h:h + 8]).split(b"\0", 1)[0].decode("ascii")
        vs, va, rs, rp = struct.unpack_from("<IIII", blob, h + 8)
        sections.append(dict(name=name, vs=vs, va=va, rs=rs, rp=rp))
    return dict(opt=opt, sections=sections)


def rva_off(pe, rva):
    for s in pe["sections"]:
        if s["va"] <= rva < s["va"] + max(s["vs"], s["rs"]):
            return s["rp"] + rva - s["va"]
    raise KeyError(hex(rva))


def read_rva(blob, pe, rva, size):
    o = rva_off(pe, rva)
    return bytes(blob[o:o + size])


def guard(blob, pe, rva, expected, label):
    actual = read_rva(blob, pe, rva, len(expected))
    if actual != expected:
        raise SystemExit(
            f"{label}: expected {expected.hex()} at 0x{rva:X}, got {actual.hex()}")
    return rva_off(pe, rva)


def relocate_helper(blob, pe):
    source = read_rva(
        blob, pe, OLD_HELPER_START, OLD_HELPER_END - OLD_HELPER_START)
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True
    insns = list(md.disasm(source, OLD_HELPER_START))
    if not insns or insns[-1].address + insns[-1].size != OLD_HELPER_END:
        raise SystemExit("Stage3AW helper did not decode to its exact boundary")
    if set(INSERT_AFTER) - {i.address for i in insns}:
        raise SystemExit("one or more framing insertion sites did not decode")

    # Inserting the four doublings pushes two formerly edge-of-range short
    # branches past +/-127.  Promote only those branches to their standard
    # rel32 forms, iterating the layout until no further promotion is needed.
    promoted = set()
    while True:
        mapping = {}
        cursor = NEW_HELPER_START
        for insn in insns:
            mapping[insn.address] = cursor
            encoded_size = (6 if insn.address in promoted and
                            0x70 <= insn.bytes[0] <= 0x7F else
                            5 if insn.address in promoted and
                            insn.bytes[0] == 0xEB else insn.size)
            cursor += encoded_size + len(INSERT_AFTER.get(insn.address, b""))
        mapping[OLD_HELPER_END] = cursor
        changed = False
        for insn in insns:
            if insn.encoding.imm_size != 1 or not \
                    (insn.group(X86_GRP_JUMP) or insn.group(X86_GRP_CALL)):
                continue
            target = insn.operands[0].imm
            if OLD_HELPER_START <= target <= OLD_HELPER_END:
                target = mapping[target]
            size = (6 if insn.address in promoted and
                    0x70 <= insn.bytes[0] <= 0x7F else
                    5 if insn.address in promoted else insn.size)
            displacement = target - (mapping[insn.address] + size)
            if not -128 <= displacement <= 127 and insn.address not in promoted:
                promoted.add(insn.address)
                changed = True
        if not changed:
            break
    if cursor > NEW_HELPER_LIMIT:
        raise SystemExit("relocated helper exceeds reserved Stage3AW tail")

    out = bytearray()
    for insn in insns:
        new_address = mapping[insn.address]
        if insn.address in promoted and 0x70 <= insn.bytes[0] <= 0x7F:
            target = insn.operands[0].imm
            if OLD_HELPER_START <= target <= OLD_HELPER_END:
                target = mapping[target]
            encoded = bytearray((0x0F, 0x80 + insn.bytes[0] - 0x70))
            encoded.extend(struct.pack("<i", target - (new_address + 6)))
        elif insn.address in promoted and insn.bytes[0] == 0xEB:
            target = insn.operands[0].imm
            if OLD_HELPER_START <= target <= OLD_HELPER_END:
                target = mapping[target]
            encoded = bytearray((0xE9,))
            encoded.extend(struct.pack("<i", target - (new_address + 5)))
        else:
            encoded = bytearray(insn.bytes)

        if insn.address not in promoted and \
                (insn.group(X86_GRP_JUMP) or insn.group(X86_GRP_CALL)) and \
                insn.operands and insn.operands[0].type == 2:
            target = insn.operands[0].imm
            if OLD_HELPER_START <= target <= OLD_HELPER_END:
                target = mapping[target]
            size = insn.encoding.imm_size
            off = insn.encoding.imm_offset
            value = target - (new_address + insn.size)
            encoded[off:off + size] = int(value).to_bytes(
                size, "little", signed=True)

        for operand in insn.operands:
            if operand.type == X86_OP_MEM and operand.mem.base == X86_REG_RIP:
                target = insn.address + insn.size + operand.mem.disp
                size = insn.encoding.disp_size
                off = insn.encoding.disp_offset
                value = target - (new_address + insn.size)
                encoded[off:off + size] = int(value).to_bytes(
                    size, "little", signed=True)
                break

        out.extend(encoded)
        out.extend(INSERT_AFTER.get(insn.address, b""))
    return bytes(out)


def main():
    if len(sys.argv) != 3:
        raise SystemExit(
            "usage: build_stage3ax_h4_reticle_center_odst_install.py "
            "<Stage3AW-HaloMCCVR.dll> <output.dll>")
    src, dst = Path(sys.argv[1]), Path(sys.argv[2])
    blob = bytearray(src.read_bytes())
    sha = hashlib.sha256(blob).hexdigest()
    print("input", sha)
    if sha != EXPECTED_STAGE3AW_SHA256:
        raise SystemExit("wrong Stage3AW input: " + sha)
    pe = parse_pe(blob)

    call_off = guard(blob, pe, RETICLE_CALL_RVA, RETICLE_CALL_EXPECT,
                     "Stage3AW reticle helper call")
    odst_context_rva = ODST_READY_BRANCH_RVA - 12
    guard(blob, pe, odst_context_rva, ODST_READY_CONTEXT,
          "ODST post-liveness readiness edge")
    free_off = rva_off(pe, NEW_HELPER_START)
    free_end = rva_off(pe, NEW_HELPER_LIMIT - 1) + 1
    if any(blob[free_off:free_end]):
        raise SystemExit("Stage3AX helper region is not zero-filled")

    helper = relocate_helper(blob, pe)
    blob[free_off:free_off + len(helper)] = helper
    call = b"\xE8" + struct.pack(
        "<i", NEW_HELPER_START - (RETICLE_CALL_RVA + 5))
    blob[call_off:call_off + len(RETICLE_CALL_EXPECT)] = \
        call + b"\x90" * (len(RETICLE_CALL_EXPECT) - len(call))

    odst_off = rva_off(pe, ODST_READY_BRANCH_RVA)
    blob[odst_off] = 0xEB

    if struct.unpack_from("<I", blob, pe["opt"] + 0x40)[0] != 0:
        raise SystemExit("unexpected non-zero PE checksum")
    if len(blob) != len(src.read_bytes()):
        raise SystemExit("Stage3AX must not change file size")
    dst.write_bytes(blob)
    print("helper", hex(NEW_HELPER_START), len(helper),
          hashlib.sha256(helper).hexdigest())
    print("reticle call", hex(RETICLE_CALL_RVA), "->", hex(NEW_HELPER_START))
    print("ODST ready branch", hex(ODST_READY_BRANCH_RVA), "75 -> EB")
    print("output", hashlib.sha256(blob).hexdigest(), len(blob))


if __name__ == "__main__":
    main()
