#include <windows.h>
#include <string>
#include <MinHook.h>
#include "../common/log.h"
#include "../common/config.h"
#include "d3d11_hook.h"
#include "vr.h"
#include "game.h"

// Entry point of the injected DLL. DllMain itself must do almost nothing
// (Windows holds a global "loader lock" while it runs), so we immediately
// spawn a thread that does the real setup.

static HMODULE g_hModule = nullptr;

static DWORD WINAPI InitThread(LPVOID)
{
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(g_hModule, path, MAX_PATH);
    std::wstring dir(path);
    dir.resize(dir.find_last_of(L'\\') + 1);

    LogInit((dir + L"halo3xr.log").c_str());
    LOG("halo3xr M0 loaded into pid %lu", GetCurrentProcessId());
    ConfigLoad((dir + L"halo3xr.cfg").c_str());

    if (MH_Initialize() != MH_OK)
    {
        LOG("FATAL: MinHook failed to initialize; mod inactive");
        return 0;
    }
    if (!InstallD3D11Hooks())
    {
        LOG("FATAL: D3D11 hooks failed to install; mod inactive");
        return 0;
    }
    LOG("D3D11 hooks installed; waiting for the game to render its first frame");

    // Install input interception immediately. OpenXR instance creation below
    // blocks for roughly 20 seconds while SteamVR starts; waiting until after
    // that made MCC finish controller enumeration before our hooks existed.
    // This same worker also waits for halo3.dll and installs camera hooks.
    Game_Init();

    // Start OpenXR on this background thread so the slow runtime startup does
    // not stall the render thread. Input interception is already active above.
    VR_InitInstance();
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_hModule = hModule;
        DisableThreadLibraryCalls(hModule);
        HANDLE t = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
        if (t)
            CloseHandle(t);
    }
    return TRUE;
}
