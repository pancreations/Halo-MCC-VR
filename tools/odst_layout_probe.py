"""Read-only live probe for the retail ODST prepared-view array.

Requests only PROCESS_QUERY_INFORMATION | PROCESS_VM_READ. It never opens the
target for writes, attaches a debugger, changes protection, or installs hooks.

Usage:
    py -3 tools/odst_layout_probe.py [pid] [samples] [interval_seconds]
"""

import ctypes as C
from ctypes import wintypes as W
import json
import struct
import sys
import time


GAME = "MCC-Win64-Shipping.exe"
ODST_IMAGE_SIZE = 0x4797000
ODST_TIMESTAMP = 0x68A0F232
VIEW_ARRAY_RVA = 0x2D73590
VIEW_STRIDE = 0x2810
VIEW_COUNT = 4

PROCESS_VM_READ = 0x10
PROCESS_QUERY_INFORMATION = 0x400
TH32CS_SNAPPROCESS = 0x2
MEM_COMMIT = 0x1000

k32 = C.WinDLL("kernel32", use_last_error=True)
k32.CreateToolhelp32Snapshot.restype = W.HANDLE
k32.OpenProcess.restype = W.HANDLE
k32.ReadProcessMemory.restype = W.BOOL


class PROCESSENTRY32(C.Structure):
    _fields_ = [
        ("dwSize", W.DWORD), ("cntUsage", W.DWORD),
        ("th32ProcessID", W.DWORD), ("th32DefaultHeapID", C.c_void_p),
        ("th32ModuleID", W.DWORD), ("cntThreads", W.DWORD),
        ("th32ParentProcessID", W.DWORD), ("pcPriClassBase", C.c_long),
        ("dwFlags", W.DWORD), ("szExeFile", C.c_char * 260),
    ]


class MEMORY_BASIC_INFORMATION(C.Structure):
    _fields_ = [
        ("BaseAddress", C.c_void_p), ("AllocationBase", C.c_void_p),
        ("AllocationProtect", W.DWORD), ("PartitionId", W.WORD),
        ("RegionSize", C.c_size_t), ("State", W.DWORD),
        ("Protect", W.DWORD), ("Type", W.DWORD),
    ]


def find_pid():
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    pe = PROCESSENTRY32()
    pe.dwSize = C.sizeof(pe)
    ok = k32.Process32First(snap, C.byref(pe))
    try:
        while ok:
            if pe.szExeFile.decode(errors="ignore").lower() == GAME.lower():
                return int(pe.th32ProcessID)
            ok = k32.Process32Next(snap, C.byref(pe))
    finally:
        k32.CloseHandle(snap)
    return 0


def allocation_sizes(handle):
    allocations = {}
    address = 0
    while address < 0x7FFF_FFFF_FFFF:
        mbi = MEMORY_BASIC_INFORMATION()
        got = k32.VirtualQueryEx(handle, C.c_void_p(address), C.byref(mbi),
                                C.sizeof(mbi))
        if not got:
            break
        base = int(mbi.BaseAddress or 0)
        end = base + int(mbi.RegionSize)
        allocation = int(mbi.AllocationBase or 0)
        if allocation and mbi.State == MEM_COMMIT:
            allocations[allocation] = allocations.get(allocation, 0) + int(mbi.RegionSize)
        address = end if end > address else address + 0x1000
    return allocations


def read_exact(handle, address, size):
    buf = (C.c_ubyte * size)()
    count = C.c_size_t()
    C.set_last_error(0)
    ok = k32.ReadProcessMemory(handle, C.c_void_p(address), buf, size,
                              C.byref(count))
    if not ok or count.value != size:
        raise OSError(C.get_last_error(),
                      "ReadProcessMemory(0x%X, 0x%X) read 0x%X" %
                      (address, size, count.value))
    return bytes(buf)


