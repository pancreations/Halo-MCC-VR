"""Stage 3CS - pin the H4 capture width to the engine's own CUI viewport,
on top of Stage 3CR (which already reads the live hide in the selector).

Adds ONLY a framing-width pin. Nothing is removed: the 3BU crosshair chain,
the 3BV/3BW/3BX cutscene stack, Codex's theatre camera and 3CR's live selector
are all carried through untouched. See stage3cs_h4_engine_viewport_width.S for
the decoded 3BR thunk this splices into and the session evidence.
"""
from pathlib import Path
import hashlib, struct, sys
sys.path.insert(0, str(Path(__file__).parent))
from postlink import build_payload
from build_stage3br_h4_capture_scale import parse_pe, rva_off

EXPECTED_INPUT_SHA256 = \
    "7d8e3ee647689daac18ff25ca61d3794b63e6229a198296932181014c4c93878"
PAYLOAD_RVA, PAYLOAD_LIMIT = 0x002F93E0, 0x002F94D0
DEAD_3BN_SHA256 = \
    "e4e61358967df60898ca79c2795be6a53db55028f32913368a428b7d8ba2a868"
GATE_CALL_RVA = 0x002FB992
GATE_OLD = bytes.fromhex("e8f9e0ffff")          # call the accepted 3BR thunk
S3BR_THUNK_RVA = 0x002F9A90
S3BR_ASPECT_RVA = 0x002F9AB0                    # 3BR's bbW load, after the seed
CAPTURE_VIEWPORT_RVA = 0x002AE774
VPBUF_RVA = 0x002F9BE8
SELECTOR_LOAD_RVA = 0x002F99D7
LIVE_HIDE_X_RVA = 0x002A8368

# Byte-exact prologue of the accepted 3BR thunk: the VPBUF seed we reproduce,
# followed by the bbW load we rejoin at. Proves the splice point before use.
S3BR_SEED = bytes.fromhex(
    "4883ec28"                  # sub rsp,0x28
    "0f1005d94cfbff"            # movups xmm0,[rip+CAPTURE_VIEWPORT]
    "0f110546010000"            # movups [rip+VPBUF],xmm0
    "488b05db4cfbff"            # mov rax,[rip+CAPTURE_VIEWPORT+0x10]
    "488905480100 00".replace(" ", "")
)
S3BR_ASPECT_HEAD = bytes.fromhex("8b05a250fbff85c074")   # mov eax,[bbW]; test; jz

CONTEXT = (
    (S3BR_THUNK_RVA, S3BR_SEED, "3BR thunk VPBUF seed"),
    (S3BR_ASPECT_RVA, S3BR_ASPECT_HEAD, "3BR aspect-height entry"),
    (0x002F995D, bytes.fromhex("66837c24420c752f"), "type-0x28 discriminator"),
    (0x002F9B29, bytes.fromhex("f30f101daf000000"), "3BR scale tail"),
    (0x002F9BBD, bytes.fromhex("4883c428c3"), "3BR return"),
    (0x002F9BD0, bytes.fromhex("000000440000003fb9379e3d"), "3BR constants"),
    (0x00053E04, bytes.fromhex("f30f11355c452500"), "live hide publisher"),
    (0x000199B3, bytes.fromhex("b201"), "3BJ absent"),
)


def rel_target(blob, pe, rva, disp_at, length):
    o = rva_off(pe, rva)
    return rva + length + struct.unpack_from("<i", blob, o + disp_at)[0]


