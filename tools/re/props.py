import sys, struct
sys.path.insert(0, r'C:\Users\pancr\AppData\Local\Temp\claude\n--dev-HaloMCCVR-518aebe-C50-Stage3AL-ODST-Teardown-Pin-Source\4662d921-f236-401c-92f9-bbf5281068d2\scratchpad')
from pe import PE
pe = PE(r'N:\SteamLibrary\steamapps\common\Halo The Master Chief Collection\halo4\halo4.dll')
IB = pe.image_base
def cstr(rva, n=64):
    try:
        o = pe.off(rva); b = pe.blob[o:o+n]; return b.split(b'\x00',1)[0].decode('latin1')
    except Exception: return '?'
def dump(label, center, before=6, after=6, stride=0x18):
    print('===', label, 'around .rdata RVA 0x%X' % center)
    for k in range(-before, after+1):
        rva = center + k*stride
        try: o = pe.off(rva)
        except Exception: continue
        raw = pe.blob[o:o+stride]
        ptr = struct.unpack_from('<Q', raw, 0)[0]
        name = cstr(ptr - IB) if ptr > IB else ''
        print('  0x%08X %s  name=%r' % (rva, raw.hex(' '), name))
dump('curvature', 0xCE5458)
print()
dump('helmet', 0xCE7098)
