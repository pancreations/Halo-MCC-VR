from pathlib import Path
import hashlib, shutil, struct, subprocess, sys, tempfile

BASE_SHA = '90798fa17c73e9b3d3fa451480e9c44ab5cda5cb41b3dc912cf4b3ae62a5036c'
CODE = 0x2F2020
ACTIVE_TITLE = 0x2BA6C8
ENGINE_PAUSE_VALIDATED = 0x2B71EC
READ_ENGINE_PAUSED = 0x7AC40
REACH_FAST_GENERATION = 0x2F01E0

REACH_SCAN_START = 0x81036
REACH_SCAN_FILTER = 0x8112D
REACH_SCAN_FILTER_LEN = 25
REACH_SCAN_CONTINUE = 0x813A8
REACH_SCAN_LOOP = 0x81092
GAME_HAS_AUTH_PROOF = 0x47396
GAME_HAS_AUTH_PROOF_LEN = 12
AUTO_READ_PAUSE_CALL = 0x4414D
AUTO_RECOVERY_VALIDATED = 0x44203
WELCOME_LEA = 0x34587

PRESERVE = [
    (0x90AC4, 5, 'H2 Classic cleanup call pin'),
    (0x90CE2, 7, 'H2 Classic release pin'),
    (0x92F06, 5, 'Stage3M H2 stock translation hook'),
    (0x95B16, 5, 'Stage3N H2 Classic carrier hook'),
    (0x40A52, 5, 'Stage3M H3/ODST stock translation hook'),
    (0x6B321, 5, 'Stage3M Reach stock translation hook'),
    (0x51289, 5, 'Stage3M H4 stock translation hook'),
    (0x48086, 8, 'H4 rollback span 1'),
    (0x48110, 8, 'H4 rollback span 2'),
    (0x5360B, 8, 'H4 rollback span 3'),
    (0x53D41, 8, 'H4 rollback span 4'),
    (0x53E37, 8, 'H4 rollback span 5'),
    # Stage3O accepted H3 stability gates: preserve all six exactly.
    (0x7CDA3, 7, 'Stage3O H3 stability gate aim retry'),
    (0x7CDAB, 7, 'Stage3O H3 stability gate cam retry'),
    (0x7CDB3, 7, 'Stage3O H3 stability gate basecam retry'),
    (0x7D167, 7, 'Stage3O H3 stability gate aim final'),
    (0x7D17A, 7, 'Stage3O H3 stability gate cam final'),
    (0x7D19E, 7, 'Stage3O H3 stability gate basecam final'),
]

def align(v,a): return (v+a-1)//a*a

def run(c): subprocess.run(c, check=True)

def peparse(b):
    p=struct.unpack_from('<I',b,0x3c)[0]; c=p+4
    n=struct.unpack_from('<H',b,c+2)[0]; osz=struct.unpack_from('<H',b,c+16)[0]
    opt=c+20; table=opt+osz; ss=[]
    for i in range(n):
        h=table+i*40; name=bytes(b[h:h+8]).split(b'\0',1)[0].decode('ascii')
        vs,va,rs,rp=struct.unpack_from('<IIII',b,h+8)
        ss.append(dict(name=name,vs=vs,va=va,rs=rs,rp=rp,h=h))
    return dict(opt=opt,sections=ss,fa=struct.unpack_from('<I',b,opt+0x24)[0],sa=struct.unpack_from('<I',b,opt+0x20)[0])

def rva_off(pe,rva):
    for s in pe['sections']:
        if s['va'] <= rva < s['va']+max(s['vs'],s['rs']): return s['rp']+rva-s['va']
    raise SystemExit(f'RVA not mapped: 0x{rva:X}')

def req(b,pe,rva,expected,label):
    o=rva_off(pe,rva); actual=bytes(b[o:o+len(expected)])
    if actual != expected: raise SystemExit(f'{label} guard 0x{rva:X}: {actual.hex()} != {expected.hex()}')

def patch(b,pe,rva,expected,replacement,label):
    if len(expected)!=len(replacement): raise SystemExit(f'{label}: size mismatch')
    req(b,pe,rva,expected,label); o=rva_off(pe,rva); b[o:o+len(expected)]=replacement
    print(f'{label}: RVA 0x{rva:X}')

def rel(op,src,dst): return bytes([op])+struct.pack('<i',dst-src-5)
def lea(prefix,src,dst): return prefix+struct.pack('<i',dst-src-7)

