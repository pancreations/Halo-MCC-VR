#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include <MinHook.h>
#include "game.h"
#include "sigscan.h"
#include "vr.h"
#include "../common/log.h"
#include "../common/config.h"

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
    constexpr uintptr_t kSrcProjX = 0x68;       // horizontal projection/FOV scale
    constexpr uintptr_t kSrcProjY = 0x6C;       // vertical projection/FOV scale

    // Gun/overlay camera: first element of the engine's 4-slot camera-object
    // array (0x2820 bytes each; see docs/RE-notes.md). +0x30/+0x34 hold the
    // overlay frustum tangents (~0.858/0.874 = ~81 deg). The overlay is
    // stretched over the whole frame, so in the widened stereo raster
    // (~123 deg) the first-person weapon and HUD appear ~2x oversized unless
    // these tangents are rewritten to match the world projection.
    constexpr uintptr_t kGunCamRva = 0x2D2F680; // expected for build 1.3528
    constexpr uintptr_t kGunProjX = 0x30;
    constexpr uintptr_t kGunProjY = 0x34;

    std::atomic<bool> g_hooked{false};
    std::atomic<bool> g_renderHooked{false};
    std::atomic<bool> g_enabled{false};      // F2
    std::atomic<bool> g_needRecenter{true};   // F3 (yaw + position)
    std::atomic<bool> g_needPosRecenter{false}; // enabling leaning: position only, no yaw snap
    std::atomic<float> g_yawSign{-1.0f};       // F4  (default matches PSVR2 mapping)
    std::atomic<float> g_pitchSign{1.0f};      // F5
    std::atomic<float> g_pitchTrim{0.0f};      // F8/F9, radians
    std::atomic<bool> g_writeUp{true};         // F7
    std::atomic<bool> g_positional{true};      // M2: 6DOF head translation on by default; F6 toggles
    std::atomic<int> g_stereoEye{-1};           // M2: -1 mono, 0 left, 1 right

    // World scale: Halo world units per real meter (1 wu ~= 3.05 m), so ~0.33
    // gives roughly 1:1 leaning. Offset is clamped so a bad value can't fling
    // the camera through the level.
    std::atomic<float> g_worldScale{0.33f};
    std::atomic<float> g_projectionTanX{1.091595f};
    std::atomic<float> g_projectionTanY{1.114286f};
    std::atomic<float> g_renderHalfFovX{atanf(1.091595f)};
    std::atomic<float> g_renderHalfFovY{atanf(1.114286f)};

    std::atomic<uintptr_t> g_gunCamera{0};   // resolved from kGunCamRefSig
    std::atomic<float> g_gunFovScale{1.0f};  // Home/End: extra tangent multiplier
                                             // on the world match (>1 = smaller gun)

    // M3 VR aim: the game's own aim-driven camera forward, captured each frame
    // BEFORE the head-look overwrite. The XInput hook steers the game's aim
    // toward the right controller by comparing this with the controller ray.
    std::atomic<bool> g_vrAim{true};         // on by default; Insert toggles
    std::atomic<float> g_aimFwdX{1}, g_aimFwdY{0}, g_aimFwdZ{0};
    std::atomic<bool> g_aimSeen{false};

    // Yaw is relative (the game's heading is arbitrary, so we recenter it to
    // the head). Pitch is absolute (head-level == game-level), which avoids
    // capturing a bad reference on recenter.
    float g_headYawRef = 0;
    float g_gameYawRef = 0;
    float g_headPosRef[3] = {0, 0, 0}; // headset position (m) captured at recenter

    using CamCopyFn = void*(__fastcall*)(void* dst, void* src);
    CamCopyFn g_origCamCopy = nullptr;
    using RenderViewFn = void(__fastcall*)(void* view);
    RenderViewFn g_origRenderView = nullptr;
    using PrepareViewFn = void(__fastcall*)(void* view, int viewIndex);
    PrepareViewFn g_prepareView = nullptr;
    using BuildViewportFn = void(__fastcall*)(void* camera, void* temporary);
    using BuildMatricesFn = void(__fastcall*)(void* camera, void* temporary, void* output, float scale);
    BuildViewportFn g_buildViewport = nullptr;
    BuildMatricesFn g_buildMatrices = nullptr;

    float Clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
    float WrapPi(float a) { while (a > 3.14159265f) a -= 6.2831853f; while (a < -3.14159265f) a += 6.2831853f; return a; }

    // Rodrigues rotation of v (in place) about the unit axis by the angle
    // whose cosine/sine are given.
    void RotateAboutAxis(float* v, const float* axis, float cosA, float sinA)
    {
        const float d = axis[0] * v[0] + axis[1] * v[1] + axis[2] * v[2];
        const float c[3] = {axis[1] * v[2] - axis[2] * v[1],
                            axis[2] * v[0] - axis[0] * v[2],
                            axis[0] * v[1] - axis[1] * v[0]};
        for (int i = 0; i < 3; ++i)
            v[i] = v[i] * cosA + c[i] * sinA + axis[i] * d * (1.0f - cosA);
    }

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

        // Head roll around the forward axis. Compare the headset's actual up
        // vector with a horizon-level up vector at the same yaw/pitch. M1 used
        // only the latter, so rolling your head made the world appear to tilt
        // with you instead of remaining fixed in space.
        const float hux = 2.0f * (x * y - w * z);
        const float huy = 1.0f - 2.0f * (x * x + z * z);
        const float huz = 2.0f * (y * z + w * x);
        float hrx = -hfz, hrz = hfx; // horizon-right = cross(head forward, room up)
        float hrLen = sqrtf(hrx * hrx + hrz * hrz);
        if (hrLen < 1e-4f) hrLen = 1e-4f;
        hrx /= hrLen; hrz /= hrLen;
        const float hnux = -hfy * hrz;
        const float hnuy = hrLen;
        const float hnuz = hfy * hrx;
        const float headRoll = atan2f(hux * hrx + huz * hrz,
                                      hux * hnux + huy * hnuy + huz * hnuz);

        float* fwd = reinterpret_cast<float*>(reinterpret_cast<char*>(src) + kSrcFwd);
        float* up = reinterpret_cast<float*>(reinterpret_cast<char*>(src) + kSrcUp);
        float* pos = reinterpret_cast<float*>(reinterpret_cast<char*>(src) + kSrcPos);

        if (g_needRecenter.exchange(false))
        {
            g_gameYawRef = atan2f(fwd[1], fwd[0]); // align current head to current heading
            g_headYawRef = hy;
            g_headPosRef[0] = hpos[0]; g_headPosRef[1] = hpos[1]; g_headPosRef[2] = hpos[2];
            g_needPosRecenter = false;
            LOG("head tracking recentered (game yaw %.1f deg)", g_gameYawRef * 57.2958f);
        }
        else if (g_needPosRecenter.exchange(false))
        {
            // Enabling leaning: capture the neutral head position only, so the
            // aim/yaw baseline is left untouched (no view snap).
            g_headPosRef[0] = hpos[0]; g_headPosRef[1] = hpos[1]; g_headPosRef[2] = hpos[2];
        }

        // Rotation: yaw relative + recenter, pitch absolute + trim.
        const float gy = g_gameYawRef + g_yawSign.load() * WrapPi(hy - g_headYawRef);
        const float gp = Clamp(g_pitchSign.load() * hp + g_pitchTrim.load(), -1.5f, 1.5f);
        const float cgp = cosf(gp), sgp = sinf(gp), cgy = cosf(gy), sgy = sinf(gy);

        fwd[0] = cgp * cgy; fwd[1] = cgp * sgy; fwd[2] = sgp;
        if (g_writeUp.load())
        {
            const float cr = cosf(headRoll), sr = sinf(headRoll);
            // Horizon-level up plus roll toward the camera's right vector.
            up[0] = (-sgp * cgy) * cr + sgy * sr;
            up[1] = (-sgp * sgy) * cr - cgy * sr;
            up[2] = cgp * cr;
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
            const float fwdComp = dx * hfhx + dz * hfhz;       // room move along look dir
            const float rightComp = dx * (-hfhz) + dz * hfhx;  // room move to the right
            const float s = g_worldScale.load();
            float ox = (cgy * fwdComp + sgy * rightComp) * s;  // game forward/right at gy
            float oy = (sgy * fwdComp - cgy * rightComp) * s;
            float oz = dy * s;
            ox = Clamp(ox, -1.5f, 1.5f); oy = Clamp(oy, -1.5f, 1.5f); oz = Clamp(oz, -1.5f, 1.5f);
            pos[0] += ox; pos[1] += oy; pos[2] += oz;
        }

        // M2 alternate-eye proof: offset only the render camera by half the
        // measured PSVR2 IPD. Halo right in the horizontal plane is
        // (sin(yaw), -cos(yaw), 0). This does not accumulate because the game
        // rewrites the source camera before every call.
        // Per-eye separation is applied later by RenderViewHook to the compact
        // render-only camera. Keeping it out of the authoritative source avoids
        // feeding stereo offsets back into simulation or temporal history.
    }

    // M3: snap/smooth turning from the right Sense stick. Rotating the yaw
    // reference turns the head-locked view instantly, and the hand-steered aim
    // follows because its target is expressed relative to the same reference.
    void ApplyVrTurn()
    {
        if (!g_vrAim.load())
            return;
        VrPadState pad;
        VR_GetPadState(pad);
        if (!pad.valid)
            return;
        static DWORD lastMs = GetTickCount();
        const DWORD now = GetTickCount();
        float dt = (now - lastMs) / 1000.0f;
        lastMs = now;
        if (dt > 0.1f) dt = 0.1f;
        const float x = pad.turnX; // stick right = turn right = yaw decreases
        if (g_config.turn_smooth)
        {
            if (fabsf(x) > 0.15f)
                g_gameYawRef = WrapPi(g_gameYawRef -
                    x * (g_config.turn_smooth_deg_s / 57.2958f) * dt);
        }
        else
        {
            static bool latched = false; // one snap per stick flick
            if (!latched && fabsf(x) > 0.6f)
            {
                g_gameYawRef = WrapPi(g_gameYawRef -
                    (x > 0 ? 1.0f : -1.0f) * g_config.turn_snap_deg / 57.2958f);
                latched = true;
            }
            else if (fabsf(x) < 0.3f)
                latched = false;
        }
    }

    void* __fastcall CamCopyHook(void* dst, void* src)
    {
        // M2 tracing: this function copies src+0x68/+0x6C into the compact
        // render camera at dst+0x28/+0x2C. Record only the first few calls so
        // we can distinguish world, weapon, and other camera passes without
        // producing a frame-sized log forever.
        static std::atomic<unsigned> traceCount{0};
        if (src)
        {
            g_projectionTanX.store(*reinterpret_cast<const float*>(
                reinterpret_cast<const char*>(src) + kSrcProjX));
            g_projectionTanY.store(*reinterpret_cast<const float*>(
                reinterpret_cast<const char*>(src) + kSrcProjY));
            // Camera buffers are heap-allocated and move on level changes;
            // log every new one so external tools (camscan aimwrite) can
            // always read the current address from the log.
            static void* seenSrc[16]{};
            static unsigned seenSrcCount = 0;
            bool newSrc = true;
            for (unsigned i = 0; i < seenSrcCount; ++i)
                if (seenSrc[i] == src) { newSrc = false; break; }
            if (newSrc && seenSrcCount < 16)
                seenSrc[seenSrcCount++] = src;
            const unsigned trace = traceCount.fetch_add(1);
            if (trace < 24 || newSrc)
            {
                const float* pos = reinterpret_cast<const float*>(
                    reinterpret_cast<const char*>(src) + kSrcPos);
                const float projX = *reinterpret_cast<const float*>(
                    reinterpret_cast<const char*>(src) + kSrcProjX);
                const float projY = *reinterpret_cast<const float*>(
                    reinterpret_cast<const char*>(src) + kSrcProjY);
                LOG("M2 camera copy %u: dst=%p src=%p pos=(%.2f,%.2f,%.2f) proj=(%.6f,%.6f)",
                    trace, dst, src, pos[0], pos[1], pos[2], projX, projY);
            }
        }
        if (src)
        {
            // The game recomputes this forward from its aim state every frame,
            // so pre-overwrite it equals the true aim direction (bullets follow
            // it even while head look repaints the view).
            const float* fwd = reinterpret_cast<const float*>(
                reinterpret_cast<const char*>(src) + kSrcFwd);
            g_aimFwdX.store(fwd[0]); g_aimFwdY.store(fwd[1]); g_aimFwdZ.store(fwd[2]);
            g_aimSeen = true;
        }
        if (g_enabled.load())
        {
            ApplyVrTurn();
            ApplyHeadLook(src); // overwrite the authoritative camera before it's copied/used
        }
        return g_origCamCopy(dst, src);
    }

    void __fastcall RenderViewHook(void* view)
    {
        if (!VR_IsStereoEnabled() || !g_enabled.load() || !view)
        {
            g_origRenderView(view);
            return;
        }

        // Compact camera produced by CamCopy: position +0x00, forward +0x0C,
        // up +0x18. It begins at view+0x08. Snapshot it so both eye calls start
        // from identical frame state and the engine sees its original afterward.
        char* camera = reinterpret_cast<char*>(view) + 8;
        alignas(16) unsigned char saved[0x90];
        alignas(16) unsigned char savedDerived[0x90];
        alignas(16) unsigned char savedCameraCopy[0x90];
        alignas(16) unsigned char savedDerivedCopy[0x90];
        memcpy(saved, camera, sizeof(saved));
        memcpy(savedDerived, reinterpret_cast<char*>(view) + 0x98, sizeof(savedDerived));
        memcpy(savedCameraCopy, reinterpret_cast<char*>(view) + 0x158, sizeof(savedCameraCopy));
        memcpy(savedDerivedCopy, reinterpret_cast<char*>(view) + 0x1E8, sizeof(savedDerivedCopy));
        const float* fwd = reinterpret_cast<const float*>(saved + 0x0C);
        const float* up = reinterpret_cast<const float*>(saved + 0x18);
        float right[3] = {
            fwd[1] * up[2] - fwd[2] * up[1],
            fwd[2] * up[0] - fwd[0] * up[2],
            fwd[0] * up[1] - fwd[1] * up[0]};
        // Stereo depth is deliberately independent from the 6DOF translation
        // scale. The physically converted 67.5 mm baseline read too flat in
        // Halo; use a fixed 2x stereo strength so the scene has clear depth.
        constexpr float kStereoWorldUnitsPerMeter = 0.33f;
        const float halfIpdWorld = 0.5f * 0.0675f * kStereoWorldUnitsPerMeter;

        // STEREO GHOSTING — settled empirically. Headset results:
        //   [A B] fixed order      -> ghost on A (the first render), steady
        //   [A B]/[B A] alternate  -> ghost FLICKERS between both eyes
        //   first-eye post anchor  -> ghost unchanged on the first render
        //   [A_discard A B] warmup -> BOTH kept renders clean (only success)
        // with `M2: view renders/sec` == `fps` proving one hook call/frame.
        //
        // Together these say: whichever render crosses the frame boundary
        // absorbs poisoned state (trails on bright pixels) regardless of eye
        // identity, and no camera anchoring or ordering trick prevents it —
        // five attempts, five failures. Every census (in-pass RTV/SRV/UAV,
        // frame-level RTV) failed to name the resource, so the shipped fix is
        // the one mechanism proven in the headset: render the first eye once
        // extra and DISCARD it, so the boundary-crossing render is never
        // shown. The frame-rate cost is paid back in the launcher, which
        // requests a game resolution sized so THREE renders fit the 120 Hz
        // budget instead of two (resolution is the intended perf lever per
        // PLAN.md). If someone later names the poisoned resource (untried:
        // CopyResource/CopySubresourceRegion hooks, draw-time
        // PSGetShaderResources snapshots), the warm-up can go and full
        // resolution comes back.
        {
            static std::atomic<unsigned> viewRenders{0};
            static std::atomic<DWORD> lastLog{GetTickCount()};
            viewRenders.fetch_add(1);
            const DWORD now = GetTickCount();
            DWORD last = lastLog.load();
            if (now - last >= 10000 && lastLog.compare_exchange_strong(last, now))
            {
                const unsigned n = viewRenders.exchange(0);
                LOG("M2: view renders %.0f/sec (equals fps => one per frame; "
                    "a multiple => extra engine views)", n * 1000.0 / (now - last));
            }
        }
        const int firstEye = g_config.right_eye_first ? 1 : 0;
        for (int pass = 0; pass < 3; ++pass)
        {
            // Pass 0 is the discarded warm-up of the first eye (see above).
            const bool keep = pass != 0;
            const int eye = pass == 2 ? 1 - firstEye : firstEye;
            g_stereoEye = eye;
            VR_BeginRasterEye(eye);
            memcpy(camera, saved, sizeof(saved));
            float* pos = reinterpret_cast<float*>(camera);
            const float sign = eye == 0 ? -1.0f : 1.0f;
            pos[0] += right[0] * halfIpdWorld * sign;
            pos[1] += right[1] * halfIpdWorld * sign;
            pos[2] += right[2] * halfIpdWorld * sign;

            // Cant: PSVR2 mounts each display angled outward a few degrees,
            // and the per-eye FOV OpenXR reports is measured around that
            // canted axis. Turn this eye's raster camera by the same relative
            // rotation (OpenXR view axes +X/+Y/+Z=right/up/-forward mapped
            // onto the camera basis) so the raster covers exactly what the
            // compositor displays; rendering both eyes straight ahead leaves
            // the outward lens edge uncovered = black border per eye. The
            // matching per-eye orientation is submitted in vr.cpp. (Assumes
            // the default yaw/pitch mapping; F4/F5 flips would mirror it.)
            float cantQuat[4];
            if (VR_GetEyeCantQuat(eye, cantQuat))
            {
                const float sinHalf = sqrtf(cantQuat[0] * cantQuat[0] +
                                            cantQuat[1] * cantQuat[1] +
                                            cantQuat[2] * cantQuat[2]);
                if (sinHalf > 1e-5f)
                {
                    float angle = 2.0f * atan2f(sinHalf, cantQuat[3]);
                    if (angle > 3.14159265f) angle -= 6.2831853f; // shortest arc
                    const float ax = cantQuat[0] / sinHalf;
                    const float ay = cantQuat[1] / sinHalf;
                    const float az = cantQuat[2] / sinHalf;
                    const float axis[3] = {
                        ax * right[0] + ay * up[0] - az * fwd[0],
                        ax * right[1] + ay * up[1] - az * fwd[1],
                        ax * right[2] + ay * up[2] - az * fwd[2]};
                    const float cosA = cosf(angle), sinA = sinf(angle);
                    RotateAboutAxis(reinterpret_cast<float*>(camera + 0x0C), axis, cosA, sinA);
                    RotateAboutAxis(reinterpret_cast<float*>(camera + 0x18), axis, cosA, sinA);
                }
            }

            // Rebuild exactly the same derived blocks as the engine's camera
            // setup function. Without this, changing the compact camera here
            // is too late and the GPU keeps using the center-eye matrices.
            alignas(16) unsigned char temporary[0x40]{};
            if (g_buildViewport && g_buildMatrices)
            {
                g_buildViewport(camera, temporary);
                g_buildMatrices(camera, temporary, reinterpret_cast<char*>(view) + 0x98, 0.0f);
                const float* finalProjection = reinterpret_cast<const float*>(
                    reinterpret_cast<const char*>(view) + 0x98 + 0x78);
                // Override Halo's capped projection with the headset's own
                // per-eye lens coverage (angles from xrLocateViews, measured
                // around the canted eye axis this raster now uses). The
                // engine field only takes a centered scale, so cover the
                // wider side of each axis symmetrically. Falls back to
                // PSVR2's measured angles until views are located. This
                // happens before PrepareView so Halo's render setup/culling
                // and OpenXR's dynamic submission see the same projection.
                float eyeFov[4];
                float halfX = 1.07338f, halfY = 0.92502f; // PSVR2: 61.5/53 deg
                if (VR_GetEyeFov(eye, eyeFov))
                {
                    halfX = fmaxf(-eyeFov[0], eyeFov[1]);
                    halfY = fmaxf(eyeFov[2], -eyeFov[3]);
                }
                float* vrProjection = reinterpret_cast<float*>(
                    reinterpret_cast<char*>(view) + 0x98 + 0x78);
                vrProjection[0] = 1.0f / tanf(halfX);
                vrProjection[5] = 1.0f / tanf(halfY);
                finalProjection = vrProjection;
                if (fabsf(finalProjection[0]) > 0.01f && fabsf(finalProjection[5]) > 0.01f)
                {
                    g_renderHalfFovX = atanf(1.0f / fabsf(finalProjection[0]));
                    g_renderHalfFovY = atanf(1.0f / fabsf(finalProjection[5]));
                }
                // Capture the engine's real matrix layout before introducing
                // off-axis center terms. This is logged once and left
                // untouched so the diagnostic build remains distortion-free.
                static std::atomic<bool> loggedProjection{false};
                if (!loggedProjection.exchange(true))
                {
                    const float* p = reinterpret_cast<const float*>(
                        reinterpret_cast<const char*>(view) + 0x98 + 0x78);
                    LOG("M2 projection rows: [%.5f %.5f %.5f %.5f] [%.5f %.5f %.5f %.5f] [%.5f %.5f %.5f %.5f] [%.5f %.5f %.5f %.5f]",
                        p[0],p[1],p[2],p[3], p[4],p[5],p[6],p[7],
                        p[8],p[9],p[10],p[11], p[12],p[13],p[14],p[15]);
                }
                memcpy(reinterpret_cast<char*>(view) + 0x158, camera, 0x90);
                memcpy(reinterpret_cast<char*>(view) + 0x1E8,
                       reinterpret_cast<char*>(view) + 0x98, 0x90);
            }
            // The draw routine consumes camera state uploaded to engine globals
            // by this per-view preparation stage, not the view structure
            // directly. Re-run it after each eye matrix rebuild.
            if (g_prepareView)
                g_prepareView(view, 0);
            // Match the gun/HUD overlay frustum to the widened world raster,
            // otherwise the ~81 deg overlay stretched across the ~123 deg
            // frame magnifies the first-person weapon and HUD ~2x. Written
            // just before the render call so it is the last writer (the game
            // recomputes these fields every frame, so no restore is needed
            // and scope zoom keeps working when stereo is off).
            if (const uintptr_t gunCam = g_gunCamera.load())
            {
                float* gunTan = reinterpret_cast<float*>(gunCam + kGunProjX);
                static std::atomic<bool> loggedGunTan{false};
                if (!loggedGunTan.exchange(true))
                    LOG("M2 gun overlay tangents: game (%.4f, %.4f) -> world match (%.4f, %.4f)",
                        gunTan[0], gunTan[1],
                        tanf(g_renderHalfFovX.load()), tanf(g_renderHalfFovY.load()));
                const float scale = g_gunFovScale.load();
                gunTan[0] = tanf(g_renderHalfFovX.load()) * scale;
                gunTan[1] = tanf(g_renderHalfFovY.load()) * scale;
            }
            g_origRenderView(view);
            if (keep)
                VR_CaptureRenderedEye(eye);
            VR_EndRasterEye();
        }
        g_stereoEye = -1;
        memcpy(camera, saved, sizeof(saved));
        memcpy(reinterpret_cast<char*>(view) + 0x98, savedDerived, sizeof(savedDerived));
        memcpy(reinterpret_cast<char*>(view) + 0x158, savedCameraCopy, sizeof(savedCameraCopy));
        memcpy(reinterpret_cast<char*>(view) + 0x1E8, savedDerivedCopy, sizeof(savedDerivedCopy));
    }

    // Byte pattern of the camera-copy function's prologue, with the RIP
    // displacement and the short-jump offset wildcarded. Found by signature so
    // the mod survives MCC updates that shift addresses (per the project rules).
    //   mov [rsp+8],rbx; push rdi; sub rsp,0x30; movaps [rsp+0x20],xmm6;
    //   mov rdi,rdx; mov rbx,rcx; test rdx,rdx; je short ??; movss xmm3,[rip+??]
    const char* kCamCopySig =
        "48 89 5C 24 08 57 48 83 EC 30 0F 29 74 24 20 48 8B FA 48 8B D9 48 85 D2 74 ?? F3 0F 10 1D ?? ?? ?? ??";

    // halo3.dll+0x286A14 in build 1.3528: inner per-view renderer, called by
    // the engine's native view loop with rcx = the prepared view structure.
    const char* kRenderViewSig =
        "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 48 83 EC 40 8B 3D ?? ?? ?? ?? 48 8B F1 85 FF 0F 84 ?? ?? ?? ??";
    const char* kPrepareViewSig =
        "48 89 5C 24 08 57 48 83 EC 20 83 3D ?? ?? ?? ?? 03 8B FA 48 8B D9 48 89 0D ?? ?? ?? ??";
    const char* kBuildViewportSig =
        "40 53 48 83 EC 30 44 0F BF 49 62 4C 8B D9 4C 8B 41 38 48 8B DA 0F BF 51 50";
    const char* kBuildMatricesSig =
        "48 8B C4 48 89 58 08 48 89 78 10 55 48 8D 68 E8 48 81 EC 10 01 00 00 80 3D ?? ?? ?? ?? 00";
    // Start of the engine function that constructs the 4-slot camera-object
    // array: mov [rsp+8],rbx; push rdi; sub rsp,0x20; lea rbx,[rip+array];
    // mov edi,4; mov rcx,rbx; call ctor; add rbx,0x2820; sub rdi,1; jnz.
    // The lea's RIP displacement is at match+13 and the instruction ends at
    // match+17, so the array (= gun/overlay camera) is match+17+disp32. The
    // 0x2820 stride distinguishes it from an identical builder of another
    // camera array.
    const char* kGunCamRefSig =
        "48 89 5C 24 08 57 48 83 EC 20 48 8D 1D ?? ?? ?? ?? BF 04 00 00 00 48 8B CB E8 ?? ?? ?? ?? 48 81 C3 20 28 00 00 48 83 EF 01 75 EB";

    void InstallHook(uintptr_t base, size_t size)
    {
        uintptr_t hit = sig::Find(base, size, kCamCopySig);
        if (!hit)
        {
            LOG("M1: camera signature NOT FOUND — MCC may have updated. Head tracking is");
            LOG("M1: disabled; the game and the VR screen still work normally.");
            return;
        }
        // Uniqueness check — if the pattern matched twice we can't trust it.
        const uintptr_t after = hit + 1;
        if (sig::Find(after, base + size - after, kCamCopySig))
            LOG("M1: WARNING camera signature is not unique; using the first match");
        LOG("M1: camera-copy found by signature at halo3.dll+0x%llX (expected 0x%llX for build 1.3528)",
            (unsigned long long)(hit - base), (unsigned long long)kCamCopyRva);

        void* target = reinterpret_cast<void*>(hit);
        if (MH_CreateHook(target, reinterpret_cast<void*>(&CamCopyHook),
                          reinterpret_cast<void**>(&g_origCamCopy)) != MH_OK ||
            MH_EnableHook(target) != MH_OK)
        {
            LOG("M1: FAILED to hook camera-copy at %p; head tracking unavailable", target);
            return;
        }
        LOG("M1: camera hooked. F2 head tracking, F3 recenter, F6 leaning, F8/F9 pitch trim, F10 screen-follow (yaw/pitch/up flips: F1 menu)");

        uintptr_t gunRef = sig::Find(base, size, kGunCamRefSig);
        if (gunRef && !sig::Find(gunRef + 1, base + size - gunRef - 1, kGunCamRefSig))
        {
            const int32_t disp = *reinterpret_cast<const int32_t*>(gunRef + 13);
            g_gunCamera = gunRef + 17 + disp;
            LOG("M2: gun/overlay camera at halo3.dll+0x%llX (expected 0x%llX for build 1.3528)",
                (unsigned long long)(g_gunCamera.load() - base),
                (unsigned long long)kGunCamRva);
        }
        else
        {
            LOG("M2: gun-camera signature missing/ambiguous; weapon/HUD will stay oversized in stereo");
        }

        uintptr_t renderHit = sig::Find(base, size, kRenderViewSig);
        uintptr_t prepareHit = sig::Find(base, size, kPrepareViewSig);
        uintptr_t viewportHit = sig::Find(base, size, kBuildViewportSig);
        uintptr_t matricesHit = sig::Find(base, size, kBuildMatricesSig);
        if (!renderHit || sig::Find(renderHit + 1, base + size - renderHit - 1, kRenderViewSig))
        {
            LOG("M2: render-frame signature missing or ambiguous; raw stereo unavailable");
            return;
        }
        if (!prepareHit || !viewportHit || !matricesHit)
        {
            LOG("M2: derived camera matrix signatures missing; raw stereo unavailable");
            return;
        }
        g_prepareView = reinterpret_cast<PrepareViewFn>(prepareHit);
        g_buildViewport = reinterpret_cast<BuildViewportFn>(viewportHit);
        g_buildMatrices = reinterpret_cast<BuildMatricesFn>(matricesHit);
        if (MH_CreateHook(reinterpret_cast<void*>(renderHit), reinterpret_cast<void*>(&RenderViewHook),
                          reinterpret_cast<void**>(&g_origRenderView)) != MH_OK ||
            MH_EnableHook(reinterpret_cast<void*>(renderHit)) != MH_OK)
        {
            LOG("M2: FAILED to hook render-frame entry");
            return;
        }
        g_renderHooked = true;
        LOG("M2: inner per-view double-render hook installed at halo3.dll+0x%llX",
            (unsigned long long)(renderHit - base));
    }

    DWORD WINAPI WaitThread(LPVOID)
    {
        // The XInput hook is wanted as soon as MCC loads an xinput DLL (so the
        // Sense controllers drive the frontend menus too), the game hooks only
        // once halo3.dll appears (entering a level). MCC loads xinput DLLs
        // lazily and can add MORE of them later (it hooked only xinput1_3 in
        // one session and read the pad through xinput1_4), so this thread
        // keeps polling forever instead of stopping at the first success.
        bool gameHooked = false;
        for (;;)
        {
            Input_InstallXInputHook();
            Input_ClaimXInputIat(); // re-assert if Steam replaces MCC's import slot
            if (!gameHooked)
            {
                uintptr_t base = 0;
                size_t size = 0;
                if (sig::ModuleRange(L"halo3.dll", base, size))
                {
                    LOG("halo3.dll loaded at %p, size 0x%zX", (void*)base, size);
                    InstallHook(base, size);
                    g_hooked = true;
                    gameHooked = true;
                }
            }
            Sleep(2000);
        }
    }
}

