#include <windows.h>
#include <tlhelp32.h>
#include <string>
#include <cstdio>
#include <cstdarg>

// Starts MCC-Win64-Shipping.exe directly — which is exactly what Steam's
// official "Play without anti-cheat" option runs, so EAC is never started —
// and loads halo3xr.dll into it before the game's first instruction runs.
//
// Injection method: classic CreateRemoteThread + LoadLibraryW. The process is
// created suspended, a remote thread loads our DLL, then the game resumes.
//
// The game is launched with the SteamAppId environment variable set so that
// Steamworks does not bounce it ("relaunch me through Steam"), which would
// kill the process we just injected into and start a fresh, unmodded one.

static const wchar_t* kSteamAppId = L"976730"; // Halo: The Master Chief Collection

static std::wstring g_logPath;

static void LauncherLog(const char* fmt, ...)
{
    if (g_logPath.empty())
        return;
    FILE* f = nullptr;
    _wfopen_s(&f, g_logPath.c_str(), L"at");
    if (!f)
        return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(f, "[%02u:%02u:%02u.%03u] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);
    fputc('\n', f);
    fclose(f);
}

static void ErrorBox(const std::wstring& text)
{
    LauncherLog("ERROR shown to user: (see message box)");
    MessageBoxW(nullptr, text.c_str(), L"Halo 3 VR launcher", MB_OK | MB_ICONERROR | MB_TOPMOST);
}

static std::wstring WinErr(DWORD code)
{
    wchar_t* buf = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, code, 0, (LPWSTR)&buf, 0, nullptr);
    std::wstring s = buf ? buf : L"(unknown error)";
    if (buf)
        LocalFree(buf);
    wchar_t num[32];
    swprintf_s(num, L" (code %lu)", code);
    return s + num;
}