def assemble(td):
    for x in ('as','ld','objcopy','nm'):
        if not shutil.which(x): raise SystemExit('missing '+x)
    src=Path(__file__).with_name('stage3p_reach_h2pause.S'); obj=td/'p.o'; elf=td/'p.elf'; raw=td/'p.bin'
    run(['as','--64','-o',str(obj),str(src)])
    defs={'ACTIVE_TITLE_RVA':ACTIVE_TITLE,'ENGINE_PAUSE_VALIDATED_RVA':ENGINE_PAUSE_VALIDATED,
          'READ_ENGINE_PAUSED_RVA':READ_ENGINE_PAUSED,'REACH_FAST_GENERATION_RVA':REACH_FAST_GENERATION,
          'REACH_SCAN_LOOP_RVA':REACH_SCAN_LOOP}
    cmd=['ld','-m','elf_x86_64',f'-Ttext=0x{CODE:x}','-e','stage3p_reach_memtype_filter']
    for k,v in defs.items(): cmd += ['--defsym',f'{k}=0x{v:x}']
    cmd += ['-o',str(elf),str(obj)]; run(cmd)
    run(['objcopy','-O','binary','-j','.text',str(elf),str(raw)])
    sy={}
    for line in subprocess.check_output(['nm','-n',str(elf)],text=True).splitlines():
        f=line.split()
        if len(f)==3 and f[2].startswith('stage3p_'): sy[f[2]]=int(f[0],16)
    return raw.read_bytes(),sy

