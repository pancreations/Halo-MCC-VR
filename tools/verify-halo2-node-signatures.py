import io, os, struct, sys, hashlib

CANDIDATES = [
    r'N:\SteamLibrary\steamapps\common\Halo The Master Chief Collection\halo2\halo2.dll',
    r'N:\XBOX\Halo- The Master Chief Collection\Content\halo2\halo2.dll',
]

def find_dll():
    found = []
    for root in [r'N:\SteamLibrary\steamapps\common\Halo The Master Chief Collection',
                 r'N:\XBOX\Halo- The Master Chief Collection']:
        if not os.path.isdir(root):
            continue
        for dirpath, dirnames, filenames in os.walk(root):
            for f in filenames:
                if f.lower() == 'halo2.dll':
                    found.append(os.path.join(dirpath, f))
    return found

def map_image(path):
    data = open(path, 'rb').read()
    e_lfanew = struct.unpack_from('<I', data, 0x3C)[0]
    assert data[e_lfanew:e_lfanew+4] == b'PE\0\0', 'not a PE'
    coff = e_lfanew + 4
    num_sections, = struct.unpack_from('<H', data, coff + 2)
    opt_size, = struct.unpack_from('<H', data, coff + 16)
    opt = coff + 20
    magic, = struct.unpack_from('<H', data, opt)
    size_of_image, = struct.unpack_from('<I', data, opt + 56)
    sec = opt + opt_size
    image = bytearray(size_of_image)
    for i in range(num_sections):
        off = sec + i * 40
        vsize, vaddr, rawsize, rawptr = struct.unpack_from('<IIII', data, off + 8)
        chunk = data[rawptr:rawptr + rawsize]
        n = min(len(chunk), max(0, size_of_image - vaddr))
        image[vaddr:vaddr + n] = chunk[:n]
    return bytes(image), size_of_image, len(data), hashlib.sha256(data).hexdigest().upper()

def compile_pattern(text):
    out = []
    for tok in text.split():
        out.append(None if tok.startswith('?') else int(tok, 16))
    return out

def scan(image, pat):
    n = len(pat)
    first = pat[0]
    hits = []
    start = 0
    while True:
        i = image.find(bytes([first]), start)
        if i < 0 or i + n > len(image):
            break
        ok = True
        for k in range(1, n):
            p = pat[k]
            if p is not None and image[i + k] != p:
                ok = False
                break
        if ok:
            hits.append(i)
        start = i + 1
    return hits

PATTERNS = {
    'animation_graph_definition_get (+8)': (
        'CC CC CC CC CC CC CC CC 48 8B 05 ?? ?? ?? ?? 0F B7 C9 48 03 C9 48 63 44 C8 08 48 03 05 ?? ?? ?? ?? C3',
        0x0079EEA0, 8),
    'get_skeleton_node (+0)': (
        '48 63 41 10 33 C9 83 F8 FF 74 26 85 C0 79 18 8B C8 48 63 C2 0F BA F1 1F 48 C1 E0 05 48 03 0D ?? ?? ?? ?? 48 03 C1 C3 48 8B C8 48 03 0D ?? ?? ?? ?? 48 63 C2 48 C1 E0 05 48 03 C1 C3',
        0x0079F430, 0),
    'find_node_by_model_flags (+0)': (
        '48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 48 63 41 0C 45 33 DB 85 C0 7E 53 4C 63 41 10 45 33 C9 48 8B 35 ?? ?? ?? ?? 45 33 D2 48 8B 3D ?? ?? ?? ?? 48 8B D8 33 C0 41 83 F8 FF 74 15 45 85 C0 79 0C 41 8B C0 0F BA F0 1F 48 03 C6 EB 04 4A 8D 04 07 41 0F B6 4C 02 0A 23 CA 3B CA 74 24',
        0x0079E8D0, 0),
    'get_node_count (+15) [identity only]': (
        '48 63 C2 48 C1 E0 05 48 03 C1 C3 CC CC CC CC 0F B7 41 0C C3',
        0x0079F470, 15),
}

paths = [p for p in CANDIDATES if os.path.isfile(p)] or find_dll()
if not paths:
    print('NO halo2.dll FOUND')
    sys.exit(1)

for path in paths:
    image, soi, filesize, sha = map_image(path)
    print('=' * 78)
    print(path)
    print('  file size %d  SizeOfImage 0x%08X  SHA-256 %s' % (filesize, soi, sha))
    print('  pinned    15807960              0x02A38000')
    for name, (pat, rva, skew) in PATTERNS.items():
        hits = scan(image, compile_pattern(pat))
        verdict = 'FAIL'
        if len(hits) == 1 and hits[0] + skew == rva:
            verdict = 'OK'
        print('  %-42s matches=%d  %s  expected RVA 0x%08X  %s'
              % (name, len(hits),
                 ('0x%08X' % (hits[0] + skew)) if hits else '   ----   ',
                 rva, verdict))
