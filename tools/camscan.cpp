// camscan.cpp - external reverse-engineering helper for the Halo 3 camera.
//
// Most modes perform differential scans and read only. The explicitly named
// poke/spin/write modes call WriteProcessMemory and can crash or corrupt the
// running session. CMake excludes this tool from normal builds. Use a write
// mode only for an approved offline diagnostic with anti-cheat disabled.
//
// Commands (state is kept in the two .bin files so steps can run separately):
//   camscan attach
//   camscan first
//   camscan narrow inc|dec|changed|unchanged
//   camscan dump [max]
//
// Scope: committed, writable memory of MCC-Win64-Shipping.exe (that's where
// mutable game state lives; code/constants are read-only and skipped).

#include <windows.h>
#include <tlhelp32.h>
#include <share.h>
#include <psapi.h>
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <vector>
#include <string>
#include <algorithm>

static const wchar_t* kGameExe = L"MCC-Win64-Shipping.exe";
static const wchar_t* kEngineDll = L"halo3.dll";
static const char* kDumpFile = "camscan_dump.bin";
static const char* kCandFile = "camscan_cands.bin";
static const float kEps = 1e-4f;
static const float kPlausible = 1e7f;

struct Cand { uint64_t addr; float value; };

static DWORD FindPid(const wchar_t* exe)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{ sizeof(pe) };
    DWORD pid = 0;
    if (Process32FirstW(snap, &pe))
        do { if (_wcsicmp(pe.szExeFile, exe) == 0) { pid = pe.th32ProcessID; break; } }
        while (Process32NextW(snap, &pe));
    CloseHandle(snap);
    return pid;
}

static bool ModuleRange(DWORD pid, const wchar_t* name, uint64_t& base, uint64_t& size)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32W me{ sizeof(me) };
    bool found = false;
    if (Module32FirstW(snap, &me))
        do {
            if (_wcsicmp(me.szModule, name) == 0)
            { base = (uint64_t)me.modBaseAddr; size = me.modBaseSize; found = true; break; }
        } while (Module32NextW(snap, &me));
    CloseHandle(snap);
    return found;
}

static HANDLE OpenGame(DWORD& pid)
{
    pid = FindPid(kGameExe);
    if (!pid) { printf("GAME NOT RUNNING (%ls not found). Launch a level first.\n", kGameExe); return nullptr; }
    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!h) printf("OpenProcess failed (%lu). Try running this from an elevated shell.\n", GetLastError());
    return h;
}

