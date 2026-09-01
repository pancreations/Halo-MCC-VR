import sys, struct, collections
sys.path.insert(0, r"C:\Users\pancr\AppData\Local\Temp\claude\n--dev-HaloMCCVR-518aebe-C50-Stage3AL-ODST-Teardown-Pin-Source\4662d921-f236-401c-92f9-bbf5281068d2\scratchpad")
from pe import PE

pe = PE(sys.argv[1])
s, data = pe.text()
base = s['va']

# call rel32 immediately followed by `cmp al, imm8` (3C xx)
targets = collections.Counter()
sites = collections.defaultdict(list)
for i in range(len(data) - 7):
    if data[i] != 0xE8 or data[i+5] != 0x3C:
        continue
    rel = struct.unpack_from('<i', data, i+1)[0]
    tgt = base + i + 5 + rel
    if not (base <= tgt < base + len(data)):
        continue
    targets[tgt] += 1
    sites[tgt].append((base + i, data[i+6]))

print("most-called functions followed by `cmp al, imm8`:")
for tgt, n in targets.most_common(6):
    imms = collections.Counter(v for _, v in sites[tgt])
    print(f"  0x{tgt:06X}  {n:4d} sites   imm8 histogram {dict(imms)}")

hot = targets.most_common(1)[0][0]
print(f"\ncall sites of 0x{hot:06X} with `cmp al, 4` (GameTitle::Halo4):")
for addr, imm in sites[hot]:
    if imm == 4:
        print(f"  0x{addr:06X}")
