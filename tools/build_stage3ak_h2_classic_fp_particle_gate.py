from pathlib import Path
import hashlib, shutil, struct, subprocess, sys, tempfile

EXPECTED_STAGE3AI_SHA256 = "36232bc077d1ca4f5080bf514f93fb5b70f746ec3baaba5e75c46a66e3a2d0a8"
CODE_RVA = 0x002F6000
DATA_RVA = 0x002F7000

# Exact Stage3AI H2 lifecycle edges. These are deliberately H2-specific; Stage3AJ
# failed because its installer was wired through a generic active-title query edge.
INSTALL_STORE_RVA = 0x0008F66B
INSTALL_STORE_EXPECT = bytes.fromhex("48 89 05 C6 B4 22 00")
CLEAN_RESTORE_CALL_RVA = 0x00090AC4
CLEAN_RESTORE_EXPECT = bytes.fromhex("E8 37 05 26 00")  # -> Stage3I +0x2F1000
CLEAN_RELEASE_CALL_RVA = 0x00090CE2
CLEAN_RELEASE_EXPECT = bytes.fromhex("E8 7F 03 26 00")  # -> Stage3I +0x2F1066

DEFS = {
    "LOGGER_RVA": 0x00001D90,
    "H2_MODULE_REFERENCE_RVA": 0x002BAB38,
    "IAT_VIRTUAL_PROTECT_RVA": 0x00180200,
    "RESTORE_PROTECT_FLUSH_RVA": 0x002F3200,
    "STAGE3I_RESTORE_PIN_RVA": 0x002F1000,
    "STAGE3I_RELEASE_PIN_RVA": 0x002F1066,
}

def parse_pe(blob):
    if blob[:2] != b"MZ": raise SystemExit("input is not MZ")
    p = struct.unpack_from("<I", blob, 0x3C)[0]
    if blob[p:p+4] != b"PE\0\0": raise SystemExit("input is not PE")
    coff = p + 4
    n = struct.unpack_from("<H", blob, coff + 2)[0]
    osz = struct.unpack_from("<H", blob, coff + 16)[0]
    opt = coff + 20
    if struct.unpack_from("<H", blob, opt)[0] != 0x20B: raise SystemExit("not PE32+")
    st = opt + osz
    sections=[]
    for i in range(n):
        o=st+i*40
        name=bytes(blob[o:o+8]).split(b"\0",1)[0].decode("ascii")
        vs,va,rs,rp=struct.unpack_from("<IIII",blob,o+8)
        ch=struct.unpack_from("<I",blob,o+36)[0]
        sections.append(dict(name=name,vs=vs,va=va,rs=rs,rp=rp,h=o,ch=ch))
    return dict(coff=coff,opt=opt,st=st,n=n,sections=sections,
                fa=struct.unpack_from("<I",blob,opt+0x24)[0],
                sa=struct.unpack_from("<I",blob,opt+0x20)[0],
                sizeimg=struct.unpack_from("<I",blob,opt+0x38)[0])

def rva_off(pe,rva):
    for s in pe["sections"]:
        if s["va"] <= rva < s["va"] + max(s["vs"],s["rs"]):
            return s["rp"] + (rva-s["va"])
    raise KeyError(hex(rva))

def rel_call(src,dst): return b"\xE8"+struct.pack("<i",dst-(src+5))

def assemble(asm):
    missing=[x for x in ("as","ld","objcopy","nm") if shutil.which(x) is None]
    if missing: raise SystemExit("missing GNU binutils: "+", ".join(missing))
    with tempfile.TemporaryDirectory(prefix="halomccvr-stage3ak-") as td:
        td=Path(td); obj=td/"s3ak.o"; elf=td/"s3ak.elf"; code=td/"code.bin"; data=td/"data.bin"
        subprocess.run(["as","--64","-o",str(obj),str(asm)],check=True)
        cmd=["ld","-m","elf_x86_64",f"--section-start=.text=0x{CODE_RVA:x}",f"--section-start=.data=0x{DATA_RVA:x}"]
        for k,v in DEFS.items(): cmd += ["--defsym",f"{k}=0x{v:x}"]
        cmd += ["-o",str(elf),str(obj)]
        subprocess.run(cmd,check=True)
        subprocess.run(["objcopy","-O","binary","-j",".text",str(elf),str(code)],check=True)
        subprocess.run(["objcopy","-O","binary","-j",".data",str(elf),str(data)],check=True)
        sy={}
        for line in subprocess.check_output(["nm","-n",str(elf)],text=True).splitlines():
            f=line.split()
            if len(f)==3 and f[2].startswith("stage3ak_"): sy[f[2]]=int(f[0],16)
        need={"stage3ak_install_and_store","stage3ak_cleanup_restore","stage3ak_cleanup_release","stage3ak_h2_particle_gate"}
        if not need.issubset(sy): raise SystemExit("missing Stage3AK symbols")
        return code.read_bytes(),data.read_bytes(),sy