// Visit every committed, writable, readable region. If [lo,hi) is non-empty,
// only regions inside that range are visited (used to scope to halo3.dll).
template <class F>
static void ForEachRegion(HANDLE h, F fn, uint64_t lo = 0, uint64_t hi = 0)
{
    MEMORY_BASIC_INFORMATION mbi{};
    uint64_t addr = 0;
    while (VirtualQueryEx(h, (LPCVOID)addr, &mbi, sizeof(mbi)) == sizeof(mbi))
    {
        const uint64_t base = (uint64_t)mbi.BaseAddress;
        const uint64_t size = (uint64_t)mbi.RegionSize;
        const DWORD p = mbi.Protect;
        const bool writable = (p & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
        const bool bad = (p & (PAGE_GUARD | PAGE_NOACCESS)) != 0;
        const bool inScope = (hi == 0) || (base >= lo && base < hi);
        if (mbi.State == MEM_COMMIT && writable && !bad && inScope && size > 0 && size < (uint64_t)3 * 1024 * 1024 * 1024)
            fn(base, size);
        addr = base + size;
        if (addr == 0) break;
    }
}

static int CmdAttach()
{
    DWORD pid = 0;
    HANDLE h = OpenGame(pid);
    if (!h) return 1;
    printf("attached to pid %lu\n", pid);
    uint64_t base = 0, size = 0;
    if (ModuleRange(pid, kEngineDll, base, size))
        printf("%ls base=0x%llX size=0x%llX (%.1f MB)\n", kEngineDll, base, size, size / 1048576.0);
    else
        printf("%ls NOT loaded yet — get into a Halo 3 level, then retry.\n", kEngineDll);
    uint64_t regions = 0, total = 0;
    ForEachRegion(h, [&](uint64_t, uint64_t s){ regions++; total += s; });
    printf("writable committed memory: %llu regions, %.1f MB total\n", regions, total / 1048576.0);
    CloseHandle(h);
    return 0;
}

static int CmdFirst(bool halo3Only)
{
    DWORD pid = 0;
    HANDLE h = OpenGame(pid);
    if (!h) return 1;
    uint64_t lo = 0, hi = 0;
    if (halo3Only)
    {
        uint64_t hbase = 0, hsize = 0;
        if (!ModuleRange(pid, kEngineDll, hbase, hsize))
        { printf("%ls not loaded; get into a level.\n", kEngineDll); CloseHandle(h); return 1; }
        lo = hbase; hi = hbase + hsize;
        printf("scanning only %ls [0x%llX .. 0x%llX]\n", kEngineDll, lo, hi);
    }
    FILE* f = nullptr; fopen_s(&f, kDumpFile, "wb");
    if (!f) { printf("cannot write %s\n", kDumpFile); CloseHandle(h); return 1; }
    std::vector<char> buf;
    uint64_t dumped = 0, regions = 0;
    ForEachRegion(h, [&](uint64_t base, uint64_t size){
        buf.resize(size);
        SIZE_T got = 0;
        if (ReadProcessMemory(h, (LPCVOID)base, buf.data(), size, &got) && got > 0)
        {
            fwrite(&base, sizeof(base), 1, f);
            fwrite(&got, sizeof(got), 1, f);
            fwrite(buf.data(), 1, got, f);
            dumped += got; regions++;
        }
    }, lo, hi);
    fclose(f);
    CloseHandle(h);
    remove(kCandFile); // fresh differential
    printf("baseline captured: %llu regions, %.1f MB. Now change the view and run 'narrow'.\n",
           regions, dumped / 1048576.0);
    return 0;
}

static bool Cmp(const char* mode, float oldv, float newv)
{
    if (!std::isfinite(oldv) || !std::isfinite(newv)) return false;
    if (fabsf(oldv) > kPlausible || fabsf(newv) > kPlausible) return false;
    if (!strcmp(mode, "inc")) return newv > oldv + kEps;
    if (!strcmp(mode, "dec")) return newv < oldv - kEps;
    if (!strcmp(mode, "changed")) return fabsf(newv - oldv) > kEps;
    if (!strcmp(mode, "unchanged")) return fabsf(newv - oldv) <= kEps;
    return false;
}

static int CmdNarrow(const char* mode)
{
    DWORD pid = 0;
    HANDLE h = OpenGame(pid);
    if (!h) return 1;

    std::vector<Cand> cands;
    FILE* cf = nullptr; fopen_s(&cf, kCandFile, "rb");
    if (cf)
    {
        // Refine existing candidate list against current memory.
        Cand c{};
        while (fread(&c, sizeof(c), 1, cf) == 1)
        {
            float cur = 0; SIZE_T got = 0;
            if (ReadProcessMemory(h, (LPCVOID)c.addr, &cur, sizeof(cur), &got) && got == sizeof(cur) && Cmp(mode, c.value, cur))
                cands.push_back({ c.addr, cur });
        }
        fclose(cf);
    }
    else
    {
        // First narrowing: build candidates from the baseline dump.
        FILE* df = nullptr; fopen_s(&df, kDumpFile, "rb");
        if (!df) { printf("no baseline; run 'first' before 'narrow'.\n"); CloseHandle(h); return 1; }
        std::vector<char> dumpBuf, curBuf;
        uint64_t base = 0; SIZE_T size = 0;
        while (fread(&base, sizeof(base), 1, df) == 1 && fread(&size, sizeof(size), 1, df) == 1)
        {
            dumpBuf.resize(size);
            if (fread(dumpBuf.data(), 1, size, df) != size) break;
            curBuf.resize(size);
            SIZE_T got = 0;
            if (!ReadProcessMemory(h, (LPCVOID)base, curBuf.data(), size, &got) || got < sizeof(float))
                continue;
            for (SIZE_T o = 0; o + sizeof(float) <= got; o += 4)
            {
                float oldv, newv;
                memcpy(&oldv, dumpBuf.data() + o, sizeof(float));
                memcpy(&newv, curBuf.data() + o, sizeof(float));
                if (Cmp(mode, oldv, newv))
                    cands.push_back({ base + o, newv });
            }
        }
        fclose(df);
    }
    CloseHandle(h);

    FILE* out = nullptr; fopen_s(&out, kCandFile, "wb");
    if (out) { if (!cands.empty()) fwrite(cands.data(), sizeof(Cand), cands.size(), out); fclose(out); }
    printf("candidates remaining: %llu\n", (unsigned long long)cands.size());
    return 0;
}

static int CmdDump(int maxN)
{
    DWORD pid = 0;
    uint64_t hbase = 0, hsize = 0;
    if (DWORD p = FindPid(kGameExe)) ModuleRange(p, kEngineDll, hbase, hsize);

    FILE* cf = nullptr; fopen_s(&cf, kCandFile, "rb");
    if (!cf) { printf("no candidates yet.\n"); return 1; }
    Cand c{}; int n = 0;
    while (fread(&c, sizeof(c), 1, cf) == 1)
    {
        if (n++ >= maxN) { printf("... (more)\n"); break; }
        if (hbase && c.addr >= hbase && c.addr < hbase + hsize)
            printf("0x%016llX  = %.5f   (%ls+0x%llX)\n", c.addr, c.value, kEngineDll, c.addr - hbase);
        else
            printf("0x%016llX  = %.5f\n", c.addr, c.value);
    }
    fclose(cf);
    return 0;
}

// Group surviving candidates that sit next to each other in memory. A camera
// stores its facing as 3 consecutive floats (a direction vector), often next
// to a position (another 3), so tight clusters are the interesting ones.
static int CmdClusters(int gap)
{
    DWORD p = FindPid(kGameExe);
    uint64_t hbase = 0, hsize = 0;
    if (p) ModuleRange(p, kEngineDll, hbase, hsize);

    FILE* cf = nullptr; fopen_s(&cf, kCandFile, "rb");
    if (!cf) { printf("no candidates.\n"); return 1; }
    std::vector<Cand> v; Cand c{};
    while (fread(&c, sizeof(c), 1, cf) == 1) v.push_back(c);
    fclose(cf);
    std::sort(v.begin(), v.end(), [](const Cand& a, const Cand& b){ return a.addr < b.addr; });

    int groups = 0;
    for (size_t i = 0; i < v.size();)
    {
        size_t j = i + 1;
        while (j < v.size() && v[j].addr - v[j - 1].addr <= (uint64_t)gap) j++;
        const size_t n = j - i;
        if (n >= 3) // a run of >=3 nearby floats is worth showing
        {
            groups++;
            const uint64_t g0 = v[i].addr;
            if (hbase && g0 >= hbase && g0 < hbase + hsize)
                printf("[group %d] %ls+0x%llX  (%zu floats)\n", groups, kEngineDll, g0 - hbase, n);
            else
                printf("[group %d] 0x%llX  (%zu floats)\n", groups, g0, n);
            for (size_t k = i; k < j; k++)
                printf("    +%02llu  = % .5f\n", v[k].addr - g0, v[k].value);
        }
        i = j;
    }
    printf("(%d clusters of >=3 adjacent floats)\n", groups);
    return 0;
}

// Live-watch N floats at halo3.dll+rva for `secs` seconds, sampling a few
// times a second. Lets me confirm which candidate actually tracks the view as
// the user looks around, and see what number format the orientation is in.
static int CmdWatch(uint64_t rva, int n, double secs)
{
    DWORD pid = 0;
    HANDLE h = OpenGame(pid);
    if (!h) return 1;
    uint64_t hbase = 0, hsize = 0;
    if (!ModuleRange(pid, kEngineDll, hbase, hsize))
    { printf("%ls not loaded.\n", kEngineDll); CloseHandle(h); return 1; }
    const uint64_t addr = hbase + rva;
    if (n > 64) n = 64;
    printf("watching %ls+0x%llX (%d floats) for %.0fs — look around now:\n", kEngineDll, rva, n, secs);
    std::vector<float> f(n);
    const DWORD start = GetTickCount();
    while ((GetTickCount() - start) < (DWORD)(secs * 1000))
    {
        SIZE_T got = 0;
        if (ReadProcessMemory(h, (LPCVOID)addr, f.data(), n * sizeof(float), &got))
        {
            printf("%5.1fs:", (GetTickCount() - start) / 1000.0);
            for (int i = 0; i < n; i++) printf(" % .3f", f[i]);
            printf("\n");
        }
        Sleep(200);
    }
    CloseHandle(h);
    return 0;
}

// Live-watch at an absolute (heap) address — same as watch, for candidates
// found by the full-memory differential scan.
static int CmdWatchAbs(uint64_t addr, int n, double secs)
{
    DWORD pid = 0;
    HANDLE h = OpenGame(pid);
    if (!h) return 1;
    if (n > 64) n = 64;
    printf("watching 0x%llX (%d floats) for %.0fs — look around now:\n", addr, n, secs);
    std::vector<float> f(n);
    const DWORD start = GetTickCount();
    while ((GetTickCount() - start) < (DWORD)(secs * 1000))
    {
        SIZE_T got = 0;
        if (ReadProcessMemory(h, (LPCVOID)addr, f.data(), n * sizeof(float), &got))
        {
            printf("%5.1fs:", (GetTickCount() - start) / 1000.0);
            for (int i = 0; i < n; i++) printf(" % .3f", f[i]);
            printf("\n");
        }
        Sleep(200);
    }
    CloseHandle(h);
    return 0;
}

// WRITE TEST: continuously force a facing vector into halo3.dll+rva for a few
// seconds, to confirm this address actually steers the rendered view. This is
// the only command that writes; everything else is read-only.
static int CmdPoke(uint64_t rva, float x, float y, float z, double secs)
{
    DWORD pid = FindPid(kGameExe);
    if (!pid) { printf("GAME NOT RUNNING.\n"); return 1; }
    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION, FALSE, pid);
    if (!h) { printf("OpenProcess(write) failed (%lu).\n", GetLastError()); return 1; }
    uint64_t hbase = 0, hsize = 0;
    if (!ModuleRange(pid, kEngineDll, hbase, hsize)) { printf("%ls not loaded.\n", kEngineDll); CloseHandle(h); return 1; }
    const float len = sqrtf(x * x + y * y + z * z);
    if (len > 1e-6f) { x /= len; y /= len; z /= len; }
    float v[3] = { x, y, z };
    const uint64_t addr = hbase + rva;
    printf("forcing facing (% .3f, % .3f, % .3f) at %ls+0x%llX for %.0fs — try to look around:\n",
           x, y, z, kEngineDll, rva, secs);
    const DWORD start = GetTickCount();
    uint64_t writes = 0;
    while ((GetTickCount() - start) < (DWORD)(secs * 1000))
    {
        SIZE_T put = 0;
        if (WriteProcessMemory(h, (LPVOID)addr, v, sizeof(v), &put) && put == sizeof(v)) writes++;
        Sleep(8);
    }
    printf("done (%llu writes).\n", (unsigned long long)writes);
    CloseHandle(h);
    return 0;
}

