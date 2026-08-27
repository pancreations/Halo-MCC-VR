from pathlib import Path
import hashlib, struct, sys

if len(sys.argv) != 3:
    raise SystemExit('usage: python build_stage3g_h2_native_crosshair.py <Stage3F-HaloMCCVR.dll> <output.dll>')
src = Path(sys.argv[1])
out = Path(sys.argv[2])
blob = bytearray(src.read_bytes())
input_sha = hashlib.sha256(blob).hexdigest()
EXPECTED = '83867929ba3678dc302e36b1a92649c08ab44a59d1e85b040a6be64d27d44c72'
if input_sha != EXPECTED:
    raise SystemExit('wrong Stage3F input DLL: ' + input_sha)

peoff = struct.unpack_from('<I', blob, 0x3C)[0]
coff = peoff + 4
optsz = struct.unpack_from('<H', blob, coff + 16)[0]
opt = coff + 20
sects = struct.unpack_from('<H', blob, coff + 2)[0]
sect_table = opt + optsz
sections=[]
for i in range(sects):
    o=sect_table+i*40
    name=bytes(blob[o:o+8]).split(b'\0',1)[0].decode('ascii',errors='ignore')
    vs,va,rs,rp=struct.unpack_from('<IIII',blob,o+8)
    sections.append((name,vs,va,rs,rp))

def rva_off(rva):
    for name,vs,va,rs,rp in sections:
        if va <= rva < va + max(vs,rs):
            return rp + (rva-va)
    raise KeyError(hex(rva))

def patch(rva, expected, replacement, label):
    off=rva_off(rva)
    old=bytes(blob[off:off+len(expected)])
    if old != expected:
        raise SystemExit(f'{label}: expected {expected.hex()} at RVA {rva:x}, got {old.hex()}')
    blob[off:off+len(replacement)] = replacement
    print(f'{label}: RVA 0x{rva:X} {expected.hex()} -> {replacement.hex()}')

# Stage 3F intentionally NOPed the compiled Stage 3E call to
# VR_BeginAuthoredReticleCapture while proving HUD ownership. Stage 3G keeps
# Stage 3F's CHUD-TLS bypass and actual-context HUD helper, and restores only
# this exact call. The surrounding precompiled role==Crosshair branch already
# sets Kind::NativeCrosshair only if the capture begin succeeds.
patch(0xBE78, bytes.fromhex('31c0909090'), bytes.fromhex('e823f90100'),
      'enable actual-draw native Halo 2 crosshair capture')

out.write_bytes(blob)
print('input sha256 ', input_sha)
print('output sha256', hashlib.sha256(blob).hexdigest())
print('output size  ', len(blob))
