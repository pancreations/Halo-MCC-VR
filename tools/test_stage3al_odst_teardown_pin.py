from pathlib import Path
import hashlib, struct, sys
STAGE3AK_SHA="4ce06fc1e33040990b4db630748d07688803a7216c74cd9d89484c9403510389"
STORE_RVA=0x5C2A6; CALLS=(0x41BD6,0x84E45); ACQUIRE_RVA=0x2F8000

def pe(blob):
 p=struct.unpack_from('<I',blob,0x3c)[0]; coff=p+4; n=struct.unpack_from('<H',blob,coff+2)[0]; os=struct.unpack_from('<H',blob,coff+16)[0]; opt=coff+20; st=opt+os; secs=[]
 for i in range(n):
  o=st+i*40; name=bytes(blob[o:o+8]).split(b'\0',1)[0].decode(); vs,va,rs,rp=struct.unpack_from('<IIII',blob,o+8); secs.append((name,vs,va,rs,rp,o))
 return opt,secs

def off(secs,rva):
 for _,vs,va,rs,rp,_ in secs:
  if va<=rva<va+max(vs,rs): return rp+rva-va
 raise KeyError(hex(rva))
def target(blob,secs,rva):
 o=off(secs,rva); assert blob[o]==0xE8; return rva+5+struct.unpack_from('<i',blob,o+1)[0]
def main():
 if len(sys.argv)!=3: raise SystemExit('usage: test <Stage3AK.dll> <Stage3AL.dll>')
 a=Path(sys.argv[1]).read_bytes(); b=Path(sys.argv[2]).read_bytes(); assert hashlib.sha256(a).hexdigest()==STAGE3AK_SHA
 oa,sa=pe(a); ob,sb=pe(b)
 assert target(b,sb,STORE_RVA)==ACQUIRE_RVA
 # Find release symbol by decoding the first cleanup call and require both equal.
 release=target(b,sb,CALLS[0]); assert release>ACQUIRE_RVA and release<0x2F9000; assert target(b,sb,CALLS[1])==release
 qa=next(x for x in sa if x[0]=='.s3qd'); qb=next(x for x in sb if x[0]=='.s3qd')
 assert qa[1]==0x5000 and qa[2]==0x2F3000 and qa[3]==0x5000
 assert qb[1]==0x6000 and qb[2]==0x2F3000 and qb[3]==0x6000
 assert a[qa[4]:qa[4]+0x5000] == b[qb[4]:qb[4]+0x5000]
 assert struct.unpack_from('<I',b,ob+0x38)[0]==0x2F9000
 # Earlier identity-only probe must stay exactly flags=6; pin acquisition is post-proof.
 assert b[off(sb,0x5C1E9):off(sb,0x5C1E9)+5]==bytes.fromhex('B9 06 00 00 00')
 print('PASS Stage3AL post-preflight ODST pin + exact release + accepted payload preservation')
if __name__=='__main__': main()
