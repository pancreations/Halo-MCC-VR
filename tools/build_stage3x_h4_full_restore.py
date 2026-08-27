from pathlib import Path
import hashlib, shutil, struct, subprocess, sys, tempfile, json

EXPECTED='4200862ac38918d5c7c88c24e31e2cf0873e7c93313b8b79438236cd17db885e'
NEW_RVA=0x2F3200
IMAGE_BASE=0x180000000
DEFS={
'ORIGINAL_LOG_RVA':0x1D90,'TITLE_ACTIVE_RVA':0x879C0,'PAUSE_TARGET_RVA':0x2EEB0,'REQUEST_PAUSE_RVA':0x30480,
'H4_EFFECT_INSTALLER_RVA':0x2EF595,'H4_EFFECT_WRAPPER_RVA':0x2EF6ED,'H4_MODULE_REF_RVA':0x2A7208,
'H4_INSTALLED_RVA':0x2A71E8,'H4_INSTALLED_AT_RVA':0x2A74A0,'H4_CLEANUP_SCRATCH_RVA':0x2F31F0,
'EFFECT_INSTALL_LOCK_RVA':0x2F0008,'EFFECT_MODULE_BASE_RVA':0x2F0010,
'CONFIG_HUD_SIZE_RVA':0x2ADC28,'CONFIG_HUD_ASPECT_RVA':0x2ADC2C,'CONFIG_HUD_CURVATURE_RVA':0x2ADC30,'CONFIG_HUD_VERTICAL_RVA':0x2ADC34,
'STAGE3X_STATE_RVA':0x2F31C0,'PAUSE_GRACE_RVA':0x2F31C8,'HUD_CONTINUE_RVA':0x2F31D0,'CURV_CONTINUE_RVA':0x2F31D8,'HEARTBEAT_RVA':0x2F31E0,
'IAT_GET_TICK_COUNT64_RVA':0x180150,'IAT_VIRTUAL_PROTECT_RVA':0x180200,'IAT_GET_CURRENT_PROCESS_RVA':0x180140,'IAT_FLUSH_INSTRUCTION_CACHE_RVA':0x1801E8,'IAT_FREE_LIBRARY_RVA':0x180230,
'H4_SETUP_HEARTBEAT_CONTINUE_RVA':0x56CB3,'H4_POLL_CONTINUE_RVA':0,'H4_POLL_REMOVE_RVA':0,'TLS_INDEX_RVA':0x2D5E48,
}
PATCHES={
 'title_query':(0x30894,bytes.fromhex('e88b082c00')),
 'pause_b_log':(0x14EFA,bytes.fromhex('e891cefeff')),
 'setup_heartbeat':(0x56CA7,bytes.fromhex('41c6030149895b0841895310')),
 'poll_installed':(0x8431C,bytes.fromhex('0fb61dc52e2200')),
 'cleanup_call':(0x7CB6F,bytes.fromhex('e885452700')),
 'effect_loc':(0x2EF71C,bytes.fromhex('41837cb614007520')),
 'effect_camera':(0x2EF72D,bytes.fromhex('6641837cb61e01750e')),
 'effect_action':(0x2EF73E,bytes.fromhex('e8001a000090')),
}

def align(v,a): return (v+a-1)//a*a

def parse(b):
 pe=struct.unpack_from('<I',b,0x3c)[0]
 if b[:2]!=b'MZ' or b[pe:pe+4]!=b'PE\0\0': raise SystemExit('bad PE')
 coff=pe+4; n=struct.unpack_from('<H',b,coff+2)[0]; osz=struct.unpack_from('<H',b,coff+16)[0]; opt=coff+20; tab=opt+osz
 if struct.unpack_from('<H',b,opt)[0]!=0x20b or struct.unpack_from('<Q',b,opt+24)[0]!=IMAGE_BASE: raise SystemExit('unexpected PE32+')
 secs=[]
 for i in range(n):
  o=tab+i*40; name=bytes(b[o:o+8]).split(b'\0',1)[0].decode('ascii'); vs,va,rs,rp=struct.unpack_from('<IIII',b,o+8)
  secs.append(dict(name=name,vs=vs,va=va,rs=rs,rp=rp,h=o,chars=struct.unpack_from('<I',b,o+36)[0]))
 return dict(pe=pe,coff=coff,opt=opt,tab=tab,n=n,osz=osz,secs=secs,fa=struct.unpack_from('<I',b,opt+0x24)[0],sa=struct.unpack_from('<I',b,opt+0x20)[0],headers=struct.unpack_from('<I',b,opt+0x3c)[0])

