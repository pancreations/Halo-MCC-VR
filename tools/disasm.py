# Disassemble a range of halo3.dll in the running game, by RVA.
# Read-only: attaches with PROCESS_VM_READ and uses capstone. No game files touched.
#   py -3 disasm.py <rva_hex> <length>
import sys, ctypes as C
from ctypes import wintypes as W
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

GAME = "MCC-Win64-Shipping.exe"
DLL  = "halo3.dll"
TH32CS_SNAPPROCESS = 0x2
TH32CS_SNAPMODULE   = 0x8
TH32CS_SNAPMODULE32 = 0x10
PROCESS_VM_READ = 0x10
PROCESS_QUERY_INFORMATION = 0x400

k32 = C.WinDLL("kernel32", use_last_error=True)

class PROCESSENTRY32(C.Structure):
    _fields_ = [("dwSize",W.DWORD),("cntUsage",W.DWORD),("th32ProcessID",W.DWORD),
        ("th32DefaultHeapID",C.POINTER(C.c_ulong)),("th32ModuleID",W.DWORD),
        ("cntThreads",W.DWORD),("th32ParentProcessID",W.DWORD),("pcPriClassBase",C.c_long),
        ("dwFlags",W.DWORD),("szExeFile",C.c_char*260)]

class MODULEENTRY32(C.Structure):
    _fields_ = [("dwSize",W.DWORD),("th32ModuleID",W.DWORD),("th32ProcessID",W.DWORD),
        ("GlblcntUsage",W.DWORD),("ProccntUsage",W.DWORD),("modBaseAddr",C.POINTER(C.c_byte)),
        ("modBaseSize",W.DWORD),("hModule",W.HMODULE),("szModule",C.c_char*256),
        ("szExePath",C.c_char*260)]

def find_pid():
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    pe = PROCESSENTRY32(); pe.dwSize = C.sizeof(pe)
    ok = k32.Process32First(snap, C.byref(pe))
    pid = 0
    while ok:
        if pe.szExeFile.decode(errors="ignore").lower() == GAME.lower():
            pid = pe.th32ProcessID; break
        ok = k32.Process32Next(snap, C.byref(pe))
    k32.CloseHandle(snap); return pid

def module_base(pid):
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPMODULE|TH32CS_SNAPMODULE32, pid)
    me = MODULEENTRY32(); me.dwSize = C.sizeof(me)
    ok = k32.Module32First(snap, C.byref(me))
    base = size = 0
    while ok:
        if me.szModule.decode(errors="ignore").lower() == DLL.lower():
            base = C.cast(me.modBaseAddr, C.c_void_p).value; size = me.modBaseSize; break
        ok = k32.Module32Next(snap, C.byref(me))
    k32.CloseHandle(snap); return base, size

def main():
    rva = int(sys.argv[1], 16)
    length = int(sys.argv[2]) if len(sys.argv) > 2 else 128
    pid = find_pid()
    if not pid: print("game not running"); return
    base, size = module_base(pid)
    if not base: print("halo3.dll not loaded"); return
    h = k32.OpenProcess(PROCESS_VM_READ|PROCESS_QUERY_INFORMATION, False, pid)
    buf = (C.c_char*length)(); read = C.c_size_t(0)
    if not k32.ReadProcessMemory(h, C.c_void_p(base+rva), buf, length, C.byref(read)):
        print("read failed", C.get_last_error()); return
    k32.CloseHandle(h)
    code = bytes(buf[:read.value])
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    for insn in md.disasm(code, rva):
        print("halo3.dll+0x%X:  %-9s %s" % (insn.address, insn.mnemonic, insn.op_str))

main()
