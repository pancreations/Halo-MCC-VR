from pathlib import Path
import hashlib, json, shutil, struct, subprocess, sys, tempfile

EXPECTED_STAGE3AC_SHA256 = "3a09288b5b8de4420ffc08695ebeb7431456971ccfe59de1f8c43f999caf700d"
IMAGE_BASE = 0x180000000
TITLE_CALL_RVA = 0x00030894
CLEANUP_EFFECT_CALL_RVA = 0x002F3B3A
TITLE_CALL_STAGE3AC = bytes.fromhex("e8ee112c00")       # -> 0x2F1A87
CLEANUP_CALL_STAGE3AC = bytes.fromhex("e88c040000")     # -> 0x2F3FCB

SECTIONS = {
    ".patcher": 0x002F114B,
    ".title":   0x002F15BB,
    ".cleanup": 0x002F1BD7,
}
LIMITS = {
    ".patcher": 181,
    ".title": 69,
    ".cleanup": 41,
}
DEFS = {
    "IAT_VIRTUAL_PROTECT_RVA": 0x00180200,
    "STAGE3X_RESTORE_PROTECT_FLUSH_RVA": 0x002F3200,
    "STAGE3AC_TITLE_WRAPPER_RVA": 0x002F1A87,
    "STAGE3AC_CLEANUP_RVA": 0x002F3FCB,
    "H4_MODULE_REF_RVA": 0x002A7208,
}

# Retail Halo 4 dispatcher identities are recorded for provenance and mirrored
# by the runtime patcher's exact state marker. These are not static edits to
# HaloMCCVR.dll; the helper applies/restores them to the retained halo4.dll.
H4_RUNTIME_PATCHES = {
    "gldf_fp_context": (0x003A0822, bytes.fromhex("840db82dd200"), bytes.fromhex("4584ed909090")),
    "ltvl_fp_skip":    (0x003A0AA1, bytes.fromhex("84c9"), bytes.fromhex("31c9")),
    "lens_fp_skip":    (0x003A0B1A, bytes.fromhex("84c9"), bytes.fromhex("31c9")),
}


def parse_pe(b):
    pe = struct.unpack_from('<I', b, 0x3C)[0]
    if b[:2] != b'MZ' or b[pe:pe+4] != b'PE\0\0':
        raise SystemExit('not PE')
    coff = pe + 4
    n = struct.unpack_from('<H', b, coff + 2)[0]
    optsz = struct.unpack_from('<H', b, coff + 16)[0]
    opt = coff + 20
    if struct.unpack_from('<H', b, opt)[0] != 0x20B:
        raise SystemExit('not PE32+')
    if struct.unpack_from('<Q', b, opt + 24)[0] != IMAGE_BASE:
        raise SystemExit('unexpected image base')
    size_image = struct.unpack_from('<I', b, opt + 56)[0]
    sh = opt + optsz
    secs = []
    for i in range(n):
        o = sh + i * 40
        name = b[o:o+8].rstrip(b'\0').decode('ascii', 'replace')
        vs, va, rs, rp = struct.unpack_from('<IIII', b, o + 8)
        chars = struct.unpack_from('<I', b, o + 36)[0]
        secs.append(dict(name=name, vs=vs, va=va, rs=rs, rp=rp, chars=chars, hoff=o))
    return dict(pe=pe, coff=coff, opt=opt, n=n, size_image=size_image, secs=secs)


def roff(pe, rva):
    for s in pe['secs']:
        if s['va'] <= rva < s['va'] + max(s['vs'], s['rs']):
            return s['rp'] + (rva - s['va'])
    raise SystemExit(f'RVA not mapped: 0x{rva:X}')


def relcall(src, dst):
    return b'\xE8' + struct.pack('<i', dst - (src + 5))


def assemble(tooldir):
    for x in ('as', 'ld', 'objcopy', 'nm'):
        if not shutil.which(x):
            raise SystemExit('missing ' + x)
    with tempfile.TemporaryDirectory(prefix='s3ad-') as td:
        td = Path(td)
        obj = td / 's3ad.o'
        elf = td / 's3ad.elf'
        subprocess.run(['as', '--64', '-o', str(obj), str(tooldir / 'stage3ad_h4_promethean_nonparticle_hide.S')], check=True, cwd=tooldir)
        cmd = ['ld', '-m', 'elf_x86_64', '-e', 'stage3ad_h4_title_wrapper']
        for sec, rva in SECTIONS.items():
            cmd += [f'--section-start={sec}=0x{rva:x}']
        for k, v in DEFS.items():
            cmd += ['--defsym', f'{k}=0x{v:x}']
        cmd += ['-o', str(elf), str(obj)]
        subprocess.run(cmd, check=True)
        raw = {}
        for sec in SECTIONS:
            p = td / (sec.strip('.') + '.bin')
            subprocess.run(['objcopy', '-O', 'binary', '-j', sec, str(elf), str(p)], check=True)
            raw[sec] = p.read_bytes()
        syms = {}
        nm_text = subprocess.check_output(['nm', '-n', str(elf)], text=True)
        for line in nm_text.splitlines():
            f = line.split()
            if len(f) == 3 and f[2].startswith('stage3ad_'):
                syms[f[2]] = int(f[0], 16)
        return raw, syms, nm_text


