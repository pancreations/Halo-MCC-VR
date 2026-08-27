from pathlib import Path
import struct, hashlib

src = Path('/mnt/data/work_stage2/bin/stage1/HaloMCCVR.dll')
out = Path('/mnt/data/work_stage2/bin/HaloMCCVR-0.3.5-C50-Stage2-H2Aim.dll')
b = bytearray(src.read_bytes())

IMAGE_BASE = 0x180000000
PATCH_VA = 0x180095641
RETURN_STOCK_VA = 0x18009564B
RETURN_DIRECT_BODY_VA = 0x1800956EB
GAME_H2_AIM_ACTIVE_VA = 0x180045EC0
G_OBJECT_ACCESSOR_VA = 0x1802D0B00
G_ARMED_VA = 0x1802B6741
G_LEVEL_LIVE_VA = 0x1802B6742
G_TEARDOWN_VA = 0x1802B6743

# PE metadata
pe = struct.unpack_from('<I', b, 0x3c)[0]
coff = pe + 4
nsec = struct.unpack_from('<H', b, coff + 2)[0]
optsz = struct.unpack_from('<H', b, coff + 16)[0]
opt = coff + 20
sect = opt + optsz
file_align = struct.unpack_from('<I', b, opt + 36)[0]
sect_align = struct.unpack_from('<I', b, opt + 32)[0]
assert nsec == 7
assert file_align == 0x200 and sect_align == 0x1000
last = sect + (nsec - 1) * 40
last_vs, last_va, last_rs, last_rp = struct.unpack_from('<IIII', b, last + 8)
new_rva = (last_va + max(last_vs, last_rs) + sect_align - 1) & ~(sect_align - 1)
new_raw = (len(b) + file_align - 1) & ~(file_align - 1)
assert new_rva == 0x2EA000, hex(new_rva)
assert new_raw == 0x2BAE00, hex(new_raw)
HELPER_VA = IMAGE_BASE + new_rva

code = bytearray()
labels = {}
fixups = []

def emit(x): code.extend(x)
def label(name): labels[name] = len(code)
def rel32_to_va(op, target_va):
    pos = len(code); emit(op + b'\0\0\0\0'); fixups.append((pos + len(op), target_va, 'va'))
def rel32_to_label(op, name):
    pos = len(code); emit(op + b'\0\0\0\0'); fixups.append((pos + len(op), name, 'label'))
def rip_disp32(prefix, target_va, suffix=b''):
    # prefix immediately before disp32; RIP base is after disp32 + suffix.
    pos = len(code); emit(prefix + b'\0\0\0\0' + suffix); fixups.append((pos + len(prefix), target_va, ('rip', len(suffix))))

# Re-execute bytes displaced from 0x180095641..0x18009564A.
emit(bytes.fromhex('33 FF'))                 # xor edi, edi
emit(bytes.fromhex('8B DF'))                 # mov ebx, edi
emit(bytes.fromhex('48 89 5C 24 28'))        # mov [rsp+28h], rbx
# If original is null, let the untouched original test/branch handle it.
emit(bytes.fromhex('48 85 F6'))              # test rsi, rsi
rel32_to_label(bytes.fromhex('0F 84'), 'stock')

# Preserve objectIndex (RCX) across Game_Halo2ControllerAimActive().  RSP is
# 16-byte aligned at the original call site; reserve shadow + local space.
emit(bytes.fromhex('48 83 EC 30'))            # sub rsp, 30h
emit(bytes.fromhex('48 89 4C 24 20'))         # mov [rsp+20h], rcx
rel32_to_va(bytes.fromhex('E8'), GAME_H2_AIM_ACTIVE_VA)
emit(bytes.fromhex('48 8B 4C 24 20'))         # mov rcx, [rsp+20h]
emit(bytes.fromhex('48 83 C4 30'))            # add rsp, 30h
emit(bytes.fromhex('84 C0'))                  # test al, al
rel32_to_label(bytes.fromhex('0F 84'), 'stock')

