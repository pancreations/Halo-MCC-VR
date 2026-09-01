"""Prove Stage 3CH == Stage 3BT everywhere the crosshair lives, and that the
cutscene/theatre work is intact."""
from pathlib import Path
import hashlib, struct, sys
import capstone
sys.path.insert(0, str(Path(__file__).parent))
from build_stage3br_h4_capture_scale import parse_pe, rva_off

ROOT = Path(__file__).parent.parent
bt = bytearray((ROOT/"built/Stage3BT-HaloMCCVR.dll").read_bytes())
out = bytearray((ROOT/"built/Stage3CH-HaloMCCVR.dll").read_bytes())
cf = bytearray((ROOT/"built/Stage3CF-HaloMCCVR.dll").read_bytes())
assert hashlib.sha256(bt).hexdigest() == \
    "4a1970734c5266688d2c61490fc485f74cbd3d39a3ffc568d0832791ada2279a"
pe = parse_pe(out); assert pe["n"] == 12
off = lambda r: rva_off(pe, r)

# 1. Every byte of the reticle-capture chain equals Stage 3BT.
CAPTURE_CHAIN = (
    (0x00011A00, 0x00011F00, "capture Begin/End + upload"),
    (0x00027600, 0x00027700, "upload stats tap"),
    (0x00053800, 0x00053A00, "capture-selection splice site"),
    (0x0000D0A0, 0x0000D260, "draw/drawIndexed census splices"),
    (0x002F9000, 0x002F9D10, "3AS/3BM/3BN/3BO/3BP/3BQ/3BR/3BS payloads"),
    (0x002FB800, 0x002FBFF0, "3BH framing const + gate + 3BI/3BK payloads"),
    (0x002FA000, 0x002FA36C, "3BL/3BH map payloads"),
)
for lo, hi, label in CAPTURE_CHAIN:
    a, b = bytes(bt[off(lo):off(hi)]), bytes(out[off(lo):off(hi)])
    assert a == b, f"{label}: differs from 3BT at 0x{lo:X}..0x{hi:X}"
print("capture chain: byte-identical to Stage 3BT (all 7 regions)")

# 2. Confirm the specific reverts really landed (not vacuously equal).
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64); md.detail = True
gate = list(md.disasm(bytes(out[off(0x2FB992):off(0x2FB992)+5]), 0x2FB992))
assert gate[0].mnemonic == "call" and gate[0].operands[0].imm == 0x2F9A90, \
    "gate must call the accepted 3BR thunk"
sel = list(md.disasm(bytes(out[off(0x2F995D):off(0x2F995D)+8]), 0x2F995D))
assert sel[0].mnemonic == "cmp" and "0xc" in sel[0].op_str, "selector id check"
unhide = list(md.disasm(bytes(out[off(0x2F99D7):off(0x2F99D7)+8]), 0x2F99D7))
tgt = unhide[0].address + unhide[0].size + \
    next(o.mem.disp for o in unhide[0].operands if o.type == 3)
assert tgt == 0x2FB800, f"un-hide source must be the 3BH constant, got {tgt:#x}"
print("reverts verified: gate->3BR, id check 0xC, un-hide->0x2FB800")

# 3. Cutscene/theatre work retained exactly as in Stage 3CF.
KEEP = ((0x0002C2BC, 6, "detector jne"), (0x00056EBF, 5, "3CB camera call"),
        (0x00068111, 5, "runtime caps"), (0x001890D4, 4, "registry caps"),
        (0x002F9D10, 0x80, "3BU write-back"), (0x002F9E10, 0x80, "3BV probe"),
        (0x002FA36C, 0x94, "3CB camera payload"),
        (0x002FAAA0, 0x200, "3BW publications"),
        (0x002FACB0, 0x300, "3BX look constraints"))
for rva, n, label in KEEP:
    assert bytes(out[off(rva):off(rva)+n]) == bytes(cf[off(rva):off(rva)+n]), \
        f"cutscene work changed: {label}"
assert struct.unpack_from("<I", out, off(0x68112))[0] == 0x1F3
assert struct.unpack_from("<I", out, off(0x1890D4))[0] == 0x1F3
print("cutscene/theatre work: identical to 3CF; both capability masks 0x1F3")

# 4. Whole-image sanity: 3CH differs from 3BT ONLY outside the capture chain.
diff = [i for i, (a, b) in enumerate(zip(bt, out)) if a != b]
chain = set()
for lo, hi, _ in CAPTURE_CHAIN:
    chain |= set(range(off(lo), off(hi)))
# the 16-byte tail of the last page (all zero in both images)
assert bytes(bt[off(0x2FBFF0):off(0x2FBFF0)+15]) ==     bytes(out[off(0x2FBFF0):off(0x2FBFF0)+15])
assert not (set(diff) & chain), "a capture-chain byte still differs from 3BT"
assert bytes(out[off(0x199B3):off(0x199B3)+2]) == bytes.fromhex("b201"), "3BJ"
print(f"PASS: 3CH = Stage 3BT's exact crosshair chain + the cutscene work "
      f"({len(diff)} differing bytes, none in the capture chain); 3BJ absent")