// Like poke, but the forced facing spins over time so the confirmation is
// unmistakable: if this address is the camera, the view rotates on its own.
static int CmdSpin(uint64_t rva, double secs)
{
    DWORD pid = FindPid(kGameExe);
    if (!pid) { printf("GAME NOT RUNNING.\n"); return 1; }
    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION, FALSE, pid);
    if (!h) { printf("OpenProcess(write) failed (%lu).\n", GetLastError()); return 1; }
    uint64_t hbase = 0, hsize = 0;
    if (!ModuleRange(pid, kEngineDll, hbase, hsize)) { printf("%ls not loaded.\n", kEngineDll); CloseHandle(h); return 1; }
    const uint64_t addr = hbase + rva;
    printf("spinning the forced facing at %ls+0x%llX for %.0fs — just watch the screen:\n", kEngineDll, rva, secs);
    const DWORD start = GetTickCount();
    uint64_t writes = 0;
    while ((GetTickCount() - start) < (DWORD)(secs * 1000))
    {
        const double t = (GetTickCount() - start) / 1000.0;
        const double a = t * 3.14159; // ~half turn per second
        float v[3] = { (float)cos(a), (float)sin(a), 0.0f };
        SIZE_T put = 0;
        if (WriteProcessMemory(h, (LPVOID)addr, v, sizeof(v), &put) && put == sizeof(v)) writes++;
        Sleep(8);
    }
    printf("done (%llu writes).\n", (unsigned long long)writes);
    CloseHandle(h);
    return 0;
}

