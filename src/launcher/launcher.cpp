#include <windows.h>
#include <tlhelp32.h>
#include <appmodel.h>
#include <shobjidl.h>
#include <objbase.h>
#include <string>
#include <vector>
#include <cstdio>
#include <cstdarg>
#include <cmath>
#include <algorithm>
// Constants only (native raster size and the resolution_scale limits) so the
// launcher and the DLL's config clamp can never disagree. The launcher does
// not link config.cpp; it reads the one line it needs itself.
#include "../common/config.h"

// Launches Halo: The Master Chief Collection without Easy Anti-Cheat — exactly
// what MCC's official "anti-cheat disabled" mode runs — and loads halo3xr.dll
// into it before (Steam) or as soon as possible after (Store) the game starts.
//
// Two store fronts are supported and auto-detected:
//
//   STEAM  The launcher sits inside the game install (…\Halo_MCC_VR\) and finds
//          MCC-Win64-Shipping.exe by walking up. The process is created
//          suspended, a remote thread loads our DLL, then the game resumes.
//          SteamAppId is set so Steamworks does not bounce the launch through
//          Steam (which would drop the process we injected into).
//
//   STORE  The Xbox / Microsoft Store build is a UWP package (Microsoft.Chelan)
//          living in the read-only WindowsApps folder, so it cannot be created
//          suspended from an arbitrary path — the license check requires it to
//          be started through the app model. We activate the package's
//          anti-cheat-disabled app (…!HaloMCCShippingNoEAC) with the same VR
//          render command line, then poll for MCCWinStore-Win64-Shipping.exe
//          and inject with CreateRemoteThread + LoadLibraryW the moment it
//          appears. The DLL's own init already waits for D3D11 and halo3.dll,
//          so slightly-later injection is safe.
//
// Injection method (both paths): classic CreateRemoteThread + LoadLibraryW.

static const wchar_t* kSteamAppId = L"976730"; // Halo: The Master Chief Collection

// Xbox / Microsoft Store package identifiers (verified from the installed
// Microsoft.Chelan appx manifest and MicrosoftGame.config).
static const wchar_t* kStorePackageFamily = L"Microsoft.Chelan_8wekyb3d8bbwe";
static const wchar_t* kStoreAppUserModelId =
    L"Microsoft.Chelan_8wekyb3d8bbwe!HaloMCCShippingNoEAC";
static const wchar_t* kStoreGameExeName = L"MCCWinStore-Win64-Shipping.exe";
static const wchar_t* kSteamGameExeName = L"MCC-Win64-Shipping.exe";

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
    MessageBoxW(nullptr, text.c_str(), L"Halo MCC VR launcher", MB_OK | MB_ICONERROR | MB_TOPMOST);
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

