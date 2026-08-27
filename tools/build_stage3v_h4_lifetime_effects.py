from pathlib import Path
import hashlib, json, struct, subprocess, sys, tempfile

BASE_SHA = 'ca2a939e24594292ee543f6498c780b1609c3dba53aabe07f65cfa720fd6a549'
CODE_RVA = 0x2F109A
H4_CLEANUP_SCRATCH_RVA = 0x2F31F0

ORIGINAL_LOG_RVA = 0x1D90
FREE_LIBRARY_IAT_RVA = 0x180230
H4_GENERATION_RVA = 0x2A7200
H4_MODULE_REF_RVA = 0x2A7208
H4_CLEANUP_CONTINUE_RVA = 0x7CA5F
TITLE_ACTIVE_RVA = 0x879C0
H4_EFFECT_INSTALLER_RVA = 0x2EF595

# Exact Stage 3U sites. Every mutation is fail-closed on byte identity.
PATCHES = {
    'h4_installed_hook_loader_pin': (0x597FB, bytes.fromhex('b906000000')),
    'h4_create_fail_log':           (0x59A9A, bytes.fromhex('e8f182faff')),
    'h4_handle_fail_log':           (0x59AA8, bytes.fromhex('e8e382faff')),
    'h4_enable_fail_generation':    (0x59A3F, bytes.fromhex('448935bad72400')),
    'h4_cleanup_capture':           (0x7CA57, bytes.fromhex('8b1da3a7220033ff')),
    'h4_cleanup_final_log':         (0x7CB6F, bytes.fromhex('e81c52f8ff')),
    'h4_reticle_title_query':       (0x30894, bytes.fromhex('e827710500')),
    'h4_local_fp_effect_action':    (0x2EF73E, bytes.fromhex('c70000000000')),
}

# Regression anchors called out by the accepted Stage 3U lineage. The full
# diff whitelist below protects every other byte too; these make failures more
# understandable if the wrong base is ever supplied.
ANCHORS = [
    (0x87FF1, 21, 'Stage3Q-R1 cross-title selector'),
    (0xDF60, 15, 'Stage3Q Present caller hint'),
    (0xE060, 15, 'Stage3Q Present1 caller hint'),
    (0x90AC4, 5, 'H2 Classic cleanup pin'),
    (0x90CE2, 7, 'H2 Classic release pin'),
    (0x92F06, 5, 'Stage3M H2 visual stock translation'),
    (0x95B16, 5, 'Stage3N H2 Classic authored barrel alignment'),
    (0x40A52, 5, 'Stage3M H3/ODST visual stock translation'),
    (0x6B321, 5, 'Stage3M Reach visual stock translation'),
    (0x51289, 5, 'Stage3M H4 visual stock translation'),
    (0x48086, 8, 'H4 procedural-reticle rollback span 1'),
    (0x48110, 8, 'H4 procedural-reticle rollback span 2'),
    (0x5360B, 8, 'H4 procedural-reticle rollback span 3'),
    (0x53D41, 8, 'H4 procedural-reticle rollback span 4'),
    (0x53E37, 8, 'H4 procedural-reticle rollback span 5'),
    (0x7CDA3, 7, 'Stage3O H3 stability 1'),
    (0x7CDAB, 7, 'Stage3O H3 stability 2'),
    (0x7CDB3, 7, 'Stage3O H3 stability 3'),
    (0x7D167, 7, 'Stage3O H3 stability 4'),
    (0x7D17A, 7, 'Stage3O H3 stability 5'),
    (0x7D19E, 7, 'Stage3O H3 stability 6'),
    (0x81036, 40, 'Stage3P Reach HUD scan'),
    (0x8112D, 25, 'Stage3P Reach HUD memory filter'),
    (0x813A8, 10, 'Stage3P Reach HUD six-anchor completion'),
    (0x47396, 12, 'Stage3P H3 pause scope'),
    (0x4414D, 5, 'Stage3P native pause scope'),
    (0x44203, 8, 'Stage3P pause recovery scope'),
    (0x34587, 7, 'community attribution'),
    (0xA2B86, 5, 'Reach cold adapter non-owning identity'),
    (0x2F1B00, 0x5A, 'Stage3T Reach loader-lifetime helper'),
    (0x2F1B60, 0xB8, 'Stage3U Reach shell-lifecycle helper'),
]


def parse_pe(b):
    pe_sig = struct.unpack_from('<I', b, 0x3c)[0]
    if b[pe_sig:pe_sig+4] != b'PE\0\0':
        raise SystemExit('not a PE image')
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
    return dict(pe_sig=pe_sig, coff=coff, opt=opt, table=table, sections=secs,
                nsec=nsec,
                image_base=struct.unpack_from('<Q', b, opt + 0x18)[0],
                size_of_image=struct.unpack_from('<I', b, opt + 0x38)[0])


def rva_off(pe, rva):
    for s in pe['sections']:
        if s['va'] <= rva < s['va'] + max(s['vs'], s['rs']):
            return s['rp'] + rva - s['va']
    raise SystemExit(f'RVA not mapped 0x{rva:X}')