// Scan halo3.dll's whole image (code included) for x64 RIP-relative references
// that point at [targetRva, targetRva+range). These are the instructions that
// read or write our value; disassembling around them reveals the real source.
static int CmdXref(uint64_t targetRva, uint64_t range)
{
    DWORD pid = FindPid(kGameExe);
    if (!pid) { printf("GAME NOT RUNNING.\n"); return 1; }
    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!h) { printf("OpenProcess failed (%lu).\n", GetLastError()); return 1; }
    uint64_t hbase = 0, hsize = 0;
    if (!ModuleRange(pid, kEngineDll, hbase, hsize)) { printf("%ls not loaded.\n", kEngineDll); CloseHandle(h); return 1; }

    // Read the module image region by region (some pages may be unreadable).
    std::vector<uint8_t> img(hsize, 0);
    std::vector<uint8_t> have(hsize, 0);
    MEMORY_BASIC_INFORMATION mbi{};
    for (uint64_t a = hbase; a < hbase + hsize; )
    {
        if (VirtualQueryEx(h, (LPCVOID)a, &mbi, sizeof(mbi)) != sizeof(mbi)) break;
        const uint64_t base = (uint64_t)mbi.BaseAddress, size = (uint64_t)mbi.RegionSize;
        const bool readable = mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
        if (readable)
        {
            SIZE_T got = 0;
            const uint64_t off = base - hbase;
            const uint64_t n = (off + size <= hsize) ? size : (hsize - off);
            if (ReadProcessMemory(h, (LPCVOID)base, img.data() + off, n, &got))
                memset(have.data() + off, 1, got);
        }
        a = base + size;
    }
    CloseHandle(h);

    int hits = 0;
    for (uint64_t i = 0; i + 4 <= hsize; i++)
    {
        if (!have[i] || !have[i + 3]) continue;
        int32_t disp; memcpy(&disp, img.data() + i, 4);
        const uint64_t tgt = (i + 4) + (int64_t)disp; // RVA the rip-rel operand points to
        if (tgt >= targetRva && tgt < targetRva + range)
        {
            hits++;
            printf("ref at %ls+0x%llX -> +0x%llX   bytes:", kEngineDll, i, tgt);
            const uint64_t s = i >= 12 ? i - 12 : 0;
            for (uint64_t b = s; b < i + 4 && b < hsize; b++) printf(" %02X", img[b]);
            printf("\n");
            if (hits >= 60) { printf("... (stopping)\n"); break; }
        }
    }
    printf("(%d references to %ls+0x%llX)\n", hits, kEngineDll, targetRva);
    return 0;
}

// Set/clear a 4-byte hardware WRITE breakpoint (DR0) on one thread.
static void SetDr0(HANDLE th, uint64_t addr, bool on)
{
    CONTEXT c{}; c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (!GetThreadContext(th, &c)) return;
    if (on)
    {
        c.Dr0 = addr;
        c.Dr7 = (c.Dr7 & ~0xF0000ull) | (1ull << 0) | (0b01ull << 16) | (0b11ull << 18); // L0, write, len4
    }
    else
    {
        c.Dr0 = 0;
        c.Dr7 &= ~((1ull << 0) | (0xFull << 16));
    }
    c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    SetThreadContext(th, &c);
}

static void ClearDrAllThreads(DWORD pid, uint64_t addr)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te{ sizeof(te) };
    if (Thread32First(snap, &te))
        do {
            if (te.th32OwnerProcessID == pid)
            {
                HANDLE th = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, te.th32ThreadID);
                if (th) { SetDr0(th, addr, false); CloseHandle(th); }
            }
        } while (Thread32Next(snap, &te));
    CloseHandle(snap);
}

// Arm DR0 on EVERY existing thread. DebugActiveProcess only reports the main
// thread plus threads created after attach, so render/worker threads that
// already exist (which is where the per-frame render palette is written) never
// get the watchpoint unless we set it on them explicitly here. Returns the
// count armed.
static int SetDrAllThreads(DWORD pid, uint64_t addr)
{
    int armed = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    THREADENTRY32 te{ sizeof(te) };
    if (Thread32First(snap, &te))
        do {
            if (te.th32OwnerProcessID == pid)
            {
                HANDLE th = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME,
                                       FALSE, te.th32ThreadID);
                if (th)
                {
                    SuspendThread(th);
                    SetDr0(th, addr, true);
                    ResumeThread(th);
                    CloseHandle(th);
                    ++armed;
                }
            }
        } while (Thread32Next(snap, &te));
    CloseHandle(snap);
    return armed;
}

