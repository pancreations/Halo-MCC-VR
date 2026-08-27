from pathlib import Path
import hashlib, struct, subprocess, sys, tempfile

EXPECTED_STAGE3AF_SHA256 = "d7648b272770d426dc458630fec331c239bca0e046a0438775d63a223c87d37d"
ACTIVE_TITLE_CALL_RVA = 0x00030894
ACTIVE_TITLE_STAGE3AF = bytes.fromhex("e8220d2c00")   # -> Stage3AD wrapper 0x2F15BB
WRONG_NESTED_CALL_RVA = 0x002F3715
WRONG_NESTED_STAGE3AF = bytes.fromhex("e8a6c0ffff") # -> old Stage3AF chain 0x2EF7C0
NESTED_RESTORE_STAGE3AE = bytes.fromhex("e8cafdffff") # -> Stage3AC H4 patch 0x2F34E4
WRAPPER_RVA = 0x002F27AC
WRAPPER_LIMIT = 84
STAGE3AD_TITLE_WRAPPER_RVA = 0x002F15BB
STAGE3AF_INSTALLER_RVA = 0x0017F680
H4_MODULE_REF_RVA = 0x002A7208


def parse_pe(b):
    pe=struct.unpack_from('<I',b,0x3c)[0]
    if b[:2]!=b'MZ' or b[pe:pe+4]!=b'PE\0\0': raise SystemExit('not PE')
    n=struct.unpack_from('<H',b,pe+6)[0]; optsz=struct.unpack_from('<H',b,pe+20)[0]
    opt=pe+24; sh=opt+optsz; secs=[]
    for i in range(n):
        o=sh+i*40; name=b[o:o+8].rstrip(b'\0').decode('ascii','replace')
        vs,va,rs,rp=struct.unpack_from('<IIII',b,o+8); chars=struct.unpack_from('<I',b,o+36)[0]
        secs.append(dict(name=name,vs=vs,va=va,rs=rs,rp=rp,chars=chars,hoff=o))
    return dict(pe=pe,n=n,opt=opt,secs=secs,size_image=struct.unpack_from('<I',b,opt+56)[0])

def roff(pe,rva):
    for s in pe['secs']:
        if s['va']<=rva<s['va']+max(s['vs'],s['rs']): return s['rp']+(rva-s['va'])
    raise SystemExit(f'RVA not mapped 0x{rva:X}')

def relcall(src,dst): return b'\xE8'+struct.pack('<i',dst-(src+5))

def assemble(src):
    with tempfile.TemporaryDirectory(prefix='s3ag-') as td:
        td=Path(td); obj=td/'x.o'; elf=td/'x.elf'; raw=td/'x.bin'
        subprocess.run(['as','--64','-o',str(obj),str(src)],check=True)
        cmd=['ld','-m','elf_x86_64',f'-Ttext=0x{WRAPPER_RVA:x}',
             '--defsym',f'STAGE3AD_TITLE_WRAPPER_RVA=0x{STAGE3AD_TITLE_WRAPPER_RVA:x}',
             '--defsym',f'STAGE3AF_INSTALLER_RVA=0x{STAGE3AF_INSTALLER_RVA:x}',
             '--defsym',f'H4_MODULE_REF_RVA=0x{H4_MODULE_REF_RVA:x}',
             '-o',str(elf),str(obj)]
        subprocess.run(cmd,check=True)
        subprocess.run(['objcopy','-O','binary','-j','.text',str(elf),str(raw)],check=True)
        return raw.read_bytes()

def main():
    if len(sys.argv)!=3: raise SystemExit('usage: builder <Stage3AF.dll> <out.dll>')
    src,out=Path(sys.argv[1]),Path(sys.argv[2]); ib=src.read_bytes(); sha=hashlib.sha256(ib).hexdigest()
    if sha!=EXPECTED_STAGE3AF_SHA256: raise SystemExit('wrong Stage3AF input '+sha)
    b=bytearray(ib); pe=parse_pe(b)
    if pe['n']!=12 or pe['size_image']!=0x2F4000: raise SystemExit('unexpected PE geometry')
    wrapper=assemble(Path(__file__).with_name('stage3ag_h4_active_title_wrapper.S'))
    if len(wrapper)>WRAPPER_LIMIT: raise SystemExit(f'wrapper too large {len(wrapper)}')
    wo=roff(pe,WRAPPER_RVA)
    if any(b[wo:wo+WRAPPER_LIMIT]): raise SystemExit('Stage3AF wrapper cave is not zero')
    ao=roff(pe,ACTIVE_TITLE_CALL_RVA)
    no=roff(pe,WRONG_NESTED_CALL_RVA)
    if bytes(b[ao:ao+5])!=ACTIVE_TITLE_STAGE3AF: raise SystemExit('active title edge guard failed '+bytes(b[ao:ao+5]).hex())
    if bytes(b[no:no+5])!=WRONG_NESTED_STAGE3AF: raise SystemExit('nested Stage3AF edge guard failed '+bytes(b[no:no+5]).hex())
    b[wo:wo+len(wrapper)] = wrapper
    b[ao:ao+5] = relcall(ACTIVE_TITLE_CALL_RVA,WRAPPER_RVA)
    b[no:no+5] = NESTED_RESTORE_STAGE3AE
    struct.pack_into('<I',b,pe['opt']+0x40,0)
    out.write_bytes(b)
    print('input_sha256',sha)
    print('wrapper_rva',hex(WRAPPER_RVA),'bytes',len(wrapper),'sha256',hashlib.sha256(wrapper).hexdigest())
    print('active_title',ACTIVE_TITLE_STAGE3AF.hex(),'->',bytes(b[ao:ao+5]).hex())
    print('nested_restore',WRONG_NESTED_STAGE3AF.hex(),'->',bytes(b[no:no+5]).hex())
    print('output_sha256',hashlib.sha256(b).hexdigest())
    print('output_size',len(b))

if __name__=='__main__': main()
