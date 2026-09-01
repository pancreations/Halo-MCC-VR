"""Stage 3AV - Halo 4 menu-resume pause exit + capture depth-target fix.

Input is the exact Stage 3AU DLL. PE geometry is untouched; the payload lands
in the free tail of the .s3qd page at 0x2FA440 (after the Stage 3AU gate,
which ends at 0x2FA439).

1. Menu-resume pause exit. The 09:10 Steam log proves the Stage 3AU session
   entered pause via Y+B (mod injects Start, presentation -> head-locked 2D)
   and resumed via A on "Resume Game" - a path that produces no B edge, so
   Stage 3X's only Halo 4 pause-exit detection never fires and presentation
   stays head-locked 2D forever while stereo keeps rendering: the black
   screen. Fix modelled on the accepted C-H2-73 clock proof: resolve
   halo4.dll's own `game_paused` boolean through the DLL's existing
   FindDebugVarSlot (0x41E10, the exact function that resolves
   `enable_first_person_squish` at install), require it to read nonzero while
   the head-locked pause target is active, then treat 8 consecutive zero
   reads as the native resume and restore stereo the way the Stage 3X B-edge
   wrapper does (VR_RequestPausePresentation(false) + the same resume-grace
   stamp). If the debug var is not live in this build, one log line says so
   and behaviour is byte-identical to Stage 3AU.

2. Capture depth-target fix. OMSetRenderTargetsHook (0xDE60) forwards the
   game's DSV unchanged; during an active Halo 4 capture the reroute swaps in
   the private 512x512 target, and a bind that also carries the full-size
   depth target is an RTV/DSV dimension mismatch - D3D11 silently drops every
   draw. That matches zero ink at BOTH tested framings with "0 write
   failures", while the capture's own opening bind (null DSV) explains the
   single non-blank capture ever seen. The hook's argument-restore tail is
   spliced: only when the reroute reported true AND the capture is active AND
   the active title is Halo 4, the forwarded DSV becomes null; a one-time log
   line proves the case occurs at all.

Both Stage 3X chain edges (poll-gate call at 0x8431C, heartbeat jump at
0x56CA7) are retargeted to stubs that end by jumping into the original
Stage 3X code, whose addresses are READ from the input displacements, not
hardcoded.
"""

from pathlib import Path
import hashlib
import struct
import sys

sys.path.insert(0, str(Path(__file__).parent))
from postlink import build_payload   # noqa: E402

EXPECTED_STAGE3AU_SHA256 = \
    "6365ac47ea7d2d7e934ba7f7bdd74bcb6c2ada1293697fb8fc2cfe52ec9d8d67"

PAYLOAD_RVA = 0x002FA440
PAYLOAD_LIMIT = 0x002FB000       # end of .s3qd raw data

# --- DLL functions / globals the payload references ------------------------
LOG_RVA = 0x00001D90
FINDDEBUGVAR_RVA = 0x00041E10
REQUEST_PAUSE_RVA = 0x00030480
PAUSE_TARGET_BYTE_RVA = 0x002AE97B   # byte VR_IsPausePresentationTarget reads
CAPTURE_ACTIVE_RVA = 0x002AE770      # g_reticleCaptureState.active
ACTIVE_TITLE_RVA = 0x002BA6C8        # byte TitleAdapter_GetActiveTitle reads
H4_MODULE_REF_RVA = 0x002A7208       # Stage 3V/3X loader-pinned halo4 base
PAUSE_GRACE_RVA = 0x002F31C8         # Stage 3X resume-grace stamp
IAT_GET_TICK_COUNT64_RVA = 0x00180150
H4_IMAGE_SIZE = 0x04A3F000

# --- splice sites -----------------------------------------------------------
# OMSetRenderTargetsHook argument-restore tail:
#   0xDEB5  4C 8B CE   mov r9, rsi     (dsv)
#   0xDEB8  8B D3      mov edx, ebx    (count)
#   0xDEBA  48 8B CF   mov rcx, rdi    (context)
#   0xDEBD  FF 15 ...  call [g_origOMSetRenderTargets]
DSV_SPLICE_RVA = 0x0000DEB5
DSV_SPLICE_EXPECT = bytes.fromhex("4c8bce8bd3488bcf")

# Stage 3X poll-gate call edge (E8 rel32 + 2 NOP over the 3V installed load).
POLL_SPLICE_RVA = 0x0008431C
POLL_SPLICE_EXPECT = bytes.fromhex("e8a6f4260090")

# Stage 3X heartbeat jump edge (E9 rel32 + NOPs over the TLS stores).
HEARTBEAT_SPLICE_RVA = 0x00056CA7
HEARTBEAT_SPLICE_EXPECT = bytes.fromhex("e9c8ca290090909090909090")