// Attach as a debugger, set a hardware write-watchpoint on an absolute
// address, and record the game instructions (and CPU registers) that write
// it. The register that holds a value near the target is the base pointer of
// the real owning structure — that's what we actually need. Detaches cleanly.
// First captured write, for programmatic chain-following.
struct WriteHit
{
    bool valid = false;
    bool inHalo3 = false;
    uint64_t rip = 0, ripRva = 0;
    uint64_t rcx = 0, rdx = 0, r8 = 0;
};

static int FindWriteAt(uint64_t addr, int wantHits, double timeoutSecs, WriteHit* first = nullptr)
{
    DWORD pid = FindPid(kGameExe);
    if (!pid) { printf("GAME NOT RUNNING.\n"); return 1; }
    uint64_t hbase = 0, hsize = 0;
    if (!ModuleRange(pid, kEngineDll, hbase, hsize)) { printf("%ls not loaded.\n", kEngineDll); return 1; }

    if (!DebugActiveProcess(pid)) { printf("DebugActiveProcess failed (%lu).\n", GetLastError()); return 1; }
    DebugSetProcessKillOnExit(FALSE); // if we die, the game must survive
    // Arm every pre-existing thread — the render/worker thread that writes the
    // per-frame palette existed before we attached, so the event-driven arming
    // below (main + new threads only) would miss it.
    const int armed = SetDrAllThreads(pid, addr);
    printf("watching writes to 0x%llX (armed %d existing threads):\n", addr, armed);

    int hits = 0;
    const DWORD start = GetTickCount();
    DEBUG_EVENT ev{};
    while (hits < wantHits && (GetTickCount() - start) < (DWORD)(timeoutSecs * 1000))
    {
        if (!WaitForDebugEvent(&ev, 200)) continue;
        DWORD cont = DBG_CONTINUE;
        if (ev.dwDebugEventCode == CREATE_PROCESS_DEBUG_EVENT)
        {
            SetDr0(ev.u.CreateProcessInfo.hThread, addr, true);
            if (ev.u.CreateProcessInfo.hFile) CloseHandle(ev.u.CreateProcessInfo.hFile);
        }
        else if (ev.dwDebugEventCode == CREATE_THREAD_DEBUG_EVENT)
            SetDr0(ev.u.CreateThread.hThread, addr, true);
        else if (ev.dwDebugEventCode == EXCEPTION_DEBUG_EVENT)
        {
            const DWORD code = ev.u.Exception.ExceptionRecord.ExceptionCode;
            if (code == EXCEPTION_SINGLE_STEP)
            {
                HANDLE th = OpenThread(THREAD_GET_CONTEXT, FALSE, ev.dwThreadId);
                if (th)
                {
                    CONTEXT c{}; c.ContextFlags = CONTEXT_FULL;
                    if (GetThreadContext(th, &c))
                    {
                        hits++;
                        if (first && !first->valid)
                        {
                            first->valid = true;
                            first->inHalo3 = c.Rip >= hbase && c.Rip < hbase + hsize;
                            first->rip = c.Rip;
                            first->ripRva = first->inHalo3 ? c.Rip - hbase : 0;
                            first->rcx = c.Rcx; first->rdx = c.Rdx; first->r8 = c.R8;
                        }
                        if (c.Rip >= hbase && c.Rip < hbase + hsize)
                            printf("write #%d by %ls+0x%llX\n", hits, kEngineDll, c.Rip - hbase);
                        else
                            printf("write #%d by 0x%llX (OUTSIDE %ls — likely the mod's "
                                   "own head-look write; turn head tracking OFF with F2 and rerun)\n",
                                   hits, c.Rip, kEngineDll);
                        const uint64_t regs[] = { c.Rax,c.Rbx,c.Rcx,c.Rdx,c.Rsi,c.Rdi,c.Rbp,
                            c.R8,c.R9,c.R10,c.R11,c.R12,c.R13,c.R14,c.R15 };
                        const char* nm[] = { "rax","rbx","rcx","rdx","rsi","rdi","rbp",
                            "r8","r9","r10","r11","r12","r13","r14","r15" };
                        for (int i = 0; i < 15; i++)
                        {
                            if (regs[i] >= hbase && regs[i] < hbase + hsize)
                                printf("    %-3s = %ls+0x%llX\n", nm[i], kEngineDll, regs[i] - hbase);
                            else if (regs[i] > 0x10000 && regs[i] < 0x00007FFFFFFFFFFFull)
                                printf("    %-3s = 0x%llX\n", nm[i], regs[i]);
                        }
                    }
                    CloseHandle(th);
                }
            }
            else if (code != EXCEPTION_BREAKPOINT)
                cont = DBG_EXCEPTION_NOT_HANDLED; // let the game handle its own exceptions
        }
        ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, cont);
    }

    ClearDrAllThreads(pid, addr);
    DebugActiveProcessStop(pid);
    printf("detached cleanly. (%d writes captured)\n", hits);
    return 0;
}

