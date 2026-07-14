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

    constexpr uintptr_t kSrcPos = 0x00;         // camera position (x,y,z) in src

    std::atomic<bool> g_hooked{false};
    std::atomic<bool> g_enabled{false};      // F2
    std::atomic<bool> g_needRecenter{true};   // F3
    std::atomic<float> g_yawSign{-1.0f};       // F4  (default matches PSVR2 mapping)
    std::atomic<float> g_pitchSign{1.0f};      // F5
    std::atomic<float> g_pitchTrim{0.0f};      // F8/F9, radians
    std::atomic<bool> g_writeUp{true};         // F7
    std::atomic<bool> g_positional{false};     // F6: leaning; off by default (opt-in)

    // World scale: Halo world units per real meter (1 wu ~= 3.05 m), so ~0.33
    // gives roughly 1:1 leaning. Offset is clamped so a bad value can't fling
    // the camera through the level.
    std::atomic<float> g_worldScale{0.33f};

    // Yaw is relative (the game's heading is arbitrary, so we recenter it to
    // the head). Pitch is absolute (head-level == game-level), which avoids
    // capturing a bad reference on recenter.
    float g_headYawRef = 0;
    float g_gameYawRef = 0;
    float g_headPosRef[3] = {0, 0, 0}; // headset position (m) captured at recenter

    using CamCopyFn = void*(__fastcall*)(void* dst, void* src);
    CamCopyFn g_origCamCopy = nullptr;

    float Clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
    float WrapPi(float a) { while (a > 3.14159265f) a -= 6.2831853f; while (a < -3.14159265f) a += 6.2831853f; return a; }

    void ApplyHeadLook(void* src)
    {
        if (!src)
            return;

        float q[4], hpos[3];
        if (!VR_GetHeadPose(q, hpos))
            return;
        // Head forward (OpenXR: -Z forward, +Y up).
        const float x = q[0], y = q[1], z = q[2], w = q[3];
        const float hfx = -2.0f * (w * y + x * z);
        const float hfy =  2.0f * (w * x - y * z);
        const float hfz = -(1.0f - 2.0f * (x * x + y * y));
        const float hy = atan2f(hfx, -hfz);
        const float hp = asinf(Clamp(hfy, -1.0f, 1.0f));

        float* fwd = reinterpret_cast<float*>(reinterpret_cast<char*>(src) + kSrcFwd);
        float* up = reinterpret_cast<float*>(reinterpret_cast<char*>(src) + kSrcUp);
        float* pos = reinterpret_cast<float*>(reinterpret_cast<char*>(src) + kSrcPos);

        if (g_needRecenter.exchange(false))
        {
            g_gameYawRef = atan2f(fwd[1], fwd[0]); // align current head to current heading
            g_headYawRef = hy;
            g_headPosRef[0] = hpos[0]; g_headPosRef[1] = hpos[1]; g_headPosRef[2] = hpos[2];
            LOG("head tracking recentered (game yaw %.1f deg)", g_gameYawRef * 57.2958f);
        }

        // Rotation: yaw relative + recenter, pitch absolute + trim.
        const float gy = g_gameYawRef + g_yawSign.load() * WrapPi(hy - g_headYawRef);
        const float gp = Clamp(g_pitchSign.load() * hp + g_pitchTrim.load(), -1.5f, 1.5f);
        const float cgp = cosf(gp), sgp = sinf(gp), cgy = cosf(gy), sgy = sinf(gy);

        fwd[0] = cgp * cgy; fwd[1] = cgp * sgy; fwd[2] = sgp;
        if (g_writeUp.load())
        {
            up[0] = -sgp * cgy; up[1] = -sgp * sgy; up[2] = cgp;
        }

        // Position (leaning): shift the camera by the headset's room-space move,
        // decomposed in the head's horizontal frame and re-applied in the game's
        // frame so it stays correct as you turn. Added to the game's own
        // position each frame (the sim rewrites pos before our hook, so this
        // does not accumulate).
        if (g_positional.load())
        {
            const float dx = hpos[0] - g_headPosRef[0];
            const float dy = hpos[1] - g_headPosRef[1];
            const float dz = hpos[2] - g_headPosRef[2];
            float hlen = sqrtf(hfx * hfx + hfz * hfz);
            if (hlen < 1e-4f) hlen = 1e-4f;
            const float hfhx = hfx / hlen, hfhz = hfz / hlen; // head forward (horizontal)
            const float fwdComp = -(dx * hfhx + dz * hfhz);    // room move along look dir
            const float rightComp = dx * (-hfhz) + dz * hfhx;  // room move to the right
            const float s = g_worldScale.load();
            float ox = (cgy * fwdComp + sgy * rightComp) * s;  // game forward/right at gy
            float oy = (sgy * fwdComp - cgy * rightComp) * s;
            float oz = dy * s;
            ox = Clamp(ox, -0.4f, 0.4f); oy = Clamp(oy, -0.4f, 0.4f); oz = Clamp(oz, -0.4f, 0.4f);
            pos[0] += ox; pos[1] += oy; pos[2] += oz;
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
        LOG("M1: F2 head tracking, F3 recenter, F4/F5 flip yaw/pitch, F6 leaning, F7 up, F8/F9 pitch trim, F10 screen-follow");
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

void Game_TogglePositional()
{
    const bool on = !g_positional.load();
    g_positional = on;
    if (on)
        g_needRecenter = true; // capture head position reference
    LOG("positional (leaning) %s", on ? "ON" : "OFF");
}

void Game_PitchTrim(int dir)
{
    const float t = Clamp(g_pitchTrim.load() + dir * 0.035f, -0.8f, 0.8f); // ~2 deg steps
    g_pitchTrim = t;
    LOG("pitch trim %.1f deg", t * 57.2958f);
}