void Game_Init()
{
    // Claim MCC's controller path synchronously, before OpenXR startup blocks
    // on SteamVR. The worker keeps re-asserting it if Steam replaces the IAT.
    Input_InstallXInputHook();
    Input_ClaimXInputIat();
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
float Game_GetYawSign()   { return g_yawSign.load(); }
float Game_GetPitchSign() { return g_pitchSign.load(); }
bool Game_GetWriteUp()    { return g_writeUp.load(); }

void Game_TogglePositional()
{
    if (VR_IsStereoEnabled())
    {
        g_positional = true;
        LOG("positional remains ON (required for stereo VR)");
        return;
    }
    const bool on = !g_positional.load();
    g_positional = on;
    if (on)
        g_needPosRecenter = true; // capture neutral head position, no yaw snap
    LOG("positional (leaning) %s", on ? "ON" : "OFF");
}

void Game_ForcePositional()
{
    g_positional = true;
    g_needPosRecenter = true;
    LOG("positional 6DOF forced ON for stereo VR");
}

void Game_PitchTrim(int dir)
{
    const float t = Clamp(g_pitchTrim.load() + dir * 0.035f, -0.8f, 0.8f); // ~2 deg steps
    g_pitchTrim = t;
    LOG("pitch trim %.1f deg", t * 57.2958f);
}

void Game_LeanScale(int dir)
{
    const float s = Clamp(g_worldScale.load() + dir * 0.05f, 0.05f, 2.0f);
    g_worldScale = s;
    LOG("lean scale %.2f (game units per meter)", s);
}

void Game_ToggleVrAim()
{
    const bool on = !g_vrAim.load();
    g_vrAim = on;
    LOG("VR aim (right controller steers weapon) %s", on ? "ON" : "OFF");
}

bool Game_ComputeAimStick(float& outRx, float& outRy)
{
    // Closed-loop aim: emit a right-stick deflection proportional to the
    // angular error between the game's aim and the controller ray. The game
    // integrates it through its normal turn-rate path, so bullets, reticle
    // logic, vehicles and turrets all behave as if the player aimed manually.
    // Diagnostic: when aim steering is not running, log WHICH precondition
    // failed (once per distinct reason) so a dead aim is explainable from the
    // log alone.
    static std::atomic<int> lastAimBlock{-1};
    auto blocked = [](int reason, const char* what) {
        int prev = lastAimBlock.exchange(reason);
        if (prev != reason)
            LOG("M3 DIAG: aim steering blocked: %s", what);
        return false;
    };
    if (!g_vrAim.load())
        return blocked(1, "VR aim toggled OFF (press Insert)");
    if (!g_enabled.load())
        return blocked(2, "head tracking OFF (press F2)");
    if (!g_aimSeen.load())
        return blocked(3, "camera hook not running (not in a level?)");
    float q[4], p[3];
    if (!VR_GetRightControllerPose(q, p))
        return blocked(4, "right controller not tracked");
    lastAimBlock = 0;

    // Controller forward (-Z of the aim pose), same mapping as the head so
    // hand and head share the F3 recenter reference.
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float fx = -2.0f * (w * y + x * z);
    const float fy =  2.0f * (w * x - y * z);
    const float fz = -(1.0f - 2.0f * (x * x + y * y));
    const float cy = atan2f(fx, -fz);
    const float cp = asinf(Clamp(fy, -1.0f, 1.0f));
    const float desiredYaw = g_gameYawRef + g_yawSign.load() * WrapPi(cy - g_headYawRef);
    const float desiredPitch = Clamp(g_pitchSign.load() * cp, -1.45f, 1.45f);

    const float ax = g_aimFwdX.load(), ay = g_aimFwdY.load(), az = g_aimFwdZ.load();
    const float aimYaw = atan2f(ay, ax);
    const float aimPitch = asinf(Clamp(az, -1.0f, 1.0f));

    const float errYaw = WrapPi(desiredYaw - aimYaw);
    const float errPitch = desiredPitch - aimPitch;

    // Full deflection at ~10 deg of error. Game yaw grows counterclockwise
    // (left); positive stick X turns right, hence the minus.
    const float k = 5.7f;
    outRx = Clamp(-errYaw * k, -1.0f, 1.0f);
    outRy = Clamp(errPitch * k, -1.0f, 1.0f);
    return true;
}

void Game_MapMoveStick(float& mx, float& my)
{
    // The game moves relative to its aim heading, which VR aim points at the
    // hand. Rotate the move vector by (head - aim) yaw so pushing forward
    // walks where you look instead of where the gun points.
    if (!g_enabled.load() || !g_aimSeen.load() || !g_vrAim.load())
        return;
    float q[4], p[3];
    if (!VR_GetHeadPose(q, p))
        return;
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float fx = -2.0f * (w * y + x * z);
    const float fz = -(1.0f - 2.0f * (x * x + y * y));
    const float hy = atan2f(fx, -fz);
    const float headYaw = g_gameYawRef + g_yawSign.load() * WrapPi(hy - g_headYawRef);
    const float aimYaw = atan2f(g_aimFwdY.load(), g_aimFwdX.load());
    const float delta = WrapPi(headYaw - aimYaw);
    const float c = cosf(delta), s = sinf(delta);
    const float nx = mx * c - my * s;
    const float ny = mx * s + my * c;
    mx = nx;
    my = ny;
}

void Game_GunFovScale(int dir)
{
    // Multiplies the world-matched overlay tangents: >1 widens the gun/HUD
    // frustum, which draws the weapon smaller. Home = bigger, End = smaller.
    const float s = Clamp(g_gunFovScale.load() * (dir > 0 ? 1.05f : 1.0f / 1.05f),
                          0.3f, 3.0f);
    g_gunFovScale = s;
    LOG("gun overlay FOV scale %.2f (higher = smaller weapon/HUD)", s);
}

float Game_GetWorldScale() { return g_worldScale.load(); }

void Game_GetProjectionTangents(float& tanX, float& tanY)
{
    tanX = g_projectionTanX.load();
    tanY = g_projectionTanY.load();
}

void Game_GetRenderHalfFov(float& halfX, float& halfY)
{
    halfX = g_renderHalfFovX.load();
    halfY = g_renderHalfFovY.load();
}


void Game_SetStereoEye(int eye)
{
    g_stereoEye = (eye == 0 || eye == 1) ? eye : -1;
}
