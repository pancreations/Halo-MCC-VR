from pathlib import Path
import hashlib, struct, subprocess, tempfile, sys
EXPECTED='7ceb1b741f94286c9766e777ec253208301fb97c8e3976b61d4fc6b141a8a402'
CODE_RVA=0x2F4000; DATA_RVA=0x2F5000
TITLE_CALL=0x30894; CLEAN_CALL=0x2F3B3A
TITLE_EXPECT=bytes.fromhex('e8131f2c00') # -> 0x2F27AC
CLEAN_EXPECT=bytes.fromhex('e898e0ffff') # -> 0x2F1BD7
DEFS={
'LOGGER_RVA':0x1D90,'TITLE_ACTIVE_RVA':0x879C0,'H4_MODULE_REF_RVA':0x2A7208,
'STAGE3X_HUD_INSTALL_RVA':0x2F323B,'STAGE3X_CURV_INSTALL_RVA':0x2F34E4,
'IAT_VIRTUAL_PROTECT_RVA':0x180200,'STAGE3X_RESTORE_PROTECT_FLUSH_RVA':0x2F3200,
}
def align(x,a): return (x+a-1)&~(a-1)
def parse(b):
 p=struct.unpack_from('<I',b,0x3c)[0]; assert b[p:p+4]==b'PE\0\0'; coff=p+4; n=struct.unpack_from('<H',b,coff+2)[0]; os=struct.unpack_from('<H',b,coff+16)[0]; opt=coff+20; sh=opt+os; ss=[]
 for i in range(n):
  o=sh+i*40; nm=b[o:o+8].rstrip(b'\0').decode();vs,va,rs,rp=struct.unpack_from('<IIII',b,o+8); ch=struct.unpack_from('<I',b,o+36)[0]; ss.append(dict(name=nm,vs=vs,va=va,rs=rs,rp=rp,h=o,ch=ch))
 return dict(coff=coff,opt=opt,sh=sh,n=n,ss=ss,fa=struct.unpack_from('<I',b,opt+0x24)[0],sa=struct.unpack_from('<I',b,opt+0x20)[0],headers=struct.unpack_from('<I',b,opt+0x3c)[0],sizeimg=struct.unpack_from('<I',b,opt+0x38)[0])
def roff(pe,r):
 for s in pe['ss']:
  if s['va']<=r<s['va']+max(s['vs'],s['rs']): return s['rp']+(r-s['va'])
 raise RuntimeError(hex(r))
def rel(src,dst): return b'\xe8'+struct.pack('<i',dst-(src+5))
def assemble_main(asm):
 with tempfile.TemporaryDirectory() as td:
  td=Path(td); o=td/'x.o'; e=td/'x.elf'; tc=td/'t.bin'; dc=td/'d.bin'
  subprocess.run(['as','--64','-o',str(o),str(asm)],check=True)
  cmd=['ld','-m','elf_x86_64',f'--section-start=.text=0x{CODE_RVA:x}',f'--section-start=.data=0x{DATA_RVA:x}']
  for k,v in DEFS.items(): cmd+=['--defsym',f'{k}=0x{v:x}']
  cmd+=['-o',str(e),str(o)]; subprocess.run(cmd,check=True)
  subprocess.run(['objcopy','-O','binary','-j','.text',str(e),str(tc)],check=True); subprocess.run(['objcopy','-O','binary','-j','.data',str(e),str(dc)],check=True)
  sy={}; out=subprocess.check_output(['nm','-n',str(e)],text=True)
  for line in out.splitlines():
   f=line.split()
   if len(f)==3 and f[2].startswith('stage3ai_'): sy[f[2]]=int(f[0],16)
  return tc.read_bytes(),dc.read_bytes(),sy
def main():
 src=Path(sys.argv[1]); out=Path(sys.argv[2]); asm=Path(sys.argv[3]); ib=src.read_bytes(); sha=hashlib.sha256(ib).hexdigest();
 if sha!=EXPECTED: raise SystemExit('wrong input '+sha)
 code,data,sy=assemble_main(asm)
 if len(code)>0x1000 or len(data)>0x1000: raise SystemExit('helper too large')
 b=bytearray(ib); pe=parse(b)
 if pe['n']!=12 or pe['sizeimg']!=0x2f4000: raise SystemExit(f'geometry {pe["n"]} {pe["sizeimg"]:#x}')
 # guard only the three exact current control edges; Stage3AH code is left byte-for-byte inert.
 for r,exp,name in [(TITLE_CALL,TITLE_EXPECT,'title'),(CLEAN_CALL,CLEAN_EXPECT,'cleanup')]:
  o=roff(pe,r); act=bytes(b[o:o+len(exp)])
  if act!=exp: raise SystemExit(f'{name} guard {act.hex()}')
 # Extend the existing final RWX .s3qd Stage3 post-link section in place.
 # There is deliberately no new section header: Stage3AH consumed the final
 # header slot. Extending only the last section changes no existing RVA/raw
 # mapping and leaves every pre-Stage3AI byte at the same address.
 qd=next((s for s in pe['ss'] if s['name']=='.s3qd'),None)
 if not qd or qd['va']!=0x2F3000 or qd['vs']!=0x1000 or qd['rs']!=0x1000:
  raise SystemExit('unexpected .s3qd geometry')
 if qd['rp']+qd['rs']!=len(b): raise SystemExit('unexpected overlay after .s3qd')
 if len(code)>0x1000 or len(data)>0x1000: raise SystemExit('Stage3AI payload too large')
 b.extend(code.ljust(0x1000,b'\0'))
 b.extend(data.ljust(0x1000,b'\0'))
 struct.pack_into('<I',b,qd['h']+8,0x3000)   # VirtualSize
 struct.pack_into('<I',b,qd['h']+16,0x3000)  # SizeOfRawData
 struct.pack_into('<I',b,pe['opt']+0x38,0x2F6000)
 # .s3qd is PE initialized data (and RWX); account for the two appended pages.
 struct.pack_into('<I',b,pe['opt']+8,struct.unpack_from('<I',b,pe['opt']+8)[0]+0x2000)
 # extend pe mapper locally for patches? old RVAs only, so offsets unchanged
 # patch control edges
 b[roff(pe,TITLE_CALL):roff(pe,TITLE_CALL)+5]=rel(TITLE_CALL,sy['stage3ai_title_wrapper'])
 b[roff(pe,CLEAN_CALL):roff(pe,CLEAN_CALL)+5]=rel(CLEAN_CALL,sy['stage3ai_effect_restore'])
 struct.pack_into('<I',b,pe['opt']+0x40,0) # PE checksum
 out.write_bytes(b)
 print('input',sha);print('code',hashlib.sha256(code).hexdigest(),len(code));print('data',hashlib.sha256(data).hexdigest(),len(data));print('symbols', {k:hex(v) for k,v in sy.items()});print('output',hashlib.sha256(b).hexdigest(),len(b))
if __name__=='__main__':main()
