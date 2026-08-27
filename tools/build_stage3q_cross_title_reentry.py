from pathlib import Path
import hashlib, shutil, struct, subprocess, sys, tempfile, json

BASE_SHA = 'b5eeec1276e91197bade35b926d9f176437e0096a40b29a137338e4f2e22abe2'
CODE = 0x2F2200
DATA = 0x2F3000
DATA_VSIZE = 0x200
VIRTUAL_QUERY_IAT = 0x180208
GET_MODULE_HANDLE_W_IAT = 0x1800F8
GET_TICK_COUNT64_IAT = 0x180150
TITLE_DESCRIPTORS = 0x189040
ACTIVE_TITLE = 0x2BA6C8

TITLE_SELECT = 0x87FF1
TITLE_SELECT_LEN = 21
PRESENT = 0xDF60
PRESENT1 = 0xE060
PRESENT_PATCH_LEN = 15

# Every accepted Stage3P behavior is pinned byte-for-byte. Stage3Q may touch
# only the generic title selector and the two process-wide Present entries.
PRESERVE = [
    (0x90AC4, 5, 'H2 Classic cleanup call pin'),
    (0x90CE2, 7, 'H2 Classic release pin'),
    (0x92F06, 5, 'Stage3M H2 stock translation hook'),
    (0x95B16, 5, 'Stage3N H2 Classic carrier/yaw-pitch hook'),
    (0x40A52, 5, 'Stage3M H3/ODST stock translation hook'),
    (0x6B321, 5, 'Stage3M Reach stock translation hook'),
    (0x51289, 5, 'Stage3M H4 stock translation hook'),
    (0x48086, 8, 'H4 rollback span 1'),
    (0x48110, 8, 'H4 rollback span 2'),
    (0x5360B, 8, 'H4 rollback span 3'),
    (0x53D41, 8, 'H4 rollback span 4'),
    (0x53E37, 8, 'H4 rollback span 5'),
    (0x7CDA3, 7, 'Stage3O H3 stability gate aim retry'),
    (0x7CDAB, 7, 'Stage3O H3 stability gate cam retry'),
    (0x7CDB3, 7, 'Stage3O H3 stability gate basecam retry'),
    (0x7D167, 7, 'Stage3O H3 stability gate aim final'),
    (0x7D17A, 7, 'Stage3O H3 stability gate cam final'),
    (0x7D19E, 7, 'Stage3O H3 stability gate basecam final'),
    # Stage3P accepted Reach HUD binding.
    (0x81036, 40, 'Stage3P Reach HUD high-first scan start'),
    (0x8112D, 25, 'Stage3P Reach HUD private+mapped filter'),
    (0x813A8, 10, 'Stage3P Reach HUD early six-record completion'),
    # Stage3P accepted H2 pause visual/resume scoping.
    (0x47396, 12, 'Stage3P H3 authoritative pause scope'),
    (0x4414D, 5, 'Stage3P generic pause native read scope'),
    (0x44203, 8, 'Stage3P pause recovery title scope'),
    # Correct attribution.
    (0x34587, 7, 'Stage3P @MeWhenINameMyself welcome xref'),
]

EXPECTED_TITLE_SELECT = bytes.fromhex('4d3bec7604b007eb0c4885ff74050fb607eb0232c0')
EXPECTED_PRESENT = bytes.fromhex('48895c240848896c24104889742418')
EXPECTED_PRESENT1 = bytes.fromhex('48895c240848896c24104889742418')


def align(v, a): return (v + a - 1) // a * a

def run(cmd):
    subprocess.run(cmd, check=True)

def parse_pe(b):
    pe_sig = struct.unpack_from('<I', b, 0x3C)[0]
    coff = pe_sig + 4
    nsec = struct.unpack_from('<H', b, coff + 2)[0]
    opt_size = struct.unpack_from('<H', b, coff + 16)[0]
    opt = coff + 20
    table = opt + opt_size
    sections = []
    for i in range(nsec):
        h = table + i * 40
        name = bytes(b[h:h+8]).split(b'\0',1)[0].decode('ascii')
        vs, va, rs, rp = struct.unpack_from('<IIII', b, h + 8)
        ch = struct.unpack_from('<I', b, h + 36)[0]
        sections.append(dict(name=name, vs=vs, va=va, rs=rs, rp=rp, ch=ch, h=h))
    return dict(pe_sig=pe_sig, coff=coff, nsec=nsec, opt=opt, table=table,
                sections=sections,
                file_align=struct.unpack_from('<I', b, opt + 0x24)[0],
                sect_align=struct.unpack_from('<I', b, opt + 0x20)[0],
                size_headers=struct.unpack_from('<I', b, opt + 0x3C)[0])

