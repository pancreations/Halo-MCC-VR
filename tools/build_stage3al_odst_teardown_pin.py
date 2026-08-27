from pathlib import Path
import hashlib, shutil, struct, subprocess, sys, tempfile

EXPECTED_STAGE3AK_SHA256 = "4ce06fc1e33040990b4db630748d07688803a7216c74cd9d89484c9403510389"
CODE_RVA = 0x002F8000

# Post-preflight store of the identity-only handle. Replace this one ownership
# store with a call that acquires a real loader pin only after all proofs pass.
ODST_MODULE_STORE_RVA = 0x0005C2A6
ODST_MODULE_STORE_EXPECT = bytes.fromhex("48 89 05 2B B1 25 00")
ODST_ROLLBACK_RELEASE_CALL_RVA = 0x00041BD6
ODST_TEARDOWN_RELEASE_CALL_RVA = 0x00084E45
ODST_CLEAR_POINTERS_RVA = 0x0007C710

DEFS = {
    "ODST_MODULE_REFERENCE_RVA": 0x002B73D8,
    "ODST_INSTALL_MAPPING_FAIL_RVA": 0x0005CC1B,
    "ODST_CLEAR_POINTERS_RVA": ODST_CLEAR_POINTERS_RVA,
    "IAT_GET_MODULE_HANDLE_EX_W_RVA": 0x00180160,
    "IAT_FREE_LIBRARY_RVA": 0x00180230,
}

def parse_pe(blob):
    if blob[:2] != b"MZ": raise SystemExit("input is not MZ")
    p=struct.unpack_from("<I",blob,0x3C)[0]
    if blob[p:p+4] != b"PE\0\0": raise SystemExit("input is not PE")
    coff=p+4; n=struct.unpack_from("<H",blob,coff+2)[0]
    osz=struct.unpack_from("<H",blob,coff+16)[0]; opt=coff+20
    if struct.unpack_from("<H",blob,opt)[0] != 0x20B: raise SystemExit("not PE32+")
    st=opt+osz; secs=[]
    for i in range(n):
        o=st+i*40; name=bytes(blob[o:o+8]).split(b"\0",1)[0].decode("ascii")
        vs,va,rs,rp=struct.unpack_from("<IIII",blob,o+8)
        secs.append(dict(name=name,vs=vs,va=va,rs=rs,rp=rp,h=o))
    return dict(coff=coff,opt=opt,n=n,sections=secs,sizeimg=struct.unpack_from("<I",blob,opt+0x38)[0])

def rva_off(pe,rva):
    for s in pe["sections"]:
        if s["va"] <= rva < s["va"] + max(s["vs"],s["rs"]): return s["rp"] + rva-s["va"]
    raise KeyError(hex(rva))

def rel_call(src,dst): return b"\xE8"+struct.pack("<i",dst-(src+5))

def assemble(asm):
    missing=[x for x in ("as","ld","objcopy","nm") if shutil.which(x) is None]
    if missing: raise SystemExit("missing GNU binutils: "+", ".join(missing))
    with tempfile.TemporaryDirectory(prefix="halomccvr-stage3al-") as td:
        td=Path(td); obj=td/"s3al.o"; elf=td/"s3al.elf"; code=td/"code.bin"
        subprocess.run(["as","--64","-o",str(obj),str(asm)],check=True)
        cmd=["ld","-m","elf_x86_64",f"--section-start=.text=0x{CODE_RVA:x}"]
        for k,v in DEFS.items(): cmd += ["--defsym",f"{k}=0x{v:x}"]
        cmd += ["-o",str(elf),str(obj)]
        subprocess.run(cmd,check=True)
        subprocess.run(["objcopy","-O","binary","-j",".text",str(elf),str(code)],check=True)
        sy={}
        for line in subprocess.check_output(["nm","-n",str(elf)],text=True).splitlines():
            f=line.split()
            if len(f)==3 and f[2].startswith("stage3al_"): sy[f[2]]=int(f[0],16)
        need={"stage3al_acquire_odst_pin","stage3al_release_odst_pin"}
        if not need.issubset(sy): raise SystemExit("missing Stage3AL symbols")
        return code.read_bytes(),sy

