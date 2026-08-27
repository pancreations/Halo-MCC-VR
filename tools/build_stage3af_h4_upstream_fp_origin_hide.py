from pathlib import Path
import hashlib, shutil, struct, subprocess, sys, tempfile

EXPECTED_STAGE3AE_SHA256 = "6adac5256a975a5f829f6a45b3bdac3280cea0bd54b36126a34cc4bab56c1fed"
CHAIN_RVA, CHAIN_LIMIT = 0x002EF7C0, 0x40
INSTALLER_RVA, INSTALLER_LIMIT = 0x0017F680, 0x180
HOOK_RVA, HOOK_LIMIT = 0x002F26F0, 0x110
DATA_RVA = 0x002F01E4
TITLE_CALL_RVA = 0x002F3715
TITLE_CALL_STAGE3AE = bytes.fromhex("e8cafdffff")
STAGE3AC_H4_PATCH_RVA = 0x002F34E4
STAGE3X_FINITE_HIDE_RVA = 0x002F3846
LOGGER_RVA = 0x00001D90
IAT_VIRTUAL_PROTECT_RVA = 0x00180200
IAT_GET_CURRENT_PROCESS_RVA = 0x00180140
IAT_FLUSH_INSTRUCTION_CACHE_RVA = 0x001801E8


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

def assemble(src,origin,defs):
    with tempfile.TemporaryDirectory(prefix='s3af-') as td:
        td=Path(td); obj=td/'x.o'; elf=td/'x.elf'; raw=td/'x.bin'
        subprocess.run(['as','--64','-o',str(obj),str(src)],check=True)
        cmd=['ld','-m','elf_x86_64',f'-Ttext=0x{origin:x}']
        for k,v in defs.items(): cmd += ['--defsym',f'{k}=0x{v:x}']
        cmd += ['-o',str(elf),str(obj)]
        subprocess.run(cmd,check=True)
        subprocess.run(['objcopy','-O','binary','-j','.text',str(elf),str(raw)],check=True)
        syms={}
        for line in subprocess.check_output(['nm','-n',str(elf)],text=True).splitlines():
            f=line.split()
            if len(f)==3 and f[2].startswith('stage3af_'): syms[f[2]]=int(f[0],16)
        return raw.read_bytes(),syms

def relcall(src,dst): return b'\xE8'+struct.pack('<i',dst-(src+5))

