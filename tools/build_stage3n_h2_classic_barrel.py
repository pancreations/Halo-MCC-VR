from pathlib import Path
import hashlib, math, shutil, struct, subprocess, sys, tempfile

BASE_SHA = 'e56e29524a2c4e778536a437b7c00b9505ea58e3e468eb148c24f60598c9fbf9'
CODE = 0x2F1600
YAW = 0x2F00F0
PITCH = 0x2F00F4
MN = 0x2F00F8
MX = 0x2F00FC
RAD = 0x2F0100
ONE3 = 0x2F0104
TWO15 = 0x2F0108
KY = 0x2F0110
KP = 0x2F0130
FY = 0x2F0150
FP = 0x2F0178
LY = 0x2F01A0
LP = 0x2F01C0
DATA_END = 0x1E0

STAGE3M_CONFIG = 0x2F1200
STAGE3M_SAVE = 0x2F1284
STAGE3M_MENU = 0x2F12D9
H2_BUILD_CARRIERS = 0x92DD0


def align(v, a): return (v + a - 1) // a * a

def run(c): subprocess.run(c, check=True)

def peparse(b):
    p = struct.unpack_from('<I', b, 0x3c)[0]
    c = p + 4
    n = struct.unpack_from('<H', b, c + 2)[0]
    osz = struct.unpack_from('<H', b, c + 16)[0]
    opt = c + 20
    table = opt + osz
    sections = []
    for i in range(n):
        h = table + i * 40
        name = bytes(b[h:h+8]).split(b'\0',1)[0].decode('ascii')
        vs, va, rs, rp = struct.unpack_from('<IIII', b, h + 8)
        sections.append(dict(name=name, vs=vs, va=va, rs=rs, rp=rp, h=h))
    return dict(opt=opt, sections=sections,
                fa=struct.unpack_from('<I', b, opt+0x24)[0],
                sa=struct.unpack_from('<I', b, opt+0x20)[0])

def rva_off(pe, rva):
    for s in pe['sections']:
        if s['va'] <= rva < s['va'] + max(s['vs'], s['rs']):
            return s['rp'] + rva - s['va']
    raise SystemExit(f'RVA not mapped: 0x{rva:X}')

def req(b, pe, rva, expected, label):
    expected = bytes.fromhex(expected) if isinstance(expected, str) else expected
    o = rva_off(pe, rva)
    actual = bytes(b[o:o+len(expected)])
    if actual != expected:
        raise SystemExit(f'{label} guard 0x{rva:X}: {actual.hex()} != {expected.hex()}')

def patch(b, pe, rva, expected, replacement, label):
    expected = bytes.fromhex(expected) if isinstance(expected, str) else expected
    if len(expected) != len(replacement):
        raise SystemExit(f'{label}: size mismatch')
    req(b, pe, rva, expected, label)
    o = rva_off(pe, rva)
    b[o:o+len(expected)] = replacement
    print(f'{label}: RVA 0x{rva:X}')

def rel(op, src, dst):
    return bytes([op]) + struct.pack('<i', dst - src - 5)

