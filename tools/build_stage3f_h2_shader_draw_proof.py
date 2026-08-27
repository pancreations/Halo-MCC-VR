from pathlib import Path
import struct, hashlib, json

import sys
if len(sys.argv) != 3:
    raise SystemExit('usage: python build_stage3f_h2_shader_draw_proof.py <Stage3E-HaloMCCVR.dll> <output.dll>')
src = Path(sys.argv[1])
out = Path(sys.argv[2])
input_bytes = src.read_bytes()
input_sha = hashlib.sha256(input_bytes).hexdigest()
if input_sha != '95f3362db179038ddbcd77fea42aea8a10d1dc086a40850d455301b17d786893':
    raise SystemExit('wrong Stage3E input DLL: ' + input_sha)
blob = bytearray(input_bytes)
helper = bytes.fromhex(
    '4881ecb8010000c7460410000000498b074c8d4608488d56044c89f9ff90f8020000448b56044585d20f84a00000004183fa100f87960000004c8d46084c8d4c24204589d3410f1000410f1101498b401049894110f3410f1040080f28c8f30f59057a000000f3410f5800f3410f1101f30f590d6c000000f3410f114908f3410f10400c0f28c8f30f590551000000f3410f584004f3410f114104f30f590d41000000f3410f11490c4983c0184983c11841ffcb758f498b074c8d4424204489d24c89f9ff9060010000c60601eb08f048ff05d1fffbff4881c4b8010000c3900000803e0000003f'
)

# PE parse
if blob[:2] != b'MZ': raise SystemExit('not MZ')
peoff = struct.unpack_from('<I', blob, 0x3c)[0]
if blob[peoff:peoff+4] != b'PE\0\0': raise SystemExit('not PE')
coff = peoff + 4
machine, nsects, timestamp, ptrsym, numsym, optsz, chars = struct.unpack_from('<HHIIIHH', blob, coff)
opt = coff + 20
magic = struct.unpack_from('<H', blob, opt)[0]
if magic != 0x20b: raise SystemExit('not PE32+')
section_align = struct.unpack_from('<I', blob, opt+0x20)[0]
file_align = struct.unpack_from('<I', blob, opt+0x24)[0]
image_base = struct.unpack_from('<Q', blob, opt+0x18)[0]
size_image_off = opt+0x38
size_code_off = opt+0x04
size_headers = struct.unpack_from('<I', blob, opt+0x3c)[0]
sect_table = opt + optsz

def align(v,a): return (v+a-1)//a*a
sections=[]
for i in range(nsects):
    o=sect_table+i*40
    name=bytes(blob[o:o+8]).split(b'\0',1)[0].decode('ascii')
    vs, va, rs, rp = struct.unpack_from('<IIII', blob, o+8)
    sections.append((name,vs,va,rs,rp,o))
print('sections',sections)
last=max(sections,key=lambda x:x[2])
new_rva=align(last[2]+max(last[1],last[3]),section_align)
new_raw=align(max(rp+rs for _,vs,va,rs,rp,o in sections),file_align)
raw_size=align(len(helper),file_align)
virt_size=len(helper)
header_off=sect_table+nsects*40
if header_off+40 > size_headers:
    raise SystemExit(f'no section header room {header_off:x}')
if new_raw < len(blob):
    # Overlay is allowed only if it's padding at expected append boundary; this file ends exactly there.
    raise SystemExit(f'new raw {new_raw:x} overlaps file size {len(blob):x}')
if len(blob)<new_raw: blob.extend(b'\0'*(new_raw-len(blob)))
blob.extend(helper)
blob.extend(b'\0'*(raw_size-len(helper)))

# New section header
name=b'.h2sf\0\0\0'
blob[header_off:header_off+8]=name
struct.pack_into('<IIIIIIHHI', blob, header_off+8,
                 virt_size,new_rva,raw_size,new_raw,0,0,0,0,0x60000020)
struct.pack_into('<H', blob, coff+2, nsects+1)
struct.pack_into('<I', blob, size_image_off, align(new_rva+virt_size,section_align))
old_code=struct.unpack_from('<I',blob,size_code_off)[0]
struct.pack_into('<I',blob,size_code_off,old_code+raw_size)

# RVA to file offset helper for existing sections / new section.
def rva_off(rva):
    for name,vs,va,rs,rp,o in sections+[( '.h2sf',virt_size,new_rva,raw_size,new_raw,header_off)]:
        if va <= rva < va + max(vs,rs):
            return rp + (rva-va)
    raise KeyError(hex(rva))

def patch(rva, expected, replacement, label):
    off=rva_off(rva)
    old=bytes(blob[off:off+len(expected)])
    if old!=expected:
        raise SystemExit(f'{label}: expected {expected.hex()} at RVA {rva:x}, got {old.hex()}')
    if len(replacement)!=len(expected): raise ValueError(label)
    blob[off:off+len(replacement)]=replacement
    print(label, hex(rva), expected.hex(), '->', replacement.hex())

# 1. Reject the disproven CHUD-TLS conjunction: make the existing gate succeed.
patch(0xBDF6, bytes.fromhex('e8b5140800'), bytes.fromhex('b001909090'), 'bypass CHUD TLS gate')
# 2. Crosshair is deliberately NOT captured in this ownership proof. Existing capture uses global immediate context.
patch(0xBE78, bytes.fromhex('e823f90100'), bytes.fromhex('31c0909090'), 'disable unsafe crosshair capture')
# 3. After the existing exact shader registry has identified GameplayHud, call fixed actual-context raster helper then return.
call_site=image_base+0xBE97
helper_va=image_base+new_rva
rel_call=helper_va-(call_site+5)
call_bytes=b'\xE8'+struct.pack('<i',rel_call)
jmp_site=image_base+0xBE9C
return_va=image_base+0xC15A
rel_jmp=return_va-(jmp_site+5)
jmp_bytes=b'\xE9'+struct.pack('<i',rel_jmp)
patch(0xBE97, bytes.fromhex('0fbf4c24220fbf442426'), call_bytes+jmp_bytes, 'route gameplay role to Stage3F helper')

out.write_bytes(blob)
print('new section RVA',hex(new_rva),'raw',hex(new_raw),'virtual',hex(virt_size),'rawsize',hex(raw_size))
print('input sha256', input_sha)
print('output sha256',hashlib.sha256(out.read_bytes()).hexdigest())
print('output size',len(blob))
