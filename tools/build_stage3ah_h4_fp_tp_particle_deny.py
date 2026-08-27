from pathlib import Path
import hashlib, shutil, struct, subprocess, sys, tempfile

EXPECTED_STAGE3AG_SHA256 = "f81bb6106e54739fc61bbe5790fb9df9b1fd7d06941b4808a4f7467ad8bdac64"
PATCH_RVA = 0x002F3F32
PATCH_LIMIT = 0x92  # original Stage3Z function 0x2F3F32..0x2F3FC3 inclusive
IAT_VIRTUAL_PROTECT_RVA = 0x00180200
STAGE3X_RESTORE_PROTECT_FLUSH_RVA = 0x002F3200


def parse_pe(b):
    pe=struct.unpack_from('<I',b,0x3c)[0]
    if b[:2]!=b'MZ' or b[pe:pe+4]!=b'PE\0\0': raise SystemExit('not PE')
    n=struct.unpack_from('<H',b,pe+6)[0]; optsz=struct.unpack_from('<H',b,pe+20)[0]
    opt=pe+24; sh=opt+optsz; secs=[]
    for i in range(n):
        o=sh+i*40; name=b[o:o+8].rstrip(b'\0').decode('ascii','replace')
        vs,va,rs,rp=struct.unpack_from('<IIII',b,o+8)
        secs.append(dict(name=name,vs=vs,va=va,rs=rs,rp=rp,hoff=o))
    return dict(n=n,opt=opt,secs=secs,size_image=struct.unpack_from('<I',b,opt+56)[0])


def roff(pe,rva):
    for s in pe['secs']:
        if s['va']<=rva<s['va']+max(s['vs'],s['rs']): return s['rp']+(rva-s['va'])
    raise SystemExit(f'RVA not mapped 0x{rva:X}')


def assemble(src):
    for x in ('as','ld','objcopy'):
        if not shutil.which(x): raise SystemExit('missing '+x)
    with tempfile.TemporaryDirectory(prefix='s3ah-') as td:
        td=Path(td); obj=td/'x.o'; elf=td/'x.elf'; raw=td/'x.bin'
        subprocess.run(['as','--64','-o',str(obj),str(src)],check=True)
        subprocess.run(['ld','-m','elf_x86_64',f'-Ttext=0x{PATCH_RVA:x}',
                        '--defsym',f'IAT_VIRTUAL_PROTECT_RVA=0x{IAT_VIRTUAL_PROTECT_RVA:x}',
                        '--defsym',f'STAGE3X_RESTORE_PROTECT_FLUSH_RVA=0x{STAGE3X_RESTORE_PROTECT_FLUSH_RVA:x}',
                        '-o',str(elf),str(obj)],check=True)
        subprocess.run(['objcopy','-O','binary','-j','.text',str(elf),str(raw)],check=True)
        return raw.read_bytes()


def main():
    if len(sys.argv)!=3: raise SystemExit('usage: builder <Stage3AG.dll> <out.dll>')
    src,out=Path(sys.argv[1]),Path(sys.argv[2]); ib=src.read_bytes(); sha=hashlib.sha256(ib).hexdigest()
    if sha!=EXPECTED_STAGE3AG_SHA256: raise SystemExit('wrong Stage3AG input '+sha)
    b=bytearray(ib); pe=parse_pe(b)
    if pe['n']!=12 or pe['size_image']!=0x2F4000: raise SystemExit('unexpected PE geometry')
    po=roff(pe,PATCH_RVA)
    old=bytes(b[po:po+PATCH_LIMIT])
    # Guard distinctive Stage3Z prologue and target LEA before replacing it.
    if old[:15] != bytes.fromhex('534883ec4089d3488d9136bd270048'):
        raise SystemExit('Stage3Z patch-function guard failed: '+old[:15].hex())
    new=assemble(Path(__file__).with_name('stage3ah_h4_fp_tp_particle_deny.S'))
    if len(new)>PATCH_LIMIT: raise SystemExit(f'Stage3AH function too large {len(new)} > {PATCH_LIMIT}')
    b[po:po+PATCH_LIMIT] = new + b'\x90'*(PATCH_LIMIT-len(new))
    struct.pack_into('<I',b,pe['opt']+0x40,0)
    out.write_bytes(b)
    print('input_sha256',sha)
    print('patch_rva',hex(PATCH_RVA),'old_bytes',PATCH_LIMIT,'new_bytes',len(new))
    print('new_code_sha256',hashlib.sha256(new).hexdigest())
    print('output_sha256',hashlib.sha256(b).hexdigest())
    print('output_size',len(b))

if __name__=='__main__': main()
