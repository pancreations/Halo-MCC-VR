"""Read-only differential scanner for MCC's native pause state.

Capture alternating known states while MCC is running:
  py -3 tools/pause_scan.py snap paused1
  py -3 tools/pause_scan.py snap unpaused1
  py -3 tools/pause_scan.py snap paused2
  py -3 tools/pause_scan.py snap unpaused2
  py -3 tools/pause_scan.py compare paused1 unpaused1 paused2 unpaused2

Snapshots are limited to committed, readable pages belonging to the main MCC
image and halo3.dll. The tool never requests process-write access.
"""

import ctypes as C
from ctypes import wintypes as W
import json
from pathlib import Path
import struct
import sys
import time

GAME = "MCC-Win64-Shipping.exe"
MODULES = ("MCC-Win64-Shipping.exe", "halo3.dll")
OUT = Path("pause_scan")

TH32CS_SNAPPROCESS = 0x2
TH32CS_SNAPMODULE = 0x8
TH32CS_SNAPMODULE32 = 0x10
PROCESS_VM_READ = 0x10
PROCESS_QUERY_INFORMATION = 0x400
MEM_COMMIT = 0x1000
PAGE_GUARD = 0x100
PAGE_NOACCESS = 0x01

k32 = C.WinDLL("kernel32", use_last_error=True)
k32.CreateToolhelp32Snapshot.restype = W.HANDLE
k32.CreateToolhelp32Snapshot.argtypes = (W.DWORD, W.DWORD)
k32.CloseHandle.argtypes = (W.HANDLE,)
k32.Process32First.argtypes = (W.HANDLE, C.c_void_p)
k32.Process32Next.argtypes = (W.HANDLE, C.c_void_p)
k32.Module32First.argtypes = (W.HANDLE, C.c_void_p)
k32.Module32Next.argtypes = (W.HANDLE, C.c_void_p)
k32.OpenProcess.restype = W.HANDLE
k32.OpenProcess.argtypes = (W.DWORD, W.BOOL, W.DWORD)
k32.VirtualQueryEx.argtypes = (W.HANDLE, C.c_void_p, C.c_void_p, C.c_size_t)
k32.VirtualQueryEx.restype = C.c_size_t
k32.ReadProcessMemory.argtypes = (W.HANDLE, C.c_void_p, C.c_void_p,
                                  C.c_size_t, C.POINTER(C.c_size_t))


class PROCESSENTRY32(C.Structure):
    _fields_ = [("dwSize", W.DWORD), ("cntUsage", W.DWORD),
        ("th32ProcessID", W.DWORD), ("th32DefaultHeapID", C.POINTER(C.c_ulong)),
        ("th32ModuleID", W.DWORD), ("cntThreads", W.DWORD),
        ("th32ParentProcessID", W.DWORD), ("pcPriClassBase", C.c_long),
        ("dwFlags", W.DWORD), ("szExeFile", C.c_char * 260)]


class MODULEENTRY32(C.Structure):
    _fields_ = [("dwSize", W.DWORD), ("th32ModuleID", W.DWORD),
        ("th32ProcessID", W.DWORD), ("GlblcntUsage", W.DWORD),
        ("ProccntUsage", W.DWORD), ("modBaseAddr", C.POINTER(C.c_byte)),
        ("modBaseSize", W.DWORD), ("hModule", W.HMODULE),
        ("szModule", C.c_char * 256), ("szExePath", C.c_char * 260)]


class MEMORY_BASIC_INFORMATION(C.Structure):
    _fields_ = [("BaseAddress", C.c_void_p), ("AllocationBase", C.c_void_p),
        ("AllocationProtect", W.DWORD), ("PartitionId", W.WORD),
        ("RegionSize", C.c_size_t), ("State", W.DWORD),
        ("Protect", W.DWORD), ("Type", W.DWORD)]


def find_pid():
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    pe = PROCESSENTRY32()
    pe.dwSize = C.sizeof(pe)
    ok = k32.Process32First(snap, C.byref(pe))
    while ok:
        if pe.szExeFile.decode(errors="ignore").lower() == GAME.lower():
            k32.CloseHandle(snap)
            return pe.th32ProcessID
        ok = k32.Process32Next(snap, C.byref(pe))
    k32.CloseHandle(snap)
    return 0


def modules(pid):
    wanted = {name.lower() for name in MODULES}
    found = {}
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid)
    me = MODULEENTRY32()
    me.dwSize = C.sizeof(me)
    ok = k32.Module32First(snap, C.byref(me))
    while ok:
        name = me.szModule.decode(errors="ignore")
        if name.lower() in wanted:
            found[name.lower()] = (name, C.cast(me.modBaseAddr, C.c_void_p).value,
                                   int(me.modBaseSize))
        ok = k32.Module32Next(snap, C.byref(me))
    k32.CloseHandle(snap)
    return found


