from pathlib import Path
import hashlib, json, shutil, struct, subprocess, sys, tempfile

EXPECTED_STAGE3X_SHA256 = "486cc6e4f943c6fc58e227328777c54926799b1053f7e402767e3e38fca78ff6"
NEW_RVA = 0x2F3EE0
TITLE_CALL_RVA = 0x00030894
CLEANUP_EFFECT_CALL_RVA = 0x002F3B3A
TITLE_CALL_STAGE3X = bytes.fromhex("e83c2e2c00")
CLEANUP_CALL_STAGE3X = bytes.fromhex("e81dfdffff")

DEFS = {
    "TITLE_ACTIVE_RVA": 0x000879C0,
    "H4_MODULE_REF_RVA": 0x002A7208,
    "IAT_VIRTUAL_PROTECT_RVA": 0x00180200,
    "STAGE3X_RESTORE_PROTECT_FLUSH_RVA": 0x002F3200,
    "STAGE3X_HUD_INSTALL_RVA": 0x002F323B,
    "STAGE3X_CURV_INSTALL_RVA": 0x002F34E4,
}

def parse_pe(b):
    pe = struct.unpack_from('<I', b, 0x3C)[0]
    if b[pe:pe+4] != b'PE\0\0': raise SystemExit('not PE')
    n = struct.unpack_from('<H', b, pe+6)[0]
    optsz = struct.unpack_from('<H', b, pe+20)[0]
    opt = pe+24
    size_image = struct.unpack_from('<I', b, opt+56)[0]
    sh = opt+optsz
    secs=[]
    for i in range(n):
        o=sh+i*40
        name=b[o:o+8].rstrip(b'\0').decode('ascii','replace')
        vs,va,rs,rp=struct.unpack_from('<IIII',b,o+8)
        chars=struct.unpack_from('<I',b,o+36)[0]
        secs.append(dict(name=name,vs=vs,va=va,rs=rs,rp=rp,chars=chars,hoff=o))
    return dict(pe=pe,n=n,opt=opt,size_image=size_image,secs=secs)

def roff(pe,rva):
    for s in pe['secs']:
        if s['va'] <= rva < s['va'] + max(s['vs'],s['rs']):
            return s['rp'] + (rva-s['va'])
    raise SystemExit(f'RVA not mapped {rva:#x}')

def relcall(src,dst): return b'\xE8'+struct.pack('<i',dst-(src+5))

def assemble(tooldir):
    for x in ('as','ld','objcopy','nm'):
        if not shutil.which(x): raise SystemExit('missing '+x)
    with tempfile.TemporaryDirectory(prefix='s3z-') as td:
        td=Path(td); obj=td/'z.o'; elf=td/'z.elf'; raw=td/'z.bin'
        subprocess.run(['as','--64','-o',str(obj),str(tooldir/'stage3z_h4_fp_particle_deny.S')],check=True)
        cmd=['ld','-m','elf_x86_64',f'-Ttext=0x{NEW_RVA:x}','-e','stage3z_h4_title_install_wrapper']
        for k,v in DEFS.items(): cmd += ['--defsym',f'{k}=0x{v:x}']
        cmd += ['-o',str(elf),str(obj)]
        subprocess.run(cmd,check=True)
        subprocess.run(['objcopy','-O','binary','-j','.text',str(elf),str(raw)],check=True)
        syms={}
        nm=subprocess.check_output(['nm','-n',str(elf)],text=True)
        for line in nm.splitlines():
            f=line.split()
            if len(f)==3 and f[2].startswith('stage3z_'): syms[f[2]]=int(f[0],16)
        return raw.read_bytes(),syms,nm

def main():
    if len(sys.argv)!=3: raise SystemExit('usage: build_stage3z_h4_fp_particle_deny.py <Stage3X.dll> <out.dll>')
    src,out=Path(sys.argv[1]),Path(sys.argv[2]); ib=src.read_bytes()
    sha=hashlib.sha256(ib).hexdigest()
    if sha != EXPECTED_STAGE3X_SHA256: raise SystemExit('wrong Stage3X input '+sha)
    helper,syms,nm=assemble(Path(__file__).resolve().parent)
    need={'stage3z_h4_title_install_wrapper','stage3z_h4_fp_particle_patch','stage3z_h4_fp_particle_restore'}
    if not need <= set(syms): raise SystemExit('missing symbols '+repr(need-set(syms)))
    b=bytearray(ib); pe=parse_pe(b)
    q=pe['secs'][-1]
    if pe['n']!=12 or q['name']!='.s3qd' or q['va']!=0x2F3000 or q['vs']!=0x1000 or q['rs']!=0x1000:
        raise SystemExit('Stage3X loader geometry mismatch')
    if pe['size_image']!=0x2F4000: raise SystemExit('Stage3X SizeOfImage mismatch')
    if NEW_RVA + len(helper) > 0x2F4000: raise SystemExit(f'helper too large: {len(helper)} bytes')
    ho=roff(pe,NEW_RVA)
    if any(b[ho:ho+len(helper)]): raise SystemExit('Stage3X tail cave is not zero')
    # Guard and patch only the two Stage3X control-flow edges.
    to=roff(pe,TITLE_CALL_RVA); co=roff(pe,CLEANUP_EFFECT_CALL_RVA)
    if bytes(b[to:to+5])!=TITLE_CALL_STAGE3X: raise SystemExit('title call guard failed')
    if bytes(b[co:co+5])!=CLEANUP_CALL_STAGE3X: raise SystemExit('cleanup call guard failed')
    b[ho:ho+len(helper)] = helper
    b[to:to+5] = relcall(TITLE_CALL_RVA,syms['stage3z_h4_title_install_wrapper'])
    b[co:co+5] = relcall(CLEANUP_EFFECT_CALL_RVA,syms['stage3z_h4_fp_particle_restore'])
    out.write_bytes(b)
    print('input',sha)
    print('helper',hashlib.sha256(helper).hexdigest(),len(helper),hex(NEW_RVA),hex(NEW_RVA+len(helper)))
    print('output',hashlib.sha256(b).hexdigest(),len(b))
    print('SizeOfImage',hex(pe['size_image']),'sections',pe['n'])
    print('symbols',json.dumps({k:hex(v) for k,v in syms.items()},sort_keys=True))
    print('title_call',TITLE_CALL_STAGE3X.hex(),'->',b[to:to+5].hex())
    print('cleanup_call',CLEANUP_CALL_STAGE3X.hex(),'->',b[co:co+5].hex())

if __name__=='__main__': main()
