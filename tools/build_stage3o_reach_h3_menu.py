from pathlib import Path
import hashlib, shutil, struct, subprocess, sys, tempfile

BASE_SHA = '9646afa98cacab7b688aff10ac93fd529ba084ebb7f81be79cc2a1f1d308ed6b'
IMAGE_BASE = 0x180000000
CODE = 0x2F1C00

ACTIVE_TITLE = 0x2BA6C8
AIM_SEEN = 0x2B71BB
CAM_VALID = 0x2B71B9
BASE_CAM_VALID = 0x2B71B8
H2_NATIVE_HUD_QUERY = 0x48350
HUD_CURVATURE_QUERY_CALL = 0x35FD9

REACH_SCAN = 0x8112D
REACH_SCAN_END = 0x81146
H3_CLEAR_SITES = [
    (0x7CDA3, '44883d11a42300', 'stage3o_clear_aim_seen'),
    (0x7CDAB, '44883d07a42300', 'stage3o_clear_cam_valid'),
    (0x7CDB3, '44883dfea32300', 'stage3o_clear_base_cam_valid'),
    (0x7D167, '44883d4da02300', 'stage3o_clear_aim_seen'),
    (0x7D17A, '44883d38a02300', 'stage3o_clear_cam_valid'),
    (0x7D19E, '44883d13a02300', 'stage3o_clear_base_cam_valid'),
]

MENU_LEAS = [
    (0x34587, '488d0d92ac1800', b'\x48\x8d\x0d', 'stage3o_str_welcome', 'welcome text'),
    (0x3588A, '488d0d3fbc1800', b'\x48\x8d\x0d', 'stage3o_str_muzzle_note1', 'muzzle note 1'),
    (0x35899, '488d0d78bc1800', b'\x48\x8d\x0d', 'stage3o_str_muzzle_note2', 'muzzle note 2'),
    (0x36160, '488d0549c01800', b'\x48\x8d\x05', 'stage3o_str_curvature_note', 'generic curvature note'),
    (0x36169, '488d0df0bf1800', b'\x48\x8d\x0d', 'stage3o_str_h2_curvature_note', 'H2 curvature note'),
]
MENU_PTRS = [
    (0x1BF1A8, 0x1801BF500, 'stage3o_str_weapon_desc', 'Weapon category description'),
    (0x1BF1E8, 0x1801BF628, 'stage3o_str_hud_desc', 'HUD category description'),
]

# Known-good transaction pins that Stage 3O must not alter.
PRESERVE = [
    (0x90AC4, 'e837052600', 'H2 Classic cleanup call pin'),
    (0x90CE2, 'e87f0326009090', 'H2 Classic release pin'),
    (0x92F06, None, 'Stage3M H2 stock translation hook'),
    (0x95B16, None, 'Stage3N H2 Classic carrier hook'),
    (0x40A52, None, 'Stage3M H3/ODST stock translation hook'),
    (0x6B321, None, 'Stage3M Reach stock translation hook'),
    (0x51289, None, 'Stage3M H4 stock translation hook'),
    (0x48086, None, 'H4 rollback span 1'),
    (0x48110, None, 'H4 rollback span 2'),
    (0x5360B, None, 'H4 rollback span 3'),
    (0x53D41, None, 'H4 rollback span 4'),
    (0x53E37, None, 'H4 rollback span 5'),
]


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
        name = bytes(b[h:h+8]).split(b'\0', 1)[0].decode('ascii')
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
        raise SystemExit(f'{label}: size mismatch {len(expected)} != {len(replacement)}')
    req(b, pe, rva, expected, label)
    o = rva_off(pe, rva)
    b[o:o+len(expected)] = replacement
    print(f'{label}: RVA 0x{rva:X}')

def rel(op, src, dst):
    return bytes([op]) + struct.pack('<i', dst - src - 5)

def lea(prefix, src, dst):
    return prefix + struct.pack('<i', dst - src - 7)

