from pathlib import Path
import hashlib, json, struct, subprocess, sys, tempfile

BASE_SHA = '7c788d598dc2685178b5b3173476e4cf24c22a814acea720cf4f0eba8ad329f4'
# Existing zero-filled executable cave INSIDE Stage3Q .s3ic VirtualSize.
# Keeping the helper here means no PE header/section geometry changes at all.
CODE_RVA = 0x2F1B00
FREE_LIBRARY_IAT_RVA = 0x180230
REACH_MODULE_REF_RVA = 0x2A6DD0
ORIGINAL_LOG_RVA = 0x1D90

REACH_MODULE_FLAGS_RVA = 0x5D7C2
REACH_EPILOGUE_RVA = 0x5E653
REACH_CLEANUP_CAPTURE_RVA = 0x7D31B
REACH_CLEANUP_LOG_RVA = 0x7D4E7

EXPECT_FLAGS = bytes.fromhex('b906000000')
EXPECT_EPILOGUE = bytes.fromhex('4c8ba424d8010000')
EXPECT_CAPTURE = bytes.fromhex('4c893dae9a2200')
EXPECT_LOG = bytes.fromhex('e8a448f8ff')

PRESERVE = [
    (0x87FF1, 21, 'Stage3Q cross-title ambiguity selector'),
    (0xDF60, 15, 'Stage3Q Present caller hint'),
    (0xE060, 15, 'Stage3Q Present1 caller hint'),
    (0x90AC4, 5, 'H2 Classic cleanup call pin'),
    (0x90CE2, 7, 'H2 Classic release pin'),
    (0x92F06, 5, 'Stage3M H2 stock translation hook'),
    (0x95B16, 5, 'Stage3N H2 Classic carrier/yaw-pitch hook'),
    (0x40A52, 5, 'Stage3M H3/ODST stock translation hook'),
    (0x6B321, 5, 'Stage3M Reach stock translation hook'),
    (0x51289, 5, 'Stage3M H4 stock translation hook'),
    (0x48086, 8, 'H4 rollback span 1'),
    (0x48110, 8, 'H4 rollback span 2'),
    (0x5360B, 8, 'H4 rollback span 3'),
    (0x53D41, 8, 'H4 rollback span 4'),
    (0x53E37, 8, 'H4 rollback span 5'),
    (0x7CDA3, 7, 'Stage3O H3 stability gate aim retry'),
    (0x7CDAB, 7, 'Stage3O H3 stability gate cam retry'),
    (0x7CDB3, 7, 'Stage3O H3 stability gate basecam retry'),
    (0x7D167, 7, 'Stage3O H3 stability gate aim final'),
    (0x7D17A, 7, 'Stage3O H3 stability gate cam final'),
    (0x7D19E, 7, 'Stage3O H3 stability gate basecam final'),
    (0x81036, 40, 'Stage3P Reach HUD high-first scan start'),
    (0x8112D, 25, 'Stage3P Reach HUD private+mapped filter'),
    (0x813A8, 10, 'Stage3P Reach HUD six-record completion'),
    (0x47396, 12, 'Stage3P H3 authoritative pause scope'),
    (0x4414D, 5, 'Stage3P generic pause native read scope'),
    (0x44203, 8, 'Stage3P pause recovery title scope'),
    (0x34587, 7, 'Stage3P attribution xref'),
    # The rejected Stage3R patch MUST remain at the Stage3Q identity-only flags.
    (0xA2B86, 5, 'Reach cold adapter identity-only GetModuleHandleEx flags'),
]

def parse_pe(b):
    pe_sig=struct.unpack_from('<I',b,0x3c)[0]; coff=pe_sig+4
    nsec=struct.unpack_from('<H',b,coff+2)[0]; opt_size=struct.unpack_from('<H',b,coff+16)[0]
    opt=coff+20; table=opt+opt_size; secs=[]
    for i in range(nsec):
        h=table+i*40; name=bytes(b[h:h+8]).split(b'\0',1)[0].decode('ascii')
        vs,va,rs,rp=struct.unpack_from('<IIII',b,h+8); ch=struct.unpack_from('<I',b,h+36)[0]
        secs.append(dict(name=name,vs=vs,va=va,rs=rs,rp=rp,ch=ch,h=h))
    return dict(pe_sig=pe_sig,coff=coff,nsec=nsec,opt=opt,table=table,sections=secs)

def rva_off(pe,rva):
    for s in pe['sections']:
        if s['va'] <= rva < s['va']+max(s['vs'],s['rs']): return s['rp']+rva-s['va']
    raise SystemExit(f'RVA not mapped 0x{rva:X}')

def req(b,pe,rva,exp,label):
    o=rva_off(pe,rva); got=bytes(b[o:o+len(exp)])
    if got!=exp: raise SystemExit(f'{label} guard failed 0x{rva:X}: {got.hex()} != {exp.hex()}')

def patch(b,pe,rva,exp,repl,label):
    if len(exp)!=len(repl): raise SystemExit(f'{label} size mismatch')
    req(b,pe,rva,exp,label); o=rva_off(pe,rva); b[o:o+len(exp)]=repl
    print(f'{label}: 0x{rva:X}')

def relcall(src,dst): return b'\xE8'+struct.pack('<i',dst-src-5)