static bool FileExists(const std::wstring& path)
{
    const DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

static bool ProcessRunning(const wchar_t* exeName)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return false;
    PROCESSENTRY32W pe{sizeof(pe)};
    bool found = false;
    if (Process32FirstW(snap, &pe))
    {
        do
        {
            if (_wcsicmp(pe.szExeFile, exeName) == 0)
            {
                found = true;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

int APIENTRY wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int)
{
    wchar_t selfPath[MAX_PATH];
    GetModuleFileNameW(nullptr, selfPath, MAX_PATH);
    std::wstring dir(selfPath);
    dir.resize(dir.find_last_of(L'\\')); // folder containing the launcher, no trailing slash

    g_logPath = dir + L"\\halo3xr_launcher.log";
    // fresh log each launch
    {
        FILE* f = nullptr;
        _wfopen_s(&f, g_logPath.c_str(), L"wt");
        if (f) fclose(f);
    }
    LauncherLog("launcher started; self dir = %ls", dir.c_str());

    const std::wstring dllPath = dir + L"\\halo3xr.dll";
    if (!FileExists(dllPath))
    {
        ErrorBox(L"halo3xr.dll is missing from:\n" + dir +
                 L"\n\nReinstall the mod (run install.bat again).");
        return 1;
    }

    // The launcher lives in <game>\halo3xr\, so the game exe is found by
    // walking up from here. A few levels are tried so a dev copy placed
    // elsewhere inside the game folder also works.
    std::wstring gameExe, gameDir, probe = dir;
    for (int i = 0; i < 6 && !probe.empty(); i++)
    {
        const std::wstring cand = probe + L"\\MCC\\Binaries\\Win64\\MCC-Win64-Shipping.exe";
        if (FileExists(cand))
        {
            gameExe = cand;
            gameDir = probe + L"\\MCC\\Binaries\\Win64";
            break;
        }
        const size_t slash = probe.find_last_of(L'\\');
        if (slash == std::wstring::npos)
            break;
        probe.resize(slash);
    }
    if (gameExe.empty())
    {
        ErrorBox(L"Could not find MCC-Win64-Shipping.exe near:\n" + dir +
                 L"\n\nThe mod folder must be inside the game's install folder.\n"
                 L"Run install.bat again.");
        return 1;
    }
    LauncherLog("game exe = %ls", gameExe.c_str());

    if (!ProcessRunning(L"steam.exe"))
    {
        ErrorBox(L"Steam is not running.\n\nStart Steam first, then run this launcher again.\n"
                 L"(The game needs Steam to verify ownership.)");
        return 1;
    }
    if (ProcessRunning(L"MCC-Win64-Shipping.exe"))
    {
        ErrorBox(L"Halo: The Master Chief Collection is already running.\n\n"
                 L"Close it first, then run this launcher again.");
        return 1;
    }

    // Make Steamworks believe the game was launched correctly, so it does not
    // ask Steam to relaunch it (which would drop the process we inject into).
    // The child process inherits these environment variables.
    SetEnvironmentVariableW(L"SteamAppId", kSteamAppId);
    SetEnvironmentVariableW(L"SteamGameId", kSteamAppId);
    SetEnvironmentVariableW(L"SteamOverlayGameId", kSteamAppId);
    LauncherLog("set SteamAppId=%ls; creating game process (suspended)", kSteamAppId);

    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    // M2 stereo: render the game into a near-square surface matching a VR
    // eye, rather than stretching the normal 16:9 desktop frame into the
    // headset's 2720x2772 projection. Windowed mode avoids asking the monitor
    // for a non-standard exclusive-fullscreen display mode.
    // PSVR2's symmetric coverage has a tangent-space aspect near 1.386:1.
    // 2912x2100 keeps approximately the same pixel cost as 2448x2496 while
    // allowing Halo to generate the correct wide raster/culling frustum.
    std::wstring cmdline = L"\"" + gameExe + L"\" -WINDOWED -ResX=2912 -ResY=2100";
    LauncherLog("VR render command line: -WINDOWED -ResX=2912 -ResY=2100");
    if (!CreateProcessW(gameExe.c_str(), cmdline.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED,
                        nullptr, gameDir.c_str(), &si, &pi))
    {
        const DWORD e = GetLastError();
        LauncherLog("CreateProcessW failed: %lu", e);
        ErrorBox(L"Could not start the game:\n" + WinErr(e));
        return 1;
    }
    LauncherLog("game process created, pid %lu", pi.dwProcessId);

    // Write the DLL path into the game process and make it call LoadLibraryW
    // on it. LoadLibraryW lives at the same address in every 64-bit process.
    bool injected = false;
    std::wstring failWhy;
    const SIZE_T bytes = (dllPath.size() + 1) * sizeof(wchar_t);
    void* remoteMem = VirtualAllocEx(pi.hProcess, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remoteMem)
        failWhy = L"VirtualAllocEx: " + WinErr(GetLastError());
    else if (!WriteProcessMemory(pi.hProcess, remoteMem, dllPath.c_str(), bytes, nullptr))
        failWhy = L"WriteProcessMemory: " + WinErr(GetLastError());
    else
    {
        auto loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(
            GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
        HANDLE thread = CreateRemoteThread(pi.hProcess, nullptr, 0, loadLibrary, remoteMem, 0, nullptr);
        if (!thread)
            failWhy = L"CreateRemoteThread: " + WinErr(GetLastError());
        else
        {
            if (WaitForSingleObject(thread, 15000) == WAIT_OBJECT_0)
            {
                DWORD exitCode = 0;
                GetExitCodeThread(thread, &exitCode);
                if (exitCode != 0) // LoadLibrary returns the module handle; 0 means it failed
                    injected = true;
                else
                    failWhy = L"LoadLibraryW returned NULL inside the game "
                              L"(missing dependency or blocked DLL?)";
            }
            else
                failWhy = L"the injection thread timed out";
            CloseHandle(thread);
        }
        VirtualFreeEx(pi.hProcess, remoteMem, 0, MEM_RELEASE);
    }

    if (!injected)
    {
        LauncherLog("injection FAILED: %ls", failWhy.c_str());
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        ErrorBox(L"Could not load the VR mod into the game, so the game was closed.\n\n"
                 L"Reason: " + failWhy);
        return 1;
    }
    LauncherLog("DLL injected OK; resuming game");

    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    // Watch the game for a short while. If it exits almost immediately, it
    // bounced through Steam or crashed on startup — capture the exit code so
    // we can see what happened instead of the process silently vanishing.
    const DWORD watchMs = 12000;
    const DWORD waitRes = WaitForSingleObject(pi.hProcess, watchMs);
    if (waitRes == WAIT_OBJECT_0)
    {
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        LauncherLog("game process EXITED within %lus, exit code %lu (0x%08X)", watchMs / 1000, code, code);
        CloseHandle(pi.hProcess);
        ErrorBox(L"The game closed itself right after starting (exit code " +
                 std::to_wstring(code) +
                 L").\n\nThis usually means it relaunched through Steam or the anti-cheat-off\n"
                 L"mode refused this launch. Details are in halo3xr_launcher.log.");
        return 1;
    }
    LauncherLog("game still running after %lus - launch looks good", watchMs / 1000);
    CloseHandle(pi.hProcess);
    return 0;
}