def main():
    if len(sys.argv)!=3: raise SystemExit('usage: build_stage3p_reach_h2pause.py <Stage3O.dll> <output.dll>')
    src,out=map(Path,sys.argv[1:]); base=src.read_bytes(); sha=hashlib.sha256(base).hexdigest()
    if sha!=BASE_SHA: raise SystemExit('wrong Stage3O base '+sha)
    b=bytearray(base); pe=peparse(b); by={s['name']:s for s in pe['sections']}
    s=by.get('.s3ic'); d=by.get('.s3hd')
    if not s or (s['va'],s['vs'],s['rs']) != (0x2F1000,0x1008,0x1200): raise SystemExit('Stage3O .s3ic geometry mismatch')
    if s['rp']+s['rs'] != len(b): raise SystemExit('Stage3O overlay mismatch')
    if not d or (d['va'],d['vs'],d['rs']) != (0x2F0000,0x1E0,0x200): raise SystemExit('Stage3O .s3hd geometry mismatch')

    expected_scan_start=bytes.fromhex('488b45f80f57c04c8b7df0')
    expected_filter=rel(0xE8,REACH_SCAN_FILTER,0x2F1C00)+b'\x90'*(REACH_SCAN_FILTER_LEN-5)
    expected_auth=bytes.fromhex('0fb6054ffe26009084c0757d')
    expected_read=rel(0xE8,AUTO_READ_PAUSE_CALL,READ_ENGINE_PAUSED)
    expected_recovery=bytes.fromhex('0fb605e22f270090')
    expected_welcome=bytes.fromhex('488d0d02d72b00')
    expected_scan_continue=bytes.fromhex('4c3b7da80f82e0fcffff')
    for args in [(REACH_SCAN_START,expected_scan_start,'Reach scan start'),(REACH_SCAN_FILTER,expected_filter,'Stage3O Reach mapped-only filter'),
                 (REACH_SCAN_CONTINUE,expected_scan_continue,'Reach scan range continuation'),
                 (GAME_HAS_AUTH_PROOF,expected_auth,'stale H3 authoritative proof'),(AUTO_READ_PAUSE_CALL,expected_read,'generic native pause read'),
                 (AUTO_RECOVERY_VALIDATED,expected_recovery,'generic pause recovery validation'),(WELCOME_LEA,expected_welcome,'Stage3O welcome xref')]:
        req(b,pe,*args)

    preserved={}
    for rva,n,label in PRESERVE:
        o=rva_off(pe,rva); preserved[(rva,label)]=bytes(b[o:o+n])

    # Writable Stage3P state occupies previously-zero padding in .s3hd.
    data_off=rva_off(pe,REACH_FAST_GENERATION)
    if bytes(b[data_off:data_off+4]) != b'\0'*4: raise SystemExit('Stage3P data slot is not zero')
    b[data_off:data_off+4]=struct.pack('<I',0xFFFFFFFF)
    struct.pack_into('<I',b,d['h']+8,max(d['vs'],0x1E4)); d['vs']=max(d['vs'],0x1E4)

    with tempfile.TemporaryDirectory(prefix='halomccvr-stage3p-') as td:
        helper,sy=assemble(Path(td))
    required={'stage3p_reach_memtype_filter','stage3p_reach_scan_start','stage3p_h3_authoritative_pause_proof',
              'stage3p_h3_read_pause_scoped','stage3p_h3_pause_recovery_guard','stage3p_reach_scan_continue','stage3p_str_welcome'}
    if not required <= sy.keys(): raise SystemExit('missing Stage3P symbols '+str(required-sy.keys()))

    helper_rel=CODE-s['va']; needed=helper_rel+len(helper); new_vs=max(s['vs'],needed); new_rs=align(needed,pe['fa'])
    if new_rs>s['rs']:
        grow=new_rs-s['rs']; b.extend(b'\0'*grow)
        size_code=struct.unpack_from('<I',b,pe['opt']+4)[0]; struct.pack_into('<I',b,pe['opt']+4,size_code+grow)
    # refresh offset remains valid because last section grows only at EOF
    helper_off=s['rp']+helper_rel
    b[helper_off:helper_off+len(helper)]=helper
    struct.pack_into('<I',b,s['h']+8,new_vs); struct.pack_into('<I',b,s['h']+16,new_rs); s['vs']=new_vs;s['rs']=new_rs
    struct.pack_into('<I',b,pe['opt']+0x38,align(s['va']+new_vs,pe['sa']))

    patch(b,pe,REACH_SCAN_START,expected_scan_start,
          rel(0xE8,REACH_SCAN_START,sy['stage3p_reach_scan_start'])+b'\x90'*6,
          'Reach HUD high-first scan start')
    patch(b,pe,REACH_SCAN_FILTER,expected_filter,
          rel(0xE8,REACH_SCAN_FILTER,sy['stage3p_reach_memtype_filter'])+b'\x90'*(REACH_SCAN_FILTER_LEN-5),
          'Reach HUD private+mapped storage filter')
    patch(b,pe,REACH_SCAN_CONTINUE,expected_scan_continue,
          rel(0xE8,REACH_SCAN_CONTINUE,sy['stage3p_reach_scan_continue'])+b'\x90'*5,
          'Reach HUD stop after six proven records')

    # Replace the stale Halo3 global early-return with a title-scoped proof.
    # false -> short JE to the original TitleAdapter path; true -> short JMP to return-true.
    auth = rel(0xE8,GAME_HAS_AUTH_PROOF,sy['stage3p_h3_authoritative_pause_proof'])
    auth += b'\x84\xC0'             # test al,al
    auth += b'\x74\x03'             # je 0x473A2 (original title-aware path)
    auth += b'\xEB\x7E'             # jmp 0x4741F (return true)
    auth += b'\x90'
    patch(b,pe,GAME_HAS_AUTH_PROOF,expected_auth,auth,'Authoritative pause proof scoped to H3')

    patch(b,pe,AUTO_READ_PAUSE_CALL,expected_read,
          rel(0xE8,AUTO_READ_PAUSE_CALL,sy['stage3p_h3_read_pause_scoped']),
          'Auto pause native read scoped to H3')
    patch(b,pe,AUTO_RECOVERY_VALIDATED,expected_recovery,
          rel(0xE8,AUTO_RECOVERY_VALIDATED,sy['stage3p_h3_pause_recovery_guard'])+b'\x0f\xb6\xc0',
          'Auto pause recovery scoped to H3')

    patch(b,pe,WELCOME_LEA,expected_welcome,
          lea(b'\x48\x8d\x0d',WELCOME_LEA,sy['stage3p_str_welcome']),
          'Menu attribution -> @MeWhenINameMyself')

    # Preserve all accepted H2/H3/H4 transaction pins from Stage3O.
    for (rva,label),data in preserved.items():
        o=rva_off(pe,rva)
        if bytes(b[o:o+len(data)])!=data: raise SystemExit(f'protected site changed: {label} at 0x{rva:X}')

    # PE checksum is intentionally zeroed, matching prior post-link candidates.
    struct.pack_into('<I',b,pe['opt']+0x40,0)
    out.write_bytes(b)
    print('input',sha)
    print('helper',hashlib.sha256(helper).hexdigest(),len(helper))
    print('output',hashlib.sha256(b).hexdigest(),len(b))
    print('s3ic',hex(new_vs),hex(new_rs),'s3hd_vs',hex(d['vs']))
    for k in sorted(sy): print(k,hex(sy[k]))

if __name__=='__main__': main()
