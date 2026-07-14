#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include <MinHook.h>
#include "game.h"
#include "sigscan.h"
#include "vr.h"
#include "../common/log.h"

// M1 head tracking. We hook the game's per-frame camera-update function and,
// each frame, overwrite the authoritative camera's forward/up vectors with the
// direction of the headset. Writing from inside the game's own frame (rather
// than poking memory from a thread) means our value lands at exactly the right
// moment and holds steady instead of flickering.
//
// The hooked function (RVA 0x2A628C for build 1.3528.0.0) is __fastcall(dst,
// src): it copies the camera from src (a double-buffered heap struct) into the
// static gun-camera. src+0x28 = forward (3 floats), src+0x34 = up (3 floats).
// See docs/RE-notes.md. These offsets are build-specific and must become AOB
// signatures before shipping.

namespace
{
    constexpr uintptr_t kCamCopyRva = 0x2A628C; // fastcall(dst, src) camera copy
    constexpr uintptr_t kSrcFwd = 0x28;         // forward vec offset in src
    constexpr uintptr_t kSrcUp = 0x34;          // up vec offset in src

    std::atomic<bool> g_hooked{false};
    std::atomic<bool> g_enabled{false};      // F2
    std::atomic<bool> g_needRecenter{true};   // F3
    std::atomic<float> g_yawSign{-1.0f};       // F4  (default matches PSVR2 mapping)
    std::atomic<float> g_pitchSign{1.0f};      // F5
    std::atomic<float> g_pitchTrim{0.0f};      // F8/F9, radians
    std::atomic<bool> g_writeUp{true};         // F7

    // Yaw is relative (the game's heading is arbitrary, so we recenter it to
    // the head). Pitch is absolute (head-level == game-level), which avoids
    // capturing a bad reference on recenter.
    float g_headYawRef = 0;
    float g_gameYawRef = 0;

    using CamCopyFn = void*(__fastcall*)(void* dst, void* src);
    CamCopyFn g_origCamCopy = nullptr;

    float Clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
    float WrapPi(float a) { while (a > 3.14159265f) a -= 6.2831853f; while (a < -3.14159265f) a += 6.2831853f; return a; }

    // Head yaw/pitch (radians) from the OpenXR head quaternion. Forward is -Z,
    // up is +Y in OpenXR; yaw 0 = looking along -Z, pitch + = looking up.
    bool HeadYawPitch(float& yaw, float& pitch)
    {
        float q[4], p[3];
        if (!VR_GetHeadPose(q, p))
            return false;
        const float x = q[0], y = q[1], z = q[2], w = q[3];
        const float fx = -2.0f * (w * y + x * z);
        const float fy =  2.0f * (w * x - y * z);
        const float fz = -(1.0f - 2.0f * (x * x + y * y));
        yaw = atan2f(fx, -fz);
        pitch = asinf(Clamp(fy, -1.0f, 1.0f));
        return true;
    }

    void ApplyHeadLook(void* src)
    {
        if (!src)
            return;
        float* fwd = reinterpret_cast<float*>(reinterpret_cast<char*>(src) + kSrcFwd);
        float* up = reinterpret_cast<float*>(reinterpret_cast<char*>(src) + kSrcUp);

        float hy, hp;
        if (!HeadYawPitch(hy, hp))
            return;

        if (g_needRecenter.exchange(false))
        {
            g_gameYawRef = atan2f(fwd[1], fwd[0]); // align current head to current heading
            g_headYawRef = hy;
            LOG("head tracking recentered (game yaw %.1f deg)", g_gameYawRef * 57.2958f);
        }

        const float gy = g_gameYawRef + g_yawSign.load() * WrapPi(hy - g_headYawRef);
        const float gp = Clamp(g_pitchSign.load() * hp + g_pitchTrim.load(), -1.5f, 1.5f);
        const float cgp = cosf(gp), sgp = sinf(gp), cgy = cosf(gy), sgy = sinf(gy);

        fwd[0] = cgp * cgy; fwd[1] = cgp * sgy; fwd[2] = sgp;
        if (g_writeUp.load())
        {
            up[0] = -sgp * cgy; up[1] = -sgp * sgy; up[2] = cgp;
        }
    }

    void* __fastcall CamCopyHook(void* dst, void* src)
    {
        if (g_enabled.load())
            ApplyHeadLook(src); // overwrite the authoritative camera before it's copied/used
        return g_origCamCopy(dst, src);
    }

    void InstallHook(uintptr_t base)
    {
        void* target = reinterpret_cast<void*>(base + kCamCopyRva);
        if (MH_CreateHook(target, reinterpret_cast<void*>(&CamCopyHook),
                          reinterpret_cast<void**>(&g_origCamCopy)) != MH_OK ||
            MH_EnableHook(target) != MH_OK)
        {
            LOG("M1: FAILED to hook camera-copy at %p; head tracking unavailable", target);
            return;
        }
        LOG("M1: camera-copy hooked at %p (halo3.dll+0x%llX)", target, (unsigned long long)kCamCopyRva);
        LOG("M1: F2 toggle head tracking, F3 recenter, F4/F5 flip yaw/pitch, F7 up-vector");
    }

    DWORD WINAPI WaitThread(LPVOID)
    {
        for (;;)
        {
            uintptr_t base = 0;
            size_t size = 0;
            if (sig::ModuleRange(L"halo3.dll", base, size))
            {
                LOG("halo3.dll loaded at %p, size 0x%zX", (void*)base, size);
                InstallHook(base);
                g_hooked = true;
                return 0;
            }
            Sleep(1000);
        }
    }
}

void Game_Init()
{
    CreateThread(nullptr, 0, WaitThread, nullptr, 0, nullptr);
}

bool Game_IsHooked() { return g_hooked; }
bool Game_IsHeadTracking() { return g_enabled.load(); }

void Game_ToggleHeadTracking()
{
    const bool on = !g_enabled.load();
    g_enabled = on;
    if (on)
        g_needRecenter = true;
    LOG("head tracking %s", on ? "ON" : "OFF");
}

void Game_Recenter() { g_needRecenter = true; }
void Game_FlipYaw()   { g_yawSign = -g_yawSign.load();   LOG("yaw sign %+.0f", g_yawSign.load()); }
void Game_FlipPitch() { g_pitchSign = -g_pitchSign.load(); LOG("pitch sign %+.0f", g_pitchSign.load()); }
void Game_ToggleUp()  { g_writeUp = !g_writeUp.load();   LOG("write up-vector %s", g_writeUp.load() ? "on" : "off"); }
void Game_CycleTarget() { LOG("target cycle: n/a (hooked camera-copy)"); }

void Game_PitchTrim(int dir)
{
    const float t = Clamp(g_pitchTrim.load() + dir * 0.035f, -0.8f, 0.8f); // ~2 deg steps
    g_pitchTrim = t;
    LOG("pitch trim %.1f deg", t * 57.2958f);
}