def rva_off(pe, rva):
    for s in pe['sections']:
        if s['va'] <= rva < s['va'] + max(s['vs'], s['rs']):
            return s['rp'] + rva - s['va']
    raise SystemExit(f'RVA not mapped: 0x{rva:X}')

def req(b, pe, rva, expected, label):
    o = rva_off(pe, rva)
    actual = bytes(b[o:o+len(expected)])
    if actual != expected:
        raise SystemExit(f'{label} guard 0x{rva:X}: {actual.hex()} != {expected.hex()}')

def patch(b, pe, rva, expected, replacement, label):
    if len(expected) != len(replacement):
        raise SystemExit(f'{label}: size mismatch {len(expected)} != {len(replacement)}')
    req(b, pe, rva, expected, label)
    o = rva_off(pe, rva)
    b[o:o+len(expected)] = replacement
    print(f'{label}: RVA 0x{rva:X}')

def relcall(src, dst):
    return b'\xE8' + struct.pack('<i', dst - src - 5)

def assemble(td):
    for exe in ('as','ld','objcopy','nm'):
        if not shutil.which(exe):
            raise SystemExit('missing tool: ' + exe)
    src = Path(__file__).with_name('stage3q_cross_title_reentry.S')
    obj, elf, raw = td/'q.o', td/'q.elf', td/'q.bin'
    run(['as','--64','-o',str(obj),str(src)])
    defs = {
        'REENTRY_DATA_RVA': DATA,
        'VIRTUAL_QUERY_IAT_RVA': VIRTUAL_QUERY_IAT,
        'GET_MODULE_HANDLE_W_IAT_RVA': GET_MODULE_HANDLE_W_IAT,
        'GET_TICK_COUNT64_IAT_RVA': GET_TICK_COUNT64_IAT,
        'TITLE_DESCRIPTORS_RVA': TITLE_DESCRIPTORS,
        'ACTIVE_TITLE_RVA': ACTIVE_TITLE,
    }
    cmd = ['ld','-m','elf_x86_64',f'-Ttext=0x{CODE:x}','-e','stage3q_title_select']
    for k,v in defs.items():
        cmd += ['--defsym', f'{k}=0x{v:x}']
    cmd += ['-o',str(elf),str(obj)]
    run(cmd)
    run(['objcopy','-O','binary','-j','.text',str(elf),str(raw)])
    sy = {}
    for line in subprocess.check_output(['nm','-n',str(elf)],text=True).splitlines():
        f=line.split()
        if len(f)==3 and f[2].startswith('stage3q_'):
            sy[f[2]] = int(f[0],16)
    return raw.read_bytes(), sy, elf.read_bytes()

def add_data_section(b, pe, s3ic, raw_ptr):
    # One more IMAGE_SECTION_HEADER fits before the existing 0x400 headers end.
    new_h = pe['table'] + pe['nsec'] * 40
    if new_h + 40 > pe['size_headers']:
        raise SystemExit('no PE section-header space for .s3qd')
    if any(b[new_h:new_h+40]):
        raise SystemExit('next PE section header is not zero padding')
    if raw_ptr % pe['file_align']:
        raise SystemExit('new .s3qd raw pointer is not file aligned')
    hdr = bytearray(40)
    hdr[:8] = b'.s3qd\0\0\0'
    struct.pack_into('<IIII', hdr, 8, DATA_VSIZE, DATA, 0x200, raw_ptr)
    struct.pack_into('<I', hdr, 36, 0xC0000040)  # initialized data, read/write
    b[new_h:new_h+40] = hdr
    struct.pack_into('<H', b, pe['coff'] + 2, pe['nsec'] + 1)
    # SizeOfInitializedData and SizeOfImage.
    init_data = struct.unpack_from('<I', b, pe['opt'] + 8)[0]
    struct.pack_into('<I', b, pe['opt'] + 8, init_data + 0x200)
    struct.pack_into('<I', b, pe['opt'] + 0x38,
                     align(DATA + DATA_VSIZE, pe['sect_align']))
    b.extend(b'\0' * 0x200)
    return new_h