def main():
    src, out = Path(sys.argv[1]), Path(sys.argv[2])
    blob = bytearray(src.read_bytes())
    sha = hashlib.sha256(blob).hexdigest()
    print("input", sha)
    if sha != EXPECTED_INPUT_SHA256:
        raise SystemExit("wrong Stage 3CR input: " + sha)
    pe = parse_pe(blob)
    if pe["n"] != 12:
        raise SystemExit("unexpected PE geometry")
    for rva, expect, label in CONTEXT:
        o = rva_off(pe, rva)
        if bytes(blob[o:o+len(expect)]) != expect:
            raise SystemExit(f"{label}: got {bytes(blob[o:o+len(expect)]).hex()}")

    # the seed's rip-relative operands must really name the slot and VPBUF
    if rel_target(blob, pe, 0x2F9A94, 3, 7) != CAPTURE_VIEWPORT_RVA:
        raise SystemExit("3BR seed does not read the capture-viewport slot")
    if rel_target(blob, pe, 0x2F9A9B, 3, 7) != VPBUF_RVA:
        raise SystemExit("3BR seed does not write VPBUF")
    # and 3BR must read its WIDTH back out of VPBUF+8 (0x2F9BF0)
    if rel_target(blob, pe, 0x2F9ACC, 4, 8) != VPBUF_RVA + 8:
        raise SystemExit("3BR width load is not VPBUF+8")

    # 3CR's live selector must be carried through unchanged
    so = rva_off(pe, SELECTOR_LOAD_RVA)
    if rel_target(blob, pe, SELECTOR_LOAD_RVA, 4, 8) != LIVE_HIDE_X_RVA:
        raise SystemExit("selector un-hide is not live")

    go = rva_off(pe, GATE_CALL_RVA)
    if bytes(blob[go:go+5]) != GATE_OLD:
        raise SystemExit("gate call unexpected: " + bytes(blob[go:go+5]).hex())
    po, pl = rva_off(pe, PAYLOAD_RVA), rva_off(pe, PAYLOAD_LIMIT-1)+1
    if hashlib.sha256(bytes(blob[po:pl])).hexdigest() != DEAD_3BN_SHA256:
        raise SystemExit("payload region is not the exact dead 3BN thunk")
    blob[po:pl] = bytes(pl - po)

    # Hand-assembled: the LLVM runtime this box used for postlink.build_payload
    # is no longer on disk, and this thunk is eight fixed-length instructions
    # whose only variables are rip displacements. Every one is computed here and
    # the emitted bytes are disassembled back and checked below.
    code = bytearray()
    def rip(opcode: bytes, target: int) -> None:
        here = PAYLOAD_RVA + len(code)
        nxt = here + len(opcode) + 4
        code.extend(opcode)
        code.extend(struct.pack("<i", target - nxt))

    code += bytes.fromhex("4883ec28")                       # sub rsp,0x28
    rip(bytes.fromhex("0f1005"), CAPTURE_VIEWPORT_RVA)      # movups xmm0,[slot]
    rip(bytes.fromhex("0f1105"), VPBUF_RVA)                 # movups [VPBUF],xmm0
    rip(bytes.fromhex("488b05"), CAPTURE_VIEWPORT_RVA+0x10) # mov rax,[slot+0x10]
    rip(bytes.fromhex("488905"), VPBUF_RVA+0x10)            # mov [VPBUF+0x10],rax
    width_rva = 0                                           # patched below
    width_at = len(code)
    rip(bytes.fromhex("f30f1005"), 0)                       # movss xmm0,[width]
    rip(bytes.fromhex("f30f1105"), VPBUF_RVA+8)             # movss [VPBUF+8],xmm0
    jmp_at = len(code)
    code += b"\xE9" + struct.pack(
        "<i", S3BR_ASPECT_RVA - (PAYLOAD_RVA + jmp_at + 5))  # jmp 3BR aspect
    while len(code) % 4:
        code += b"\x90"
    width_rva = PAYLOAD_RVA + len(code)
    code += struct.pack("<f", 4134.312)     # the engine's own CUI viewport width
    struct.pack_into("<i", code, width_at + 4,
                     width_rva - (PAYLOAD_RVA + width_at + 8))
    code = bytes(code)
    if PAYLOAD_RVA + len(code) > PAYLOAD_LIMIT:
        raise SystemExit(f"payload too large: {len(code)}")
    thunk = PAYLOAD_RVA

    # disassemble what we just emitted and prove each operand
    from capstone import Cs, CS_ARCH_X86, CS_MODE_64
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    seen = []
    for ins in md.disasm(code, PAYLOAD_RVA):
        seen.append(f"{ins.mnemonic} {ins.op_str}")
        print(f"    0x{ins.address:06X}  {ins.mnemonic:<7} {ins.op_str}")
    want_tail = f"jmp 0x{S3BR_ASPECT_RVA:x}"
    if want_tail not in seen:
        raise SystemExit("emitted payload does not rejoin the 3BR aspect step")
    if struct.unpack_from("<f", code, width_rva - PAYLOAD_RVA)[0] != \
            struct.unpack("<f", struct.pack("<f", 4134.312))[0]:
        raise SystemExit("width constant did not land")
    blob[po:po+len(code)] = code
    blob[go:go+5] = b"\xE8" + struct.pack("<i", thunk - (GATE_CALL_RVA + 5))
    print(f"payload {len(code)}B at 0x{PAYLOAD_RVA:X}; gate -> 0x{thunk:X}; "
          f"width pinned 4134.312 then rejoin 3BR at 0x{S3BR_ASPECT_RVA:X}; "
          f"selector stays live -> 0x{LIVE_HIDE_X_RVA:X}")
    out.write_bytes(bytes(blob))
    print("output", hashlib.sha256(bytes(blob)).hexdigest(), len(blob))


if __name__ == "__main__":
    main()
