from pathlib import Path
import hashlib, struct, sys

BASE_SHA='b5eeec1276e91197bade35b926d9f176437e0096a40b29a137338e4f2e22abe2'
CAND_SHA='7c788d598dc2685178b5b3173476e4cf24c22a814acea720cf4f0eba8ad329f4'
PATCHES=[(0x87FF1,21),(0xDF60,15),(0xE060,15)]
PRESERVE=[
(0x90AC4,5),(0x90CE2,7),(0x92F06,5),(0x95B16,5),(0x40A52,5),(0x6B321,5),(0x51289,5),
(0x48086,8),(0x48110,8),(0x5360B,8),(0x53D41,8),(0x53E37,8),
(0x7CDA3,7),(0x7CDAB,7),(0x7CDB3,7),(0x7D167,7),(0x7D17A,7),(0x7D19E,7),
(0x81036,40),(0x8112D,25),(0x813A8,10),(0x47396,12),(0x4414D,5),(0x44203,8),(0x34587,7),
]

def parse(b):
    pe=struct.unpack_from('<I',b,0x3c)[0]; coff=pe+4
    n=struct.unpack_from('<H',b,coff+2)[0]; os=struct.unpack_from('<H',b,coff+16)[0]
    opt=coff+20; tab=opt+os; ss=[]
    for i in range(n):
        h=tab+i*40; name=b[h:h+8].split(b'\0')[0].decode()
        vs,va,rs,rp=struct.unpack_from('<IIII',b,h+8); ch=struct.unpack_from('<I',b,h+36)[0]
        ss.append(dict(name=name,vs=vs,va=va,rs=rs,rp=rp,ch=ch,h=h))
    return dict(pe=pe,coff=coff,opt=opt,tab=tab,n=n,ss=ss,sect_align=struct.unpack_from('<I',b,opt+0x20)[0],file_align=struct.unpack_from('<I',b,opt+0x24)[0])

def off(pe,r):
    for s in pe['ss']:
        if s['va']<=r<s['va']+max(s['vs'],s['rs']): return s['rp']+r-s['va']
    raise AssertionError(hex(r))

def ranges_contains(ranges, i):
    return any(a<=i<b for a,b in ranges)

def main():
    if len(sys.argv)!=3: raise SystemExit('usage: audit_stage3q_cross_title_reentry.py <Stage3P.dll> <Stage3Q.dll>')
    base=Path(sys.argv[1]).read_bytes(); cand=Path(sys.argv[2]).read_bytes()
    assert hashlib.sha256(base).hexdigest()==BASE_SHA
    assert hashlib.sha256(cand).hexdigest()==CAND_SHA
    bp=parse(base); cp=parse(cand)
    assert bp['n']==11 and cp['n']==12
    bs={x['name']:x for x in bp['ss']}; cs={x['name']:x for x in cp['ss']}
    assert (bs['.s3ic']['va'],bs['.s3ic']['vs'],bs['.s3ic']['rs'],bs['.s3ic']['ch'])==(0x2F1000,0x11FE,0x1200,0x60000020)
    assert (cs['.s3ic']['va'],cs['.s3ic']['vs'],cs['.s3ic']['rs'],cs['.s3ic']['ch'])==(0x2F1000,0x16F0,0x1800,0x60000020)
    assert (cs['.s3qd']['va'],cs['.s3qd']['vs'],cs['.s3qd']['rs'],cs['.s3qd']['ch'])==(0x2F3000,0x200,0x200,0xC0000040)
    assert cs['.s3qd']['rp']==bs['.s3ic']['rp']+0x1800
    assert cand[cs['.s3qd']['rp']:cs['.s3qd']['rp']+0x200]==b'\0'*0x200
    assert struct.unpack_from('<I',cand,cp['opt']+0x38)[0]==0x2F4000
    assert struct.unpack_from('<I',cand,cp['opt']+0x40)[0]==0

    # Windows image loader expects image sections to advance contiguously at
    # SectionAlignment boundaries. The original Stage3Q package left a 0x1000
    # hole before .s3qd and was rejected with STATUS_INVALID_IMAGE_FORMAT.
    def align(v,a): return (v+a-1)//a*a
    for prev,cur in zip(cp['ss'],cp['ss'][1:]):
        expected=align(prev['va']+max(prev['vs'],prev['rs']),cp['sect_align'])
        assert cur['va']==expected, (prev['name'],cur['name'],hex(expected),hex(cur['va']))

    # Existing accepted title-specific bytes must be exactly Stage3P.
    for r,n in PRESERVE:
        assert base[off(bp,r):off(bp,r)+n]==cand[off(cp,r):off(cp,r)+n], hex(r)

    # Verify patch opcodes and destinations.
    expected_targets={0x87FF1:0x2F2200,0xDF60:0x2F22DE,0xE060:0x2F22EF}
    for r,n in PATCHES:
        chunk=cand[off(cp,r):off(cp,r)+n]
        assert chunk[0]==0xE8 and chunk[5:]==b'\x90'*(n-5), hex(r)
        disp=struct.unpack_from('<i',chunk,1)[0]
        assert r+5+disp==expected_targets[r], (hex(r),hex(r+5+disp))

    # Existing-file differences are restricted to the three guarded code
    # patches plus PE bookkeeping needed to grow .s3ic and add .s3qd.
    allowed=[]
    for r,n in PATCHES:
        o=off(bp,r); allowed.append((o,o+n))
    allowed += [
        (bp['coff']+2,bp['coff']+4),      # NumberOfSections
        (bp['opt']+4,bp['opt']+12),       # SizeOfCode + SizeOfInitializedData
        (bp['opt']+0x38,bp['opt']+0x3c),  # SizeOfImage
        (bp['opt']+0x40,bp['opt']+0x44),  # CheckSum
        (bs['.s3ic']['h']+8,bs['.s3ic']['h']+12),
        (bs['.s3ic']['h']+16,bs['.s3ic']['h']+20),
        (bp['tab']+bp['n']*40,bp['tab']+(bp['n']+1)*40),
    ]
    unexpected=[]
    for i,(a,b) in enumerate(zip(base,cand[:len(base)])):
        if a!=b and not ranges_contains(allowed,i): unexpected.append(i)
    assert not unexpected, 'unexpected pre-existing byte changes: '+','.join(hex(x) for x in unexpected[:20])

    # New helper begins exactly at old EOF; no old helper byte was overwritten.
    assert off(cp,0x2F2200)==len(base)
    assert len(cand)==len(base)+0x800  # +0x600 .s3ic growth +0x200 .s3qd
    print('Stage3Q static audit PASS')
    print('base',hashlib.sha256(base).hexdigest(),len(base))
    print('candidate',hashlib.sha256(cand).hexdigest(),len(cand))
    print('only existing code patches:', ', '.join(hex(r) for r,_ in PATCHES))
    print('all Stage3P protected title-specific sites are byte-identical')

if __name__=='__main__': main()
