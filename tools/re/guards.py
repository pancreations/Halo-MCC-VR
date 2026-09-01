import sys
sys.path.insert(0, r'C:\Users\pancr\AppData\Local\Temp\claude\n--dev-HaloMCCVR-518aebe-C50-Stage3AL-ODST-Teardown-Pin-Source\4662d921-f236-401c-92f9-bbf5281068d2\scratchpad')
from pe import PE
pe = PE(r'N:\dev\HaloMCCVR-518aebe-C50-Stage3AL-ODST-Teardown-Pin-Source\built\Stage3AQ-HaloMCCVR.dll')
for rva, n in ((0x053780, 12), (0x005CA0, 8), (0x00537BE, 4), (0x005CE9, 4), (0x005CA5, 4)):
    o = pe.off(rva)
    print('0x%06X  %s' % (rva, pe.blob[o:o+n].hex(' ')))
