import sys, struct
sys.path.insert(0, r"C:\Users\pancr\AppData\Local\Temp\claude\n--dev-HaloMCCVR-518aebe-C50-Stage3AL-ODST-Teardown-Pin-Source\4662d921-f236-401c-92f9-bbf5281068d2\scratchpad")
from pe import PE
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

SITES = [0x0203D7,0x02C2A8,0x02C62C,0x02FF4D,0x03022B,0x043B18,0x044A3F,0x045C5C,
         0x046742,0x047184,0x0477BE,0x051A95,0x051AEC,0x053424,0x055ABE,0x05729D,0x0572E0]
pe = PE(sys.argv[1])
s, data = pe.text()
base = s['va']
md = Cs(CS_ARCH_X86, CS_MODE_64)

def prologue_distance(site):
    """Bytes back to the nearest 0xCC padding run (function entry marker)."""
    off = site - base
    for d in range(1, 400):
        if data[off-d] == 0xCC and data[off-d-1] == 0xCC:
            return d, base + off - d + 1
    return None, None

for site in SITES:
    d, entry = prologue_distance(site)
    off = site - base
    ins = list(md.disasm(data[off:off+26], site))[:6]
    txt = " | ".join(f"{i.mnemonic} {i.op_str}" for i in ins)
    e = f"0x{entry:06X}" if entry else "?"; print(f"site 0x{site:06X}  entry {e} (+{d})  {txt}")
