from pathlib import Path
import hashlib, json, struct, subprocess, sys, tempfile

BASE_SHA = '86919573435dd19ab68c36e78c84ac698208a7350f7b117c709d2bfc3b8eef7c'
CODE_RVA = 0x2F1B60
GET_TICK_COUNT64_IAT_RVA = 0x180150
REACH_ACTIVE_CALLBACKS_RVA = 0x2A6DB4
REACH_LAST_CAM_COPY_MS_RVA = 0x2B8D90
REACH_PAUSE_CACHE_RVA = 0x2A6C7C
REACH_TEARDOWN_REQUESTED_RVA = 0x2A6DB2

REACH_MAIN_DETOUR_HEARTBEAT_RVA = 0x72D43
REACH_PRESENT_SYNTHETIC_HEARTBEAT_RVA = 0x435BD

EXPECT_MAIN_DETOUR_INC = bytes.fromhex('f0ff056a402300')
EXPECT_PRESENT_SYNTHETIC = bytes.fromhex('ff158dcb130090488905c5572700')

# Stage 3T lifetime fix must remain exactly present. These are the four sites
# that make Reach teardown safe while haloreach.dll would otherwise disappear.
STAGE3T_GUARDS = [
    (0x5D7C2, bytes.fromhex('b904000000'), 'Stage3T installed-hook loader pin'),
    (0x5E653, bytes.fromhex('e8a8342900909090'), 'Stage3T failed-install pin release'),
    (0x7D31B, bytes.fromhex('4c8b35ae9a2200'), 'Stage3T retired module pin capture'),
    (0x7D4E7, bytes.fromhex('e846462700'), 'Stage3T post-teardown pin release'),
]

# Critical known-good bytes outside the two Stage 3U sites.  These cover the
# accepted title selector, H2, H3, H4, Reach HUD, and all-title gun calibration.
PRESERVE = [
    (0x87FF1, 21, 'Stage3Q-R1 cross-title ambiguity selector'),
    (0xDF60, 15, 'Stage3Q Present caller hint'),
    (0xE060, 15, 'Stage3Q Present1 caller hint'),
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
    (0x81036, 40, 'Stage3P Reach HUD high-first scan start'),
    (0x8112D, 25, 'Stage3P Reach HUD private+mapped filter'),
    (0x813A8, 10, 'Stage3P Reach HUD six-record completion'),
    (0x47396, 12, 'Stage3P H3 authoritative pause scope'),
    (0x4414D, 5, 'Stage3P generic pause native read scope'),
    (0x44203, 8, 'Stage3P pause recovery title scope'),
    (0x34587, 7, 'Stage3P attribution xref'),
    (0xA2B86, 5, 'Reach cold adapter identity-only handle'),
    # Stage 3T helper itself must remain untouched.
    (0x2F1B00, 0x5A, 'Stage3T loader-lifetime helper'),
]


def parse_pe(b):
    pe_sig = struct.unpack_from('<I', b, 0x3c)[0]
    coff = pe_sig + 4
    nsec = struct.unpack_from('<H', b, coff + 2)[0]
    opt_size = struct.unpack_from('<H', b, coff + 16)[0]
    opt = coff + 20
    table = opt + opt_size
    secs = []
    for i in range(nsec):
        h = table + i * 40
        name = bytes(b[h:h+8]).split(b'\0', 1)[0].decode('ascii')
        vs, va, rs, rp = struct.unpack_from('<IIII', b, h + 8)
        ch = struct.unpack_from('<I', b, h + 36)[0]
        secs.append(dict(name=name, vs=vs, va=va, rs=rs, rp=rp, ch=ch, h=h))
    size_of_image = struct.unpack_from('<I', b, opt + 0x38)[0]
    return dict(pe_sig=pe_sig, coff=coff, nsec=nsec, opt=opt, table=table,
                sections=secs, size_of_image=size_of_image)


def rva_off(pe, rva):
    for s in pe['sections']:
        if s['va'] <= rva < s['va'] + max(s['vs'], s['rs']):
            return s['rp'] + rva - s['va']
    raise SystemExit(f'RVA not mapped 0x{rva:X}')


def req(b, pe, rva, exp, label):
    o = rva_off(pe, rva)
    got = bytes(b[o:o+len(exp)])
    if got != exp:
        raise SystemExit(f'{label} guard failed 0x{rva:X}: {got.hex()} != {exp.hex()}')


def patch(b, pe, rva, exp, repl, label):
    if len(exp) != len(repl):
        raise SystemExit(f'{label} size mismatch {len(exp)} != {len(repl)}')
    req(b, pe, rva, exp, label)
    o = rva_off(pe, rva)
    b[o:o+len(exp)] = repl
    print(f'{label}: 0x{rva:X}')


def relcall(src, dst):
    return b'\xE8' + struct.pack('<i', dst - src - 5)