def assemble(td):
    for x in ('as','ld','objcopy','nm'):
        if not shutil.which(x): raise SystemExit('missing ' + x)
    src = Path(__file__).with_name('stage3n_h2_classic_barrel.S')
    obj, elf, raw = td/'n.o', td/'n.elf', td/'n.bin'
    run(['as','--64','-o',str(obj),str(src)])
    defs = {
        'STRCMP_RVA':0x177330,
        'ATOF_RVA':0x1537F8,
        'FPRINTF_RVA':0x21A0,
        'SLIDERFLOAT_RVA':0x131E20,
        'STAGE3M_CONFIG_HOOK_RVA':STAGE3M_CONFIG,
        'STAGE3M_SAVE_HOOK_RVA':STAGE3M_SAVE,
        'STAGE3M_MENU_HOOK_RVA':STAGE3M_MENU,
        'H2_BUILD_STABLE_CARRIERS_RVA':H2_BUILD_CARRIERS,
        'CLASSIC_YAW_RVA':YAW,
        'CLASSIC_PITCH_RVA':PITCH,
        'MINUS_30_RVA':MN,
        'PLUS_30_RVA':MX,
        'RAD_PER_DEG_RVA':RAD,
        'ONE_THIRD_RVA':ONE3,
        'TWO_FIFTEENTHS_RVA':TWO15,
        'KEY_YAW_RVA':KY,
        'KEY_PITCH_RVA':KP,
        'SAVE_FMT_YAW_RVA':FY,
        'SAVE_FMT_PITCH_RVA':FP,
        'MENU_LABEL_YAW_RVA':LY,
        'MENU_LABEL_PITCH_RVA':LP,
        'SLIDER_FMT_RVA':0x2F00E0,
    }
    cmd = ['ld','-m','elf_x86_64',f'-Ttext=0x{CODE:x}','-e','stage3n_config_key_hook']
    for k,v in defs.items(): cmd += ['--defsym', f'{k}=0x{v:x}']
    cmd += ['-o',str(elf),str(obj)]
    run(cmd)
    run(['objcopy','-O','binary','-j','.text',str(elf),str(raw)])
    sy = {}
    for line in subprocess.check_output(['nm','-n',str(elf)], text=True).splitlines():
        f = line.split()
        if len(f)==3 and f[2].startswith('stage3n_'):
            sy[f[2]] = int(f[0],16)
    return raw.read_bytes(), sy

def write_data(b, pe):
    s = next(x for x in pe['sections'] if x['name']=='.s3hd')
    if (s['va'],s['rs'],s['vs']) != (0x2F0000,0x200,0xF0):
        raise SystemExit(f'.s3hd geometry mismatch: {s}')
    start = s['rp'] + (YAW - s['va'])
    end = s['rp'] + DATA_END
    if any(b[start:end]):
        raise SystemExit('Stage3M .s3hd unused tail is not zero')
    def put(rva, data):
        i = s['rp'] + rva - s['va']
        b[i:i+len(data)] = data
    put(YAW, struct.pack('<f',0.0))
    put(PITCH, struct.pack('<f',0.0))
    put(MN, struct.pack('<f',-30.0))
    put(MX, struct.pack('<f',30.0))
    put(RAD, struct.pack('<f',math.pi/180.0))
    put(ONE3, struct.pack('<f',1.0/3.0))
    put(TWO15, struct.pack('<f',2.0/15.0))
    strings = {
        KY:b'h2_classic_gun_yaw_deg\0',
        KP:b'h2_classic_gun_pitch_deg\0',
        FY:b'h2_classic_gun_yaw_deg = %.2f\n\0',
        FP:b'h2_classic_gun_pitch_deg = %.2f\n\0',
        LY:b'H2 Classic gun yaw (deg)\0',
        LP:b'H2 Classic gun pitch (deg)\0',
    }
    bounds = [(KY,KP),(KP,FY),(FY,FP),(FP,LY),(LY,LP),(LP,0x2F0000+DATA_END)]
    for (rva,data),(lo,hi) in zip(strings.items(),bounds):
        assert rva==lo
        if len(data) > hi-lo:
            raise SystemExit(f'data string too long at 0x{rva:X}: {len(data)}>{hi-lo}')
        put(rva,data)
    struct.pack_into('<I', b, s['h']+8, DATA_END)

