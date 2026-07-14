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

    // Start OpenXR now, on this background thread, so the slow instance
    // creation overlaps game loading instead of stalling the render thread.
    VR_InitInstance();

    // Start watching for halo3.dll (loads when a level begins) so we can find
    // the camera for head tracking. Runs on its own thread.
    Game_Init();
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
