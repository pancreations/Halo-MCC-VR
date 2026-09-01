import sys, re, struct
sys.path.insert(0, r'C:\Users\pancr\AppData\Local\Temp\claude\n--dev-HaloMCCVR-518aebe-C50-Stage3AL-ODST-Teardown-Pin-Source\4662d921-f236-401c-92f9-bbf5281068d2\scratchpad')
from pe import PE
pe = PE(r'N:\dev\HaloMCCVR-518aebe-C50-Stage3AL-ODST-Teardown-Pin-Source\built\Stage3AL-HaloMCCVR.dll')
s, data = pe.text(); base = s['va']
def sites(off):
    out=[]
    for pre in (b'\xFF', ):
        for modrm in range(0x90, 0x98):
            p = bytes([0xFF, modrm]) + struct.pack('<I', off)
            out += [base+m.start() for m in re.finditer(re.escape(p), data)]
        for modrm in range(0x90, 0x98):   # REX.B forms
            p = bytes([0x41, 0xFF, modrm]) + struct.pack('<I', off)
            out += [base+m.start() for m in re.finditer(re.escape(p), data)]
    return sorted(out)
omget = sites(0x2C8); rsget = sites(0x300); rsscis = sites(0x308)
print('OMGetRenderTargets:', ' '.join('0x%X'%x for x in omget))
print('RSGetViewports    :', ' '.join('0x%X'%x for x in rsget))
print('RSGetScissorRects :', ' '.join('0x%X'%x for x in rsscis))
