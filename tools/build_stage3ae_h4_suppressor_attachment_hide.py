from pathlib import Path
import hashlib, shutil, struct, subprocess, sys, tempfile

EXPECTED_STAGE3AD_SHA256 = "92108c3235e2ef9d3d97ba33b09ae900fa7ce2f46bb67ccf5195d7ef4541b5bd"
NEW_RVA = 0x002F2007
NEW_LIMIT = 0x19
EFFECT_CAMERA_RVA = 0x002EF72D
EFFECT_NEGATIVE_GATE_RVA = 0x002EF736
EFFECT_WRAPPER_CONTINUE_RVA = 0x002EF744

CAMERA_STAGE3AD = bytes.fromhex("6641837cb61e02740e")
CAMERA_STAGE3AE = bytes.fromhex("6641837cb61e01750e")
NEGATIVE_GATE_STAGE3AD = bytes.fromhex("6641837f02007c06")


def parse_pe(b):
    pe = struct.unpack_from('<I', b, 0x3C)[0]
    if b[:2] != b'MZ' or b[pe:pe+4] != b'PE\0\0':
        raise SystemExit('not PE')
    n = struct.unpack_from('<H', b, pe+6)[0]
    optsz = struct.unpack_from('<H', b, pe+20)[0]
    opt = pe+24
    sh = opt+optsz
    secs=[]
    for i in range(n):
        o=sh+i*40
        name=b[o:o+8].rstrip(b'\0').decode('ascii','replace')
        vs,va,rs,rp=struct.unpack_from('<IIII',b,o+8)
        chars=struct.unpack_from('<I',b,o+36)[0]
        secs.append(dict(name=name,vs=vs,va=va,rs=rs,rp=rp,chars=chars,hoff=o))
    return dict(pe=pe,n=n,opt=opt,secs=secs,size_image=struct.unpack_from('<I',b,opt+56)[0])


def roff(pe,rva):
    for s in pe['secs']:
        if s['va'] <= rva < s['va'] + max(s['vs'],s['rs']):
            return s['rp'] + (rva-s['va'])
    raise SystemExit(f'RVA not mapped: 0x{rva:X}')


def relcall(src,dst):
    return b'\xE8' + struct.pack('<i', dst-(src+5))


def assemble(tooldir):
    for x in ('as','ld','objcopy','nm'):
        if not shutil.which(x):
            raise SystemExit('missing '+x)
    with tempfile.TemporaryDirectory(prefix='s3ae-') as td:
        td=Path(td); obj=td/'ae.o'; elf=td/'ae.elf'; raw=td/'ae.bin'
        subprocess.run(['as','--64','-o',str(obj),str(tooldir/'stage3ae_h4_suppressor_attachment_gate.S')],check=True)
        subprocess.run([
            'ld','-m','elf_x86_64',f'-Ttext=0x{NEW_RVA:x}',
            '-e','stage3ae_h4_suppressor_attachment_gate',
            '-o',str(elf),str(obj)
        ],check=True)
        subprocess.run(['objcopy','-O','binary','-j','.text',str(elf),str(raw)],check=True)
        syms={}
        for line in subprocess.check_output(['nm','-n',str(elf)],text=True).splitlines():
            f=line.split()
            if len(f)==3 and f[2].startswith('stage3ae_'):
                syms[f[2]]=int(f[0],16)
        return raw.read_bytes(),syms


def main():
    if len(sys.argv)!=3:
        raise SystemExit('usage: build_stage3ae_h4_suppressor_attachment_hide.py <Stage3AD.dll> <out.dll>')
    src,out=Path(sys.argv[1]),Path(sys.argv[2])
    ib=src.read_bytes(); sha=hashlib.sha256(ib).hexdigest()
    if sha != EXPECTED_STAGE3AD_SHA256:
        raise SystemExit('wrong Stage3AD input '+sha)
    helper,syms=assemble(Path(__file__).resolve().parent)
    gate=syms.get('stage3ae_h4_suppressor_attachment_gate')
    if gate != NEW_RVA:
        raise SystemExit(f'helper symbol mismatch: {gate!r}')
    if len(helper)>NEW_LIMIT:
        raise SystemExit(f'helper too large: {len(helper)} > {NEW_LIMIT}')

    b=bytearray(ib); pe=parse_pe(b)
    if pe['n']!=12 or pe['size_image']!=0x2F4000:
        raise SystemExit('Stage3AD loader geometry mismatch')
    s3ic=next((s for s in pe['secs'] if s['name']=='.s3ic'),None)
    if not s3ic or not (s3ic['va'] <= NEW_RVA < s3ic['va']+max(s3ic['vs'],s3ic['rs'])):
        raise SystemExit('helper is not inside .s3ic')
    ho=roff(pe,NEW_RVA)
    if any(b[ho:ho+NEW_LIMIT]):
        raise SystemExit('Stage3AD .s3ic helper cave guard failed')

    co=roff(pe,EFFECT_CAMERA_RVA)
    go=roff(pe,EFFECT_NEGATIVE_GATE_RVA)
    if bytes(b[co:co+len(CAMERA_STAGE3AD)]) != CAMERA_STAGE3AD:
        raise SystemExit('Stage3AD camera filter guard failed')
    if bytes(b[go:go+len(NEGATIVE_GATE_STAGE3AD)]) != NEGATIVE_GATE_STAGE3AD:
        raise SystemExit('Stage3AD designator gate guard failed')

    b[ho:ho+len(helper)] = helper
    b[co:co+len(CAMERA_STAGE3AE)] = CAMERA_STAGE3AE
    b[go:go+8] = relcall(EFFECT_NEGATIVE_GATE_RVA,NEW_RVA) + b'\x90\x90\x90'

    # Match the existing post-link convention: checksum cleared, no PE layout edits.
    struct.pack_into('<I', b, pe['opt']+0x40, 0)
    out.parent.mkdir(parents=True,exist_ok=True)
    out.write_bytes(b)
    print('input_sha256',sha)
    print('helper_sha256',hashlib.sha256(helper).hexdigest())
    print('helper_rva',hex(NEW_RVA),'bytes',len(helper))
    print('camera',CAMERA_STAGE3AD.hex(),'->',CAMERA_STAGE3AE.hex())
    print('negative_gate',NEGATIVE_GATE_STAGE3AD.hex(),'->',bytes(b[go:go+8]).hex())
    print('output_sha256',hashlib.sha256(b).hexdigest())
    print('output_size',len(b))

if __name__=='__main__': main()