def roff(pe,r):
 for s in pe['secs']:
  if s['va']<=r<s['va']+max(s['vs'],s['rs']): return s['rp']+(r-s['va'])
 raise KeyError(hex(r))

def write(b,pe,r,old,new,label):
 if len(old)!=len(new): raise SystemExit(label+' length mismatch')
 o=roff(pe,r); actual=bytes(b[o:o+len(old)])
 if actual!=old: raise SystemExit(f'{label} guard fail RVA 0x{r:X}: {actual.hex()} != {old.hex()}')
 b[o:o+len(new)]=new; print(f'{label}: 0x{r:X} {old.hex()} -> {new.hex()}')

def relop(op,src,dst): return bytes([op])+struct.pack('<i',dst-(src+5))

def assemble(tooldir):
 for x in ('as','ld','objcopy','nm'):
  if not shutil.which(x): raise SystemExit('missing '+x)
 with tempfile.TemporaryDirectory(prefix='s3x-') as td:
  td=Path(td); obj=td/'x.o'; elf=td/'x.elf'; raw=td/'x.bin'
  subprocess.run(['as','--64','-o',str(obj),str(tooldir/'stage3x_h4_full_restore.S')],check=True)
  cmd=['ld','-m','elf_x86_64',f'-Ttext=0x{NEW_RVA:x}','-e','stage3x_h4_title_install_wrapper']
  for k,v in DEFS.items(): cmd += ['--defsym',f'{k}=0x{v:x}']
  cmd += ['-o',str(elf),str(obj)]
  subprocess.run(cmd,check=True)
  subprocess.run(['objcopy','-O','binary','-j','.text',str(elf),str(raw)],check=True)
  syms={}
  out=subprocess.check_output(['nm','-n',str(elf)],text=True)
  for line in out.splitlines():
   f=line.split()
   if len(f)==3 and f[2].startswith('stage3x_'): syms[f[2]]=int(f[0],16)
  return raw.read_bytes(),syms,out

