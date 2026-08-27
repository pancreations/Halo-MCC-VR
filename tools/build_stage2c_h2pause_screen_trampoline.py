from pathlib import Path
import struct, hashlib

src = Path('/mnt/data/stage3_work/stage2b_bin/HaloMCCVR.dll')
out = Path('/mnt/data/HaloMCCVR-0.3.5-C50-Stage2C-H2PauseScreen.dll')
b = bytearray(src.read_bytes())

IMAGE_BASE = 0x180000000
# In SubmitPreparedFrame, this block computes halo2StrictStockScreen from
# (halo2Title, frameDisposition, presentationDecision). Stage2C interposes just
# before it. If active H2 is already displaying the pause presentation, the
# source's C-H2-72 rule makes presentationDecision SharedDefault (0), so the
# shared head-locked screen/backbuffer path is used instead of Drop.
PATCH_VA = 0x1800256B3
RETURN_VA = 0x1800256CA
G_PAUSE_PRESENTATION_VA = 0x1802AA54A

pe = struct.unpack_from('<I', b, 0x3c)[0]
coff = pe + 4
nsec = struct.unpack_from('<H', b, coff + 2)[0]
optsz = struct.unpack_from('<H', b, coff + 16)[0]
opt = coff + 20
sect = opt + optsz
file_align = struct.unpack_from('<I', b, opt + 36)[0]
sect_align = struct.unpack_from('<I', b, opt + 32)[0]
assert nsec == 8, nsec
assert file_align == 0x200 and sect_align == 0x1000
last = sect + (nsec - 1) * 40
last_vs, last_va, last_rs, last_rp = struct.unpack_from('<IIII', b, last + 8)
new_rva = (last_va + max(last_vs, last_rs) + sect_align - 1) & ~(sect_align - 1)
new_raw = (len(b) + file_align - 1) & ~(file_align - 1)
HELPER_VA = IMAGE_BASE + new_rva

code = bytearray()
fixups = []
labels = {}

def emit(x): code.extend(x)
def label(n): labels[n] = len(code)
def rel32_label(op, n):
    pos=len(code); emit(op+b'\0\0\0\0'); fixups.append((pos+len(op), n, 'label', 0))
def rel32_va(op, va):
    pos=len(code); emit(op+b'\0\0\0\0'); fixups.append((pos+len(op), va, 'va', 0))
def rip(prefix, va, suffix=b''):
    pos=len(code); emit(prefix+b'\0\0\0\0'+suffix); fixups.append((pos+len(prefix), va, 'rip', len(suffix)))

# C-H2-72: only once pausePresentation itself is active (not merely target),
# and only for the active H2 frame, force decision=SharedDefault. The comfort
# fade protects the edge before this bool flips true.
rip(bytes.fromhex('80 3D'), G_PAUSE_PRESENTATION_VA, bytes.fromhex('00')) # cmp byte [rip+g_pausePresentation],0
rel32_label(bytes.fromhex('0F 84'), 'normal')
emit(bytes.fromhex('45 84 C0'))  # test r8b,r8b (halo2Title)
rel32_label(bytes.fromhex('0F 84'), 'normal')
emit(bytes.fromhex('40 32 FF'))  # xor dil,dil (SharedDefault)

label('normal')
# Re-execute the exact 23-byte block displaced at PATCH_VA. This computes
# halo2StrictStockScreen and then falls through to stereoWorldFrame selection.
emit(bytes.fromhex(
    '45 84 C0 '      # test r8b,r8b
    '74 0F '         # je false
    '84 C0 '         # test al,al (Unclaimed == 0)
    '75 0B '         # jne false
    '40 F6 C7 FD '   # test dil,0FDh (decision 0 or 2)
    '75 05 '         # jne false
    '41 B5 01 '      # mov r13b,1
    'EB 03 '         # jmp done
    '45 32 ED'       # false: xor r13b,r13b
))
rel32_va(bytes.fromhex('E9'), RETURN_VA)

for off,target,kind,suffix_len in fixups:
    if kind=='label': target_va=HELPER_VA+labels[target]
    else: target_va=target
    next_va=HELPER_VA+off+4+suffix_len
    rel=target_va-next_va
    assert -(1<<31)<=rel<(1<<31)
    struct.pack_into('<i',code,off,rel)

raw_size=(len(code)+file_align-1)&~(file_align-1)
if len(b)<new_raw: b.extend(b'\0'*(new_raw-len(b)))
b.extend(code)
b.extend(b'\xCC'*(raw_size-len(code)))
new_hdr=sect+nsec*40
assert new_hdr+40 <= 0x400, hex(new_hdr)
b[new_hdr:new_hdr+8]=b'.h2pm\0\0\0'
chars=0x60000020
struct.pack_into('<IIIIIIHHI',b,new_hdr+8,len(code),new_rva,raw_size,new_raw,0,0,0,0,chars)
struct.pack_into('<H',b,coff+2,nsec+1)
new_size_image=(new_rva+len(code)+sect_align-1)&~(sect_align-1)
struct.pack_into('<I',b,opt+56,new_size_image)
old_size_code=struct.unpack_from('<I',b,opt+4)[0]
struct.pack_into('<I',b,opt+4,old_size_code+raw_size)

# Replace the strict-stock computation block with one jump. The helper performs
# the pause override and then replays all displaced instructions exactly.
text_rva,text_raw=0x1000,0x400
patch_raw=text_raw+((PATCH_VA-IMAGE_BASE)-text_rva)
expected=bytes.fromhex('45 84 C0 74 0F 84 C0 75 0B 40 F6 C7 FD 75 05 41 B5 01 EB 03 45 32 ED')
actual=bytes(b[patch_raw:patch_raw+len(expected)])
assert actual==expected,(hex(patch_raw),actual.hex(' '))
rel=HELPER_VA-(PATCH_VA+5)
patch=b'\xE9'+struct.pack('<i',rel)+b'\x90'*(len(expected)-5)
b[patch_raw:patch_raw+len(expected)]=patch

out.write_bytes(b)
print('input sha256', hashlib.sha256(src.read_bytes()).hexdigest())
print('helper',hex(HELPER_VA),'rva',hex(new_rva),'raw',hex(new_raw),'len',len(code),'rawsize',hex(raw_size))
print('patch raw',hex(patch_raw),'len',len(expected))
print('SizeOfImage',hex(new_size_image),'file',hex(len(b)))
print('output sha256',hashlib.sha256(b).hexdigest())
