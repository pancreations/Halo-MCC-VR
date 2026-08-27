from pathlib import Path
import hashlib, json, struct, sys

PROTECTED = {
    'H4 B-edge pause': (0x14EFA, 5),
    'H4 real heartbeat': (0x56CA7, 12),
    'H4 heartbeat gate': (0x8431C, 7),
    'H4 procedural rollback A': (0x48086, 16),
    'H4 procedural rollback B': (0x48110, 16),
    'H4 procedural rollback C': (0x5360B, 16),
    'H4 procedural rollback D': (0x53D41, 16),
    'H4 procedural rollback E': (0x53E37, 16),
    'H2 cleanup pin A': (0x90AC4, 8),
    'H2 cleanup pin B': (0x90CE2, 8),
    'H2 stock translation': (0x92F06, 8),
    'H2 Classic carrier': (0x95B16, 8),
    'H3/ODST stock translation': (0x40A52, 8),
    'Reach stock translation': (0x6B321, 8),
    'Reach HUD A': (0x81036, 8),
    'Reach HUD B': (0x8112D, 8),
    'Reach HUD C': (0x813A8, 8),
}
INTENDED = [
    (0x30894, 5, 'title edge'),
    (0x2F114B, 181, 'verified .s3ic patcher gap'),
    (0x2F15BB, 69, 'verified .s3ic title gap'),
    (0x2F1BD7, 41, 'verified .s3ic cleanup gap'),
    (0x2F3B3A, 5, 'cleanup edge'),
]

def sha(b): return hashlib.sha256(b).hexdigest()
def parse(b):
    pe=struct.unpack_from('<I',b,0x3c)[0]; coff=pe+4; n=struct.unpack_from('<H',b,coff+2)[0]; osz=struct.unpack_from('<H',b,coff+16)[0]; opt=coff+20; tab=opt+osz
    secs=[]
    for i in range(n):
        o=tab+i*40; name=b[o:o+8].split(b'\0',1)[0].decode(); vs,va,rs,rp=struct.unpack_from('<IIII',b,o+8); chars=struct.unpack_from('<I',b,o+36)[0]
        secs.append(dict(name=name,vs=vs,va=va,rs=rs,rp=rp,chars=chars,hoff=o))
    return dict(pe=pe,coff=coff,opt=opt,tab=tab,n=n,osz=osz,secs=secs,size_headers=struct.unpack_from('<I',b,opt+0x3c)[0],size_image=struct.unpack_from('<I',b,opt+0x38)[0])
def roff(pe,r):
    for s in pe['secs']:
        if s['va']<=r<s['va']+max(s['vs'],s['rs']): return s['rp']+(r-s['va'])
    raise KeyError(hex(r))
def rva_for(pe,o):
    for s in pe['secs']:
        if s['rp']<=o<s['rp']+s['rs']: return s['va']+(o-s['rp'])
    return None