def guard(blob,pe,rva,expected,name):
    o=rva_off(pe,rva); actual=bytes(blob[o:o+len(expected)])
    if actual != expected: raise SystemExit(f"{name}: expected {expected.hex()} at 0x{rva:X}, got {actual.hex()}")
    return o

def main():
    if len(sys.argv) not in (3,4): raise SystemExit("usage: build_stage3al_odst_teardown_pin.py <Stage3AK-HaloMCCVR.dll> <output.dll> [asm]")
    src=Path(sys.argv[1]); out=Path(sys.argv[2]); asm=Path(sys.argv[3]) if len(sys.argv)==4 else Path(__file__).with_name("stage3al_odst_teardown_pin.S")
    ib=src.read_bytes(); sha=hashlib.sha256(ib).hexdigest()
    if sha != EXPECTED_STAGE3AK_SHA256: raise SystemExit("wrong Stage3AK input DLL: "+sha)
    code,sy=assemble(asm)
    if len(code)>0x1000: raise SystemExit("Stage3AL payload exceeds one page")
    blob=bytearray(ib); pe=parse_pe(blob)
    if pe["n"] != 12 or pe["sizeimg"] != 0x2F8000: raise SystemExit(f"unexpected Stage3AK PE geometry: n={pe['n']} image={pe['sizeimg']:#x}")
    qd=next((s for s in pe["sections"] if s["name"]==".s3qd"),None)
    if not qd or qd["va"]!=0x2F3000 or qd["vs"]!=0x5000 or qd["rs"]!=0x5000: raise SystemExit("unexpected Stage3AK .s3qd geometry")
    if qd["rp"]+qd["rs"] != len(blob): raise SystemExit("unexpected overlay after .s3qd")

    store_o=guard(blob,pe,ODST_MODULE_STORE_RVA,ODST_MODULE_STORE_EXPECT,"ODST post-preflight module store")
    old=lambda r: rel_call(r,ODST_CLEAR_POINTERS_RVA)
    rollback_o=guard(blob,pe,ODST_ROLLBACK_RELEASE_CALL_RVA,old(ODST_ROLLBACK_RELEASE_CALL_RVA),"ODST rollback release call")
    teardown_o=guard(blob,pe,ODST_TEARDOWN_RELEASE_CALL_RVA,old(ODST_TEARDOWN_RELEASE_CALL_RVA),"ODST teardown release call")

    blob.extend(code.ljust(0x1000,b"\0"))
    struct.pack_into("<I",blob,qd["h"]+8,0x6000); struct.pack_into("<I",blob,qd["h"]+16,0x6000)
    struct.pack_into("<I",blob,pe["opt"]+0x38,0x2F9000)
    struct.pack_into("<I",blob,pe["opt"]+8,struct.unpack_from("<I",blob,pe["opt"]+8)[0]+0x1000)

    blob[store_o:store_o+7] = rel_call(ODST_MODULE_STORE_RVA,sy["stage3al_acquire_odst_pin"])+b"\x90\x90"
    blob[rollback_o:rollback_o+5] = rel_call(ODST_ROLLBACK_RELEASE_CALL_RVA,sy["stage3al_release_odst_pin"])
    blob[teardown_o:teardown_o+5] = rel_call(ODST_TEARDOWN_RELEASE_CALL_RVA,sy["stage3al_release_odst_pin"])
    struct.pack_into("<I",blob,pe["opt"]+0x40,0)
    out.write_bytes(blob)
    print("input",sha); print("code",hashlib.sha256(code).hexdigest(),len(code)); print("symbols",{k:hex(v) for k,v in sy.items()}); print("output",hashlib.sha256(blob).hexdigest(),len(blob))

if __name__ == "__main__": main()
