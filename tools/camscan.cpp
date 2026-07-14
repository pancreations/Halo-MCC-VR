// camscan.cpp — external, READ-ONLY memory finder for the Halo 3 camera.
//
// This replaces having the user drive Cheat Engine. It attaches to the running
// game with read-only access and does a differential float scan: the user just
// looks up / down / holds still on cue, and this tool narrows millions of
// values down to the handful that track the camera. It never writes to the
// game — it only reads — so it cannot break or crash anything.
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

// Attach as a debugger, set a hardware write-watchpoint on halo3.dll+rva, and
// record the game instructions (and CPU registers) that write it. The register
// that holds a value near the target is the base pointer of the real camera
// structure — that's what we actually need. Detaches cleanly.
static int CmdFindWrite(uint64_t rva, int wantHits, double timeoutSecs)
{
    DWORD pid = FindPid(kGameExe);
    if (!pid) { printf("GAME NOT RUNNING.\n"); return 1; }
    uint64_t hbase = 0, hsize = 0;
    if (!ModuleRange(pid, kEngineDll, hbase, hsize)) { printf("%ls not loaded.\n", kEngineDll); return 1; }
    const uint64_t addr = hbase + rva;

    if (!DebugActiveProcess(pid)) { printf("DebugActiveProcess failed (%lu).\n", GetLastError()); return 1; }
    DebugSetProcessKillOnExit(FALSE); // if we die, the game must survive
    printf("watching writes to %ls+0x%llX (=0x%llX) — look around now:\n", kEngineDll, rva, addr);

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
                        printf("write #%d by %ls+0x%llX\n", hits, kEngineDll, c.Rip - hbase);
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
    if (cmd == L"hex")
    {
        if (argc < 3) { printf("hex <rva> [nbytes]\n"); return 2; }
        uint64_t rva = wcstoull(argv[2], nullptr, 16);
        return CmdHex(rva, argc >= 4 ? _wtoi(argv[3]) : 64);
    }
    printf("unknown command\n");
    return 2;
}
