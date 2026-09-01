import sys
sys.path.insert(0, r'C:\Users\pancr\AppData\Local\Temp\claude\n--dev-HaloMCCVR-518aebe-C50-Stage3AL-ODST-Teardown-Pin-Source\4662d921-f236-401c-92f9-bbf5281068d2\scratchpad')
from pe import PE
pe = PE(r'N:\SteamLibrary\steamapps\common\Halo The Master Chief Collection\halo4\halo4.dll')
for rva, label in ((0xCE7098,'helmet record'), (0xCE5458,'curvature record')):
    hits = pe.rip_refs(rva)
    print('%s 0x%X -> .text refs: %d %s' % (label, rva, len(hits), ' '.join('0x%X'%h for h in hits[:8])))
# where does this table start / end?
import struct
IB = pe.image_base
def isptr(rva):
    o = pe.off(rva); v = struct.unpack_from('<Q', pe.blob, o)[0]
    return IB < v < IB + 0x2000000
lo = 0xCE7098
while isptr(lo - 0x10): lo -= 0x10
hi = 0xCE7098
while isptr(hi + 0x10): hi += 0x10
print('table spans .rdata 0x%X .. 0x%X (%d records)' % (lo, hi, (hi-lo)//0x10 + 1))
print('refs to table start:', ' '.join('0x%X'%h for h in pe.rip_refs(lo)[:8]))
