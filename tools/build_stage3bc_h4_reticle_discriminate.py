"""Stage 3BC - put Halo 4's own reticle art on the VR crosshair.

Two facts drive this candidate, both measured rather than assumed.

1. The preserved Stage 3AW Steam log records, in every 2s window,
   `461 main gameplay CUI passes, 1386 begin markers, 1386 native hides` -
   exactly 3.000 type-0x28 transform pushes per gameplay CUI pass, and the
   mod moves ALL THREE offscreen. `Halo4DecideCuiReticleAction` keys only on
   `command == 0x28 && payloadSize == 0x0C`; it never asks which container
   this is. The parity trace hides the problem because it buckets by the
   payload transform ID, which is always 0, so the three collapse into slot 0
   and overwrite each other. Every failed capture in the C-H4-43q..48 /
   3AW / 3AX series therefore captured an undiscriminated one-of-three, which
   is exactly what "a visor fragment" and "some random asset" describe.

2. Stage 3BB differs from Stage 3AX by ONE byte: 0x53634, `74 56` -> `EB 56`,
   which forces the capture predicate's success edge straight to the normal
   pass. Stage 3AX itself captured stable non-blank art (`art 819..847`) and
   did NOT crash; the crashes belong to 3AY's differential double-replay and
   3AZ's attempted ABI fix, neither of which is restored here.

So the working capture path is one byte away, and the only thing wrong with
it is that it never identified the reticle. This candidate restores that byte
and adds the missing discriminator.

The discriminator is H4EK-proven, not a guess. `ReticuleOffsetContainerWidget`
slot 27 (tag_test 0xADE020, verified against tag_play 0x8AD6E4 and
sapien_play 0xC160FC) builds its optional argument as `float2{0.0f,
[self+0x1F0]}` before calling the type-0x28 producer 0x9B6800. The 0x0C-byte
payload is `{int32 transform_id; float x; float y}`, so the reticle offset
container is the one whose payload X is exactly 0.0f.

The splice replaces the dispatcher's existing 4-byte payload-ID read with a
stub that makes the SAME `Halo4SafeRead` call, then reads payload+4 and
returns success only when that float is +/-0.0f. A non-reticle type-0x28
therefore reports `beginPayloadReadable = false` and takes the untouched stock
path - it is neither hidden nor captured. Nothing else changes: no new hook,
no replay, no render-target or viewport change, no other title touched.

Fail-safety: if no container ever matches, Halo 4 loses only this optional
feature - the native reticle stays face-centred and the procedural weapon-ray
quad stays visible, which is Stage 3BA's confirmed-good behaviour. Reverting
is the single byte at 0x53634.
"""

from pathlib import Path
import hashlib
import struct
import sys

EXPECTED_STAGE3BB = \
    "10e39cf66862f4e88eba245fc22da750c0817c4684a1af114c466703722a8192"

PAYLOAD_RVA = 0x002FB800
PAYLOAD_LIMIT = 0x002FC000        # end of .s3qd raw data
SCRATCH_OFFSET = 0x100            # payload-relative, clear of the code

# --- addresses the stub references ----------------------------------------
SAFE_READ_RVA = 0x00056C10        # Halo4SafeRead(src, dst, size) -> eax

# --- splice sites ----------------------------------------------------------
# Dispatcher payload-ID read, immediately after the 0x28 / 0x0C checks:
#   0x53835  49 8D 4D 04           lea  rcx, [r13+4]      (payload)
#   0x53839  41 B8 04 00 00 00     mov  r8d, 4
#   0x5383F  48 8D 54 24 24        lea  rdx, [rsp+0x24]
#   0x53844  E8 C7 33 00 00        call Halo4SafeRead
#   0x53849  8B 7C 24 24           mov  edi, [rsp+0x24]   <- return here
GATE_SPLICE_RVA = 0x00053835
GATE_SPLICE_EXPECT = bytes.fromhex(
    "498d4d0441b804000000488d542424e8c7330000")
