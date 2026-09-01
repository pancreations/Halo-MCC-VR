"""Stage 3AW - Halo 4 pause stock-state + authored-reticle capture space.

This forward-only Stage 3AW candidate consumes the exact headset-tested Stage
3AV DLL and keeps all AP-through-AV code in place.

The Halo 4 capture replay already executes the H4EK-proven type-0x28 command,
which pushes a 0x34-byte reticle-only transform.  Stage 3AV then safely reads
that top entry but only records telemetry.  Stage 3AW calls a guarded helper
at that exact seam.  Capture begins at that exact marker, not at an unrelated
earlier CUI command.  The helper validates the selected transform's live half
extents and publishes a symmetric 2x viewport into the already-owned capture
state, which every later exact OM rebind reasserts.  The companion H4-only
initial viewport becomes a neutral 512x512.  This removes Stage 3AV's stale
resolution-class framing without changing the authored matrix itself.

No full-size intermediate, new D3D hook, resource lifetime, game-file patch,
or other-title path is introduced.  A refusal increments the existing Halo 4
reticle failure counter; the blank-art guard retains the procedural quad.

The independent pause change fixes the measured producer mismatch: while the
head-locked pause target is active, Halo4WrapperBody now takes its existing
stock-wrapper tail so the stock backbuffer feeding the pause quad is rendered.
The Halo 4 core remains armed.  Halo 4's H4EK-proven native pause-reason-3 bit
is latched while the start-menu component exists; its later clear restores
stereo and stamps the established Stage 3X resume grace.  No raw A/B input is
treated as native pause state, and Stage 3X's existing B/Y+B exit remains
intact.
"""

from pathlib import Path
import hashlib
import struct
import sys

sys.path.insert(0, str(Path(__file__).parent))
from postlink import build_payload  # noqa: E402


EXPECTED_STAGE3AV_SHA256 = \
    "2baf0a3e7d654a0cda701399d672cec7c582202e138537f8193e34e5d72aca16"

PAYLOAD_RVA = 0x002FA790
PAYLOAD_LIMIT = 0x002FB000

RETICLE_FAILURES_RVA = 0x002A7518
CAPTURE_ACTIVE_RVA = 0x002AE770
CAPTURE_VIEWPORT_RVA = 0x002AE774
PAUSE_TARGET_RVA = 0x0002EEB0
REQUEST_PAUSE_RVA = 0x00030480
H4_ARMED_RVA = 0x002A71E9
H4_ARMED_CONTINUE_RVA = 0x00058BF5
H4_STOCK_TAIL_RVA = 0x00058D5D
H4_MODULE_REF_RVA = 0x002A7208
PAUSE_GRACE_RVA = 0x002F31C8
IAT_GET_TICK_COUNT64_RVA = 0x00180150

# Capture-replay branch, immediately after Halo4SafeRead copied the pushed
# top transform to [rsp+0x48].  The displaced MOVZX/CMP feed the original JE
# at 0x539CA and are replayed at the helper's return edge.
RETICLE_SPLICE_RVA = 0x000539C0
RETICLE_SPLICE_EXPECT = bytes.fromhex(
    "45 0F B7 C4 81 FF 00 00 00 80")

# Delay target binding/clear until the exact type-0x28/0x0C marker.  The
# original branch starts the capture on the first unrelated CUI command.
CAPTURE_BEGIN_SPLICE_RVA = 0x000538D3
CAPTURE_BEGIN_SPLICE_EXPECT = bytes.fromhex(
    "43 38 5C 07 05 75 3A")
CAPTURE_BEGIN_CALL_RVA = 0x00053914
CAPTURE_BEGIN_ATTEMPT_RVA = 0x000538DA

# Halo4WrapperBody's armed load.  Pause branches before the normal admission
# gate directly to the existing stock-wrapper tail, avoiding false NotArmed
# telemetry while the H4 core is still live.
PAUSE_SPLICE_RVA = 0x00058BED
PAUSE_SPLICE_EXPECT = bytes.fromhex(
    "0F B6 05 F5 E5 24 00 90")

# Stage 3AU/AV's resolution-coupled H4 capture viewport.  The sole reference
# is the title==Halo4 load at 0x11C77.  BeginAuthoredReticleCaptureInternal's
# existing formula turns 512x512 into TopLeft 0/0; its 0..512 scissor and each
# exact OM-rebind framing reassert remain untouched.
VIEWPORT_CONST_RVA = 0x001BF0C0
VIEWPORT_CONST_EXPECT = struct.pack(
    "<ffff", 7572.0, 4258.068, 0.0, 1.0)