// RVA-based wrapper kept for the original workflow.
static int CmdFindWrite(uint64_t rva, int wantHits, double timeoutSecs)
{
    DWORD pid = FindPid(kGameExe);
    if (!pid) { printf("GAME NOT RUNNING.\n"); return 1; }
    uint64_t hbase = 0, hsize = 0;
    if (!ModuleRange(pid, kEngineDll, hbase, hsize)) { printf("%ls not loaded.\n", kEngineDll); return 1; }
    printf("target %ls+0x%llX\n", kEngineDll, rva);
    return FindWriteAt(hbase + rva, wantHits, timeoutSecs);
}

// The mod's CamCopyHook logs the heap address of the game's authoritative
// camera buffer ("src=...") into halo3xr.log. Locate that log via the running
// game's exe path (<install>\MCC\Binaries\Win64\...exe -> <install>\Halo_MCC_VR\)
// and return the most recently logged src pointer, so heap targets can be
// watched without the user copying addresses around.
static uint64_t FindLoggedCamSrc()
{
    DWORD pid = FindPid(kGameExe);
    if (!pid) { printf("GAME NOT RUNNING.\n"); return 0; }
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) { printf("OpenProcess failed (%lu).\n", GetLastError()); return 0; }
    wchar_t exePath[MAX_PATH]{};
    DWORD len = MAX_PATH;
    const bool ok = QueryFullProcessImageNameW(h, 0, exePath, &len);
    CloseHandle(h);
    if (!ok) { printf("QueryFullProcessImageName failed (%lu).\n", GetLastError()); return 0; }

    std::wstring path(exePath);
    for (int up = 0; up < 4; ++up) // strip exe name + Win64 + Binaries + MCC
    {
        const size_t slash = path.find_last_of(L"\\/");
        if (slash == std::wstring::npos) { printf("unexpected exe path layout\n"); return 0; }
        path.resize(slash);
    }
    path += L"\\Halo_MCC_VR\\halo3xr.log";

    // The mod keeps the log open for writing; share-friendly open required.
    FILE* f = _wfsopen(path.c_str(), L"rb", _SH_DENYNO);
    if (!f)
    {
        printf("could not open mod log: %ls\n", path.c_str());
        return 0;
    }
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::string text((size_t)size, '\0');
    fread(text.data(), 1, (size_t)size, f);
    fclose(f);

    uint64_t src = 0;
    for (size_t at = text.find("src="); at != std::string::npos; at = text.find("src=", at + 4))
    {
        const uint64_t v = strtoull(text.c_str() + at + 4, nullptr, 16);
        if (v > 0x10000)
            src = v; // keep the last plausible one (buffers move per level)
    }
    if (!src)
        printf("no 'src=' entries in %ls — enter a Halo 3 level first\n", path.c_str());
    else
        printf("mod log: latest camera src buffer = 0x%llX\n", src);
    return src;
}

// One-shot M3 helper: watch writes to the camera buffer's forward vector
// (src+0x28). The game rebuilds it from the player's aim state every frame,
// so the writer this catches is the aim->camera function we need to trace.
// Head tracking must be OFF (F2) or the mod's own write is caught instead.
static int CmdAimWrite(int wantHits, double timeoutSecs)
{
    const uint64_t src = FindLoggedCamSrc();
    if (!src) return 1;
    return FindWriteAt(src + 0x28, wantHits, timeoutSecs);
}

// M3: follow the aim data upstream automatically. The camera src forward
// (src+0x28) is refreshed by bulk memcpy from earlier buffers each frame;
// forward memcpys keep (dst - src) constant while their pointers advance, so
// the equivalent upstream address of the watched byte is
// watched - rcx + rdx. Hop until the writer is real halo3.dll code (the
// aim->camera math), then report its RVA for disassembly.
static int CmdAimChain(double secsPerHop)
{
    const uint64_t src = FindLoggedCamSrc();
    if (!src) return 1;
    uint64_t watched = src + 0x28;
    for (int hop = 0; hop < 6; ++hop)
    {
        printf("--- hop %d: watching 0x%llX\n", hop, watched);
        WriteHit hit{};
        FindWriteAt(watched, 1, secsPerHop, &hit);
        if (!hit.valid)
        {
            printf("no write captured — is the game in a level and unpaused?\n");
            return 1;
        }
        if (hit.inHalo3)
        {
            printf("*** GAME WRITER FOUND at halo3.dll+0x%llX (after %d memcpy hops)\n",
                   hit.ripRva, hop);
            printf("*** disassemble with: py tools\\disasm.py %llX 200\n", hit.ripRva > 0x60 ? hit.ripRva - 0x60 : hit.ripRva);
            return 0;
        }
        // Writer is library code (memcpy). Its dst/src pointers advance in
        // lockstep, so dst-src stays constant: the watched byte's twin in the
        // source buffer is watched - (rcx - rdx).
        const int64_t delta = (int64_t)(hit.rcx - hit.rdx);
        const bool canonical = hit.rcx > 0x10000 && hit.rcx < 0x7FFFFFFFFFFFull &&
                               hit.rdx > 0x10000 && hit.rdx < 0x7FFFFFFFFFFFull;
        if (!canonical || delta == 0)
        {
            printf("writer 0x%llX doesn't look like memcpy (rcx=0x%llX rdx=0x%llX r8=0x%llX); stopping\n",
                   hit.rip, hit.rcx, hit.rdx, hit.r8);
            return 1;
        }
        const uint64_t upstream = watched - (uint64_t)delta;
        printf("memcpy hop: writer 0x%llX, dst-src delta 0x%llX -> upstream 0x%llX\n",
               hit.rip, (uint64_t)delta, upstream);
        watched = upstream;
    }
    printf("gave up after 6 hops (unusually deep buffer chain)\n");
    return 1;
}