def capture(label, explicit=None):
    pid = explicit[0] if explicit else find_pid()
    if not pid:
        raise SystemExit("MCC is not running")
    if explicit:
        mods = {
            MODULES[0].lower(): (MODULES[0], explicit[1], explicit[2]),
            MODULES[1].lower(): (MODULES[1], explicit[3], explicit[4]),
        }
    else:
        mods = modules(pid)
    h = k32.OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, False, pid)
    if not h:
        raise SystemExit("OpenProcess failed: %d" % C.get_last_error())
    OUT.mkdir(exist_ok=True)
    manifest = {"pid": pid, "modules": []}
    try:
        for key in (name.lower() for name in MODULES):
            if key not in mods:
                print("not loaded: %s" % key)
                continue
            name, base, size = mods[key]
            blob = bytearray(size)
            valid = bytearray(size)
            cursor = base
            end = base + size
            while cursor < end:
                mbi = MEMORY_BASIC_INFORMATION()
                got = k32.VirtualQueryEx(h, C.c_void_p(cursor), C.byref(mbi), C.sizeof(mbi))
                if not got:
                    break
                region_base = max(cursor, int(mbi.BaseAddress or 0))
                region_end = min(end, int(mbi.BaseAddress or 0) + int(mbi.RegionSize))
                n = max(0, region_end - region_base)
                readable = (mbi.State == MEM_COMMIT and not (mbi.Protect & PAGE_GUARD)
                            and not (mbi.Protect & PAGE_NOACCESS))
                if readable and n:
                    buf = (C.c_ubyte * n)()
                    read = C.c_size_t()
                    if k32.ReadProcessMemory(h, C.c_void_p(region_base), buf, n, C.byref(read)):
                        off = region_base - base
                        blob[off:off + read.value] = bytes(buf[:read.value])
                        valid[off:off + read.value] = b"\x01" * read.value
                cursor = region_end if region_end > cursor else cursor + 0x1000
            stem = "%s_%s" % (label, name.replace(".", "_"))
            (OUT / (stem + ".bin")).write_bytes(blob)
            (OUT / (stem + ".valid")).write_bytes(valid)
            manifest["modules"].append({"name": name, "base": base, "size": size,
                                         "stem": stem})
            print("captured %s: base=0x%X size=0x%X" % (name, base, size))
        (OUT / (label + ".json")).write_text(json.dumps(manifest, indent=2))
    finally:
        k32.CloseHandle(h)


def load(label, module):
    meta = json.loads((OUT / (label + ".json")).read_text())
    item = next(x for x in meta["modules"] if x["name"].lower() == module.lower())
    return item, (OUT / (item["stem"] + ".bin")).read_bytes(), \
        (OUT / (item["stem"] + ".valid")).read_bytes()


def compare(p1, u1, p2, u2):
    for module in MODULES:
        records = [load(label, module) for label in (p1, u1, p2, u2)]
        metas = [x[0] for x in records]
        if len({x["size"] for x in metas}) != 1:
            print("%s changed size; skipping" % module)
            continue
        blobs = [x[1] for x in records]
        valid = [x[2] for x in records]
        hits = []
        for off, vals in enumerate(zip(*blobs)):
            if not all(mask[off] for mask in valid):
                continue
            if vals[0] == vals[2] and vals[1] == vals[3] and vals[0] != vals[1]:
                hits.append((off, vals[0], vals[1]))
        print("%s: %d repeated byte candidates" % (module, len(hits)))
        # Prefer boolean-looking values and collapse adjacent identical bytes.
        shown = 0
        previous = -2
        for off, paused, unpaused in hits:
            if paused not in (0, 1) or unpaused not in (0, 1):
                continue
            if off == previous + 1:
                previous = off
                continue
            print("  +0x%X  paused=%d unpaused=%d" % (off, paused, unpaused))
            previous = off
            shown += 1
            if shown >= 200:
                print("  ...")
                break


def watch(pid, main_base, halo_base, seconds):
    candidates = {
        "mcc+3FA3AB0": main_base + 0x3FA3AB0,
        "mcc+3FB9938": main_base + 0x3FB9938,
        "mcc+3FBE54E": main_base + 0x3FBE54E,
        "mcc+3FBE556": main_base + 0x3FBE556,
        "mcc+4000B8B": main_base + 0x4000B8B,
        "mcc+4000B97": main_base + 0x4000B97,
        "h3+A3CA9A": halo_base + 0xA3CA9A,
        "h3+A731F6": halo_base + 0xA731F6,
        "h3+A74936": halo_base + 0xA74936,
    }
    h = k32.OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, False, pid)
    if not h:
        raise SystemExit("OpenProcess failed: %d" % C.get_last_error())
    values = {}
    start = time.perf_counter()
    try:
        while time.perf_counter() - start < seconds:
            changed = []
            for name, address in candidates.items():
                value = C.c_ubyte()
                read = C.c_size_t()
                if not k32.ReadProcessMemory(h, C.c_void_p(address), C.byref(value), 1, C.byref(read)):
                    continue
                old = values.get(name)
                if old is None or old != value.value:
                    changed.append("%s:%s>%d" %
                                   (name, "?" if old is None else old, value.value))
                    values[name] = value.value
            if changed:
                print("%8.3f  %s" % (time.perf_counter() - start, "  ".join(changed)), flush=True)
            time.sleep(0.002)
    finally:
        k32.CloseHandle(h)


def main():
    if len(sys.argv) in (3, 8) and sys.argv[1] == "snap":
        explicit = [int(x, 0) for x in sys.argv[3:]] if len(sys.argv) == 8 else None
        capture(sys.argv[2], explicit)
    elif len(sys.argv) == 6 and sys.argv[1] == "watch":
        watch(int(sys.argv[2], 0), int(sys.argv[3], 0), int(sys.argv[4], 0),
              float(sys.argv[5]))
    elif len(sys.argv) == 6 and sys.argv[1] == "compare":
        compare(*sys.argv[2:])
    else:
        raise SystemExit("usage: pause_scan.py snap LABEL [PID MAIN_BASE MAIN_SIZE H3_BASE H3_SIZE] | compare P1 U1 P2 U2 | watch PID MAIN_BASE H3_BASE SECONDS")


if __name__ == "__main__":
    main()