# Mirror Halo2Observer6Dof_DirectWeaponAimArmed()'s remaining predicates.
# g_nativeAimOriginal is already proven non-null in RSI.
rip_disp32(bytes.fromhex('48 83 3D'), G_OBJECT_ACCESSOR_VA, bytes.fromhex('00'))
rel32_to_label(bytes.fromhex('0F 84'), 'stock')
rip_disp32(bytes.fromhex('80 3D'), G_ARMED_VA, bytes.fromhex('00'))
rel32_to_label(bytes.fromhex('0F 84'), 'stock')
rip_disp32(bytes.fromhex('80 3D'), G_LEVEL_LIVE_VA, bytes.fromhex('00'))
rel32_to_label(bytes.fromhex('0F 84'), 'stock')
rip_disp32(bytes.fromhex('80 3D'), G_TEARDOWN_VA, bytes.fromhex('00'))
rel32_to_label(bytes.fromhex('0F 85'), 'stock')

# directVrAim=true: mirror the source's cached-true path. Initialize result
# and the existing 'applied' local, then enter the already-compiled vector
# write body directly. This avoids re-evaluating the ownership predicate after
# the skipped stock call.
emit(bytes.fromhex('31 C0'))                  # xor eax, eax
emit(bytes.fromhex('48 8B D8'))               # mov rbx, rax
emit(bytes.fromhex('48 89 44 24 28'))         # mov [rsp+28h], rax
emit(bytes.fromhex('45 32 F6'))               # xor r14b, r14b
emit(bytes.fromhex('44 88 74 24 20'))         # mov [rsp+20h], r14b
rel32_to_va(bytes.fromhex('E9'), RETURN_DIRECT_BODY_VA)
label('stock')
rel32_to_va(bytes.fromhex('E9'), RETURN_STOCK_VA)

# Resolve fixups.
for disp_off, target, kind in fixups:
    if kind == 'va':
        target_va = target
        next_va = HELPER_VA + disp_off + 4
    elif kind == 'label':
        target_va = HELPER_VA + labels[target]
        next_va = HELPER_VA + disp_off + 4
    else:
        tag, suffix_len = kind
        assert tag == 'rip'
        target_va = target
        next_va = HELPER_VA + disp_off + 4 + suffix_len
    rel = target_va - next_va
    assert -(1<<31) <= rel < (1<<31)
    struct.pack_into('<i', code, disp_off, rel)

# Add new RX code section.
raw_size = (len(code) + file_align - 1) & ~(file_align - 1)
if len(b) < new_raw:
    b.extend(b'\0' * (new_raw - len(b)))
b.extend(code)
b.extend(b'\xCC' * (raw_size - len(code)))
new_hdr = sect + nsec * 40
assert new_hdr + 40 <= 0x400
name = b'.h2aa\0\0\0'
chars = 0x60000020  # code | execute | read
b[new_hdr:new_hdr+8] = name
struct.pack_into('<IIIIIIHHI', b, new_hdr+8,
                 len(code), new_rva, raw_size, new_raw,
                 0, 0, 0, 0, chars)
struct.pack_into('<H', b, coff+2, nsec+1)
new_size_image = (new_rva + len(code) + sect_align - 1) & ~(sect_align - 1)
struct.pack_into('<I', b, opt+56, new_size_image)
# Keep PE accounting coherent: .h2aa is an additional RX code section.
old_size_code = struct.unpack_from('<I', b, opt+4)[0]
struct.pack_into('<I', b, opt+4, old_size_code + raw_size)

# Replace only the 10-byte pre-call setup with JMP helper + NOPs. Original
# test/JE/call at 0x9564B remains byte-for-byte intact and under original SEH.
text_rva, text_raw = 0x1000, 0x400
patch_raw = text_raw + ((PATCH_VA - IMAGE_BASE) - text_rva)
expected = bytes.fromhex('90 33 FF 8B DF 48 89 5C 24 28')
actual = bytes(b[patch_raw:patch_raw+10])
assert actual == expected, (hex(patch_raw), actual.hex(' '))
rel = HELPER_VA - (PATCH_VA + 5)
patch = b'\xE9' + struct.pack('<i', rel) + b'\x90'*5
b[patch_raw:patch_raw+10] = patch

out.write_bytes(b)
print('helper VA', hex(HELPER_VA), 'RVA', hex(new_rva), 'raw', hex(new_raw), 'len', len(code))
print('patch raw', hex(patch_raw), 'old', expected.hex(' '), 'new', patch.hex(' '))
print('SizeOfImage', hex(new_size_image), 'file', hex(len(b)))
print('SHA256', hashlib.sha256(b).hexdigest())