def main():
    if len(sys.argv)<4: raise SystemExit('usage: audit_stage3ad.py <base> <out> <report.json> [report.txt]')
    base=Path(sys.argv[1]).read_bytes(); out=Path(sys.argv[2]).read_bytes(); pb=parse(base); po=parse(out)
    if len(base)!=len(out): raise SystemExit('size changed')
    if base[:pb['size_headers']] != out[:po['size_headers']]: raise SystemExit('PE headers changed')
    if pb['n']!=po['n'] or pb['size_image']!=po['size_image']: raise SystemExit('PE geometry changed')
    # Difference runs.
    changed=[i for i,(a,b) in enumerate(zip(base,out)) if a!=b]
    runs=[]
    if changed:
        st=pr=changed[0]
        for x in changed[1:]:
            if x==pr+1: pr=x
            else: runs.append((st,pr-st+1)); st=pr=x
        runs.append((st,pr-st+1))
    mapped=[]
    for o,l in runs:
        r=rva_for(po,o); mapped.append(dict(file_offset=hex(o),rva=(hex(r) if r is not None else None),length=l,base=base[o:o+l].hex(),output=out[o:o+l].hex()))
    # Every changed byte must fit one intended region.
    allowed=[]
    for r,l,label in INTENDED:
        o=roff(po,r); allowed.append((o,o+l,label))
    for o in changed:
        if not any(a<=o<b for a,b,_ in allowed): raise SystemExit(f'change outside intended regions: file 0x{o:X}, rva {rva_for(po,o)}')
    protected={}
    for name,(r,l) in PROTECTED.items():
        ob=roff(pb,r); oo=roff(po,r); same=base[ob:ob+l]==out[oo:oo+l]
        if not same: raise SystemExit('protected site changed: '+name)
        protected[name]=dict(rva=hex(r),length=l,sha256=sha(out[oo:oo+l]))
    # Exact live state prefix protected from 3AA/3AB mistake.
    a=roff(pb,0x2F3000); z=roff(pb,0x2F3200); a2=roff(po,0x2F3000)
    live_same=base[a:z]==out[a2:a2+(z-a)]
    if not live_same: raise SystemExit('.s3qd live-state prefix changed')
    # Section hashes and identity outside intended sections.
    sections={}
    for sb,so in zip(pb['secs'],po['secs']):
        if sb['name']!=so['name'] or (sb['vs'],sb['va'],sb['rs'],sb['rp'],sb['chars'])!=(so['vs'],so['va'],so['rs'],so['rp'],so['chars']): raise SystemExit('section header changed '+sb['name'])
        bb=base[sb['rp']:sb['rp']+sb['rs']]; oo=out[so['rp']:so['rp']+so['rs']]
        sections[sb['name']]=dict(base_sha256=sha(bb),out_sha256=sha(oo),identical=bb==oo,rva=hex(sb['va']),raw_size=hex(sb['rs']),virtual_size=hex(sb['vs']))
    # Call target decode.
    calls={}
    for r in (0x30894,0x2F3B3A):
        o=roff(po,r); x=out[o:o+5]
        if x[0]!=0xE8: raise SystemExit('expected call at '+hex(r))
        tgt=r+5+struct.unpack_from('<i',x,1)[0]
        calls[hex(r)]=dict(bytes=x.hex(),target=hex(tgt))
    report={
        'base_sha256':sha(base),'output_sha256':sha(out),'size':len(out),
        'pe_headers_identical':True,'sections_count':po['n'],'size_of_image':hex(po['size_image']),
        'changed_byte_count':len(changed),'diff_runs':mapped,'protected_sites':protected,
        's3qd_live_state_2f3000_2f31ff_identical':live_same,
        'sections':sections,'calls':calls,
    }
    Path(sys.argv[3]).write_text(json.dumps(report,indent=2)+'\n')
    if len(sys.argv)>4:
        lines=[
            'HaloMCCVR Stage 3AD static audit',
            '=================================',
            f"Base Stage3AC SHA-256: {report['base_sha256']}",
            f"Stage3AD DLL SHA-256:  {report['output_sha256']}",
            f"Size: {len(out)} bytes",
            f"Changed file bytes: {len(changed)}",
            f"PE headers: IDENTICAL; sections={po['n']}; SizeOfImage={hex(po['size_image'])}",
            'Q-R1 loader geometry: PRESERVED',
            '.s3qd RVA 0x2F3000..0x2F31FF live-state prefix: BYTE-IDENTICAL to Stage3AC',
            '.rdata section (imports/strings): '+('BYTE-IDENTICAL' if sections['.rdata']['identical'] else 'CHANGED'),
            '.data section: '+('BYTE-IDENTICAL' if sections['.data']['identical'] else 'CHANGED'),
            '', 'Diff runs:'
        ]
        for d in mapped: lines.append(f"  RVA {d['rva']} length {d['length']} file {d['file_offset']}")
        lines += ['', 'Protected title sites: all BYTE-IDENTICAL to Stage3AC']
        for n,v in protected.items(): lines.append(f"  {n}: {v['rva']} ({v['length']} bytes)")
        lines += ['', 'New call edges:']
        for r,v in calls.items(): lines.append(f"  {r} -> {v['target']} ({v['bytes']})")
        Path(sys.argv[4]).write_text('\n'.join(lines)+'\n')
    print(json.dumps({'output_sha256':report['output_sha256'],'changed':len(changed),'runs':[(x['rva'],x['length']) for x in mapped]},indent=2))

if __name__=='__main__': main()
