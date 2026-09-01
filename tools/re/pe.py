"""Minimal PE reader + RIP-relative reference finder for HaloMCCVR.dll."""
import struct

class PE:
    def __init__(self, path):
        self.blob = open(path, 'rb').read()
        b = self.blob
        p = struct.unpack_from('<I', b, 0x3C)[0]
        assert b[p:p+4] == b'PE\0\0'
        coff = p + 4
        n = struct.unpack_from('<H', b, coff+2)[0]
        osz = struct.unpack_from('<H', b, coff+16)[0]
        opt = coff + 20
        self.image_base = struct.unpack_from('<Q', b, opt+24)[0]
        tab = opt + osz
        self.sections = []
        for i in range(n):
            o = tab + i*40
            name = bytes(b[o:o+8]).split(b'\0', 1)[0].decode('ascii')
            vs, va, rs, rp = struct.unpack_from('<IIII', b, o+8)
            self.sections.append(dict(name=name, vs=vs, va=va, rs=rs, rp=rp))

    def sec(self, name):
        return next(s for s in self.sections if s['name'] == name)

    def off(self, rva):
        for s in self.sections:
            if s['va'] <= rva < s['va'] + max(s['vs'], s['rs']):
                return s['rp'] + (rva - s['va'])
        raise KeyError(hex(rva))

    def rva(self, off):
        for s in self.sections:
            if s['rp'] <= off < s['rp'] + s['rs']:
                return s['va'] + (off - s['rp'])
        raise KeyError(hex(off))

    def text(self):
        s = self.sec('.text')
        return s, self.blob[s['rp']:s['rp']+s['rs']]

    def rip_refs(self, target_rva, section='.text'):
        """Every position whose trailing disp32 would resolve to target_rva."""
        s = self.sec(section)
        data = self.blob[s['rp']:s['rp']+s['rs']]
        hits = []
        for i in range(len(data) - 4):
            disp = struct.unpack_from('<i', data, i)[0]
            if s['va'] + i + 4 + disp == target_rva:
                hits.append(s['va'] + i)
        return hits