// Returns the PID of the first process matching exeName, or 0 if none.
static DWORD FindProcessId(const wchar_t* exeName)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return 0;
    PROCESSENTRY32W pe{sizeof(pe)};
    DWORD pid = 0;
    if (Process32FirstW(snap, &pe))
    {
        do
        {
            if (_wcsicmp(pe.szExeFile, exeName) == 0)
            {
                pid = pe.th32ProcessID;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

static float ReadResolutionScale(const std::wstring& path)
{
    float scale = 1.0f;
    FILE* f = nullptr;
    _wfopen_s(&f, path.c_str(), L"rt");
    if (f)
    {
        char line[512];
        while (fgets(line, sizeof(line), f))
        {
            float parsed = 1.0f;
            if (sscanf_s(line, " resolution_scale = %f", &parsed) == 1 &&
                std::isfinite(parsed))
                scale = parsed;
        }
        fclose(f);
    }
    // Any value in range is honored exactly; the named tiers are only F1
    // shortcuts. (Before 2026-07-20 this snapped to those six tiers,
    // so a hand-typed 0.90 silently became 0.80.)
    return std::clamp(scale, kResolutionScaleMin, kResolutionScaleMax);
}

static int ScaleEven(int base, float scale)
{
    int value = static_cast<int>(std::lround(static_cast<float>(base) * scale));
    if (value & 1)
        ++value;
    return value;
}

// Builds the VR render command-line suffix ("-WINDOWED -ResX=.. -ResY=..") from
// resolution_scale, and logs the pixel size chosen. Shared by both store paths.
static std::wstring BuildRenderArgs(const std::wstring& launcherDir)
{
    const std::wstring primaryConfig = launcherDir + L"/halomccvr.cfg";
    const std::wstring legacyConfig = launcherDir + L"/halo3xr.cfg";
    const DWORD primaryAttributes = GetFileAttributesW(primaryConfig.c_str());
    const bool primaryExists = primaryAttributes != INVALID_FILE_ATTRIBUTES &&
        (primaryAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
    const float resolutionScale =
        ReadResolutionScale(primaryExists ? primaryConfig : legacyConfig);
    const int renderWidth = ScaleEven(kNativeRenderWidth, resolutionScale);
    const int renderHeight = ScaleEven(kNativeRenderHeight, resolutionScale);
    wchar_t renderArgs[96];
    // M2 stereo: render the game into a wide surface matching Halo's VR
    // projection rather than the normal 16:9 desktop frame. resolution_scale
    // reduces both dimensions together, preserving Halo's culling/projection.
    swprintf_s(renderArgs, L"-WINDOWED -ResX=%d -ResY=%d", renderWidth, renderHeight);
    LauncherLog("VR render command line: %ls (resolution_scale %.2f; native %dx%d)",
                renderArgs, resolutionScale, kNativeRenderWidth, kNativeRenderHeight);
    return renderArgs;
}

// CreateRemoteThread + LoadLibraryW injection into an already-open process
// handle. hProcess must have PROCESS_CREATE_THREAD | VM_OPERATION | VM_WRITE |
// VM_READ | QUERY_INFORMATION. Returns true on success; failWhy is set on error.
static bool InjectDll(HANDLE hProcess, const std::wstring& dllPath, std::wstring& failWhy)
{
    const SIZE_T bytes = (dllPath.size() + 1) * sizeof(wchar_t);
    void* remoteMem = VirtualAllocEx(hProcess, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remoteMem)
    {
        failWhy = L"VirtualAllocEx: " + WinErr(GetLastError());
        return false;
    }
    bool injected = false;
    if (!WriteProcessMemory(hProcess, remoteMem, dllPath.c_str(), bytes, nullptr))
    {
        failWhy = L"WriteProcessMemory: " + WinErr(GetLastError());
    }
    else
    {
        auto loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(
            GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
        HANDLE thread = CreateRemoteThread(hProcess, nullptr, 0, loadLibrary, remoteMem, 0, nullptr);
        if (!thread)
        {
            failWhy = L"CreateRemoteThread: " + WinErr(GetLastError());
        }
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
    }
    VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
    return injected;
}

// Returns the install folder of the Microsoft.Chelan (Store MCC) package, or an
// empty string if the Store build is not installed for this user.
static std::wstring FindStorePackagePath()
{
    UINT32 count = 0, length = 0;
    LONG rc = GetPackagesByPackageFamily(kStorePackageFamily, &count, nullptr, &length, nullptr);
    if (rc != ERROR_INSUFFICIENT_BUFFER || count == 0)
        return L"";
    std::vector<PWSTR> fullNames(count);
    std::vector<wchar_t> buffer(length);
    rc = GetPackagesByPackageFamily(kStorePackageFamily, &count, fullNames.data(),
                                    &length, buffer.data());
    if (rc != ERROR_SUCCESS || count == 0)
        return L"";

    UINT32 pathLen = 0;
    rc = GetPackagePathByFullName(fullNames[0], &pathLen, nullptr);
    if (rc != ERROR_INSUFFICIENT_BUFFER || pathLen == 0)
        return L"";
    std::vector<wchar_t> pathBuf(pathLen);
    rc = GetPackagePathByFullName(fullNames[0], &pathLen, pathBuf.data());
    if (rc != ERROR_SUCCESS)
        return L"";
    return std::wstring(pathBuf.data());
}

// STEAM path: create the game suspended, inject, resume, and watch for an
// immediate exit. Returns process exit code semantics via ErrorBox on failure.
static int LaunchSteam(const std::wstring& dir, const std::wstring& dllPath,
                       const std::wstring& gameExe, const std::wstring& gameDir)
{
    if (!ProcessRunning(L"steam.exe"))
    {
        ErrorBox(L"Steam is not running.\n\nStart Steam first, then run this launcher again.\n"
                 L"(The game needs Steam to verify ownership.)");
        return 1;
    }
    if (ProcessRunning(kSteamGameExeName))
    {
        ErrorBox(L"Halo: The Master Chief Collection is already running.\n\n"
                 L"Close it first, then run this launcher again.");
        return 1;
    }

    // Make Steamworks believe the game was launched correctly, so it does not
    // ask Steam to relaunch it (which would drop the process we inject into).
    SetEnvironmentVariableW(L"SteamAppId", kSteamAppId);
    SetEnvironmentVariableW(L"SteamGameId", kSteamAppId);
    SetEnvironmentVariableW(L"SteamOverlayGameId", kSteamAppId);
    LauncherLog("STEAM mode: set SteamAppId=%ls; creating game process (suspended)", kSteamAppId);

    const std::wstring renderArgs = BuildRenderArgs(dir);
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    const wchar_t quote = 34;
    std::wstring cmdline(1, quote);
    cmdline += gameExe;
    cmdline += quote;
    cmdline += L" ";
    cmdline += renderArgs;
    if (!CreateProcessW(gameExe.c_str(), cmdline.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED,
                        nullptr, gameDir.c_str(), &si, &pi))
    {
        const DWORD e = GetLastError();
        LauncherLog("CreateProcessW failed: %lu", e);
        ErrorBox(L"Could not start the game:\n" + WinErr(e));
        return 1;
    }
    LauncherLog("game process created, pid %lu", pi.dwProcessId);

    std::wstring failWhy;
    const bool injected = InjectDll(pi.hProcess, dllPath, failWhy);
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

// STORE path: activate the anti-cheat-disabled UWP app with the VR render args,
// then poll for the shipping exe and inject once it appears.
static int LaunchStore(const std::wstring& dir, const std::wstring& dllPath,
                       const std::wstring& packagePath)
{
    if (ProcessRunning(kStoreGameExeName))
    {
        ErrorBox(L"Halo: The Master Chief Collection is already running.\n\n"
                 L"Close it first, then run this launcher again.");
        return 1;
    }

    const std::wstring renderArgs = BuildRenderArgs(dir);
    LauncherLog("STORE mode: package at %ls; activating %ls",
                packagePath.c_str(), kStoreAppUserModelId);

    HRESULT hrCom = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    IApplicationActivationManager* mgr = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ApplicationActivationManager, nullptr,
                                  CLSCTX_LOCAL_SERVER, IID_IApplicationActivationManager,
                                  reinterpret_cast<void**>(&mgr));
    if (FAILED(hr) || !mgr)
    {
        LauncherLog("CoCreateInstance(ApplicationActivationManager) failed: 0x%08X", hr);
        if (SUCCEEDED(hrCom)) CoUninitialize();
        ErrorBox(L"Could not start the Store version of the game.\n\n"
                 L"Windows would not create the app activation manager (0x" +
                 WinErr((DWORD)hr) + L").");
        return 1;
    }

    DWORD activatedPid = 0;
    hr = mgr->ActivateApplication(kStoreAppUserModelId, renderArgs.c_str(),
                                  AO_NONE, &activatedPid);
    mgr->Release();
    if (FAILED(hr))
    {
        LauncherLog("ActivateApplication failed: 0x%08X", hr);
        if (SUCCEEDED(hrCom)) CoUninitialize();
        ErrorBox(L"Could not start the anti-cheat-disabled Store app.\n\n"
                 L"Activation failed (0x" + WinErr((DWORD)hr) +
                 L").\nMake sure Halo: MCC is installed from the Xbox app and has been\n"
                 L"launched once normally first.");
        return 1;
    }
    LauncherLog("activated app, launcher/helper pid %lu; polling for %ls",
                activatedPid, kStoreGameExeName);
    if (SUCCEEDED(hrCom)) CoUninitialize();

    // The shipping exe is a child of the game-launch helper and appears a few
    // seconds later. Poll until it shows up, then inject immediately. 90s covers
    // a slow first-run shader/EAC-removal step on a cold machine.
    DWORD gamePid = 0;
    const int maxWaitMs = 90000, stepMs = 100;
    for (int waited = 0; waited < maxWaitMs; waited += stepMs)
    {
        gamePid = FindProcessId(kStoreGameExeName);
        if (gamePid)
            break;
        Sleep(stepMs);
    }
    if (!gamePid)
    {
        LauncherLog("timed out waiting for %ls to start", kStoreGameExeName);
        ErrorBox(L"The Store game did not start within 90 seconds.\n\n"
                 L"If the Xbox app showed an update or a first-time setup, let it\n"
                 L"finish and try again. Details are in halo3xr_launcher.log.");
        return 1;
    }
    LauncherLog("found game process pid %lu; opening for injection", gamePid);

    const DWORD access = PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                         PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ;
    HANDLE hProcess = nullptr;
    std::wstring failWhy;
    // The exe exists a moment before it is injectable; retry OpenProcess briefly.
    for (int tries = 0; tries < 50 && !hProcess; ++tries)
    {
        hProcess = OpenProcess(access, FALSE, gamePid);
        if (!hProcess)
        {
            failWhy = L"OpenProcess: " + WinErr(GetLastError());
            Sleep(100);
        }
    }
    if (!hProcess)
    {
        LauncherLog("OpenProcess failed for pid %lu: %ls", gamePid, failWhy.c_str());
        ErrorBox(L"Found the game but could not attach the VR mod to it.\n\n"
                 L"Reason: " + failWhy +
                 L"\n\nTry running this launcher as administrator.");
        return 1;
    }

    const bool injected = InjectDll(hProcess, dllPath, failWhy);
    if (!injected)
    {
        LauncherLog("injection FAILED: %ls", failWhy.c_str());
        CloseHandle(hProcess);
        ErrorBox(L"Could not load the VR mod into the Store game.\n\n"
                 L"Reason: " + failWhy);
        return 1;
    }
    LauncherLog("DLL injected OK into Store game pid %lu", gamePid);

    // Watch briefly for an immediate exit (a licensing bounce closes fast).
    const DWORD watchMs = 12000;
    const DWORD waitRes = WaitForSingleObject(hProcess, watchMs);
    if (waitRes == WAIT_OBJECT_0)
    {
        DWORD code = 0;
        GetExitCodeProcess(hProcess, &code);
        LauncherLog("game process EXITED within %lus, exit code %lu (0x%08X)", watchMs / 1000, code, code);
        CloseHandle(hProcess);
        ErrorBox(L"The game closed itself right after starting (exit code " +
                 std::to_wstring(code) +
                 L").\n\nDetails are in halo3xr_launcher.log.");
        return 1;
    }
    LauncherLog("Store game still running after %lus - launch looks good", watchMs / 1000);
    CloseHandle(hProcess);
    return 0;
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
                 L"\n\nCopy halo3xr.dll from the package into this same folder,\n"
                 L"next to halo3xr_launcher.exe.");
        return 1;
    }

    // Prefer the STEAM build if the launcher sits inside a Steam install: walk
    // up looking for MCC-Win64-Shipping.exe. A few levels are tried so a dev
    // copy placed elsewhere inside the game folder also works.
    std::wstring gameExe, gameDir, probe = dir;
    for (int i = 0; i < 6 && !probe.empty(); i++)
    {
        const std::wstring cand = probe + L"\\MCC\\Binaries\\Win64\\" + kSteamGameExeName;
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

    if (!gameExe.empty())
    {
        LauncherLog("detected STEAM install; game exe = %ls", gameExe.c_str());
        return LaunchSteam(dir, dllPath, gameExe, gameDir);
    }

    // Otherwise look for the Xbox / Microsoft Store (UWP) build.
    const std::wstring storePath = FindStorePackagePath();
    if (!storePath.empty())
    {
        LauncherLog("detected STORE install; package path = %ls", storePath.c_str());
        return LaunchStore(dir, dllPath, storePath);
    }

    ErrorBox(L"Could not find Halo: The Master Chief Collection.\n\n"
             L"Steam copy: put the Halo_MCC_VR folder (with this launcher and\n"
             L"halo3xr.dll) inside the game's install folder - the one that\n"
             L"contains the MCC folder - then run it again.\n\n"
             L"Xbox / Microsoft Store copy: install MCC from the Xbox app and\n"
             L"launch it once normally first, then run this launcher again from\n"
             L"any folder.");
    return 1;
}