def main():
 if len(sys.argv)!=3: raise SystemExit('usage: build_stage3x_h4_full_restore.py <Stage3V.dll> <out.dll>')
 src,out=Path(sys.argv[1]),Path(sys.argv[2]); ib=src.read_bytes(); sha=hashlib.sha256(ib).hexdigest()
 if sha!=EXPECTED: raise SystemExit('wrong Stage3V '+sha)
 helper,syms,nm=assemble(Path(__file__).resolve().parent)
 required_syms={'stage3x_h4_title_install_wrapper','stage3x_h4_pause_b_edge_wrapper','stage3x_h4_setup_heartbeat_bridge','stage3x_h4_poll_heartbeat_gate','stage3x_h4_cleanup_restore_release','stage3x_h4_hide_local_fp_effect'}
 if not required_syms<=set(syms): raise SystemExit('missing helper symbols '+repr(required_syms-set(syms)))
 b=bytearray(ib); pe=parse(b)
 if pe['n']!=12 or pe['secs'][-1]['name']!='.s3qd' or pe['secs'][-1]['va']!=0x2F3000: raise SystemExit('Stage3V section baseline mismatch')
 # Scratch slots must be zero in exact base.
 for r,n in [(0x2F31C0,4),(0x2F31C8,8),(0x2F31D0,8),(0x2F31D8,8),(0x2F31E0,8)]:
  if bytes(b[roff(pe,r):roff(pe,r)+n])!=b'\0'*n: raise SystemExit(f'scratch not zero 0x{r:X}')
 # Extend the existing adjacent .s3qd section in-place. Q-R1 already made
 # .s3qd the final loader-safe 0x2F3000 section; there is no header room for a
 # 13th section. Keep the exact section count/SizeOfImage and use the previously
 # zero 0x2F3200..0x2F3FFF tail. The first 0x200 bytes (including Stage3V's
 # retained H4 HMODULE scratch at 0x2F31F0) are preserved byte-for-byte.
 qd=pe['secs'][-1]
 if qd['rs']!=0x200 or qd['vs']!=0x200 or qd['rp']+qd['rs']!=len(b):
  raise SystemExit('unexpected .s3qd raw geometry')
 if NEW_RVA!=qd['va']+0x200: raise SystemExit('helper must start immediately after preserved qd data')
 needed=0x200+len(helper)
 new_rs=align(needed,pe['fa'])
 if new_rs>0x1000: raise SystemExit(f'helper does not fit qd page: {len(helper)}')
 b.extend(b'\0'*(new_rs-qd['rs']))
 helper_off=qd['rp']+0x200
 b[helper_off:helper_off+len(helper)]=helper
 struct.pack_into('<I',b,qd['h']+8,new_rs)      # VirtualSize covers data+code tail
 struct.pack_into('<I',b,qd['h']+16,new_rs)     # SizeOfRawData
 struct.pack_into('<I',b,qd['h']+36,0xE0000040) # initialized data + R/W/X
 # SizeOfImage deliberately remains 0x2F4000, identical to loader-proven Q-R1/3V.
 if struct.unpack_from('<I',b,pe['opt']+0x38)[0]!=0x2F4000:
  raise SystemExit('unexpected Stage3V SizeOfImage')
 # patches
 write(b,pe,*PATCHES['title_query'],relop(0xE8,0x30894,syms['stage3x_h4_title_install_wrapper']),'H4 title/install wrapper')
 write(b,pe,*PATCHES['pause_b_log'],relop(0xE8,0x14EFA,syms['stage3x_h4_pause_b_edge_wrapper']),'H4 B-edge pause restore')
 write(b,pe,*PATCHES['setup_heartbeat'],relop(0xE9,0x56CA7,syms['stage3x_h4_setup_heartbeat_bridge'])+b'\x90'*7,'H4 real camera heartbeat')
 write(b,pe,*PATCHES['poll_installed'],relop(0xE8,0x8431C,syms['stage3x_h4_poll_heartbeat_gate'])+b'\x90\x90','H4 heartbeat teardown gate')
 write(b,pe,*PATCHES['cleanup_call'],relop(0xE8,0x7CB6F,syms['stage3x_h4_cleanup_restore_release']),'H4 full direct-patch teardown')
 write(b,pe,*PATCHES['effect_loc'],b'\x90'*8,'H4 local-FP effects all authored locations')
 write(b,pe,*PATCHES['effect_camera'],bytes.fromhex('6641837cb61e02740e'),'H4 effects reject 3P-only')
 write(b,pe,*PATCHES['effect_action'],relop(0xE8,0x2EF73E,syms['stage3x_h4_hide_local_fp_effect'])+b'\x90','H4 effects finite XYZ hide')
 # checksum clear
 struct.pack_into('<I',b,pe['opt']+0x40,0)
 out.parent.mkdir(parents=True,exist_ok=True); out.write_bytes(b)
 print('input',sha); print('helper',hashlib.sha256(helper).hexdigest(),len(helper)); print('output',hashlib.sha256(b).hexdigest(),len(b)); print('section .s3qd expanded raw',hex(new_rs),'helper_rva',hex(NEW_RVA)); print('symbols',json.dumps({k:hex(v) for k,v in syms.items() if k in required_syms},sort_keys=True))
if __name__=='__main__': main()