def main():
    if len(sys.argv)!=3: raise SystemExit('usage: builder <Stage3AE.dll> <out.dll>')
    src,out=Path(sys.argv[1]),Path(sys.argv[2]); ib=src.read_bytes(); sha=hashlib.sha256(ib).hexdigest()
    if sha!=EXPECTED_STAGE3AE_SHA256: raise SystemExit('wrong Stage3AE input '+sha)
    b=bytearray(ib); pe=parse_pe(b)
    if pe['n']!=12 or pe['size_image']!=0x2F4000: raise SystemExit('unexpected PE geometry')
    text=next(s for s in pe['secs'] if s['name']=='.text')
    s3hc=next(s for s in pe['secs'] if s['name']=='.s3hc')
    s3hd=next(s for s in pe['secs'] if s['name']=='.s3hd')
    s3ic=next(s for s in pe['secs'] if s['name']=='.s3ic')
    if (text['vs'],text['rs'])!=(0x17E680,0x17E800): raise SystemExit('unexpected .text geometry')
    if (s3hc['vs'],s3hc['rs'])!=(0x7C0,0x800): raise SystemExit('unexpected .s3hc geometry')
    if (s3hd['vs'],s3hd['rs'])!=(0x1E4,0x200): raise SystemExit('unexpected .s3hd geometry')
    if (s3ic['vs'],s3ic['rs'])!=(0x16F0,0x1800): raise SystemExit('unexpected .s3ic geometry')

    # New writable state uses only the old unmapped raw tail of .s3hd.
    data_off=roff(pe,DATA_RVA)
    if any(b[data_off:data_off+16]): raise SystemExit('.s3hd tail state cave guard failed')
    b[data_off:data_off+16]=b'\0'*16
    struct.pack_into('<I',b,s3hd['hoff']+8,0x200)

    chain,cs=assemble(Path(__file__).with_name('stage3af_h4_upstream_chain.S'),CHAIN_RVA,{
        'STAGE3AC_H4_PATCH_RVA':STAGE3AC_H4_PATCH_RVA,'STAGE3AF_INSTALLER_RVA':INSTALLER_RVA,
        'H4_BASE_DATA_RVA':DATA_RVA})
    if len(chain)>CHAIN_LIMIT: raise SystemExit(f'chain too large {len(chain)}')
    co=roff(pe,CHAIN_RVA)
    if any(b[co:co+CHAIN_LIMIT]): raise SystemExit('.s3hc tail guard failed')
    b[co:co+len(chain)]=chain; struct.pack_into('<I',b,s3hc['hoff']+8,0x800)
    tramp=cs['stage3af_h4_origin_trampoline']

    hook,hs=assemble(Path(__file__).with_name('stage3af_h4_upstream_origin_hide.S'),HOOK_RVA,{
        'STAGE3AF_TRAMPOLINE_RVA':tramp,'STAGE3X_FINITE_HIDE_RVA':STAGE3X_FINITE_HIDE_RVA,
        'H4_BASE_DATA_RVA':DATA_RVA,'H4_HIT_FLAG_RVA':DATA_RVA+8,'LOGGER_RVA':LOGGER_RVA})
    if hs.get('stage3af_h4_origin_hide')!=HOOK_RVA or len(hook)>HOOK_LIMIT: raise SystemExit(f'hook invalid/too large {len(hook)}')
    ho=roff(pe,HOOK_RVA)
    if any(b[ho:ho+HOOK_LIMIT]): raise SystemExit('.s3ic tail guard failed')
    b[ho:ho+len(hook)]=hook; struct.pack_into('<I',b,s3ic['hoff']+8,0x1800)

    inst,ins=assemble(Path(__file__).with_name('stage3af_h4_upstream_installer.S'),INSTALLER_RVA,{
        'STAGE3AF_HOOK_RVA':HOOK_RVA,'H4_BASE_DATA_RVA':DATA_RVA,'H4_HIT_FLAG_RVA':DATA_RVA+8,
        'H4_BAD_FLAG_RVA':DATA_RVA+12,'LOGGER_RVA':LOGGER_RVA,'IAT_VIRTUAL_PROTECT_RVA':IAT_VIRTUAL_PROTECT_RVA,
        'IAT_GET_CURRENT_PROCESS_RVA':IAT_GET_CURRENT_PROCESS_RVA,'IAT_FLUSH_INSTRUCTION_CACHE_RVA':IAT_FLUSH_INSTRUCTION_CACHE_RVA})
    if ins.get('stage3af_h4_origin_installer')!=INSTALLER_RVA or len(inst)>INSTALLER_LIMIT: raise SystemExit(f'installer invalid/too large {len(inst)}')
    io=roff(pe,INSTALLER_RVA)
    if any(b[io:io+INSTALLER_LIMIT]): raise SystemExit('.text raw-tail guard failed')
    b[io:io+len(inst)]=inst; struct.pack_into('<I',b,text['hoff']+8,0x17E800)

    tco=roff(pe,TITLE_CALL_RVA)
    if bytes(b[tco:tco+5])!=TITLE_CALL_STAGE3AE: raise SystemExit('title call guard failed '+bytes(b[tco:tco+5]).hex())
    b[tco:tco+5]=relcall(TITLE_CALL_RVA,cs['stage3af_h4_title_chain'])
    struct.pack_into('<I',b,pe['opt']+0x40,0)
    out.parent.mkdir(parents=True,exist_ok=True); out.write_bytes(b)
    print('input_sha256',sha)
    print('chain',hex(CHAIN_RVA),len(chain),'trampoline',hex(tramp))
    print('installer',hex(INSTALLER_RVA),len(inst))
    print('hook',hex(HOOK_RVA),len(hook))
    print('state',hex(DATA_RVA),'16 bytes')
    print('title_call',TITLE_CALL_STAGE3AE.hex(),'->',bytes(b[tco:tco+5]).hex())
    print('output_sha256',hashlib.sha256(b).hexdigest())
    print('output_size',len(b))

if __name__=='__main__': main()