def req(b, pe, rva, exp, label):
    o = rva_off(pe, rva)
    got = bytes(b[o:o+len(exp)])
    if got != exp:
        raise SystemExit(f'{label} guard failed at 0x{rva:X}: {got.hex()} != {exp.hex()}')


def write(b, pe, rva, exp, repl, label):
    if len(exp) != len(repl):
        raise SystemExit(f'{label}: patch length mismatch')
    req(b, pe, rva, exp, label)
    o = rva_off(pe, rva)
    b[o:o+len(exp)] = repl
    print(f'{label}: 0x{rva:X} {exp.hex()} -> {repl.hex()}')


def relop(op, src, dst):
    return bytes([op]) + struct.pack('<i', dst - src - 5)


def assemble(td):
    src = Path(__file__).with_name('stage3v_h4_lifetime_effects.S')
    obj, elf, raw = td/'s.o', td/'s.elf', td/'s.bin'
    subprocess.run(['as', '--64', '-o', str(obj), str(src)], check=True)
    subprocess.run([
        'ld', '-m', 'elf_x86_64', f'-Ttext=0x{CODE_RVA:x}',
        '-e', 'stage3v_h4_install_log_release',
        '--defsym', f'ORIGINAL_LOG_RVA=0x{ORIGINAL_LOG_RVA:x}',
        '--defsym', f'FREE_LIBRARY_IAT_RVA=0x{FREE_LIBRARY_IAT_RVA:x}',
        '--defsym', f'H4_GENERATION_RVA=0x{H4_GENERATION_RVA:x}',
        '--defsym', f'H4_MODULE_REF_RVA=0x{H4_MODULE_REF_RVA:x}',
        '--defsym', f'H4_CLEANUP_SCRATCH_RVA=0x{H4_CLEANUP_SCRATCH_RVA:x}',
        '--defsym', f'H4_CLEANUP_CONTINUE_RVA=0x{H4_CLEANUP_CONTINUE_RVA:x}',
        '--defsym', f'TITLE_ACTIVE_RVA=0x{TITLE_ACTIVE_RVA:x}',
        '--defsym', f'H4_EFFECT_INSTALLER_RVA=0x{H4_EFFECT_INSTALLER_RVA:x}',
        '-o', str(elf), str(obj),
    ], check=True)
    subprocess.run(['objcopy', '-O', 'binary', '-j', '.text', str(elf), str(raw)], check=True)
    syms = {}
    for line in subprocess.check_output(['nm', '-n', str(elf)], text=True).splitlines():
        f = line.split()
        if len(f) == 3 and f[2].startswith('stage3v_'):
            syms[f[2]] = int(f[0], 16)
    return raw.read_bytes(), syms