def assemble(td):
    src = Path(__file__).with_name('stage3u_reach_shell_lifecycle.S')
    obj = td / 's.o'
    elf = td / 's.elf'
    raw = td / 's.bin'
    subprocess.run(['as', '--64', '-o', str(obj), str(src)], check=True)
    cmd = [
        'ld', '-m', 'elf_x86_64', f'-Ttext=0x{CODE_RVA:x}',
        '-e', 'stage3u_reach_camera_heartbeat',
        '--defsym', f'GET_TICK_COUNT64_IAT_RVA=0x{GET_TICK_COUNT64_IAT_RVA:x}',
        '--defsym', f'REACH_ACTIVE_CALLBACKS_RVA=0x{REACH_ACTIVE_CALLBACKS_RVA:x}',
        '--defsym', f'REACH_LAST_CAM_COPY_MS_RVA=0x{REACH_LAST_CAM_COPY_MS_RVA:x}',
        '--defsym', f'REACH_PAUSE_CACHE_RVA=0x{REACH_PAUSE_CACHE_RVA:x}',
        '--defsym', f'REACH_TEARDOWN_REQUESTED_RVA=0x{REACH_TEARDOWN_REQUESTED_RVA:x}',
        '-o', str(elf), str(obj),
    ]
    subprocess.run(cmd, check=True)
    subprocess.run(['objcopy', '-O', 'binary', '-j', '.text', str(elf), str(raw)], check=True)
    symbols = {}
    for line in subprocess.check_output(['nm', '-n', str(elf)], text=True).splitlines():
        f = line.split()
        if len(f) == 3 and f[2].startswith('stage3u_'):
            symbols[f[2]] = int(f[0], 16)
    return raw.read_bytes(), symbols


def main():
    if len(sys.argv) != 3:
        raise SystemExit('usage: builder <Stage3T-HaloMCCVR.dll> <out.dll>')
    src, out = map(Path, sys.argv[1:])
    base = src.read_bytes()
    sha = hashlib.sha256(base).hexdigest()
    if sha != BASE_SHA:
        raise SystemExit('wrong Stage3T base ' + sha)

    b = bytearray(base)
    pe = parse_pe(b)
    by = {s['name']: s for s in pe['sections']}
    s3ic = by['.s3ic']
    s3qd = by['.s3qd']
    if pe['nsec'] != 12 or pe['size_of_image'] != 0x2F4000:
        raise SystemExit('Stage3T loader-safe PE identity mismatch')
    if (s3ic['va'], s3ic['vs'], s3ic['rs'], s3ic['ch']) != (0x2F1000, 0x16F0, 0x1800, 0x60000020):
        raise SystemExit('Stage3T .s3ic geometry mismatch')
    if s3qd['va'] != 0x2F3000:
        raise SystemExit('Stage3T .s3qd loader-fix RVA regressed')

    for rva, exp, label in STAGE3T_GUARDS:
        req(b, pe, rva, exp, label)
    req(b, pe, REACH_MAIN_DETOUR_HEARTBEAT_RVA, EXPECT_MAIN_DETOUR_INC,
        'Reach admitted callback active-counter increment')
    req(b, pe, REACH_PRESENT_SYNTHETIC_HEARTBEAT_RVA, EXPECT_PRESENT_SYNTHETIC,
        'Reach synthetic Present heartbeat')

    preserved = {(r,n,l): bytes(b[rva_off(pe,r):rva_off(pe,r)+n])
                 for r,n,l in PRESERVE}

    with tempfile.TemporaryDirectory(prefix='stage3u-') as t:
        helper, symbols = assemble(Path(t))
    needed = {'stage3u_reach_camera_heartbeat', 'stage3u_reach_publish_real_heartbeat'}
    if not needed <= symbols.keys():
        raise SystemExit('missing Stage3U helper symbols')

    cave_off = rva_off(pe, CODE_RVA)
    if CODE_RVA + len(helper) > s3ic['va'] + s3ic['vs']:
        raise SystemExit('Stage3U helper exceeds existing .s3ic VirtualSize')
    if any(b[cave_off:cave_off+len(helper)]):
        raise SystemExit('Stage3U helper cave is not zero in exact Stage3T')
    b[cave_off:cave_off+len(helper)] = helper

    patch(
        b, pe, REACH_MAIN_DETOUR_HEARTBEAT_RVA, EXPECT_MAIN_DETOUR_INC,
        relcall(REACH_MAIN_DETOUR_HEARTBEAT_RVA,
                symbols['stage3u_reach_camera_heartbeat']) + b'\x90\x90',
        'Reach real-camera heartbeat publication')
    patch(
        b, pe, REACH_PRESENT_SYNTHETIC_HEARTBEAT_RVA, EXPECT_PRESENT_SYNTHETIC,
        relcall(REACH_PRESENT_SYNTHETIC_HEARTBEAT_RVA,
                symbols['stage3u_reach_publish_real_heartbeat']) + b'\x90' * 9,
        'Reach Present heartbeat de-synthesis + stale-core teardown request')

    for (r,n,l), data in preserved.items():
        o = rva_off(pe, r)
        if bytes(b[o:o+n]) != data:
            raise SystemExit(f'protected site changed {l} 0x{r:X}')
    for rva, exp, label in STAGE3T_GUARDS:
        req(b, pe, rva, exp, label)

    # Loader-hotfix PE headers stay byte-for-byte identical to Stage 3T/Q-R1.
    header_end = min(sec['rp'] for sec in pe['sections'])
    if b[:header_end] != base[:header_end]:
        raise SystemExit('PE headers changed unexpectedly')

    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(b)
    print(json.dumps({
        'input_sha256': sha,
        'output_sha256': hashlib.sha256(b).hexdigest(),
        'output_size': len(b),
        'helper_rva': hex(CODE_RVA),
        'helper_size': len(helper),
        'pe_headers_identical': True,
        'size_of_image': hex(pe['size_of_image']),
        's3qd_rva': hex(s3qd['va']),
        'symbols': {k: hex(v) for k,v in symbols.items()},
    }, indent=2))

if __name__ == '__main__':
    main()
