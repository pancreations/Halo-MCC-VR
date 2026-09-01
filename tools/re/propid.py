import sys, re, struct
sys.path.insert(0, r'C:\Users\pancr\AppData\Local\Temp\claude\n--dev-HaloMCCVR-518aebe-C50-Stage3AL-ODST-Teardown-Pin-Source\4662d921-f236-401c-92f9-bbf5281068d2\scratchpad')
from pe import PE
pe = PE(r'N:\SteamLibrary\steamapps\common\Halo The Master Chief Collection\halo4\halo4.dll')
s, data = pe.text()
base = s['va']
for pid, label in ((0x0038041F,'prop_hud_helmet_visible'), (0x0038025B,'prop_curvature_theta'), (0x00380422,'prop_hud_reticule_visible')):
    pat = struct.pack('<I', pid)
    hits = [m.start() for m in re.finditer(re.escape(pat), data)]
    print('%s id=0x%08X  .text hits: %d  %s' % (label, pid, len(hits), ' '.join('0x%X' % (base+h) for h in hits[:10])))
