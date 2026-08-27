from pathlib import Path
import hashlib, json, struct, sys

EXPECTED_IN='92108c3235e2ef9d3d97ba33b09ae900fa7ce2f46bb67ccf5195d7ef4541b5bd'
EXPECTED_OUT='6adac5256a975a5f829f6a45b3bdac3280cea0bd54b36126a34cc4bab56c1fed'
ALLOWED=[(0x2EF72D,0x2EF73E),(0x2F2007,0x2F201D)]
PROTECTED={
'H4 B-edge pause':(0x14EFA,5),
'H4 real heartbeat':(0x56CA7,12),
'H4 heartbeat gate':(0x8431C,7),
'H4 procedural rollback A':(0x48086,16),
'H4 procedural rollback B':(0x48110,16),
'H4 procedural rollback C':(0x5360B,16),
'H4 procedural rollback D':(0x53D41,16),
'H4 procedural rollback E':(0x53E37,16),
'H2 cleanup pin A':(0x90AC4,8),
'H2 cleanup pin B':(0x90CE2,8),
'H2 stock translation':(0x92F06,8),
'H2 Classic carrier':(0x95B16,8),
'H3/ODST stock translation':(0x40A52,8),
'Reach stock translation':(0x6B321,8),
'Reach HUD A':(0x81036,8),
'Reach HUD B':(0x8112D,8),
'Reach HUD C':(0x813A8,8),
'Stage3AD title edge':(0x30894,5),
'Stage3AD cleanup edge':(0x2F3B3A,5),
'Stage3AD patcher':(0x2F114B,0xA5),
'Stage3AD title wrapper':(0x2F15BB,0x2F),
'Stage3AD cleanup wrapper':(0x2F1BD7,0x25),
'.s3qd live state':(0x2F3000,0x200),
}

def parse(b):
 pe=struct.unpack_from('<I',b,0x3c)[0]; n=struct.unpack_from('<H',b,pe+6)[0]; osz=struct.unpack_from('<H',b,pe+20)[0]; opt=pe+24; tab=opt+osz
 secs=[]
 for i in range(n):
  o=tab+i*40; name=b[o:o+8].split(b'\0',1)[0].decode('ascii','replace'); vs,va,rs,rp=struct.unpack_from('<IIII',b,o+8)
  secs.append(dict(name=name,vs=vs,va=va,rs=rs,rp=rp,h=o))
 return dict(pe=pe,n=n,opt=opt,tab=tab,secs=secs,size_image=struct.unpack_from('<I',b,opt+56)[0],headers=struct.unpack_from('<I',b,opt+60)[0])

def roff(pe,r):
 for s in pe['secs']:
  if s['va']<=r<s['va']+max(s['vs'],s['rs']): return s['rp']+(r-s['va'])
 raise KeyError(hex(r))

def rva_for(pe,o):
 for s in pe['secs']:
  if s['rp']<=o<s['rp']+s['rs']: return s['va']+(o-s['rp'])
 return None

def runs(base,new,pe):
 out=[]; st=None
 for i,(a,b) in enumerate(zip(base,new)):
  if a!=b and st is None: st=i
  if a==b and st is not None:
   out.append((st,i-st,rva_for(pe,st))); st=None
 if st is not None: out.append((st,len(base)-st,rva_for(pe,st)))
 return out

def allowed_rva(r,n):
 if r is None: return False
 return any(a<=r and r+n<=b for a,b in ALLOWED)

def main():
 if len(sys.argv)!=3: raise SystemExit('usage: audit_stage3ae.py <Stage3AD.dll> <Stage3AE.dll>')
 bp,np=map(Path,sys.argv[1:]); b=bp.read_bytes(); n=np.read_bytes()
 hs=lambda x: hashlib.sha256(x).hexdigest()
 assert hs(b)==EXPECTED_IN,(hs(b),'input')
 assert hs(n)==EXPECTED_OUT,(hs(n),'output')
 assert len(b)==len(n)==2890752
 pe=parse(b); pne=parse(n)
 assert pe['n']==pne['n']==12 and pe['size_image']==pne['size_image']==0x2F4000
 assert b[:pe['headers']]==n[:pne['headers']], 'PE headers changed'
 diff=runs(b,n,pe)
 assert diff and all(allowed_rva(r,l) for _,l,r in diff),diff
 # Every protected span must be byte-identical.
 protected=[]
 for name,(r,l) in PROTECTED.items():
  o=roff(pe,r); on=roff(pne,r); assert b[o:o+l]==n[on:on+l],name
  protected.append((name,r,l,hashlib.sha256(b[o:o+l]).hexdigest()))
 # Stage3AE exact wrapper bytes and helper.
 co=roff(pne,0x2EF72D); go=roff(pne,0x2EF736); ho=roff(pne,0x2F2007)
 assert n[co:co+9]==bytes.fromhex('6641837cb61e01750e')
 assert n[go:go+8]==bytes.fromhex('e8cc280000909090')
 helper=n[ho:ho+22]
 assert hashlib.sha256(helper).hexdigest()=='749241b22692633c1029d81dba6f85aec12743011350a978f3f48dea39e64e18'
 report={
  'input_sha256':hs(b),'output_sha256':hs(n),'size':len(n),'sections':pe['n'],'size_of_image':hex(pe['size_image']),
  'diff_runs':[{'file_offset':hex(o),'length':l,'rva':hex(r) if r is not None else None} for o,l,r in diff],
  'helper':{'rva':'0x2F2007','length':22,'sha256':hashlib.sha256(helper).hexdigest()},
  'camera_filter':{'rva':'0x2EF72D','stage3ad':'6641837cb61e02740e','stage3ae':'6641837cb61e01750e'},
  'negative_designator_gate':{'rva':'0x2EF736','stage3ad':'6641837f02007c06','stage3ae':'e8cc280000909090'},
  'protected':[{'name':x[0],'rva':hex(x[1]),'length':x[2],'sha256':x[3]} for x in protected]
 }
 print(json.dumps(report,indent=2))

if __name__=='__main__': main()