def assemble(td):
    for x in ('as', 'ld', 'objcopy', 'nm'):
        if not shutil.which(x): raise SystemExit('missing ' + x)
    src = Path(__file__).with_name('stage3o_reach_h3_menu.S')
    obj, elf, raw = td/'o.o', td/'o.elf', td/'o.bin'
    run(['as', '--64', '-o', str(obj), str(src)])
    defs = {
        'ACTIVE_TITLE_RVA': ACTIVE_TITLE,
        'AIM_SEEN_RVA': AIM_SEEN,
        'CAM_VALID_RVA': CAM_VALID,
        'BASE_CAM_VALID_RVA': BASE_CAM_VALID,
        'H2_NATIVE_HUD_QUERY_RVA': H2_NATIVE_HUD_QUERY,
    }
    cmd = ['ld', '-m', 'elf_x86_64', f'-Ttext=0x{CODE:x}', '-e', 'stage3o_reach_memtype_filter']
    for k, v in defs.items(): cmd += ['--defsym', f'{k}=0x{v:x}']
    cmd += ['-o', str(elf), str(obj)]
    run(cmd)
    run(['objcopy', '-O', 'binary', '-j', '.text', str(elf), str(raw)])
    sy = {}
    for line in subprocess.check_output(['nm', '-n', str(elf)], text=True).splitlines():
        f = line.split()
        if len(f) == 3 and f[2].startswith('stage3o_'):
            sy[f[2]] = int(f[0], 16)
    return raw.read_bytes(), sy

