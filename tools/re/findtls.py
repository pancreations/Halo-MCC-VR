import sys, re
sys.path.insert(0, r'C:\Users\pancr\AppData\Local\Temp\claude\n--dev-HaloMCCVR-518aebe-C50-Stage3AL-ODST-Teardown-Pin-Source\4662d921-f236-401c-92f9-bbf5281068d2\scratchpad')
from pe import PE
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
pe = PE(r'N:\dev\HaloMCCVR-518aebe-C50-Stage3AL-ODST-Teardown-Pin-Source\built\Stage3AQ-HaloMCCVR.dll')
qd = [s for s in pe.sections if s['name']=='.s3qd'][0]
data = pe.blob[qd['rp']:qd['rp']+qd['rs']]
base = qd['va']
md = Cs(CS_ARCH_X86, CS_MODE_64)
pat = bytes.fromhex('65488b042558000000')     # mov rax, qword ptr gs:[0x58]
hits = [m.start() for m in re.finditer(re.escape(pat), data)]
print('gs:[0x58] sites in .s3qd:', ' '.join('0x%X' % (base+h) for h in hits))
for h in hits[:3]:
    a = base + h - 7
    for i in md.disasm(data[a-base:a-base+40], a):
        print('   0x%06X  %-8s %s' % (i.address, i.mnemonic, i.op_str))
    print()