def guard(blob,pe,rva,expected,name):
    o=rva_off(pe,rva); actual=bytes(blob[o:o+len(expected)])
    if actual != expected: raise SystemExit(f"{name}: expected {expected.hex()} at 0x{rva:X}, got {actual.hex()}")
    return o

def main():
    if len(sys.argv) not in (3,4):
        raise SystemExit("usage: build_stage3ak_h2_classic_fp_particle_gate.py <Stage3AI-HaloMCCVR.dll> <output.dll> [asm]")
    src=Path(sys.argv[1]); out=Path(sys.argv[2]); asm=Path(sys.argv[3]) if len(sys.argv)==4 else Path(__file__).with_name("stage3ak_h2_classic_fp_particle_gate.S")
    ib=src.read_bytes(); sha=hashlib.sha256(ib).hexdigest()
    if sha != EXPECTED_STAGE3AI_SHA256: raise SystemExit("wrong Stage3AI input DLL: "+sha)
    code,data,sy=assemble(asm)
    if len(code)>0x1000 or len(data)>0x1000: raise SystemExit("Stage3AK payload exceeds two pages")
    blob=bytearray(ib); pe=parse_pe(blob)
    if pe["n"] != 12 or pe["sizeimg"] != 0x2F6000: raise SystemExit(f"unexpected Stage3AI PE geometry: n={pe['n']} image={pe['sizeimg']:#x}")
    qd=next((s for s in pe["sections"] if s["name"]==".s3qd"),None)
    if not qd or qd["va"]!=0x2F3000 or qd["vs"]!=0x3000 or qd["rs"]!=0x3000:
        raise SystemExit("unexpected Stage3AI .s3qd geometry")
    if qd["rp"]+qd["rs"] != len(blob): raise SystemExit("unexpected overlay after .s3qd")

    install_o=guard(blob,pe,INSTALL_STORE_RVA,INSTALL_STORE_EXPECT,"H2 successful module-reference store")
    clean1_o=guard(blob,pe,CLEAN_RESTORE_CALL_RVA,CLEAN_RESTORE_EXPECT,"Stage3I restore/pin call")
    clean2_o=guard(blob,pe,CLEAN_RELEASE_CALL_RVA,CLEAN_RELEASE_EXPECT,"Stage3I release/clear call")

    # Append beyond Stage3AI's original .s3qd bytes. The original 0x3000-byte
    # .s3qd payload (including its live state at the beginning) is not rewritten.
    blob.extend(code.ljust(0x1000,b"\0")); blob.extend(data.ljust(0x1000,b"\0"))
    struct.pack_into("<I",blob,qd["h"]+8,0x5000)
    struct.pack_into("<I",blob,qd["h"]+16,0x5000)
    struct.pack_into("<I",blob,pe["opt"]+0x38,0x2F8000)
    struct.pack_into("<I",blob,pe["opt"]+8,struct.unpack_from("<I",blob,pe["opt"]+8)[0]+0x2000)

    blob[install_o:install_o+7] = rel_call(INSTALL_STORE_RVA,sy["stage3ak_install_and_store"])+b"\x90\x90"
    blob[clean1_o:clean1_o+5] = rel_call(CLEAN_RESTORE_CALL_RVA,sy["stage3ak_cleanup_restore"])
    blob[clean2_o:clean2_o+5] = rel_call(CLEAN_RELEASE_CALL_RVA,sy["stage3ak_cleanup_release"])
    struct.pack_into("<I",blob,pe["opt"]+0x40,0)
    out.write_bytes(blob)
    print("input",sha)
    print("code",hashlib.sha256(code).hexdigest(),len(code))
    print("data",hashlib.sha256(data).hexdigest(),len(data))
    print("symbols",{k:hex(v) for k,v in sorted(sy.items())})
    print("output",hashlib.sha256(blob).hexdigest(),len(blob))

if __name__ == "__main__": main()
