import sys
sys.path.insert(0, r"C:\Users\pancr\AppData\Local\Temp\claude\n--dev-HaloMCCVR-518aebe-C50-Stage3AL-ODST-Teardown-Pin-Source\4662d921-f236-401c-92f9-bbf5281068d2\scratchpad")
from pe import PE
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

dll, start, count = sys.argv[1], int(sys.argv[2], 0), int(sys.argv[3], 0)
pe = PE(dll)
off = pe.off(start)
data = pe.blob[off:off+count]
md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = False
for ins in md.disasm(data, start):
    tgt = ''
    if 'rip' in ins.op_str:
        # resolve rip-relative for readability
        try:
            import re
            m = re.search(r'rip \+ (0x[0-9a-f]+)|rip - (0x[0-9a-f]+)', ins.op_str)
            if m:
                d = int(m.group(1), 16) if m.group(1) else -int(m.group(2), 16)
                tgt = f"   ; -> 0x{ins.address + ins.size + d:X}"
        except Exception:
            pass
    print(f"0x{ins.address:06X}  {ins.bytes.hex():<24} {ins.mnemonic:<8} {ins.op_str}{tgt}")
