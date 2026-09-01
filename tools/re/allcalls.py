import sys, struct
sys.path.insert(0, r"C:\Users\pancr\AppData\Local\Temp\claude\n--dev-HaloMCCVR-518aebe-C50-Stage3AL-ODST-Teardown-Pin-Source\4662d921-f236-401c-92f9-bbf5281068d2\scratchpad")
from pe import PE
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

TARGET = 0x879C0
pe = PE(sys.argv[1]); s, data = pe.text(); base = s['va']
md = Cs(CS_ARCH_X86, CS_MODE_64)
sites = []
for i in range(len(data) - 5):
    if data[i] != 0xE8:
        continue
    rel = struct.unpack_from('<i', data, i+1)[0]
    if base + i + 5 + rel == TARGET:
        sites.append(base + i)
print(f"{len(sites)} calls to 0x{TARGET:X}")
for site in sites:
    off = site - base + 5
    ins = list(md.disasm(data[off:off+20], site+5))[:4]
    print(f"0x{site:06X}: " + " | ".join(f"{i.mnemonic} {i.op_str}" for i in ins))