def locate_odst(handle):
    candidates = [base for base, size in allocation_sizes(handle).items()
                  if size == ODST_IMAGE_SIZE]
    errors = []
    for base in candidates:
        try:
            header = read_exact(handle, base, 0x1000)
            if header[:2] != b"MZ":
                continue
            pe = struct.unpack_from("<I", header, 0x3C)[0]
            timestamp = struct.unpack_from("<I", header, pe + 8)[0]
            image_size = struct.unpack_from("<I", header, pe + 24 + 56)[0]
            if timestamp == ODST_TIMESTAMP and image_size == ODST_IMAGE_SIZE:
                return base
        except OSError as exc:
            errors.append(str(exc))
    if errors:
        raise RuntimeError("ODST-sized image found, but reading it was denied: " +
                           "; ".join(errors))
    raise RuntimeError("loaded ODST image was not found")


def floats(blob, offset, count):
    return list(struct.unpack_from("<%df" % count, blob, offset))


def shorts(blob, offset, count=4):
    return list(struct.unpack_from("<%dh" % count, blob, offset))


def compact(blob):
    return {
        "position": floats(blob, 0x00, 3),
        "forward": floats(blob, 0x0C, 3),
        "up": floats(blob, 0x18, 3),
        "mode_24": blob[0x24],
        "unknown_25_27": blob[0x25:0x28].hex(),
        "vertical_fov": floats(blob, 0x28, 1)[0],
        "fov_reference_2c": floats(blob, 0x2C, 1)[0],
        "observer_scalars_30_34": floats(blob, 0x30, 2),
        "window_bounds_38": shorts(blob, 0x38),
        "working_bounds_40": shorts(blob, 0x40),
        "saved_origin_48": shorts(blob, 0x48, 2),
        "render_bounds_4c": shorts(blob, 0x4C),
        "reference_bounds_54": shorts(blob, 0x54),
        "active_bounds_5c": shorts(blob, 0x5C),
        "z_near": floats(blob, 0x64, 1)[0],
        "z_far": floats(blob, 0x68, 1)[0],
        "optional_6c": floats(blob, 0x6C, 4),
        "custom_enable_7c": blob[0x7C],
        "unknown_7d_7f": blob[0x7D:0x80].hex(),
        "custom_projection_80": floats(blob, 0x80, 4),
    }


def capture_views(handle, module_base):
    array = read_exact(handle, module_base + VIEW_ARRAY_RVA,
                       VIEW_COUNT * VIEW_STRIDE)
    views = []
    camera_offsets = {
        "root_current": 0x008,
        "root_secondary": 0x158,
        "fp_current": 0x6D0,
        "fp_secondary": 0x820,
    }
    for index in range(VIEW_COUNT):
        view = array[index * VIEW_STRIDE:(index + 1) * VIEW_STRIDE]
        tail = struct.unpack_from("<8i", view, 0x27F0)
        entry = {
            "index": index,
            "vtable": "0x%X" % struct.unpack_from("<Q", view, 0)[0],
            "tail_27f0_280c": list(tail),
            "root_field_2a8": "0x%X" % struct.unpack_from("<Q", view, 0x2A8)[0],
            "fp_source_camera_970": "0x%X" % struct.unpack_from("<Q", view, 0x970)[0],
            "root_projection_110": floats(view, 0x110, 16),
            "cameras": {},
        }
        for name, offset in camera_offsets.items():
            entry["cameras"][name] = compact(view[offset:offset + 0x90])
        views.append(entry)
    return views


def main():
    pid = int(sys.argv[1], 0) if len(sys.argv) > 1 else find_pid()
    samples = int(sys.argv[2], 0) if len(sys.argv) > 2 else 1
    interval = float(sys.argv[3]) if len(sys.argv) > 3 else 0.25
    if not pid:
        raise SystemExit("MCC is not running")
    handle = k32.OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
                             False, pid)
    if not handle:
        raise SystemExit("OpenProcess failed: %d" % C.get_last_error())
    try:
        module_base = locate_odst(handle)
        for sample in range(samples):
            record = {
                "pid": pid,
                "module_base": "0x%X" % module_base,
                "sample": sample,
                "unix_time": time.time(),
                "views": capture_views(handle, module_base),
            }
            print(json.dumps(record, sort_keys=True))
            if sample + 1 < samples:
                time.sleep(interval)
    finally:
        k32.CloseHandle(handle)


if __name__ == "__main__":
    main()