// Dump raw bytes at halo3.dll+rva (for disassembling a code window).
static int CmdHex(uint64_t rva, int n)
{
    DWORD pid = 0;
    HANDLE h = OpenGame(pid);
    if (!h) return 1;
    uint64_t hbase = 0, hsize = 0;
    if (!ModuleRange(pid, kEngineDll, hbase, hsize)) { CloseHandle(h); return 1; }
    std::vector<uint8_t> buf(n);
    SIZE_T got = 0;
    if (ReadProcessMemory(h, (LPCVOID)(hbase + rva), buf.data(), n, &got) && got)
    {
        for (SIZE_T i = 0; i < got; i++)
        {
            if (i % 16 == 0) printf("\n%ls+0x%llX:", kEngineDll, rva + i);
            printf(" %02X", buf[i]);
        }
        printf("\n");
    }
    else printf("read failed at 0x%llX\n", hbase + rva);
    CloseHandle(h);
    return 0;
}

// Find direct x64 CALL rel32 instructions whose destination is the requested
// halo3.dll RVA. Walking callers upward from the confirmed camera-copy hook is
// the quickest route to the enclosing scene-render pass.
static int CmdCallers(uint64_t targetRva)
{
    DWORD pid = 0;
    HANDLE h = OpenGame(pid);
    if (!h) return 1;
    uint64_t base = 0, size = 0;
    if (!ModuleRange(pid, kEngineDll, base, size)) { CloseHandle(h); return 1; }
    std::vector<uint8_t> image((size_t)size);
    SIZE_T got = 0;
    if (!ReadProcessMemory(h, (LPCVOID)base, image.data(), image.size(), &got))
    {
        printf("could not read %ls image\n", kEngineDll);
        CloseHandle(h);
        return 1;
    }
    int hits = 0;
    for (SIZE_T i = 0; i + 5 <= got; ++i)
    {
        if (image[i] != 0xE8) continue;
        int32_t rel = 0;
        memcpy(&rel, &image[i + 1], sizeof(rel));
        const int64_t destination = (int64_t)i + 5 + rel;
        if (destination == (int64_t)targetRva)
        {
            printf("call at %ls+0x%llX -> +0x%llX\n", kEngineDll,
                   (unsigned long long)i, (unsigned long long)targetRva);
            ++hits;
        }
    }
    printf("(%d direct callers)\n", hits);
    CloseHandle(h);
    return 0;
}