def main():
    if len(sys.argv)!=3:
        raise SystemExit('usage: build_stage3n_h2_classic_barrel.py <Stage3M.dll> <output.dll>')
    src, out = map(Path,sys.argv[1:])
    bb = src.read_bytes()
    sha = hashlib.sha256(bb).hexdigest()
    if sha != BASE_SHA:
        raise SystemExit('wrong Stage3M base ' + sha)
    b = bytearray(bb)
    pe = peparse(b)
    by = {x['name']:x for x in pe['sections']}
    for n,r in (('.h2sf',0x2EE000),('.s3hc',0x2EF000),('.s3hd',0x2F0000),('.s3ic',0x2F1000)):
        if n not in by or by[n]['va'] != r:
            raise SystemExit(n + ' geometry')
    s = by['.s3ic']
    if (s['rs'],s['vs']) != (0x600,0x5BB) or s['rp']+s['rs'] != len(b):
        raise SystemExit(f'Stage3M .s3ic geometry/overlay mismatch: {s}, size {len(b)}')

    # Guard all four Stage3M transaction boundaries before changing them.
    req(b,pe,0x5443, rel(0xE9,0x5443,STAGE3M_CONFIG)+b'\x90\x90', 'Stage3M config hook')
    req(b,pe,0x71EC, rel(0xE8,0x71EC,STAGE3M_SAVE), 'Stage3M save hook')
    req(b,pe,0x3585E, rel(0xE8,0x3585E,STAGE3M_MENU), 'Stage3M menu hook')
    req(b,pe,0x95B16, rel(0xE8,0x95B16,H2_BUILD_CARRIERS), 'H2 stable-carrier call')
    # Guard the exact packet-builder register contract: r15b carries the final
    # publishToRenderer argument and r8 is rightCarrier at the call.
    req(b,pe,0x951C4,'440fb6bc24b81d0000','publishToRenderer -> r15b')
    req(b,pe,0x95AFB,'4c8d8c24700200004c8d842448020000418bd5488d8c2420030000','carrier call setup')

    with tempfile.TemporaryDirectory(prefix='halomccvr-stage3n-') as d:
        helper, sy = assemble(Path(d))
    need = {'stage3n_config_key_hook','stage3n_save_classic_offsets',
            'stage3n_menu_classic_offsets','stage3n_h2_classic_carrier'}
    if not need <= sy.keys():
        raise SystemExit('Stage3N symbols missing: ' + str(need-sy.keys()))
    if len(helper) > 0xA00:
        raise SystemExit(f'Stage3N helper too large: {len(helper)}')

    add = align(len(helper), pe['fa'])
    b.extend(helper.ljust(add,b'\0'))
    new_vs = 0x600 + len(helper)
    new_rs = 0x600 + add
    struct.pack_into('<I', b, s['h']+8, new_vs)
    struct.pack_into('<I', b, s['h']+16, new_rs)
    size_code = struct.unpack_from('<I', b, pe['opt']+4)[0]
    struct.pack_into('<I', b, pe['opt']+4, size_code + add)
    struct.pack_into('<I', b, pe['opt']+0x38,
                     align(0x2F1000 + new_vs, pe['sa']))
    s['vs'],s['rs'] = new_vs,new_rs
    write_data(b,pe)

    patch(b,pe,0x5443,rel(0xE9,0x5443,STAGE3M_CONFIG)+b'\x90\x90',
          rel(0xE9,0x5443,sy['stage3n_config_key_hook'])+b'\x90\x90',
          'config parse -> Stage3N')
    patch(b,pe,0x71EC,rel(0xE8,0x71EC,STAGE3M_SAVE),
          rel(0xE8,0x71EC,sy['stage3n_save_classic_offsets']),
          'config save -> Stage3N')
    patch(b,pe,0x3585E,rel(0xE8,0x3585E,STAGE3M_MENU),
          rel(0xE8,0x3585E,sy['stage3n_menu_classic_offsets']),
          'F1 menu -> Stage3N')
    patch(b,pe,0x95B16,rel(0xE8,0x95B16,H2_BUILD_CARRIERS),
          rel(0xE8,0x95B16,sy['stage3n_h2_classic_carrier']),
          'H2 packet carrier -> Classic-only Stage3N')

    struct.pack_into('<I', b, pe['opt']+0x40, 0)
    out.write_bytes(b)
    print('input',sha)
    print('helper',hashlib.sha256(helper).hexdigest(),len(helper))
    print('output',hashlib.sha256(b).hexdigest(),len(b))
    for k in sorted(sy): print(k,hex(sy[k]))

if __name__=='__main__': main()
