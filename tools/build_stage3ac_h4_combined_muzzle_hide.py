from pathlib import Path
import hashlib, json, shutil, struct, subprocess, sys, tempfile

EXPECTED_STAGE3Z_SHA256 = "154f31b34049ef2797eb8993a252a51c411df3ec6a8b30f583b0b8137e66ebce"
TITLE_CALL_RVA = 0x00030894
CLEANUP_EFFECT_CALL_RVA = 0x002F3B3A
TITLE_CALL_STAGE3Z = bytes.fromhex("e847362c00")
CLEANUP_CALL_STAGE3Z = bytes.fromhex("e885040000")

SECTIONS = {".wrapper": 0x002F1A87, ".cleanup": 0x002F3FCB}
LIMITS = {".wrapper": 121, ".cleanup": 53}
DEFS = {
    "TITLE_ACTIVE_RVA": 0x000879C0,
    "H4_MODULE_REF_RVA": 0x002A7208,
    "STAGE3X_EFFECT_INSTALLER_RVA": 0x002EF595,
    "STAGE3X_EFFECT_RESTORE_RVA": 0x002F385C,
    "STAGE3X_HUD_INSTALL_RVA": 0x002F323B,
    "STAGE3X_CURV_INSTALL_RVA": 0x002F34E4,
    "STAGE3Z_PARTICLE_PATCH_RVA": 0x002F3F32,
    "STAGE3Z_PARTICLE_RESTORE_RVA": 0x002F3FC4,
}

def parse_pe(b):
    pe=struct.unpack_from('<I',b,0x3C)[0]
    if b[pe:pe+4]!=b'PE\0\0': raise SystemExit('not PE')
    n=struct.unpack_from('<H',b,pe+6)[0]; optsz=struct.unpack_from('<H',b,pe+20)[0]; opt=pe+24
    size_image=struct.unpack_from('<I',b,opt+56)[0]; sh=opt+optsz; secs=[]
    for i in range(n):
        o=sh+i*40; name=b[o:o+8].rstrip(b'\0').decode('ascii','replace')
        vs,va,rs,rp=struct.unpack_from('<IIII',b,o+8); chars=struct.unpack_from('<I',b,o+36)[0]
        secs.append(dict(name=name,vs=vs,va=va,rs=rs,rp=rp,chars=chars,hoff=o))
    return dict(pe=pe,n=n,opt=opt,size_image=size_image,secs=secs)

def roff(pe,rva):
    for s in pe['secs']:
        if s['va']<=rva<s['va']+max(s['vs'],s['rs']): return s['rp']+(rva-s['va'])
    raise SystemExit(f'RVA not mapped {rva:#x}')

def relcall(src,dst): return b'\xE8'+struct.pack('<i',dst-(src+5))

def assemble(tooldir):
    for x in ('as','ld','objcopy','nm'):
        if not shutil.which(x): raise SystemExit('missing '+x)
    with tempfile.TemporaryDirectory(prefix='s3ac-') as td:
        td=Path(td); obj=td/'ac.o'; elf=td/'ac.elf'
        subprocess.run(['as','--64','-o',str(obj),str(tooldir/'stage3ac_h4_combined_muzzle_hide.S')],check=True,cwd=tooldir)
        cmd=['ld','-m','elf_x86_64','-e','stage3ac_h4_title_install_wrapper']
        for sec,rva in SECTIONS.items(): cmd += [f'--section-start={sec}=0x{rva:x}']
        for k,v in DEFS.items(): cmd += ['--defsym',f'{k}=0x{v:x}']
        cmd += ['-o',str(elf),str(obj)]
        subprocess.run(cmd,check=True)
        raw={}
        for sec in SECTIONS:
            p=td/(sec.strip('.')+'.bin')
            subprocess.run(['objcopy','-O','binary','-j',sec,str(elf),str(p)],check=True)
            raw[sec]=p.read_bytes()
        syms={}; nm=subprocess.check_output(['nm','-n',str(elf)],text=True)
        for line in nm.splitlines():
            f=line.split()
            if len(f)==3 and f[2].startswith('stage3ac_'): syms[f[2]]=int(f[0],16)
        return raw,syms,nm

def main():
    if len(sys.argv)!=3: raise SystemExit('usage: build_stage3ac_h4_combined_muzzle_hide.py <Stage3Z.dll> <out.dll>')
    src,out=Path(sys.argv[1]),Path(sys.argv[2]); ib=src.read_bytes(); sha=hashlib.sha256(ib).hexdigest()
    if sha!=EXPECTED_STAGE3Z_SHA256: raise SystemExit('wrong Stage3Z input '+sha)
    raw,syms,nm=assemble(Path(__file__).resolve().parent)
    need={'stage3ac_h4_title_install_wrapper','stage3ac_combined_effect_restore'}
    if not need<=set(syms): raise SystemExit('missing symbols '+repr(need-set(syms)))
    b=bytearray(ib); pe=parse_pe(b); q=pe['secs'][-1]
    if pe['n']!=12 or q['name']!='.s3qd' or q['va']!=0x2F3000 or q['vs']!=0x1000 or q['rs']!=0x1000 or pe['size_image']!=0x2F4000:
        raise SystemExit('Stage3Z loader geometry mismatch')
    # .wrapper is an executable .s3ic post-function gap. .cleanup is the final
    # mapped tail immediately after Stage3Z's particle-restorer RET. Neither is
    # the live .s3qd+0 runtime BSS that caused 3AA/3AB.
    for sec,data in raw.items():
        if len(data)>LIMITS[sec]: raise SystemExit(f'{sec} too large {len(data)} > {LIMITS[sec]}')
        o=roff(pe,SECTIONS[sec]); span=bytes(b[o:o+LIMITS[sec]])
        if any(span): raise SystemExit(f'{sec} exact Stage3Z cave guard failed; nonzero byte present')
    to=roff(pe,TITLE_CALL_RVA); co=roff(pe,CLEANUP_EFFECT_CALL_RVA)
    if bytes(b[to:to+5])!=TITLE_CALL_STAGE3Z: raise SystemExit('Stage3Z title edge guard failed')
    if bytes(b[co:co+5])!=CLEANUP_CALL_STAGE3Z: raise SystemExit('Stage3Z cleanup edge guard failed')
    for sec,data in raw.items():
        o=roff(pe,SECTIONS[sec]); b[o:o+len(data)]=data
    b[to:to+5]=relcall(TITLE_CALL_RVA,syms['stage3ac_h4_title_install_wrapper'])
    b[co:co+5]=relcall(CLEANUP_EFFECT_CALL_RVA,syms['stage3ac_combined_effect_restore'])
    out.write_bytes(b)
    print('input',sha)
    for sec,data in raw.items(): print(sec,hex(SECTIONS[sec]),len(data),hashlib.sha256(data).hexdigest())
    print('output',hashlib.sha256(b).hexdigest(),len(b)); print('SizeOfImage',hex(pe['size_image']),'sections',pe['n'])
    print('symbols',json.dumps({k:hex(v) for k,v in syms.items()},sort_keys=True))
    print('title_call',TITLE_CALL_STAGE3Z.hex(),'->',b[to:to+5].hex())
    print('cleanup_call',CLEANUP_CALL_STAGE3Z.hex(),'->',b[co:co+5].hex())
if __name__=='__main__': main()