def main():
    if len(sys.argv) != 3:
        raise SystemExit('usage: build_stage3ad_h4_promethean_nonparticle_hide.py <Stage3AC.dll> <out.dll>')
    src, out = Path(sys.argv[1]), Path(sys.argv[2])
    ib = src.read_bytes()
    sha = hashlib.sha256(ib).hexdigest()
    if sha != EXPECTED_STAGE3AC_SHA256:
        raise SystemExit('wrong Stage3AC input ' + sha)

    raw, syms, nm_text = assemble(Path(__file__).resolve().parent)
    required = {'stage3ad_h4_nonparticle_patch', 'stage3ad_h4_title_wrapper', 'stage3ad_h4_cleanup_wrapper'}
    if not required <= set(syms):
        raise SystemExit('missing symbols ' + repr(required - set(syms)))

    b = bytearray(ib)
    pe = parse_pe(b)
    qd = pe['secs'][-1]
    if (pe['n'] != 12 or qd['name'] != '.s3qd' or qd['va'] != 0x2F3000 or
            qd['vs'] != 0x1000 or qd['rs'] != 0x1000 or pe['size_image'] != 0x2F4000):
        raise SystemExit('Stage3AC/Q-R1 loader geometry mismatch')
    s3ic = next((s for s in pe['secs'] if s['name'] == '.s3ic'), None)
    if not s3ic or s3ic['va'] != 0x2F1000 or s3ic['vs'] != 0x16F0 or s3ic['chars'] != 0x60000020:
        raise SystemExit('Stage3AC .s3ic geometry mismatch')

    # The exact Stage3AC hash plus full zero-span guards makes these genuine
    # post-function gaps. Never substitute .s3qd+0 runtime state.
    for sec, data in raw.items():
        if len(data) > LIMITS[sec]:
            raise SystemExit(f'{sec} too large {len(data)} > {LIMITS[sec]}')
        o = roff(pe, SECTIONS[sec])
        span = bytes(b[o:o + LIMITS[sec]])
        if span != b'\0' * LIMITS[sec]:
            raise SystemExit(f'{sec} Stage3AC cave guard failed; expected {LIMITS[sec]} zero bytes')

    # Guard the exact current call edges before replacing them.
    to = roff(pe, TITLE_CALL_RVA)
    co = roff(pe, CLEANUP_EFFECT_CALL_RVA)
    if bytes(b[to:to+5]) != TITLE_CALL_STAGE3AC:
        raise SystemExit('Stage3AC title edge guard failed: ' + bytes(b[to:to+5]).hex())
    if bytes(b[co:co+5]) != CLEANUP_CALL_STAGE3AC:
        raise SystemExit('Stage3AC cleanup edge guard failed: ' + bytes(b[co:co+5]).hex())

    # Guard Stage3X's protection/flush helper used by Stage3AD runtime patcher.
    h = roff(pe, DEFS['STAGE3X_RESTORE_PROTECT_FLUSH_RVA'])
    if bytes(b[h:h+4]) != bytes.fromhex('4883ec38'):
        raise SystemExit('Stage3X restore/flush helper identity guard failed')

    for sec, data in raw.items():
        o = roff(pe, SECTIONS[sec])
        b[o:o+len(data)] = data

    b[to:to+5] = relcall(TITLE_CALL_RVA, syms['stage3ad_h4_title_wrapper'])
    b[co:co+5] = relcall(CLEANUP_EFFECT_CALL_RVA, syms['stage3ad_h4_cleanup_wrapper'])

    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(b)
    print('input', sha)
    for sec, data in raw.items():
        print(sec, hex(SECTIONS[sec]), len(data), hashlib.sha256(data).hexdigest())
    print('output', hashlib.sha256(b).hexdigest(), len(b))
    print('SizeOfImage', hex(pe['size_image']), 'sections', pe['n'])
    print('symbols', json.dumps({k: hex(v) for k, v in syms.items()}, sort_keys=True))
    print('title_call', TITLE_CALL_STAGE3AC.hex(), '->', bytes(b[to:to+5]).hex())
    print('cleanup_call', CLEANUP_CALL_STAGE3AC.hex(), '->', bytes(b[co:co+5]).hex())
    print('runtime_h4_patches', json.dumps({k: {'rva': hex(v[0]), 'stock': v[1].hex(), 'stage3ad': v[2].hex()} for k,v in H4_RUNTIME_PATCHES.items()}, sort_keys=True))


if __name__ == '__main__':
    main()