def assemble(td):
    src=Path(__file__).with_name('stage3t_reach_hook_lifetime.S')
    obj=td/'s.o'; elf=td/'s.elf'; raw=td/'s.bin'
    subprocess.run(['as','--64','-o',str(obj),str(src)],check=True)
    cmd=['ld','-m','elf_x86_64',f'-Ttext=0x{CODE_RVA:x}','-e','stage3r_reach_install_epilogue',
         '--defsym',f'FREE_LIBRARY_IAT_RVA=0x{FREE_LIBRARY_IAT_RVA:x}',
         '--defsym',f'REACH_MODULE_REF_RVA=0x{REACH_MODULE_REF_RVA:x}',
         '--defsym',f'ORIGINAL_LOG_RVA=0x{ORIGINAL_LOG_RVA:x}',
         '-o',str(elf),str(obj)]
    subprocess.run(cmd,check=True)
    subprocess.run(['objcopy','-O','binary','-j','.text',str(elf),str(raw)],check=True)
    sy={}
    for line in subprocess.check_output(['nm','-n',str(elf)],text=True).splitlines():
        f=line.split()
        if len(f)==3 and f[2].startswith('stage3r_'): sy[f[2]]=int(f[0],16)
    return raw.read_bytes(),sy

def main():
    if len(sys.argv)!=3: raise SystemExit('usage: builder <Stage3Q.dll> <out.dll>')
    src,out=map(Path,sys.argv[1:]); base=src.read_bytes(); sha=hashlib.sha256(base).hexdigest()
    if sha!=BASE_SHA: raise SystemExit('wrong Stage3Q-R1 base '+sha)
    b=bytearray(base); pe=parse_pe(b); by={s['name']:s for s in pe['sections']}; s=by['.s3ic']
    if (s['va'],s['vs'],s['rs'],s['ch']) != (0x2F1000,0x16F0,0x1800,0x60000020):
        raise SystemExit('Stage3Q-R1 .s3ic geometry mismatch')
    if pe['nsec']!=12: raise SystemExit('Stage3Q-R1 section count mismatch')
    req(b,pe,REACH_MODULE_FLAGS_RVA,EXPECT_FLAGS,'Reach install identity-only module handle flags')
    req(b,pe,REACH_EPILOGUE_RVA,EXPECT_EPILOGUE,'Reach install epilogue R12 restore')
    req(b,pe,REACH_CLEANUP_CAPTURE_RVA,EXPECT_CAPTURE,'Reach cleanup module-reference clear')
    req(b,pe,REACH_CLEANUP_LOG_RVA,EXPECT_LOG,'Reach cleanup final LOG call')
    preserved={(r,n,l):bytes(b[rva_off(pe,r):rva_off(pe,r)+n]) for r,n,l in PRESERVE}
    # Explicitly guard rejected Stage3R cold-selector pin.
    req(b,pe,0xA2B86,bytes.fromhex('b906000000'),'Reach cold adapter must remain UNCHANGED_REFCOUNT')
    with tempfile.TemporaryDirectory(prefix='stage3s-') as t: helper,sy=assemble(Path(t))
    need={'stage3r_reach_install_epilogue','stage3r_reach_cleanup_release_log'}
    if not need<=sy.keys(): raise SystemExit('missing helper symbols')
    # This cave is inside existing VirtualSize and was 0-filled in exact Stage3Q.
    cave_off=rva_off(pe,CODE_RVA)
    if CODE_RVA+len(helper) > s['va']+s['vs']:
        raise SystemExit('helper exceeds existing .s3ic VirtualSize')
    if any(b[cave_off:cave_off+len(helper)]):
        raise SystemExit('loader-safe helper cave is not zero')
    b[cave_off:cave_off+len(helper)] = helper

    patch(b,pe,REACH_MODULE_FLAGS_RVA,EXPECT_FLAGS,bytes.fromhex('b904000000'),
          'Reach installed-hook loader pin')
    patch(b,pe,REACH_EPILOGUE_RVA,EXPECT_EPILOGUE,
          relcall(REACH_EPILOGUE_RVA,sy['stage3r_reach_install_epilogue'])+b'\x90'*3,
          'Reach unretained install-pin release')
    patch(b,pe,REACH_CLEANUP_CAPTURE_RVA,EXPECT_CAPTURE,bytes.fromhex('4c8b35ae9a2200'),
          'Reach retired module pin capture')
    patch(b,pe,REACH_CLEANUP_LOG_RVA,EXPECT_LOG,
          relcall(REACH_CLEANUP_LOG_RVA,sy['stage3r_reach_cleanup_release_log']),
          'Reach post-teardown loader-pin release')

    for (r,n,l),data in preserved.items():
        o=rva_off(pe,r)
        if bytes(b[o:o+n])!=data: raise SystemExit(f'protected site changed {l} 0x{r:X}')
    # PE headers/checksum remain byte-identical to Stage3Q-R1 by design.
    out.parent.mkdir(parents=True,exist_ok=True); out.write_bytes(b)
    # Guard all header bytes through the section table are unchanged.
    header_end=min(s0['rp'] for s0 in pe['sections'])
    if b[:header_end] != base[:header_end]: raise SystemExit('PE headers changed unexpectedly')
    print(json.dumps({'input_sha256':sha,'output_sha256':hashlib.sha256(b).hexdigest(),
                      'output_size':len(b),'helper_rva':hex(CODE_RVA),'helper_size':len(helper),
                      'pe_headers_identical':True,'s3ic_virtual_size':hex(s['vs']),
                      'symbols':{k:hex(v) for k,v in sy.items()}},indent=2))
if __name__=='__main__': main()