def main():
    if len(sys.argv) != 3:
        raise SystemExit('usage: build_stage3q_cross_title_reentry.py <Stage3P.dll> <output.dll>')
    src, out = map(Path, sys.argv[1:])
    base = src.read_bytes()
    sha = hashlib.sha256(base).hexdigest()
    if sha != BASE_SHA:
        raise SystemExit('wrong Stage3P base: ' + sha)
    b = bytearray(base)
    pe = parse_pe(b)
    by = {s['name']:s for s in pe['sections']}
    s = by.get('.s3ic')
    d = by.get('.s3hd')
    if not s or (s['va'],s['vs'],s['rs'],s['ch']) != (0x2F1000,0x11FE,0x1200,0x60000020):
        raise SystemExit('Stage3P .s3ic geometry mismatch')
    if not d or (d['va'],d['vs'],d['rs']) != (0x2F0000,0x1E4,0x200):
        raise SystemExit('Stage3P .s3hd geometry mismatch')
    if s['rp'] + s['rs'] != len(b):
        raise SystemExit('Stage3P overlay/EOF geometry mismatch')
    if pe['nsec'] != 11:
        raise SystemExit(f'unexpected Stage3P section count {pe["nsec"]}')

    req(b, pe, TITLE_SELECT, EXPECTED_TITLE_SELECT, 'stock ambiguity value selector')
    req(b, pe, PRESENT, EXPECTED_PRESENT, 'PresentHook prologue')
    req(b, pe, PRESENT1, EXPECTED_PRESENT1, 'Present1Hook prologue')

    preserved = {}
    for rva,n,label in PRESERVE:
        o = rva_off(pe,rva)
        preserved[(rva,label)] = bytes(b[o:o+n])

    with tempfile.TemporaryDirectory(prefix='halomccvr-stage3q-') as tmp:
        helper, sy, _ = assemble(Path(tmp))
    required = {
        'stage3q_title_select','stage3q_present_probe_4arg','stage3q_present_probe_5arg',
        'stage3q_present_common','stage3q_hash_region','stage3q_activity_probe',
        'stage3q_evidence_table'
    }
    if not required <= sy.keys():
        raise SystemExit('missing Stage3Q symbols: ' + str(required - sy.keys()))
    if sy['stage3q_title_select'] != CODE:
        raise SystemExit('Stage3Q helper base mismatch')

    # Grow the last executable helper section at EOF, starting exactly after its
    # current raw allocation. Then create a distinct RW data section for probe
    # state so no executable section becomes writable.
    helper_rel = CODE - s['va']
    if helper_rel < s['rs']:
        raise SystemExit('Stage3Q code overlaps Stage3P helper bytes')
    needed = helper_rel + len(helper)
    new_vs = max(s['vs'], needed)
    new_rs = align(needed, pe['file_align'])
    old_rs = s['rs']
    grow = new_rs - old_rs
    if grow < 0:
        raise SystemExit('Stage3Q helper section unexpectedly shrank')
    if grow:
        b.extend(b'\0' * grow)
        size_code = struct.unpack_from('<I', b, pe['opt'] + 4)[0]
        struct.pack_into('<I', b, pe['opt'] + 4, size_code + grow)
    helper_off = s['rp'] + helper_rel
    b[helper_off:helper_off+len(helper)] = helper
    struct.pack_into('<I', b, s['h'] + 8, new_vs)
    struct.pack_into('<I', b, s['h'] + 16, new_rs)
    s['vs'], s['rs'] = new_vs, new_rs

    data_raw = s['rp'] + new_rs
    if data_raw != len(b):
        raise SystemExit('Stage3Q .s3qd raw pointer does not match EOF')
    add_data_section(b, pe, s, data_raw)

    patch(b, pe, TITLE_SELECT, EXPECTED_TITLE_SELECT,
          relcall(TITLE_SELECT, sy['stage3q_title_select']) + b'\x90'*(TITLE_SELECT_LEN-5),
          'General cross-title ambiguity selector')
    patch(b, pe, PRESENT, EXPECTED_PRESENT,
          relcall(PRESENT, sy['stage3q_present_probe_4arg']) + b'\x90'*(PRESENT_PATCH_LEN-5),
          'Present caller re-entry hint')
    patch(b, pe, PRESENT1, EXPECTED_PRESENT1,
          relcall(PRESENT1, sy['stage3q_present_probe_5arg']) + b'\x90'*(PRESENT_PATCH_LEN-5),
          'Present1 caller re-entry hint')

    # Prove accepted Stage3P title-specific behavior stayed byte-identical.
    for (rva,label),data in preserved.items():
        o = rva_off(pe,rva)
        if bytes(b[o:o+len(data)]) != data:
            raise SystemExit(f'protected Stage3P site changed: {label} at 0x{rva:X}')

    # New data section must be fully zero initialized on disk.
    if any(b[data_raw:data_raw+0x200]):
        raise SystemExit('.s3qd initial state is not zero')
    struct.pack_into('<I', b, pe['opt'] + 0x40, 0)  # checksum
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(b)

    manifest = {
        'input_sha256': sha,
        'output_sha256': hashlib.sha256(b).hexdigest(),
        'output_size': len(b),
        'helper_sha256': hashlib.sha256(helper).hexdigest(),
        'helper_size': len(helper),
        'helper_rva': hex(CODE),
        'data_rva': hex(DATA),
        's3ic_virtual_size': hex(new_vs),
        's3ic_raw_size': hex(new_rs),
        's3qd_raw_pointer': hex(data_raw),
        'symbols': {k:hex(v) for k,v in sorted(sy.items())},
    }
    print(json.dumps(manifest, indent=2))

if __name__ == '__main__':
    main()