CONTEXT = (
    # OMSetRenderTargetsHook shape around the splice
    (0x0000DEAE, bytes.fromhex("84c075034c8bc5"),
     "hook test/branch/stock-r8 before the splice"),
    (0x0000DEBD, bytes.fromhex("ff1505022a00"),
     "original OMSetRenderTargets IAT-style call after the splice"),
    # functions the payload calls
    (FINDDEBUGVAR_RVA, bytes.fromhex("48895c240848896c2410"),
     "FindDebugVarSlot prologue"),
    (0x0002EEB0, bytes.fromhex("0fb605c4fa270090c3"),
     "VR_IsPausePresentationTarget reads the pause-target byte"),
    (REQUEST_PAUSE_RVA, bytes.fromhex("0fb6c18605f2e42700"),
     "VR_RequestPausePresentation atomic prologue"),
    (LOG_RVA, bytes.fromhex("48894c2408488954"), "LOG prologue"),
    # squish-resolve call site: proves FindDebugVarSlot's ABI, the exact
    # H4 image-size constant, and the call target match the compiled C++
    # (mov edx,0x4A3F000 / mov rcx,rdi / call 0x41E10)
    (0x0005993E, bytes.fromhex("ba00f0a304488bcfe8c584feff"),
     "squish resolve call site"),
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


def read_rel32_target(blob, pe, rva):
    o = rva_off(pe, rva)
    disp = struct.unpack_from("<i", blob, o + 1)[0]
    return rva + 5 + disp


def verify(code, code_rva, checks):
    from capstone import Cs, CS_ARCH_X86, CS_MODE_64
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True
    targets = set()
    for ins in md.disasm(bytes(code), code_rva):
        for op in ins.operands:
            if op.type == 3 and op.mem.base == 41:
                targets.add(ins.address + ins.size + op.mem.disp)
            elif op.type == 2:
                t = op.imm
                if not (code_rva <= t < code_rva + len(code) + 0x40):
                    targets.add(t)
    for name, rva in checks.items():
        if rva not in targets:
            raise SystemExit(
                f"payload verify: no reference to {name} 0x{rva:X}")
    print(f"verify ok: {len(targets)} external targets, "
          f"{len(checks)} asserted")


def main():
    if len(sys.argv) != 3:
        raise SystemExit(
            "usage: build_stage3av_h4_pause_flag_and_dsv.py "
            "<Stage3AU-HaloMCCVR.dll> <output.dll>")
    src, out = Path(sys.argv[1]), Path(sys.argv[2])

    blob = bytearray(src.read_bytes())
    sha = hashlib.sha256(blob).hexdigest()
    print("input", sha)
    if sha != EXPECTED_STAGE3AU_SHA256:
        raise SystemExit("wrong Stage3AU input DLL: " + sha)

    pe = parse_pe(blob)
    if pe["n"] != 12:
        raise SystemExit(f"unexpected PE geometry: n={pe['n']}")
    qd = next((s for s in pe["sections"] if s["name"] == ".s3qd"), None)
    if not qd or qd["va"] != 0x2F3000 or qd["vs"] != 0x8000 \
            or qd["rs"] != 0x8000:
        raise SystemExit("unexpected .s3qd geometry")

    for rva, expect, label in CONTEXT:
        guard(blob, pe, rva, expect, label)
    guard(blob, pe, DSV_SPLICE_RVA, DSV_SPLICE_EXPECT, "DSV splice site")
    guard(blob, pe, POLL_SPLICE_RVA, POLL_SPLICE_EXPECT, "poll splice site")
    guard(blob, pe, HEARTBEAT_SPLICE_RVA, HEARTBEAT_SPLICE_EXPECT,
          "heartbeat splice site")

    # Read the Stage 3X continuation addresses from the input itself.
    s3x_poll_gate = read_rel32_target(blob, pe, POLL_SPLICE_RVA)
    s3x_heartbeat = read_rel32_target(blob, pe, HEARTBEAT_SPLICE_RVA)
    print(f"stage3x poll gate 0x{s3x_poll_gate:X}, "
          f"heartbeat bridge 0x{s3x_heartbeat:X}")
    for t, label in ((s3x_poll_gate, "poll gate"),
                     (s3x_heartbeat, "heartbeat bridge")):
        if not 0x2F3200 <= t < 0x2F4000:
            raise SystemExit(
                f"stage3x {label} 0x{t:X} outside the Stage 3X helper page")

    defs = {
        "LOG_RVA": LOG_RVA,
        "FINDDEBUGVAR_RVA": FINDDEBUGVAR_RVA,
        "REQUEST_PAUSE_RVA": REQUEST_PAUSE_RVA,
        "PAUSE_TARGET_BYTE_RVA": PAUSE_TARGET_BYTE_RVA,
        "CAPTURE_ACTIVE_RVA": CAPTURE_ACTIVE_RVA,
        "ACTIVE_TITLE_RVA": ACTIVE_TITLE_RVA,
        "H4_MODULE_REF_RVA": H4_MODULE_REF_RVA,
        "PAUSE_GRACE_RVA": PAUSE_GRACE_RVA,
        "IAT_GET_TICK_COUNT64_RVA": IAT_GET_TICK_COUNT64_RVA,
        "S3X_POLL_GATE_RVA": s3x_poll_gate,
        "S3X_HEARTBEAT_BRIDGE_RVA": s3x_heartbeat,
    }
    code, syms = build_payload(
        Path(__file__).with_name("stage3av_pause_flag_and_dsv.S"),
        PAYLOAD_RVA, defs,
        ("stage3av_data", "stage3av_dsv_gate", "stage3av_poll_chain",
         "stage3av_pause_monitor"))
    print("payload", hashlib.sha256(code).hexdigest(), len(code))
    if syms["stage3av_data"] != PAYLOAD_RVA:
        raise SystemExit("payload data block moved")
    if PAYLOAD_RVA + len(code) > PAYLOAD_LIMIT:
        raise SystemExit(f"payload does not fit the page tail: {len(code)}")
    if any(bytes(code[:16])):
        raise SystemExit("state block must assemble to zero bytes")
    verify(code, PAYLOAD_RVA, {
        "LOG": LOG_RVA,
        "FindDebugVarSlot": FINDDEBUGVAR_RVA,
        "VR_RequestPausePresentation": REQUEST_PAUSE_RVA,
        "pause-target byte": PAUSE_TARGET_BYTE_RVA,
        "capture-active byte": CAPTURE_ACTIVE_RVA,
        "active-title byte": ACTIVE_TITLE_RVA,
        "H4 module reference": H4_MODULE_REF_RVA,
        "pause grace": PAUSE_GRACE_RVA,
        "GetTickCount64 IAT": IAT_GET_TICK_COUNT64_RVA,
        "stage3x poll gate": s3x_poll_gate,
        "stage3x heartbeat bridge": s3x_heartbeat,
    })

    # The payload region must be genuinely unused.
    pay_off = rva_off(pe, PAYLOAD_RVA)
    if any(blob[pay_off:rva_off(pe, PAYLOAD_LIMIT - 1) + 1]):
        raise SystemExit("Stage3AV payload region is not free")

    blob[pay_off:pay_off + len(code)] = code

    # 1. DSV gate: call + 3 NOPs over the three argument-restore moves.
    o = guard(blob, pe, DSV_SPLICE_RVA, DSV_SPLICE_EXPECT, "DSV splice site")
    jump = bytes([0xE8]) + struct.pack(
        "<i", syms["stage3av_dsv_gate"] - (DSV_SPLICE_RVA + 5))
    blob[o:o + len(DSV_SPLICE_EXPECT)] = \
        jump + bytes([0x90]) * (len(DSV_SPLICE_EXPECT) - len(jump))
    print(f"DSV gate: 0x{DSV_SPLICE_RVA:X} -> "
          f"0x{syms['stage3av_dsv_gate']:X}")

    # 2. poll chain: retarget the existing E8 displacement only.
    o = rva_off(pe, POLL_SPLICE_RVA)
    struct.pack_into("<i", blob, o + 1,
                     syms["stage3av_poll_chain"] - (POLL_SPLICE_RVA + 5))
    print(f"poll chain: 0x{POLL_SPLICE_RVA:X} -> "
          f"0x{syms['stage3av_poll_chain']:X} -> 0x{s3x_poll_gate:X}")

    # 3. pause monitor: retarget the existing E9 displacement only.
    o = rva_off(pe, HEARTBEAT_SPLICE_RVA)
    struct.pack_into("<i", blob, o + 1,
                     syms["stage3av_pause_monitor"] - (HEARTBEAT_SPLICE_RVA + 5))
    print(f"pause monitor: 0x{HEARTBEAT_SPLICE_RVA:X} -> "
          f"0x{syms['stage3av_pause_monitor']:X} -> 0x{s3x_heartbeat:X}")

    if struct.unpack_from("<I", blob, pe["opt"] + 0x40)[0] != 0:
        raise SystemExit("unexpected non-zero PE checksum")
    if len(blob) != 2919424:
        raise SystemExit("output size changed unexpectedly")

    out.write_bytes(bytes(blob))
    print("output", hashlib.sha256(bytes(blob)).hexdigest(), len(blob))


if __name__ == "__main__":
    main()