GATE_RETURN_RVA = 0x00053849

# Stage 3BB forced the capture predicate's success edge to the normal pass.
# Restoring `74` reinstates the exact Stage 3AX capture edge.
CAPTURE_EDGE_RVA = 0x00053634
CAPTURE_EDGE_EXPECT = bytes.fromhex("eb56")
CAPTURE_EDGE_PATCH = bytes.fromhex("7456")

CONTEXT = (
    (0x00053827, bytes.fromhex("664183fe28752d664183fc0c7526"),
     "type-0x28 and payload-size-0x0C checks before the splice"),
    (GATE_RETURN_RVA, bytes.fromhex("8b7c242485c0740a"),
     "transform-id load and success test after the splice"),
    (SAFE_READ_RVA, bytes.fromhex("4883ec28488bc2488bd1"),
     "Halo4SafeRead prologue"),
    (0x00053632, bytes.fromhex("84c0"),
     "capture predicate test feeding the restored edge"),
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
        secs.append(dict(name=name, vs=vs, va=va, rs=rs, rp=rp))
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


def assemble(base_rva):
    """Emit the discriminator stub with hand-pinned encodings.

    Every instruction below is a fixed-length encoding and every
    position-dependent field is computed from this exact layout, so the stub
    is byte-deterministic rather than dependent on an assembler's choices.
    `done` is the closing rel32 jump at +72; `reticle` is the `mov eax,1`
    at +67.
    """
    scratch = base_rva + SCRATCH_OFFSET
    done, reticle = 72, 67

    def rel8(target, next_off):
        d = target - next_off
        if not -128 <= d <= 127:
            raise SystemExit("rel8 out of range")
        return bytes([d & 0xFF])

    def rel32(target_rva, next_off):
        return struct.pack("<i", target_rva - (base_rva + next_off))

    code = b"".join((
        bytes.fromhex("498d4d04"),          # +0  lea  rcx, [r13+4]
        bytes.fromhex("41b804000000"),      # +4  mov  r8d, 4
        bytes.fromhex("488d542424"),        # +10 lea  rdx, [rsp+0x24]
        b"\xE8" + rel32(SAFE_READ_RVA, 20),  # +15 call Halo4SafeRead
        bytes.fromhex("85c0"),              # +20 test eax, eax
        b"\x74" + rel8(done, 24),           # +22 jz   done
        bytes.fromhex("498d4d08"),          # +24 lea  rcx, [r13+8]
        bytes.fromhex("41b804000000"),      # +28 mov  r8d, 4
        b"\x48\x8D\x15" + rel32(scratch, 41),   # +34 lea rdx, [rip+scratch]
        b"\xE8" + rel32(SAFE_READ_RVA, 46),  # +41 call Halo4SafeRead
        bytes.fromhex("85c0"),              # +46 test eax, eax
        b"\x74" + rel8(done, 50),           # +48 jz   done
        b"\x8B\x05" + rel32(scratch, 56),   # +50 mov  eax, [rip+scratch]
        bytes.fromhex("25ffffff7f"),        # +56 and  eax, 0x7fffffff
        b"\x74" + rel8(reticle, 63),        # +61 jz   reticle
        bytes.fromhex("31c0"),              # +63 xor  eax, eax
        b"\xEB" + rel8(done, 67),           # +65 jmp  done
        bytes.fromhex("b801000000"),        # +67 reticle: mov eax, 1
        b"\xE9" + rel32(GATE_RETURN_RVA, 77),   # +72 done: jmp dispatcher
    ))
    if len(code) != 77:
        raise SystemExit(f"stub layout drifted: {len(code)} bytes")
    return code, scratch


def main():
    if len(sys.argv) != 3:
        raise SystemExit(
            "usage: build_stage3bc_h4_reticle_discriminate.py "
            "<Stage3BB-HaloMCCVR.dll> <output.dll>")
    src, out = Path(sys.argv[1]), Path(sys.argv[2])

    blob = bytearray(src.read_bytes())
    sha = hashlib.sha256(blob).hexdigest()
    print("input", sha)
    if sha != EXPECTED_STAGE3BB:
        raise SystemExit("wrong Stage3BB input DLL: " + sha)

    pe = parse_pe(blob)
    if pe["n"] != 12:
        raise SystemExit(f"unexpected PE geometry: n={pe['n']}")
    qd = next((s for s in pe["sections"] if s["name"] == ".s3qd"), None)
    if not qd or qd["va"] != 0x2F3000 or qd["vs"] != 0x9000:
        raise SystemExit("unexpected .s3qd geometry")

    for rva, expect, label in CONTEXT:
        guard(blob, pe, rva, expect, label)
    guard(blob, pe, GATE_SPLICE_RVA, GATE_SPLICE_EXPECT, "gate splice site")
    guard(blob, pe, CAPTURE_EDGE_RVA, CAPTURE_EDGE_EXPECT, "capture edge")

    code, scratch = assemble(PAYLOAD_RVA)
    print(f"stub {len(code)} bytes at 0x{PAYLOAD_RVA:X}, "
          f"scratch 0x{scratch:X}")
    if PAYLOAD_RVA + SCRATCH_OFFSET + 4 > PAYLOAD_LIMIT:
        raise SystemExit("stub does not fit the page tail")
    if len(code) > SCRATCH_OFFSET:
        raise SystemExit("stub code overruns its scratch slot")

    # The whole region the stub and its scratch occupy must be genuinely free.
    pay_off = rva_off(pe, PAYLOAD_RVA)
    end_off = rva_off(pe, PAYLOAD_RVA + SCRATCH_OFFSET + 4)
    if any(blob[pay_off:end_off]):
        raise SystemExit("Stage3BC payload region is not free")

    verify(code, PAYLOAD_RVA, {
        "Halo4SafeRead": SAFE_READ_RVA,
        "dispatcher return": GATE_RETURN_RVA,
    })

    blob[pay_off:pay_off + len(code)] = code

    # 1. Gate splice: jmp to the stub, NOPs over the rest of the displaced run.
    o = rva_off(pe, GATE_SPLICE_RVA)
    jump = bytes([0xE9]) + struct.pack(
        "<i", PAYLOAD_RVA - (GATE_SPLICE_RVA + 5))
    blob[o:o + len(GATE_SPLICE_EXPECT)] = \
        jump + bytes([0x90]) * (len(GATE_SPLICE_EXPECT) - len(jump))
    print(f"gate: 0x{GATE_SPLICE_RVA:X} -> 0x{PAYLOAD_RVA:X} "
          f"-> 0x{GATE_RETURN_RVA:X}")

    # 2. Restore the Stage 3AX capture edge.
    o = rva_off(pe, CAPTURE_EDGE_RVA)
    blob[o:o + len(CAPTURE_EDGE_PATCH)] = CAPTURE_EDGE_PATCH
    print(f"capture edge: 0x{CAPTURE_EDGE_RVA:X} "
          f"{CAPTURE_EDGE_EXPECT.hex()} -> {CAPTURE_EDGE_PATCH.hex()}")

    if struct.unpack_from("<I", blob, pe["opt"] + 0x40)[0] != 0:
        raise SystemExit("unexpected non-zero PE checksum")
    if len(blob) != len(src.read_bytes()):
        raise SystemExit("output size changed unexpectedly")

    out.write_bytes(bytes(blob))
    print("output", hashlib.sha256(bytes(blob)).hexdigest(), len(blob))


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
                if not (code_rva <= t < code_rva + len(code)):
                    targets.add(t)
    for name, rva in checks.items():
        if rva not in targets:
            raise SystemExit(
                f"payload verify: no reference to {name} 0x{rva:X}")
    print(f"verify ok: {len(targets)} external targets, "
          f"{len(checks)} asserted")


if __name__ == "__main__":
    main()