def main():
    if len(sys.argv) != 3:
        raise SystemExit('usage: build_stage3v_h4_lifetime_effects.py <Stage3U-HaloMCCVR.dll> <out.dll>')
    src, out = map(Path, sys.argv[1:])
    base = src.read_bytes()
    sha = hashlib.sha256(base).hexdigest()
    if sha != BASE_SHA:
        raise SystemExit('wrong accepted Stage3U base: ' + sha)

    b = bytearray(base)
    pe = parse_pe(b)
    by = {s['name']: s for s in pe['sections']}
    if pe['image_base'] != 0x180000000 or pe['nsec'] != 12 or pe['size_of_image'] != 0x2F4000:
        raise SystemExit('Stage3U loader-safe PE identity mismatch')
    s3ic, s3qd = by.get('.s3ic'), by.get('.s3qd')
    if not s3ic or not s3qd:
        raise SystemExit('Stage3U helper sections missing')
    if (s3ic['va'], s3ic['vs'], s3ic['rs'], s3ic['ch']) != (0x2F1000, 0x16F0, 0x1800, 0x60000020):
        raise SystemExit('Stage3U .s3ic geometry mismatch')
    if (s3qd['va'], s3qd['rs']) != (0x2F3000, 0x200):
        raise SystemExit('Stage3U Q-R1 .s3qd loader layout regressed')

    for name, (rva, exp) in PATCHES.items():
        req(b, pe, rva, exp, name)

    anchors = {(r,n,label): bytes(b[rva_off(pe,r):rva_off(pe,r)+n])
               for r,n,label in ANCHORS}

    scratch_off = rva_off(pe, H4_CLEANUP_SCRATCH_RVA)
    if any(b[scratch_off:scratch_off+8]):
        raise SystemExit('H4 teardown scratch is not zero in accepted Stage3U')

    with tempfile.TemporaryDirectory(prefix='stage3v-') as t:
        helper, syms = assemble(Path(t))
    needed = {
        'stage3v_h4_install_log_release', 'stage3v_h4_enable_fail_release',
        'stage3v_h4_cleanup_capture', 'stage3v_h4_cleanup_log_release',
        'stage3v_h4_title_and_install_effect', 'stage3v_h4_hide_local_fp_effect'
    }
    if not needed <= syms.keys():
        raise SystemExit('missing Stage3V helper symbols: ' + repr(sorted(needed - syms.keys())))

    cave_off = rva_off(pe, CODE_RVA)
    if CODE_RVA + len(helper) > 0x2F1200:
        raise SystemExit(f'Stage3V helper exceeds the proven 0x2F109A..0x2F11FF zero cave ({len(helper)} bytes)')
    if any(b[cave_off:cave_off+len(helper)]):
        raise SystemExit('Stage3V helper cave is not zero in accepted Stage3U')
    b[cave_off:cave_off+len(helper)] = helper

    write(b, pe, 0x597FB, PATCHES['h4_installed_hook_loader_pin'][1],
          bytes.fromhex('b904000000'), 'H4 installed-hook loader ownership')
    write(b, pe, 0x59A9A, PATCHES['h4_create_fail_log'][1],
          relop(0xE8, 0x59A9A, syms['stage3v_h4_install_log_release']),
          'H4 create-failure untransferred pin release')
    write(b, pe, 0x59AA8, PATCHES['h4_handle_fail_log'][1],
          relop(0xE8, 0x59AA8, syms['stage3v_h4_install_log_release']),
          'H4 handle-failure/mismatch pin release')
    write(b, pe, 0x59A3F, PATCHES['h4_enable_fail_generation'][1],
          relop(0xE8, 0x59A3F, syms['stage3v_h4_enable_fail_release']) + b'\x90\x90',
          'H4 enable-failure post-MinHook pin release')
    write(b, pe, 0x7CA57, PATCHES['h4_cleanup_capture'][1],
          relop(0xE9, 0x7CA57, syms['stage3v_h4_cleanup_capture']) + b'\x90\x90\x90',
          'H4 successful teardown retained-module capture')
    write(b, pe, 0x7CB6F, PATCHES['h4_cleanup_final_log'][1],
          relop(0xE8, 0x7CB6F, syms['stage3v_h4_cleanup_log_release']),
          'H4 successful teardown post-hook pin release')

    # Effect suppression is activated through the existing title query, without
    # touching the following C-H4-50 cmp/branch that keeps the procedural
    # controller/bullet-ray reticle authoritative for Halo 4.
    write(b, pe, 0x30894, PATCHES['h4_reticle_title_query'][1],
          relop(0xE8, 0x30894, syms['stage3v_h4_title_and_install_effect']),
          'H4 guarded local-FP effect installer activation')
    write(b, pe, 0x2EF73E, PATCHES['h4_local_fp_effect_action'][1],
          relop(0xE8, 0x2EF73E, syms['stage3v_h4_hide_local_fp_effect']) + b'\x90',
          'H4 selected local-FP effect finite far-translation')

    # Explicitly prove accepted procedural H4 reticle branch remains byte-identical.
    req(b, pe, 0x30899, bytes.fromhex('3c040f84a2000000'), 'C-H4-50 procedural reticle H4 branch')

    # Full-file allowed-difference proof. Nothing outside the eight exact patch
    # spans and helper cave may differ from the headset-accepted Stage3U DLL.
    allowed = set(range(cave_off, cave_off + len(helper)))
    for rva, exp in PATCHES.values():
        o = rva_off(pe, rva)
        allowed.update(range(o, o + len(exp)))
    diffs = [i for i,(x,y) in enumerate(zip(base,b)) if x != y]
    extras = [i for i in diffs if i not in allowed]
    if extras:
        raise SystemExit('unexpected byte changes outside guarded Stage3V spans: ' + ','.join(hex(x) for x in extras[:16]))

    for (r,n,label), data in anchors.items():
        o = rva_off(pe, r)
        if bytes(b[o:o+n]) != data:
            raise SystemExit(f'accepted protected anchor changed: {label} 0x{r:X}')

    # Runtime-only scratch must remain zero on disk.
    if any(b[scratch_off:scratch_off+8]):
        raise SystemExit('Stage3V accidentally initialized runtime cleanup scratch on disk')

    header_end = min(s['rp'] for s in pe['sections'])
    if bytes(b[:header_end]) != base[:header_end]:
        raise SystemExit('PE headers/section table changed unexpectedly')

    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(b)
    print(json.dumps({
        'input_sha256': sha,
        'output_sha256': hashlib.sha256(b).hexdigest(),
        'output_size': len(b),
        'helper_rva': hex(CODE_RVA),
        'helper_size': len(helper),
        'helper_sha256': hashlib.sha256(helper).hexdigest(),
        'changed_file_bytes': len(diffs),
        'pe_headers_identical': True,
        'section_geometry_identical': True,
        'size_of_image': hex(pe['size_of_image']),
        's3qd_rva': hex(s3qd['va']),
        'runtime_scratch_rva': hex(H4_CLEANUP_SCRATCH_RVA),
        'symbols': {k: hex(v) for k,v in syms.items()},
    }, indent=2))

if __name__ == '__main__':
    main()