def main():
    if len(sys.argv) != 3:
        raise SystemExit('usage: build_stage3o_reach_h3_menu.py <Stage3N.dll> <output.dll>')
    src, out = map(Path, sys.argv[1:])
    base = src.read_bytes()
    sha = hashlib.sha256(base).hexdigest()
    if sha != BASE_SHA:
        raise SystemExit('wrong Stage3N base ' + sha)
    b = bytearray(base)
    pe = peparse(b)
    by = {x['name']: x for x in pe['sections']}
    for n, r in (('.h2sf', 0x2EE000), ('.s3hc', 0x2EF000),
                 ('.s3hd', 0x2F0000), ('.s3ic', 0x2F1000)):
        if n not in by or by[n]['va'] != r:
            raise SystemExit(n + ' geometry')
    s = by['.s3ic']
    if (s['rs'], s['vs']) != (0xC00, 0xA87) or s['rp'] + s['rs'] != len(b):
        raise SystemExit(f'Stage3N .s3ic geometry/overlay mismatch: {s}, size {len(b)}')

    reach_expected = bytes.fromhex(
        '3d00000200741080390074073d00000400740432c0eb02b001')
    if len(reach_expected) != REACH_SCAN_END - REACH_SCAN:
        raise SystemExit('Reach scan guard length mismatch')
    req(b, pe, REACH_SCAN, reach_expected, 'Reach SAFEFRAME old type filter')
    for rva, exp, _ in H3_CLEAR_SITES:
        req(b, pe, rva, exp, 'Reach shared-camera clear')
    for rva, exp, _, _, label in MENU_LEAS:
        req(b, pe, rva, exp, label)
    for rva, oldva, _, label in MENU_PTRS:
        o = rva_off(pe, rva)
        actual = struct.unpack_from('<Q', b, o)[0]
        if actual != oldva:
            raise SystemExit(f'{label} guard 0x{rva:X}: 0x{actual:X} != 0x{oldva:X}')
    req(b, pe, HUD_CURVATURE_QUERY_CALL, rel(0xE8, HUD_CURVATURE_QUERY_CALL, H2_NATIVE_HUD_QUERY),
        'HUD curvature availability query')

    # Snapshot preserved sites/spans from the exact input for post-build audit.
    preserved = {}
    default_lengths = {0x92F06: 5, 0x95B16: 5, 0x40A52: 5, 0x6B321: 5,
                       0x51289: 5, 0x48086: 8, 0x48110: 8, 0x5360B: 8,
                       0x53D41: 8, 0x53E37: 8}
    for rva, fixed, label in PRESERVE:
        if fixed:
            data = bytes.fromhex(fixed)
            req(b, pe, rva, data, label)
        else:
            data = bytes(b[rva_off(pe, rva):rva_off(pe, rva)+default_lengths[rva]])
        preserved[(rva, label)] = data

    with tempfile.TemporaryDirectory(prefix='halomccvr-stage3o-') as d:
        helper, sy = assemble(Path(d))
    required = {
        'stage3o_reach_memtype_filter', 'stage3o_clear_aim_seen',
        'stage3o_clear_cam_valid', 'stage3o_clear_base_cam_valid',
        'stage3o_no_live_hud_curvature',
        'stage3o_str_welcome', 'stage3o_str_weapon_desc', 'stage3o_str_hud_desc',
        'stage3o_str_muzzle_note1', 'stage3o_str_muzzle_note2',
        'stage3o_str_h2_curvature_note', 'stage3o_str_curvature_note'
    }
    if not required <= sy.keys():
        raise SystemExit('Stage3O symbols missing: ' + str(required - sy.keys()))
    if len(helper) > 0x1000:
        raise SystemExit(f'Stage3O helper too large: {len(helper)}')

    add = align(len(helper), pe['fa'])
    b.extend(helper.ljust(add, b'\0'))
    new_vs = 0xC00 + len(helper)
    new_rs = 0xC00 + add
    struct.pack_into('<I', b, s['h']+8, new_vs)
    struct.pack_into('<I', b, s['h']+16, new_rs)
    size_code = struct.unpack_from('<I', b, pe['opt']+4)[0]
    struct.pack_into('<I', b, pe['opt']+4, size_code + add)
    struct.pack_into('<I', b, pe['opt']+0x38,
                     align(0x2F1000 + new_vs, pe['sa']))
    s['vs'], s['rs'] = new_vs, new_rs

    # Reach HUD: replace the 25-byte type-selection block with one call.
    replacement = rel(0xE8, REACH_SCAN, sy['stage3o_reach_memtype_filter'])
    replacement += b'\x90' * (len(reach_expected) - len(replacement))
    patch(b, pe, REACH_SCAN, reach_expected, replacement,
          'Reach SAFEFRAME mapped-only fast path')

    # Reach stale teardown: six byte stores become title-gated calls.
    for rva, exp, sym in H3_CLEAR_SITES:
        repl = rel(0xE8, rva, sy[sym]) + b'\x90\x90'
        patch(b, pe, rva, exp, repl, f'Reach teardown gate -> {sym}')

    # Menu HUD curvature: existing Halo2 test OR Reach active title.
    patch(b, pe, HUD_CURVATURE_QUERY_CALL,
          rel(0xE8, HUD_CURVATURE_QUERY_CALL, H2_NATIVE_HUD_QUERY),
          rel(0xE8, HUD_CURVATURE_QUERY_CALL, sy['stage3o_no_live_hud_curvature']),
          'HUD curvature disable -> Halo2/Reach')

    # Menu direct string xrefs.
    for rva, exp, prefix, sym, label in MENU_LEAS:
        patch(b, pe, rva, exp, lea(prefix, rva, sy[sym]), label + ' -> Stage3O')

    # Category table pointers. These fields already have PE base relocations;
    # changing the in-image VA retains ASLR behavior at the existing relocation sites.
    for rva, oldva, sym, label in MENU_PTRS:
        o = rva_off(pe, rva)
        struct.pack_into('<Q', b, o, IMAGE_BASE + sy[sym])
        print(f'{label}: RVA 0x{rva:X} -> RVA 0x{sy[sym]:X}')

    # Guard that the protected H2/H4 and stock-calibration sites are untouched.
    for (rva, label), data in preserved.items():
        o = rva_off(pe, rva)
        actual = bytes(b[o:o+len(data)])
        if actual != data:
            raise SystemExit(f'protected site changed: {label} at 0x{rva:X}')

    struct.pack_into('<I', b, pe['opt']+0x40, 0)
    out.write_bytes(b)
    print('input', sha)
    print('helper', hashlib.sha256(helper).hexdigest(), len(helper))
    print('output', hashlib.sha256(b).hexdigest(), len(b))
    for k in sorted(sy): print(k, hex(sy[k]))

if __name__ == '__main__':
    main()
