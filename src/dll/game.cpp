#include <windows.h>
#include <psapi.h>
#include <atomic>
#include "game.h"
#include "sigscan.h"
#include "vr.h"
#include "../common/log.h"

// Waits for halo3.dll, then locates the camera (M1). Everything that touches
// the engine goes through here so the RE-specific, patch-fragile code is in
// one place.

namespace
{
    std::atomic<bool> g_hooked{false};

    void OnHalo3Loaded(uintptr_t base, size_t size)
    {
        LOG("halo3.dll loaded at %p, size %zu bytes (0x%zX)", (void*)base, size, size);

        // M1 camera hook goes here once we have the signature from RE.
        // Placeholder so the structure is in place:
        //   uintptr_t hit = sig::FindInModule(L"halo3.dll", CAMERA_PATTERN);
        //   if (!hit) { LOG("camera signature not found - MCC update? head tracking disabled"); return; }
        //   ... resolve the camera pointer, install the write ...

        LOG("M1: camera signature not wired up yet (reverse engineering in progress)");
    }

    DWORD WINAPI WaitThread(LPVOID)
    {
        // Poll for the engine DLL. Entering a level can take a while, and the
        // user might sit at menus first, so we wait patiently and quietly.
        for (;;)
        {
            uintptr_t base = 0;
            size_t size = 0;
            if (sig::ModuleRange(L"halo3.dll", base, size))
            {
                OnHalo3Loaded(base, size);
                g_hooked = true;
                return 0;
            }
            Sleep(1000);
        }
    }
}

void Game_Init()
{
    HANDLE t = CreateThread(nullptr, 0, WaitThread, nullptr, 0, nullptr);
    if (t)
        CloseHandle(t);
    else
        LOG("game: could not start the halo3.dll waiter thread");
}

bool Game_IsHooked()
{
    return g_hooked;
}
