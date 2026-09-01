import sys, struct, re
sys.path.insert(0, r'C:\Users\pancr\AppData\Local\Temp\claude\n--dev-HaloMCCVR-518aebe-C50-Stage3AL-ODST-Teardown-Pin-Source\4662d921-f236-401c-92f9-bbf5281068d2\scratchpad')
from pe import PE
pe = PE(r'N:\dev\HaloMCCVR-518aebe-C50-Stage3AL-ODST-Teardown-Pin-Source\built\Stage3AL-HaloMCCVR.dll')
rd = pe.sec('.rdata')
data = pe.blob[rd['rp']:rd['rp']+rd['rs']]
for val, name in ((2048.0,'2048.0f (512*4)'), (-768.0,'-768.0f (TopLeft)')):
    pat = struct.pack('<f', val)
    hits = [rd['va']+m.start() for m in re.finditer(re.escape(pat), data) if (m.start() % 4)==0]
    print('%-22s %d in .rdata' % (name, len(hits)))
    for h in hits[:14]:
        refs = pe.rip_refs(h)
        if refs:
            print('   const 0x%06X refs: %s' % (h, ' '.join('0x%X'%r for r in refs[:6])))