static int CmdFunction(uint64_t rva)
{
    DWORD pid = 0;
    HANDLE h = OpenGame(pid);
    if (!h) return 1;
    uint64_t base = 0, size = 0;
    if (!ModuleRange(pid, kEngineDll, base, size)) { CloseHandle(h); return 1; }
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS64 nt{};
    SIZE_T got = 0;
    if (!ReadProcessMemory(h, (LPCVOID)base, &dos, sizeof(dos), &got) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
        !ReadProcessMemory(h, (LPCVOID)(base + dos.e_lfanew), &nt, sizeof(nt), &got) ||
        nt.Signature != IMAGE_NT_SIGNATURE)
    {
        printf("invalid PE headers\n"); CloseHandle(h); return 1;
    }
    const auto& dir = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    std::vector<RUNTIME_FUNCTION> functions(dir.Size / sizeof(RUNTIME_FUNCTION));
    if (!ReadProcessMemory(h, (LPCVOID)(base + dir.VirtualAddress), functions.data(), dir.Size, &got))
    {
        printf("could not read exception directory\n"); CloseHandle(h); return 1;
    }
    for (const auto& fn : functions)
    {
        if (rva >= fn.BeginAddress && rva < fn.EndAddress)
        {
            printf("function containing +0x%llX: +0x%X..+0x%X (size 0x%X, unwind +0x%X)\n",
                (unsigned long long)rva, fn.BeginAddress, fn.EndAddress,
                fn.EndAddress - fn.BeginAddress, fn.UnwindData);
            CloseHandle(h);
            return 0;
        }
    }
    printf("no runtime function contains +0x%llX\n", (unsigned long long)rva);
    CloseHandle(h);
    return 1;
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2) { printf("usage: camscan attach|first|narrow <inc|dec|changed|unchanged>|dump [max]\n"); return 2; }
    std::wstring cmd = argv[1];
    if (cmd == L"attach") return CmdAttach();
    if (cmd == L"first")  return CmdFirst(argc >= 3 && std::wstring(argv[2]) == L"halo3");
    if (cmd == L"narrow")
    {
        if (argc < 3) { printf("narrow needs a mode: inc|dec|changed|unchanged\n"); return 2; }
        char mode[32]; size_t n = 0; wcstombs_s(&n, mode, argv[2], sizeof(mode));
        return CmdNarrow(mode);
    }
    if (cmd == L"dump")   return CmdDump(argc >= 3 ? _wtoi(argv[2]) : 40);
    if (cmd == L"clusters") return CmdClusters(argc >= 3 ? _wtoi(argv[2]) : 16);
    if (cmd == L"watch")
    {
        uint64_t rva = argc >= 3 ? wcstoull(argv[2], nullptr, 16) : 0;
        int n = argc >= 4 ? _wtoi(argv[3]) : 16;
        double secs = argc >= 5 ? _wtof(argv[4]) : 10.0;
        return CmdWatch(rva, n, secs);
    }
    if (cmd == L"poke")
    {
        if (argc < 7) { printf("poke <rva> <x> <y> <z> <secs>\n"); return 2; }
        uint64_t rva = wcstoull(argv[2], nullptr, 16);
        return CmdPoke(rva, (float)_wtof(argv[3]), (float)_wtof(argv[4]), (float)_wtof(argv[5]), _wtof(argv[6]));
    }
    if (cmd == L"spin")
    {
        if (argc < 3) { printf("spin <rva> [secs]\n"); return 2; }
        uint64_t rva = wcstoull(argv[2], nullptr, 16);
        return CmdSpin(rva, argc >= 4 ? _wtof(argv[3]) : 6.0);
    }
    if (cmd == L"xref")
    {
        if (argc < 3) { printf("xref <rva> [rangeBytes]\n"); return 2; }
        uint64_t rva = wcstoull(argv[2], nullptr, 16);
        return CmdXref(rva, argc >= 4 ? wcstoull(argv[3], nullptr, 0) : 4);
    }
    if (cmd == L"findwrite")
    {
        if (argc < 3) { printf("findwrite <rva> [hits] [secs]\n"); return 2; }
        uint64_t rva = wcstoull(argv[2], nullptr, 16);
        return CmdFindWrite(rva, argc >= 4 ? _wtoi(argv[3]) : 6, argc >= 5 ? _wtof(argv[4]) : 8.0);
    }
    if (cmd == L"aimwrite")
        return CmdAimWrite(argc >= 3 ? _wtoi(argv[2]) : 8, argc >= 4 ? _wtof(argv[3]) : 8.0);
    if (cmd == L"pokeabs") // WRITE TEST at an absolute address: force 1-2 floats for secs
    {
        if (argc < 4) { printf("pokeabs <absaddr> <v0> [v1] [secs]\n"); return 2; }
        const uint64_t addr = wcstoull(argv[2], nullptr, 16);
        float v[2] = {(float)_wtof(argv[3]), 0};
        const int nv = argc >= 5 ? 2 : 1;
        if (nv == 2) v[1] = (float)_wtof(argv[4]);
        const double secs = argc >= 6 ? _wtof(argv[5]) : 4.0;
        DWORD pid = FindPid(kGameExe);
        if (!pid) { printf("GAME NOT RUNNING.\n"); return 1; }
        HANDLE h = OpenProcess(PROCESS_VM_WRITE | PROCESS_VM_OPERATION | PROCESS_QUERY_INFORMATION, FALSE, pid);
        if (!h) { printf("OpenProcess failed (%lu)\n", GetLastError()); return 1; }
        printf("forcing %d float(s) (%.4f%s%.4f) at 0x%llX for %.0fs — watch the screen:\n",
               nv, v[0], nv == 2 ? ", " : "", nv == 2 ? v[1] : 0.0f, addr, secs);
        const DWORD start = GetTickCount();
        SIZE_T put = 0;
        while ((GetTickCount() - start) < (DWORD)(secs * 1000))
        {
            WriteProcessMemory(h, (LPVOID)addr, v, nv * sizeof(float), &put);
            Sleep(2);
        }
        CloseHandle(h);
        printf("done.\n");
        return 0;
    }
    if (cmd == L"watchabs")
    {
        if (argc < 3) { printf("watchabs <absaddr> [n] [secs]\n"); return 2; }
        return CmdWatchAbs(wcstoull(argv[2], nullptr, 16),
                           argc >= 4 ? _wtoi(argv[3]) : 16, argc >= 5 ? _wtof(argv[4]) : 10.0);
    }
    if (cmd == L"aimchain")
        return CmdAimChain(argc >= 3 ? _wtof(argv[2]) : 8.0);
    if (cmd == L"findwriteabs") // absolute heap address (for chasing buffer chains)
    {
        if (argc < 3) { printf("findwriteabs <absaddr> [hits] [secs]\n"); return 2; }
        return FindWriteAt(wcstoull(argv[2], nullptr, 16),
                           argc >= 4 ? _wtoi(argv[3]) : 8, argc >= 5 ? _wtof(argv[4]) : 8.0);
    }
    if (cmd == L"hex")
    {
        if (argc < 3) { printf("hex <rva> [nbytes]\n"); return 2; }
        uint64_t rva = wcstoull(argv[2], nullptr, 16);
        return CmdHex(rva, argc >= 4 ? _wtoi(argv[3]) : 64);
    }
    if (cmd == L"callers")
    {
        if (argc < 3) { printf("callers <targetRva>\n"); return 2; }
        return CmdCallers(wcstoull(argv[2], nullptr, 16));
    }
    if (cmd == L"function")
    {
        if (argc < 3) { printf("function <rva>\n"); return 2; }
        return CmdFunction(wcstoull(argv[2], nullptr, 16));
    }
    printf("unknown command\n");
    return 2;
}