VIEWPORT_CONST_PATCH = struct.pack(
    "<ffff", 512.0, 512.0, 0.0, 1.0)

CONTEXT = (
    (0x000539B2, bytes.fromhex("0F 11 44 24 68 E8 54 32 00 00"),
     "capture top-transform SafeRead call"),
    (0x000539BC, bytes.fromhex("85 C0 74 4A"),
     "capture SafeRead success gate"),
    (0x00053934, bytes.fromhex("38 5C 24 21"),
     "exact-marker payload-readable gate"),
    (0x000539CA, bytes.fromhex("74 36 48 8D 0D 75 43 25 00"),
     "capture telemetry continuation"),
    (CAPTURE_BEGIN_ATTEMPT_RVA, bytes.fromhex(
        "43 C6 44 07 05 01 E8 BB 7E FD FF"),
     "exact-marker capture-attempt continuation"),
    (CAPTURE_BEGIN_CALL_RVA, bytes.fromhex(
        "4C 8B 4C 24 38 49 8B D5"),
     "capture original-command call path"),
    (PAUSE_TARGET_RVA, bytes.fromhex("0F B6 05 C4 FA 27 00 90 C3"),
     "pause-target getter"),
    (REQUEST_PAUSE_RVA, bytes.fromhex("0F B6 C1 86 05 F2 E4 27 00"),
     "pause-presentation request"),
    (H4_ARMED_CONTINUE_RVA, bytes.fromhex("84 C0 0F 84 56 01 00 00"),
     "H4 armed-gate continuation"),
    (H4_STOCK_TAIL_RVA, bytes.fromhex(
        "44 8B C6 48 8B D7 F0 48 FF 05 45 E7 24 00"),
     "H4 existing stock-wrapper tail"),
    (0x0002FFDE, bytes.fromhex(
        "4C 8D 05 8F E7 27 00 BA 01 00 00 00 FF 90 60 01 00 00"),
     "exact OM rebind reasserts saved capture viewport"),
    (0x00011C73, bytes.fromhex(
        "3C 04 75 16 66 0F 6F 0D 41 D4 1A 00"),
     "H4-only capture viewport load"),
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
    st = opt + osz
    sections = []
    for index in range(n):
        offset = st + index * 40
        name = bytes(blob[offset:offset + 8]).split(b"\0", 1)[0].decode(
            "ascii")
        virtual_size, virtual_address, raw_size, raw_pointer = \
            struct.unpack_from("<IIII", blob, offset + 8)
        sections.append(dict(
            name=name, vs=virtual_size, va=virtual_address,
            rs=raw_size, rp=raw_pointer, header=offset))
    return dict(opt=opt, n=n, sections=sections)


def rva_offset(pe, rva):
    for section in pe["sections"]:
        if section["va"] <= rva < \
                section["va"] + max(section["vs"], section["rs"]):
            return section["rp"] + rva - section["va"]
    raise KeyError(hex(rva))


def guard(blob, pe, rva, expected, label):
    offset = rva_offset(pe, rva)
    actual = bytes(blob[offset:offset + len(expected)])
    if actual != expected:
        raise SystemExit(
            f"{label}: expected {expected.hex()} at 0x{rva:X}, "
            f"got {actual.hex()}")
    return offset


def call_bytes(source_rva, target_rva):
    return b"\xE8" + struct.pack("<i", target_rva - (source_rva + 5))


def jump_bytes(source_rva, target_rva):
    return b"\xE9" + struct.pack("<i", target_rva - (source_rva + 5))


def assert_sole_viewport_reference(blob, pe):
    """Prove the patched 16-byte viewport is H4-only and singly loaded."""
    from capstone import Cs, CS_ARCH_X86, CS_MODE_64
    from capstone.x86_const import X86_OP_MEM, X86_REG_RIP

    text_section = next(
        section for section in pe["sections"] if section["name"] == ".text")
    text_blob = bytes(blob[
        text_section["rp"]:text_section["rp"] + text_section["rs"]])
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True
    # The linked .text section contains small embedded data islands.  Without
    # skipdata Capstone stops at the first one and can falsely report that a
    # later RIP-relative reference is absent.
    md.skipdata = True
    hits = set()
    for instruction in md.disasm(text_blob, text_section["va"]):
        if instruction.id == 0:
            continue
        for operand in instruction.operands:
            if operand.type == X86_OP_MEM and \
                    operand.mem.base == X86_REG_RIP:
                target = instruction.address + instruction.size + \
                    operand.mem.disp
                if VIEWPORT_CONST_RVA <= target < VIEWPORT_CONST_RVA + 16:
                    hits.add(instruction.address)
    if hits != {0x00011C77}:
        raise SystemExit(
            "H4 viewport constant references changed: " +
            ", ".join(f"0x{hit:X}" for hit in sorted(hits)))
    print("viewport constant sole reference: 0x11C77")


def verify_payload(code, code_rva, expected_targets):
    from capstone import Cs, CS_ARCH_X86, CS_MODE_64
    from capstone.x86_const import X86_OP_IMM, X86_OP_MEM, X86_REG_RIP

    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True
    targets = set()
    for instruction in md.disasm(code, code_rva):
        for operand in instruction.operands:
            if operand.type == X86_OP_MEM and \
                    operand.mem.base == X86_REG_RIP:
                targets.add(
                    instruction.address + instruction.size + operand.mem.disp)
            elif operand.type == X86_OP_IMM:
                target = operand.imm
                if not code_rva <= target < code_rva + len(code) + 0x40:
                    targets.add(target)
    for label, target in expected_targets.items():
        if target not in targets:
            raise SystemExit(
                f"payload verify: no reference to {label} 0x{target:X}")
    print(f"verify ok: {len(targets)} external targets, "
          f"{len(expected_targets)} asserted")


def main():
    if len(sys.argv) != 3:
        raise SystemExit(
            "usage: build_stage3aw_h4_pause_stock_and_reticle.py "
            "<Stage3AV-HaloMCCVR.dll> <output.dll>")
    source = Path(sys.argv[1])
    output = Path(sys.argv[2])

    blob = bytearray(source.read_bytes())
    input_hash = hashlib.sha256(blob).hexdigest()
    print("input", input_hash)
    if input_hash != EXPECTED_STAGE3AV_SHA256:
        raise SystemExit("wrong Stage3AV input DLL: " + input_hash)

    pe = parse_pe(blob)
    if pe["n"] != 12:
        raise SystemExit(f"unexpected PE geometry: n={pe['n']}")
    qd = next(
        (section for section in pe["sections"]
         if section["name"] == ".s3qd"), None)
    if not qd or qd["va"] != 0x2F3000 or qd["vs"] != 0x8000 or \
            qd["rs"] != 0x8000:
        raise SystemExit("unexpected .s3qd geometry")

    for rva, expected, label in CONTEXT:
        guard(blob, pe, rva, expected, label)
    reticle_offset = guard(
        blob, pe, RETICLE_SPLICE_RVA, RETICLE_SPLICE_EXPECT,
        "capture-local framing splice")
    capture_begin_offset = guard(
        blob, pe, CAPTURE_BEGIN_SPLICE_RVA, CAPTURE_BEGIN_SPLICE_EXPECT,
        "exact-marker capture-begin splice")
    pause_offset = guard(
        blob, pe, PAUSE_SPLICE_RVA, PAUSE_SPLICE_EXPECT,
        "H4 pause stock-wrapper splice")
    viewport_offset = guard(
        blob, pe, VIEWPORT_CONST_RVA, VIEWPORT_CONST_EXPECT,
        "Stage3AV H4 viewport constant")
    assert_sole_viewport_reference(blob, pe)

    code, symbols = build_payload(
        Path(__file__).with_name(
            "stage3aw_h4_pause_stock_and_reticle.S"),
        PAYLOAD_RVA,
        {
            "RETICLE_FAILURES_RVA": RETICLE_FAILURES_RVA,
            "CAPTURE_ACTIVE_RVA": CAPTURE_ACTIVE_RVA,
            "CAPTURE_VIEWPORT_RVA": CAPTURE_VIEWPORT_RVA,
            "CAPTURE_BEGIN_CALL_RVA": CAPTURE_BEGIN_CALL_RVA,
            "CAPTURE_BEGIN_ATTEMPT_RVA": CAPTURE_BEGIN_ATTEMPT_RVA,
            "PAUSE_TARGET_RVA": PAUSE_TARGET_RVA,
            "REQUEST_PAUSE_RVA": REQUEST_PAUSE_RVA,
            "H4_ARMED_RVA": H4_ARMED_RVA,
            "H4_ARMED_CONTINUE_RVA": H4_ARMED_CONTINUE_RVA,
            "H4_STOCK_TAIL_RVA": H4_STOCK_TAIL_RVA,
            "H4_MODULE_REF_RVA": H4_MODULE_REF_RVA,
            "PAUSE_GRACE_RVA": PAUSE_GRACE_RVA,
            "IAT_GET_TICK_COUNT64_RVA": IAT_GET_TICK_COUNT64_RVA,
        },
        ("stage3aw_capture_begin_gate", "stage3aw_capture_local_framing",
         "stage3aw_pause_stock_gate"))
    print("payload", hashlib.sha256(code).hexdigest(), len(code))
    if symbols["stage3aw_capture_begin_gate"] != PAYLOAD_RVA:
        raise SystemExit("capture-begin gate moved from payload start")
    if PAYLOAD_RVA + len(code) > PAYLOAD_LIMIT:
        raise SystemExit(
            f"payload does not fit Stage3AW page tail: {len(code)}")
    verify_payload(code, PAYLOAD_RVA, {
        "Halo 4 reticle failures": RETICLE_FAILURES_RVA,
        "capture-active byte": CAPTURE_ACTIVE_RVA,
        "saved capture viewport": CAPTURE_VIEWPORT_RVA,
        "capture original-command path": CAPTURE_BEGIN_CALL_RVA,
        "capture attempt path": CAPTURE_BEGIN_ATTEMPT_RVA,
        "pause-target getter": PAUSE_TARGET_RVA,
        "pause-presentation request": REQUEST_PAUSE_RVA,
        "H4 armed byte": H4_ARMED_RVA,
        "H4 armed continuation": H4_ARMED_CONTINUE_RVA,
        "H4 stock-wrapper tail": H4_STOCK_TAIL_RVA,
        "loader-pinned halo4 base": H4_MODULE_REF_RVA,
        "pause resume grace": PAUSE_GRACE_RVA,
        "GetTickCount64 IAT": IAT_GET_TICK_COUNT64_RVA,
    })

    payload_offset = rva_offset(pe, PAYLOAD_RVA)
    payload_limit_offset = rva_offset(pe, PAYLOAD_LIMIT - 1) + 1
    if any(blob[payload_offset:payload_limit_offset]):
        raise SystemExit("Stage3AW payload region is not free")
    blob[payload_offset:payload_offset + len(code)] = code

    replacement = call_bytes(
        RETICLE_SPLICE_RVA,
        symbols["stage3aw_capture_local_framing"])
    replacement += b"\x90" * (
        len(RETICLE_SPLICE_EXPECT) - len(replacement))
    blob[reticle_offset:reticle_offset + len(replacement)] = replacement

    capture_begin_replacement = jump_bytes(
        CAPTURE_BEGIN_SPLICE_RVA, symbols["stage3aw_capture_begin_gate"])
    capture_begin_replacement += b"\x90" * (
        len(CAPTURE_BEGIN_SPLICE_EXPECT) - len(capture_begin_replacement))
    blob[capture_begin_offset:
         capture_begin_offset + len(capture_begin_replacement)] = \
        capture_begin_replacement
    print(
        f"capture begin gate: 0x{CAPTURE_BEGIN_SPLICE_RVA:X} -> "
        f"0x{symbols['stage3aw_capture_begin_gate']:X}")

    pause_replacement = jump_bytes(
        PAUSE_SPLICE_RVA, symbols["stage3aw_pause_stock_gate"])
    pause_replacement += b"\x90" * (
        len(PAUSE_SPLICE_EXPECT) - len(pause_replacement))
    blob[pause_offset:pause_offset + len(pause_replacement)] = \
        pause_replacement
    print(
        f"pause stock gate: 0x{PAUSE_SPLICE_RVA:X} -> "
        f"0x{symbols['stage3aw_pause_stock_gate']:X}")

    blob[viewport_offset:viewport_offset + len(VIEWPORT_CONST_PATCH)] = \
        VIEWPORT_CONST_PATCH

    if struct.unpack_from("<I", blob, pe["opt"] + 0x40)[0] != 0:
        raise SystemExit("unexpected non-zero PE checksum")
    if len(blob) != 2919424:
        raise SystemExit("output size changed unexpectedly")

    output.write_bytes(blob)
    print("output", hashlib.sha256(blob).hexdigest(), len(blob))


if __name__ == "__main__":
    main()
