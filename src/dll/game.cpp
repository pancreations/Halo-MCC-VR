#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <intrin.h>
#include <MinHook.h>
#include "game.h"
#include "sigscan.h"
#include "vr.h"
#include "ik.h"
#include "title_adapter.h"
#include "../common/log.h"
#include "../common/config.h"
#include "../common/input_logic.h"

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
    // True only when both halves of the visible-palette path are live:
    // 0x184B08 identifies the interpolated slot, and 0x2C561C consumes a
    // private reconstructed copy with the exact render root. While live, all
    // older sim-bank/root experiments are bypassed so no controller pose can
    // feed back into the gameplay camera.
    std::atomic<bool> g_fpInterpolatorHooked{false};
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
    std::atomic<float> g_zoomFactor{1.0f}; // >1 while the player is zoomed (scope)
    std::atomic<float> g_renderHalfFovX{atanf(1.091595f)};
    std::atomic<float> g_renderHalfFovY{atanf(1.114286f)};

    // The exact camera state the engine consumed this tick (position from
    // kSrcPos, fwd/up = the very floats ApplyHeadLook wrote). Captured in
    // CamCopyHook; consumed by the first-person transform (head-bake
    // cancellation MUST use these, not a re-derivation) and by FpRootShim.
    std::atomic<float> g_camX{0}, g_camY{0}, g_camZ{0};
    // Gameplay camera origin before ApplyHeadLook adds headset leaning. A
    // controller world position is based on this stable origin plus
    // controller-minus-recenter, never camera-plus-controller-minus-current-
    // head (which mixes samples and shortens apparent reach).
    std::atomic<float> g_baseCamX{0}, g_baseCamY{0}, g_baseCamZ{0};
    std::atomic<bool> g_baseCamValid{false};
    std::atomic<float> g_camFwd[3] = {{1},{0},{0}};
    std::atomic<float> g_camUp[3] = {{0},{0},{1}};
    std::atomic<bool> g_camValid{false};

    // MEASURED world-up (gravity axis) for shoulder leveling. We do NOT assume it
    // — the earlier hardcoded (0,0,1) broke the arm. Instead we EMA the engine's
    // own camera-up each tick; over normal (mostly level) play that average
    // converges to true vertical regardless of the engine's axis convention.
    std::atomic<float> g_worldUp[3] = {{0},{0},{1}};
    std::atomic<bool> g_worldUpInit{false};

    std::atomic<uintptr_t> g_gunCamera{0};   // resolved from kGunCamRefSig
    // The overlay frustum is pinned to the exact world projection: the
    // first-person bones are camera-space positions in world units, so only an
    // exact match projects the weapon at the controller's true screen position.
    // Weapon size is a MESH scale (config gun_scale, Home/End), never a
    // frustum scale — 07-15 shipped both at once (2.0 frustum x 0.33 mesh) and
    // the gun shrank to ~1/6 size ("barely visible").

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

    // Final first-person pose records proved by offline RE: four players, two
    // held-weapon slots, up to 64 composed 0x34-byte bone matrices per slot.
    struct BoneMatrix;
    using ComposeBonesFn = void(__fastcall*)(void*, int, int, BoneMatrix*, void*, void*);
    using ComposeSpecialBonesFn = void(__fastcall*)(void*, BoneMatrix*, void*, void*, int, int);
    ComposeBonesFn g_origComposeBones = nullptr;
    ComposeSpecialBonesFn g_origComposeSpecialBones = nullptr;

    uint32_t* g_engineTlsIndex = nullptr;
    unsigned char** g_animationTagData = nullptr;
    // Halo real_matrix4x3: uniform scale, then forward/left/up basis vectors,
    // then translation. The first headset build incorrectly put scale last,
    // shifting every basis read by one float and making weapon pieces diverge.
    struct BoneMatrix { float scale; float rotation[9]; float translation[3]; };
    static_assert(sizeof(BoneMatrix) == 0x34);

    using FpInterpolateFn = bool(__fastcall*)(int view, int id, int slot,
                                              BoneMatrix** outBones, int* outCount);
    FpInterpolateFn g_origFpInterpolate = nullptr;
    using FpVisiblePaletteFn = void(__fastcall*)(uint16_t tag, const BoneMatrix* root,
        BoneMatrix* destination, uintptr_t unused, const BoneMatrix* source,
        const int32_t* boneMap);
    FpVisiblePaletteFn g_origFpVisiblePalette = nullptr;
    std::atomic<int> g_fpBoneCount[2] = {{0},{0}};
    std::atomic<int> g_fpWristIndex[2] = {{-1},{-1}};
    std::atomic<int> g_fpOrientationIndex[2] = {{-1},{-1}};
    // Arm-IK chain, walked up the node parent table from the wrist:
    //   wrist (r_hand) -> parent = elbow (r_forearm) -> parent = shoulder
    //   (r_upperarm). Node names confirmed from the H3EK fp_body render_model.
    std::atomic<int> g_fpElbowIndex[2] = {{-1},{-1}};
    std::atomic<int> g_fpShoulderIndex[2] = {{-1},{-1}};
    // Bones at or below the wrist (hand, fingers, held weapon). They ride the
    // wrist rigidly so the gun stays gripped while the arm articulates.
    std::atomic<uint64_t> g_fpWristDescendants[2] = {{0},{0}};
    // LEFT arm chain (l_hand global string id 0xA1, derived offline from the
    // engine string list, anchored on r_hand=0xA6). Same parent-walk capture.
    std::atomic<int> g_fpLWristIndex[2] = {{-1},{-1}};
    std::atomic<int> g_fpLElbowIndex[2] = {{-1},{-1}};
    std::atomic<int> g_fpLShoulderIndex[2] = {{-1},{-1}};
    std::atomic<uint64_t> g_fpLWristDescendants[2] = {{0},{0}};
    // (Weapon-node anchoring for the dual seat was tried and headset-DISPROVEN
    // 2026-07-19 23:3x: the weapon node's origin/axes are arbitrary per weapon
    // — plasma rifle center, spiker backwards. The universal anchor is the
    // carrier hand bone, whose authored grip is correct for every weapon.)

    // The composer sees authored bones immediately before Halo applies its
    // camera-lag rotation to every bone except camera_control. Cache the full
    // wrist->camera_control relation with an atomic seqlock. The visible
    // palette hook uses it to synthesize a lag-consistent camera_control bone,
    // then removes the common lag as one rigid transform without changing the
    // weapon's already-correct authored barrel alignment.
    struct AtomicBoneMatrix
    {
        std::atomic<uint32_t> sequence{0};
        std::atomic<float> value[13]{};
    };
    AtomicBoneMatrix g_fpWristToCamera[2];

    struct FpInterpolationContext
    {
        const BoneMatrix* source = nullptr;
        int count = 0;
        int player = -1;
        int slot = -1;
        int wrist = -1;
        int cameraControl = -1;
        int elbow = -1;
        int shoulder = -1;
        uint64_t wristDescendants = 0;
        int lWrist = -1;
        int lElbow = -1;
        int lShoulder = -1;
        uint64_t lWristDescendants = 0;
        bool valid = false;
    };
    // One context per held-weapon slot: slot 0 is the primary (right-hand)
    // weapon, slot 1 is the dual-wield secondary (left-hand) weapon. The
    // palette hook matches its `source` pointer against both, so any
    // interpolate/palette call ordering pairs correctly.
    thread_local FpInterpolationContext g_fpInterpolationContexts[2];
    thread_local BoneMatrix g_fpUnmodifiedInterpolations[2][64];
    thread_local BoneMatrix g_fpPaletteScratch[64];
    // The render-thread IK path publishes only pointer-sized diagnostics.
    // Present consumes them and owns all logging, keeping file I/O and log
    // locks out of the palette hot path.
    thread_local const char* g_armFailWhy = nullptr;
    std::atomic<const char*> g_armFailurePublished{nullptr};
    // 0 = both arms applied, 1 = right-arm/fallback failure, 2 = left-arm failure.
    std::atomic<int> g_armFailureSide{-1};
    std::atomic<uint64_t> g_fpSkeletonKey{0};
    struct FpBoneMapSnapshot
    {
        std::atomic<uint32_t> sequence{0};
        std::atomic<uint64_t> skeletonKey{0};
        std::atomic<uint32_t> tag{0};
        std::atomic<int> count{0};
        std::atomic<int> reconstructed{0};
        std::atomic<int32_t> map[64]{};
    };
    FpBoneMapSnapshot g_fpBoneMapSnapshots[16];
    std::atomic<int> g_fpBoneMapSnapshotCount{0};

    // One visible-palette pose per stereo pair. Halo calls the interpolation
    // and palette path from each eye render, but an articulated body must be
    // posed once and merely viewed from two cameras. Re-solving independently
    // lets tiny input/timing differences put an arm in two places and wastes
    // the analytic IK work. The cache remains thread-local because the measured
    // FP prepare/palette/render sequence is synchronous on one render thread.
    struct FpStereoPaletteCache
    {
        bool valid = false;
        uint16_t tag = 0;
        int player = -1;
        int slot = -1;
        int count = 0;
        int wrist = -1;
        int elbow = -1;
        int shoulder = -1;
        uint64_t wristDescendants = 0;
        int lWrist = -1;
        int lElbow = -1;
        int lShoulder = -1;
        uint64_t lWristDescendants = 0;
        bool armIk = false;
        BoneMatrix root{};
        BoneMatrix source[64]{};
    };

    struct FpStereoSolveScope
    {
        bool armed = false;
        bool centerRootValid = false;
        BoneMatrix centerRoot{};
        FpStereoPaletteCache palettes[4]{};
    };
    thread_local FpStereoSolveScope g_fpStereoSolveScope;

    // THE FLAT-GUN FIX (2026-07-18, proven offline). The engine renders the
    // first-person layer (gun + arms + CHUD) through the view's SECOND camera
    // pair {compact @view+0x08, derived @view+0x1E8}, rebuilt by 0x279BEC
    // inside every per-view render immediately before each first-person draw
    // pass (6 call sites in the render driver 0x2837xx-0x283Bxx). Each call
    // re-copies the compact camera from the pointer at view+0x2A8 (the
    // CENTER-eye pose CamCopy left), forces the tangents to a fixed viewmodel
    // FOV (publishing render_first_person_fov_scale), derives matrices into
    // view+0x1E8 via the same buildViewport/buildMatrices helpers we already
    // use, and tail-jumps into the shader-constant uploader 0x2770F0. So the
    // gun/HUD layer was drawn IDENTICALLY in both eyes (zero stereo disparity,
    // wrong FOV) no matter what any bone hook did: a flat mono layer over a
    // stereo world. The fix: after the engine's rebuild, overwrite the pair
    // with the CURRENT EYE's world camera + derived block (snapshotted by
    // RenderViewHook) and re-run the uploader so the constants match.
    using FpCameraRebuildFn = void(__fastcall*)(void* view, unsigned char flag);
    using FpCameraUploadFn = void(__fastcall*)(void* compactCamera, void* derivedBlock);
    FpCameraRebuildFn g_origFpCameraRebuild = nullptr;
    FpCameraUploadFn g_fpCameraUpload = nullptr;

    // GAME BRIGHTNESS (2026-07-19). 0x278EE0 was MISIDENTIFIED as the HUD-scale
    // transform. In the headset, multiplying its two float args changes the GAME
    // BRIGHTNESS/gamma, not the HUD size — a0/a1 feed a screen color/gamma
    // constant (slots 0x280000/0x2D0000), NOT a HUD geometry transform. The user
    // liked the effect and asked for it as its own control, so we keep the hook
    // but drive it from `game_brightness` (default 1.0 = untouched). This does
    // NOT resize the HUD; real HUD scaling needs a different mechanism (a
    // captured VR panel — the deferred 2D HUD has no single geometry lever).
    typedef void (__fastcall *HudXformFn)(float, float, float);
    HudXformFn g_realHudXform = nullptr;
    void __fastcall HudXformHook(float x, float y, float z)
    {
        const float b = g_config.game_brightness;
        if (isfinite(b) && b > 0.05f && b != 1.0f) { x *= b; y *= b; }
        g_realHudXform(x, y, z);
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true))
            LOG("M3: brightness hook active (game_brightness %.2f)", b);
    }

    // HUD CROSSHAIR CLASS HIDER.
    // H3EK's complete ui/chud tag set marks native reticles with scripting
    // class 2. Halo already resolves that class safely inside chud_draw_widget;
    // normal gameplay merely short-circuits around the check. The install code
    // below enables the existing check and hooks its visibility predicate.
    typedef bool (__fastcall *HudCrosshairVisibleFn)(int);
    typedef bool (__fastcall *GameIsPlaybackFn)();
    HudCrosshairVisibleFn g_realHudCrosshairVisible = nullptr;
    GameIsPlaybackFn g_gameIsPlayback = nullptr;

    bool __fastcall HudCrosshairVisibleHook(int userIndex)
    {
        if (g_config.kill_reticle)
            return false;
        // Preserve the original short-circuit exactly when hiding is disabled.
        if (!g_gameIsPlayback)
            return true;
        if (!g_gameIsPlayback())
            return true;
        return g_realHudCrosshairVisible(userIndex);
    }

    // (HudPlaceHook removed 2026-07-19: the 0x2EEFC8 out-struct was MEASURED —
    // user sliders + log dump — to hold colors/alpha/animation state only, no
    // screen coordinates. Halo's HUD has no per-element position to edit; the
    // HUD panel in vr.cpp (capture diff) is the real fix.)

    // Per-eye snapshot handed from RenderViewHook to the FP hooks below. The
    // buffers are written before g_origRenderView and read during it (same
    // thread ordering via the render call; atomic pointer pairs visibility).
    std::atomic<void*> g_eyeFpView{nullptr};
    alignas(16) unsigned char g_eyeCompactCamera[0x90];
    alignas(16) unsigned char g_eyeDerivedBlock[0x90];

    // RECONSTRUCTION Phase 0 (2026-07-19): the engine's FP render driver
    // (0x2835D4; contains all six FP camera rebuild calls + the FP passes).
    // Instrumented to answer, from one desktop run: does it execute inside our
    // per-eye windows (g_stereoEye 0/1) or outside (-1), on which thread, how
    // often, with which flag. Phase A then invokes it per eye ourselves.
    using FpDriverFn = void(__fastcall*)(void* view, unsigned char flag);
    FpDriverFn g_origFpDriver = nullptr;
    int32_t* g_fpDriverGuard = nullptr; // zero-init global gating call site 1

    void __fastcall FpDriverHook(void* view, unsigned char flag)
    {
        VR_TraceEvent("fp-driver", (int)flag, g_stereoEye.load());
        // (The hud_zoom layout-factor poke that lived here is retired
        // 2026-07-19: [view+0x2B0]+0x174 never resized the visible HUD. HUD
        // sizing is now the vr.cpp HUD panel — capture, erase, re-present.)
        // RECONSTRUCTION Phase A (2026-07-19). Measured architecture: the
        // engine STAGES the FP camera once per frame outside the eye windows
        // (center pose, crushed depth) and its in-window driver runs (~2 per
        // eye, this exact call) DRAW using that staged camera — hence the flat
        // zero-disparity gun. Fix: immediately before each in-window run,
        // stamp BOTH staged FP camera pairs ({view+0x158,view+0x1E8} and the
        // sub-view {+0x6C8+0x08,+0x6C8+0x1E8}) with THIS EYE's world camera +
        // full world projection; the driver's own apply/upload then pushes OUR
        // values to the GPU through the engine's own path. Per-frame weapon
        // pose, per-eye camera — the standard VR renderer architecture. The
        // out-of-window staging runs are left untouched (they also rebuild
        // packets/palette); their center camera is re-stamped here before any
        // in-window draw, same thread, so ordering is deterministic.
        char* eyeView = static_cast<char*>(g_eyeFpView.load(std::memory_order_acquire));
        if (eyeView && view)
        {
            // Stamp the DRIVER'S OWN view object (the earlier == eyeView gate
            // silently never matched — different object; identity settled by
            // the one-shot below). Both FP pairs + the engine's own constant
            // upload, so recorded FP draws executing later in this eye window
            // bind this eye's camera.
            static std::atomic<bool> loggedIdentity{false};
            if (!loggedIdentity.exchange(true))
                LOG("P0: in-window driver view=%p vs eye window view=%p (delta=0x%llX)",
                    view, eyeView,
                    (unsigned long long)((char*)view > eyeView ? (char*)view - eyeView
                                                               : eyeView - (char*)view));
            char* base = static_cast<char*>(view);
            // The FP driver runs ~3x per eye; after the first stamp of a given
            // eye the pairs already hold this eye's camera. Skip the re-stamp +
            // (costly) constant upload when nothing changed. memcmp is cheap and
            // self-correcting: if the engine rewrote the pair between runs the
            // compare fails and we stamp + upload exactly as before. Counters
            // below prove how often we stamp vs skip.
            static std::atomic<uint32_t> stamps{0}, skips{0};
            const bool needStamp =
                memcmp(base + 0x158, g_eyeCompactCamera, sizeof(g_eyeCompactCamera)) != 0 ||
                memcmp(base + 0x1E8, g_eyeDerivedBlock, sizeof(g_eyeDerivedBlock)) != 0 ||
                memcmp(base + 0x6C8 + 0x08, g_eyeCompactCamera, sizeof(g_eyeCompactCamera)) != 0 ||
                memcmp(base + 0x6C8 + 0x1E8, g_eyeDerivedBlock, sizeof(g_eyeDerivedBlock)) != 0;
            if (needStamp)
            {
                stamps.fetch_add(1);
                memcpy(base + 0x158, g_eyeCompactCamera, sizeof(g_eyeCompactCamera));
                memcpy(base + 0x1E8, g_eyeDerivedBlock, sizeof(g_eyeDerivedBlock));
                memcpy(base + 0x6C8 + 0x08, g_eyeCompactCamera, sizeof(g_eyeCompactCamera));
                memcpy(base + 0x6C8 + 0x1E8, g_eyeDerivedBlock, sizeof(g_eyeDerivedBlock));
                if (g_fpCameraUpload)
                    g_fpCameraUpload(base + 0x6C8 + 0x08, base + 0x6C8 + 0x1E8);
            }
            else
            {
                skips.fetch_add(1);
            }
            {
                static std::atomic<DWORD> lastLog{GetTickCount()};
                const DWORD now=GetTickCount(); DWORD last=lastLog.load();
                if (now-last>=10000 && lastLog.compare_exchange_strong(last,now))
                    LOG("PERF: FP driver camera stamps=%u skips=%u per 10s "
                        "(skips = uploads avoided)",
                        stamps.exchange(0),skips.exchange(0));
            }
            static std::atomic<bool> logged{false};
            if (!logged.exchange(true))
                LOG("M3: per-eye FP render active — eye camera stamped into both FP pairs "
                    "before the in-window driver (true stereo weapon)");
        }
        g_origFpDriver(view, flag);
    }
    // Motion blur (2026-07-19): Halo 3's multi-tap camera motion blur derives
    // its blur vector from a previous-frame camera. With two eye renders per
    // frame, an eye's "previous" camera is the OTHER eye's — a constant fake
    // velocity that smears bright content into discrete repeated echoes even
    // with the head still (the long-standing "left-eye ghost", reopened by the
    // user 2026-07-18). The engine's live tuning globals are exposed through
    // its own debug-var table ({name_ptr, type, value_ptr} entries in .data);
    // we resolve them BY NAME at runtime — no hardcoded RVAs — and force the
    // blur scales/max to zero while the user has motion blur off (default:
    // off, the VR-comfort standard). The tag loader at ~0x28D3E0 rewrites
    // these globals when effect params load, so they are re-zeroed each frame.
    struct MotionBlurVar { float* slot; float original; };
    MotionBlurVar g_motionBlurVars[4]{};
    std::atomic<int> g_motionBlurVarCount{-1}; // -1 = not yet resolved
    std::atomic<bool> g_motionBlurZeroed{false};

    float* FindDebugVarFloat(uintptr_t base, size_t size, const char* name)
    {
        const size_t nameLen = strlen(name);
        const uint8_t* module = reinterpret_cast<const uint8_t*>(base);
        uintptr_t nameVa = 0;
        for (size_t i = 1; i + nameLen + 1 < size; ++i)
        {
            if (module[i] == static_cast<uint8_t>(name[0]) && module[i - 1] == 0 &&
                module[i + nameLen] == 0 && !memcmp(module + i, name, nameLen))
            {
                nameVa = base + i;
                break;
            }
        }
        if (!nameVa) return nullptr;
        for (size_t i = 0; i + 24 <= size; i += 8)
        {
            if (*reinterpret_cast<const uintptr_t*>(module + i) != nameVa) continue;
            const uintptr_t value = *reinterpret_cast<const uintptr_t*>(module + i + 16);
            // A real value slot points back into the module's data, never at
            // code below the data sections and never outside the module.
            if (value > base && value < base + size)
                return reinterpret_cast<float*>(value);
        }
        return nullptr;
    }

    void ResolveMotionBlurVars(uintptr_t base, size_t size)
    {
        static const char* kNames[4] = {
            "motion_blur_scale_x", "motion_blur_scale_y",
            "motion_blur_max_x", "motion_blur_max_y"};
        int count = 0;
        for (const char* name : kNames)
        {
            if (float* slot = FindDebugVarFloat(base, size, name))
                g_motionBlurVars[count++] = {slot, *slot};
        }
        g_motionBlurVarCount.store(count, std::memory_order_release);
        if (count == 4)
            LOG("M3: motion-blur tuning vars resolved (scale/max x+y); toggle is live");
        else
            LOG("M3: motion-blur vars: only %d of 4 resolved; toggle disabled", count);
    }

    // VRIK stage: the engine's own switches for body-in-first-person, found in
    // the same debug-var table (resolved BY NAME, no RVAs):
    //   director_disable_first_person — the camera director stops treating the
    //     view as first person, which is the engine's condition for drawing
    //     the player biped (running legs, crouch — game-animated).
    //   render_first_person — master switch for the old viewmodel layer.
    // Applied per frame while the F1 "Show body" WIP toggle is on; original
    // dwords are captured on first apply and restored when toggled off.
    // Each lever: value slot + captured original + the value to force when the
    // "Show body" toggle is ON. onValue is a best guess pending the live poke
    // session (docs/VRIK-ROADMAP.md); the toggle now works if ANY lever
    // resolves, instead of the old all-or-nothing that disabled everything
    // because director/render_first_person were null at install time.
    struct EngineVarSlot { int32_t* slot=nullptr; int32_t original=0; int32_t onValue=0; };
    EngineVarSlot g_bodyVars[3];
    std::atomic<int> g_bodyVarCount{0};
    std::atomic<bool> g_bodyApplied{false};

    // DIAGNOSTIC (hud_probe): dump engine debug-var NAMES that look HUD-related,
    // so we can find a safe-area / HUD-scale / crosshair lever for the edge-crop
    // fix (the HUD gets cut off at the headset lens edges). Read-only one-time
    // scan of the module's strings; logs any name containing a HUD-ish token
    // that also resolves to a live value slot (name -> resolvable = a real var).
    void DumpHudDebugVars(uintptr_t base, size_t size)
    {
        if (!g_config.hud_probe) return;
        static const char* kTokens[] = {
            "safe_area","safe_frame","hud_scale","hud_","chud","reticle",
            "crosshair","widescreen","aspect","overscan","fov_scale"};
        const uint8_t* m=reinterpret_cast<const uint8_t*>(base);
        int dumped=0;
        LOG("HUD-VARS: scanning module for HUD-related debug-var names...");
        for (size_t i=1; i+4<size && dumped<80; ++i)
        {
            // Start of a C string (preceded by a null, printable ASCII run).
            if (m[i-1]!=0 || m[i]<0x20 || m[i]>0x7E) continue;
            size_t len=0;
            while (i+len<size && m[i+len]>=0x20 && m[i+len]<=0x7E && len<64) ++len;
            if (len<4 || i+len>=size || m[i+len]!=0) { i+=len; continue; }
            const char* s=reinterpret_cast<const char*>(m+i);
            bool hit=false;
            for (const char* tok : kTokens) if (strstr(s,tok)) { hit=true; break; }
            if (hit)
            {
                float* slot=FindDebugVarFloat(base,size,s);
                if (slot)
                {
                    LOG("HUD-VARS: '%s' = %.4f (@%p)", s, *slot, (void*)slot);
                    ++dumped;
                }
            }
            i+=len;
        }
        LOG("HUD-VARS: %d resolvable HUD-related var(s) logged", dumped);
    }

    // HUD SAFE-FRAME LOCATOR (2026-07-19 probe). Halo 3 lays the whole HUD out
    // inside a "global safe frame" fraction stored in the chud_globals tag
    // (skins -> curvature infos): 0.87 x 0.87 of the screen. DESKTOP-PROVEN in
    // H3EK tag_test: writing 0.5 into the tag pulls every element toward the
    // screen center — exactly the VR shrink we want. At runtime those floats
    // live in loaded tag data (heap, not the module image), so we find them by
    // their immutable neighborhood, byte-verified against the shipped tag:
    //   [int32 1280][int32 720][55.f][661.f][58.f][4.f]
    //   (virtual canvas w/h, sensor origin x/y, sensor radius, blip radius)
    // with the two safe-frame floats at +24/+28. Exactly 3 such blocks exist
    // in ui\chud\globals (one per skin: default/dervish/monitor, 720p
    // fullscreen). A hit only counts if +24/+28 hold the exact shipped 0.87f
    // bit pattern (0x3F5EB852) AND the region is private read-write — pattern
    // and payload must BOTH agree before we ever write (no-guessing rule).
    // User-triggered from the F1 menu; the scan runs on its own thread, never
    // the render thread. The poke buttons are the decisive MCC experiment:
    // does the live game re-layout the HUD when these floats change?
    constexpr int kMaxSafeFrameHits = 16;
    constexpr uint32_t kSafeFrameBits = 0x3F5EB852u; // 0.87f, bit-exact as shipped
    static const unsigned char kSafeFrameAnchor[24] = {
        0x00,0x05,0x00,0x00, 0xD0,0x02,0x00,0x00,   // int32 1280, int32 720
        0x00,0x00,0x5C,0x42, 0x00,0x40,0x25,0x44,   // 55.0f, 661.0f
        0x00,0x00,0x68,0x42, 0x00,0x00,0x80,0x40 }; // 58.0f, 4.0f
    std::atomic<uintptr_t> g_safeFrameSlots[kMaxSafeFrameHits]{};
    std::atomic<int> g_safeFrameHitCount{-1}; // -1 never/none, -2 scanning, >0 found
    std::atomic<uint32_t> g_hudAppliedBits{0}; // last value confirmed in live slots
    // Freshness beacon shared with auto-VR: CamCopyHook updates it only while a
    // level camera is running. HUD discovery reads it from Present.
    std::atomic<uint64_t> g_lastCamCopyMs{0};
    std::atomic<uintptr_t> g_nativePauseFlag{0};
    std::atomic<bool> g_enginePauseValidated{false};
    // Addresses that verified in a previous locate this session. The tag
    // allocator tends to reuse the same block on map reload, so re-checking
    // these (anchor + payload, same certainty as the scan) usually re-acquires
    // instantly instead of costing a full rescan.
    std::atomic<uintptr_t> g_safeFrameLastGood[kMaxSafeFrameHits]{};
    std::atomic<int> g_safeFrameLastGoodCount{0};

    // Plain helpers: SEH frames must stay free of C++ unwinding (C2712), and a
    // region can decommit between VirtualQuery and the read, so every touch of
    // foreign memory is guarded.
    static int SafeFrameScanRegion(uintptr_t regionBase, size_t len,
                                   uintptr_t* out, int maxOut)
    {
        int found = 0;
        const unsigned char* p = reinterpret_cast<const unsigned char*>(regionBase);
        __try
        {
            const uint64_t prefix = 0x000002D000000500ULL; // [1280][720] LE
            for (size_t i = 0; i + 32 <= len && found < maxOut; ++i)
            {
                if (*reinterpret_cast<const uint64_t*>(p + i) != prefix) continue;
                if (memcmp(p + i + 8, kSafeFrameAnchor + 8, 16) != 0) continue;
                out[found++] = regionBase + i + 24;
                i += 23;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return found; }
        return found;
    }

    static int SafeFrameReadPair(uintptr_t slot, uint32_t* h, uint32_t* v)
    {
        __try
        {
            *h = *reinterpret_cast<const volatile uint32_t*>(slot);
            *v = *reinterpret_cast<const volatile uint32_t*>(slot + 4);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
        return 1;
    }

    static int SafeFrameWritePair(uintptr_t slot, float horizontal, float vertical)
    {
        __try
        {
            *reinterpret_cast<volatile float*>(slot) = horizontal;
            *reinterpret_cast<volatile float*>(slot + 4) = vertical;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
        return 1;
    }

    // Full verification of an address: the 24-byte anchor must sit at slot-24;
    // on success the payload pair is read out under the same guard.
    static int SafeFrameVerifySlot(uintptr_t slot, uint32_t* h, uint32_t* v)
    {
        __try
        {
            if (memcmp(reinterpret_cast<const void*>(slot - 24),
                       kSafeFrameAnchor, 24) != 0) return 0;
            *h = *reinterpret_cast<const volatile uint32_t*>(slot);
            *v = *reinterpret_cast<const volatile uint32_t*>(slot + 4);
            return 1;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    }

    // Payload plausibility: both safe-frame axes must remain in a sane fraction
    // range. The shipped pair is 0.87/0.87; headset aspect correction can make
    // our applied pair anisotropic. (The first
    // release accepted ONLY the shipped bits, which deadlocked: after our own
    // apply, every rescan rejected our own value — 2026-07-19 15:00 log.)
    static bool SafeFramePairPlausible(uint32_t h, uint32_t v)
    {
        float hf = 0, vf = 0;
        memcpy(&hf, &h, 4);
        memcpy(&vf, &v, 4);
        return hf >= 0.15f && hf <= 1.05f &&
               vf >= 0.15f && vf <= 1.05f;
    }

    // Halo builds its native HUD for the game render surface's pixel aspect.
    // In VR that image is submitted across the headset's tangent-space FOV.
    // Those happened to agree on the PSVR2 setup the launcher was designed
    // around. Quest 3 without OpenXR Toolkit measured 1.386 game pixels versus
    // ~0.964 tangent-space, making the HUD geometry visibly too narrow. Keep
    // hud_size as the larger safe-frame axis and reduce the other axis so the
    // HUD's perceived X:Y geometry remains square on any runtime/headset.
    static void ComputeHudSafeFramePair(float size, float& horizontal, float& vertical)
    {
        horizontal = vertical = size;
        float gameAspect = 0.0f;
        float eyeFov[4]{};
        if (!VR_GetGameRenderAspect(gameAspect) || !VR_GetEyeFov(0, eyeFov))
            return;
        const float halfX = fmaxf(-eyeFov[0], eyeFov[1]);
        const float halfY = fmaxf(eyeFov[2], -eyeFov[3]);
        const float tanX = tanf(halfX), tanY = tanf(halfY);
        if (!isfinite(tanX) || !isfinite(tanY) || tanX <= 0.01f || tanY <= 0.01f)
            return;
        const float headsetAspect = tanX / tanY;
        const float correction = gameAspect / headsetAspect;
        if (!isfinite(correction) || correction < 0.25f || correction > 4.0f)
            return;
        if (correction > 1.0f)
            vertical = size / correction;
        else
            horizontal = size * correction;
        horizontal = fmaxf(0.15f, fminf(horizontal, 1.0f));
        vertical = fmaxf(0.15f, fminf(vertical, 1.0f));
    }

    DWORD WINAPI SafeFrameScanThread(LPVOID)
    {
        const uint64_t t0 = GetTickCount64();
        // Exclude our own image: kSafeFrameAnchor itself lives in it.
        uintptr_t selfBase = 0; size_t selfSize = 0;
        sig::ModuleRange(L"halo3xr.dll", selfBase, selfSize);

        int accepted = 0, rawHits = 0;
        SYSTEM_INFO si; GetSystemInfo(&si);
        uintptr_t addr = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
        const uintptr_t addrMax = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);
        MEMORY_BASIC_INFORMATION mbi{};
        while (addr < addrMax && accepted < kMaxSafeFrameHits &&
               VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi)) == sizeof(mbi))
        {
            const uintptr_t regionBase = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
            const uintptr_t next = regionBase + mbi.RegionSize;
            // Only private read-write regions can ever be ACCEPTED (that is
            // where loaded tag data lives — probe-verified), so only those are
            // scanned at all. This skips every module image and mapped file
            // view and cuts the scan from ~20s to a few seconds.
            const bool candidate =
                mbi.State == MEM_COMMIT && !(mbi.Protect & PAGE_GUARD) &&
                mbi.Protect == PAGE_READWRITE && mbi.Type == MEM_PRIVATE;
            const bool self =
                selfBase && regionBase >= selfBase && regionBase < selfBase + selfSize;
            if (candidate && !self)
            {
                uintptr_t hits[kMaxSafeFrameHits];
                const int n = SafeFrameScanRegion(regionBase, mbi.RegionSize,
                                                  hits, kMaxSafeFrameHits);
                for (int k = 0; k < n; ++k)
                {
                    ++rawHits;
                    uint32_t h = 0, v = 0;
                    if (!SafeFrameReadPair(hits[k], &h, &v)) continue;
                    const bool payloadOk = SafeFramePairPlausible(h, v);
                    LOG("SAFEFRAME: anchor at %p (type 0x%X protect 0x%X) "
                        "payload %08X/%08X -> %s",
                        reinterpret_cast<void*>(hits[k]),
                        (unsigned)mbi.Type, (unsigned)mbi.Protect, h, v,
                        payloadOk ? "VERIFIED" : "payload implausible, REJECTED");
                    if (payloadOk && accepted < kMaxSafeFrameHits)
                        g_safeFrameSlots[accepted++].store(hits[k]);
                }
            }
            addr = next;
        }
        LOG("SAFEFRAME: scan done in %llu ms (private-RW only) — %d raw anchor "
            "hit(s), %d verified pair(s)%s",
            (unsigned long long)(GetTickCount64() - t0), rawHits, accepted,
            accepted ? "; hud_size applies from the next frame" : "");
        if (accepted > 0) // remember for instant reacquire after map changes
        {
            for (int i = 0; i < accepted; ++i)
                g_safeFrameLastGood[i].store(g_safeFrameSlots[i].load());
            g_safeFrameLastGoodCount.store(accepted, std::memory_order_release);
        }
        g_safeFrameHitCount.store(accepted, std::memory_order_release);
        return 0;
    }

    void LaunchSafeFrameScan(const char* why)
    {
        if (g_safeFrameHitCount.load(std::memory_order_acquire) == -2) return;
        for (int i = 0; i < kMaxSafeFrameHits; ++i) g_safeFrameSlots[i].store(0);
        g_hudAppliedBits.store(0, std::memory_order_relaxed);
        g_safeFrameHitCount.store(-2, std::memory_order_release);
        HANDLE th = CreateThread(nullptr, 0, SafeFrameScanThread, nullptr, 0, nullptr);
        if (th) { CloseHandle(th); LOG("SAFEFRAME: scan started (%s)", why); }
        else { g_safeFrameHitCount.store(-1); LOG("SAFEFRAME: scan thread create FAILED"); }
    }

    // Present-thread, change/validation only. This used to run from every
    // camera-copy hot-hook call; there is no need to reapply persistent tag
    // floats hundreds of times per second. HudSizeAutoTick invokes it when the
    // slider changes and once per second to catch a map reload.
    void ApplyHudSizeOnce()
    {
        const int n = g_safeFrameHitCount.load(std::memory_order_acquire);
        if (n <= 0) return;
        const float want = g_config.hud_size;
        if (!(want >= 0.30f && want <= 1.00f)) return; // bad cfg: leave stock
        float wantH = want, wantV = want;
        ComputeHudSafeFramePair(want, wantH, wantV);
        uint32_t wantBits = 0;
        uint32_t wantHBits = 0, wantVBits = 0;
        memcpy(&wantBits, &want, 4);
        memcpy(&wantHBits, &wantH, 4);
        memcpy(&wantVBits, &wantV, 4);
        int live = 0;
        for (int i = 0; i < n && i < kMaxSafeFrameHits; ++i)
        {
            const uintptr_t slot = g_safeFrameSlots[i].load(std::memory_order_relaxed);
            if (!slot) continue;
            uint32_t h = 0, v = 0;
            if (!SafeFrameVerifySlot(slot, &h, &v)) continue; // anchor gone: stale
            if (!SafeFramePairPlausible(h, v)) continue;      // junk pair: stale
            if (h == wantHBits && v == wantVBits) { ++live; continue; } // already ours
            if (SafeFrameWritePair(slot, wantH, wantV)) ++live;
        }
        if (live > 0)
        {
            g_hudAppliedBits.store(wantBits, std::memory_order_relaxed);
            // Pair is re-evaluated once per second as runtime FOV becomes valid.
        }
        else
        {
            g_hudAppliedBits.store(0, std::memory_order_relaxed);
            g_safeFrameHitCount.store(-1, std::memory_order_release); // relocate
        }
    }

    // Instant re-acquire: after a map change the tag allocator usually puts
    // chud_globals back at the same addresses. Re-verify the remembered ones
    // (anchor + plausible pair — the scan's own acceptance rule); on any
    // success, skip the full rescan entirely.
    bool TryReacquireSafeFrames()
    {
        const int m = g_safeFrameLastGoodCount.load(std::memory_order_acquire);
        if (m <= 0) return false;
        int accepted = 0;
        for (int i = 0; i < m && i < kMaxSafeFrameHits; ++i)
        {
            const uintptr_t slot = g_safeFrameLastGood[i].load(std::memory_order_relaxed);
            uint32_t h = 0, v = 0;
            if (slot && SafeFrameVerifySlot(slot, &h, &v) && SafeFramePairPlausible(h, v))
                g_safeFrameSlots[accepted++].store(slot);
        }
        if (!accepted) return false;
        for (int i = accepted; i < kMaxSafeFrameHits; ++i) g_safeFrameSlots[i].store(0);
        g_hudAppliedBits.store(0, std::memory_order_relaxed); // force one apply
        g_safeFrameHitCount.store(accepted, std::memory_order_release);
        LOG("SAFEFRAME: reacquired %d pair(s) at previous address(es); scan skipped", accepted);
        return true;
    }

    // Called every frame from Game_AutoVrTick (Present thread — logging and
    // thread creation are fine here). While hud_size is non-stock and no
    // verified slots exist: first try the instant reacquire (once a second),
    // then fall back to the full background scan (at most every 15 seconds).
    // Both only while a level is actually rendering (CamCopyHook heartbeat).
    void HudSizeAutoTick()
    {
        const float want = g_config.hud_size;
        uint32_t wantBits = 0;
        memcpy(&wantBits, &want, sizeof(wantBits));
        const int c = g_safeFrameHitCount.load(std::memory_order_acquire);
        const uint64_t now = GetTickCount64();
        if (c > 0)
        {
            static uint64_t lastVerifyMs = 0;
            if (g_hudAppliedBits.load(std::memory_order_relaxed) != wantBits ||
                now - lastVerifyMs >= 1000)
            {
                lastVerifyMs = now;
                ApplyHudSizeOnce();
            }
            return;
        }
        if (c == -2) return; // scan already running
        if (want >= 0.8695f && want <= 0.8705f) return; // stock needs no locate
        const uint64_t lastCam = g_lastCamCopyMs.load(std::memory_order_relaxed);
        if (!lastCam || now - lastCam > 1000) return; // not rendering a level
        static uint64_t lastReacquireMs = 0;
        if (now - lastReacquireMs >= 1000)
        {
            lastReacquireMs = now;
            if (TryReacquireSafeFrames()) return;
        }
        static uint64_t lastAttemptMs = 0;
        if (now - lastAttemptMs < 15000) return;
        lastAttemptMs = now;
        LaunchSafeFrameScan("auto: hud_size is set and no slots are located");
    }

    void ResolveBodyVars(uintptr_t base, size_t size)
    {
        struct { const char* name; int32_t on; } wanted[3] = {
            {"director_disable_first_person", 1}, // stop treating view as FP
            {"render_first_person", 0},           // hide the viewmodel layer
            {"debug_first_person_models", 1},     // has a live value slot on disk
        };
        int n=0;
        for (auto& w : wanted)
        {
            int32_t* slot=reinterpret_cast<int32_t*>(FindDebugVarFloat(base,size,w.name));
            if (slot) { g_bodyVars[n++]={slot,0,w.on}; }
            LOG("VRIK: body switch '%s' -> %p%s", w.name, slot,
                slot?"":" (null at install; may init at runtime)");
        }
        g_bodyVarCount.store(n,std::memory_order_release);
        LOG("VRIK: %d/3 body switches resolved; Show body toggle %s",
            n, n?"available":"disabled");
    }

    void ApplyBodySetting()
    {
        const int n=g_bodyVarCount.load(std::memory_order_acquire);
        if (!n) return;
        if (g_config.body_wip)
        {
            if (!g_bodyApplied.exchange(true))
            {
                for (int i=0;i<n;++i) g_bodyVars[i].original=*g_bodyVars[i].slot;
                LOG("VRIK: body mode ON (%d switches forced)", n);
            }
            for (int i=0;i<n;++i) *g_bodyVars[i].slot=g_bodyVars[i].onValue;
        }
        else if (g_bodyApplied.exchange(false))
        {
            for (int i=0;i<n;++i) *g_bodyVars[i].slot=g_bodyVars[i].original;
            LOG("VRIK: body mode OFF (engine values restored)");
        }
    }

    // (Removed 2026-07-19: the old ResolveChudScale/ApplyChudScale patched the
    // 1.0f immediates in 0x278EE0 — the headset proved those are the CHUD ALPHA,
    // not size. That function turned out to drive game BRIGHTNESS; it's now the
    // brightness hook, HudXformHook. HUD size has no single in-place lever; it
    // needs a captured VR panel, tracked separately.)

    // Bullet-origin measurement: on each right-trigger pull, log where Halo
    // spawns the bullet (the camera) vs the gun muzzle world position, so the
    // "bullets from thin air" gap is quantified. The true fix moves the spawn
    // to the muzzle via a fire hook (runtime hunt); this proves + measures it.
    bool DesiredWristWorld(bool left, BoneMatrix& out, float& meshScale); // defined below
    void ProbeBulletOrigin()
    {
        if (!g_config.bullet_probe) return;
        VrPadState pad; VR_GetPadState(pad);
        static bool prev=false;
        const bool trig = pad.valid && pad.trigR>0.5f;
        if (trig && !prev)
        {
            const float cam[3]={g_camX.load(),g_camY.load(),g_camZ.load()};
            BoneMatrix w{}; float ms=1.0f;
            if (DesiredWristWorld(false,w,ms))
            {
                const float dx=cam[0]-w.translation[0],dy=cam[1]-w.translation[1],
                            dz=cam[2]-w.translation[2];
                LOG("BULLET-PROBE shot: spawn(camera)=(%.2f,%.2f,%.2f) "
                    "gun=(%.2f,%.2f,%.2f) offset=%.2f wu (%.2f m)",
                    cam[0],cam[1],cam[2],
                    w.translation[0],w.translation[1],w.translation[2],
                    sqrtf(dx*dx+dy*dy+dz*dz), sqrtf(dx*dx+dy*dy+dz*dz)*3.048f);
            }
        }
        prev=trig;
    }

    // Called every frame from CamCopyHook. Zero wins over the engine's tag
    // reload while the toggle is off; originals are restored on re-enable.
    void ApplyMotionBlurSetting()
    {
        if (g_motionBlurVarCount.load(std::memory_order_acquire) != 4) return;
        if (!g_config.motion_blur)
        {
            for (auto& var : g_motionBlurVars)
            {
                if (*var.slot != 0.0f) var.original = *var.slot;
                *var.slot = 0.0f;
            }
            if (!g_motionBlurZeroed.exchange(true))
                LOG("M3: motion blur forced OFF (blur scale/max zeroed; artifact probe active)");
        }
        else if (g_motionBlurZeroed.exchange(false))
        {
            for (auto& var : g_motionBlurVars)
                *var.slot = var.original;
            LOG("M3: motion blur restored to engine values");
        }
    }

    // Locate the first-person weapon slot that owns a composed bone array.
    // Pointer compares only, so it is safe to call before composition too.
    bool FindFirstPersonWeapon(BoneMatrix* bones, int& outSlot, unsigned char*& outWeapon)
    {
        if (!bones || !g_engineTlsIndex) return false;
        auto** slots=reinterpret_cast<void**>(__readgsqword(0x58));
        if (!slots) return false;
        auto* tls=reinterpret_cast<unsigned char*>(slots[*g_engineTlsIndex]);
        if (!tls) return false;
        auto* weapons=*reinterpret_cast<unsigned char**>(tls+0x568);
        if (!weapons) return false;
        for(int candidate=0;candidate<2;++candidate)
        {
            auto* w=weapons+candidate*0x11BC;
            if (reinterpret_cast<BoneMatrix*>(w+0x4A4)==bones)
            { outSlot=candidate; outWeapon=w; return true; }
        }
        return false;
    }

    void RotateByQuat(const float q[4], const float in[3], float out[3])
    {
        const float x=q[0], y=q[1], z=q[2], w=q[3];
        const float tx=2*(y*in[2]-z*in[1]), ty=2*(z*in[0]-x*in[2]), tz=2*(x*in[1]-y*in[0]);
        out[0]=in[0]+w*tx+(y*tz-z*ty);
        out[1]=in[1]+w*ty+(z*tx-x*tz);
        out[2]=in[2]+w*tz+(x*ty-y*tx);
    }

    float Clamp(float v, float lo, float hi);
    float WrapPi(float a);
    bool ControllerWorldPose(float basis[9],float pos[3],float& scale);
    bool ControllerWorldPoseEx(bool left,float basis[9],float pos[3],float& scale);
    bool DesiredWristWorld(bool left, BoneMatrix& out, float& meshScale);

    void BuildTrackedGameBasis(const float q[4], bool head, float basis[9])
    {
        const float xrForward[3]={0,0,-1}, xrUp[3]={0,1,0};
        float f[3],u[3];
        RotateByQuat(q,xrForward,f);
        RotateByQuat(q,xrUp,u);
        const float yaw=atan2f(f[0],-f[2]);
        const float pitch=asinf(Clamp(f[1],-1.0f,1.0f));

        // Roll is measured around the tracked forward axis exactly as it is in
        // ApplyHeadLook, so head and controller pass through the same mapping.
        float rx=-f[2], rz=f[0];
        float rl=sqrtf(rx*rx+rz*rz);
        if (rl<1e-4f) rl=1e-4f;
        rx/=rl; rz/=rl;
        const float nux=-f[1]*rz, nuy=rl, nuz=f[1]*rx;
        const float roll=atan2f(u[0]*rx+u[2]*rz,u[0]*nux+u[1]*nuy+u[2]*nuz);
        const float gy=g_gameYawRef+g_yawSign.load()*WrapPi(yaw-g_headYawRef);
        const float gp=Clamp(g_pitchSign.load()*pitch+(head?g_pitchTrim.load():0.0f),-1.5f,1.5f);
        const float cp=cosf(gp),sp=sinf(gp),cy=cosf(gy),sy=sinf(gy);
        const float cr=cosf(roll),sr=sinf(roll);
        const float forward[3]={cp*cy,cp*sy,sp};
        const float up[3]={(-sp*cy)*cr+sy*sr,(-sp*sy)*cr-cy*sr,cp*cr};
        const float left[3]={up[1]*forward[2]-up[2]*forward[1],
                             up[2]*forward[0]-up[0]*forward[2],
                             up[0]*forward[1]-up[1]*forward[0]};
        memcpy(basis,forward,sizeof(forward));
        memcpy(basis+3,left,sizeof(left));
        memcpy(basis+6,up,sizeof(up));
    }

    // Basis columns (forward,left,up) from game-frame yaw/pitch/roll.
    void BasisFromAngles(float yaw, float pitch, float roll, float basis[9])
    {
        const float cp=cosf(pitch),sp=sinf(pitch),cy=cosf(yaw),sy=sinf(yaw);
        const float cr=cosf(roll),sr=sinf(roll);
        const float forward[3]={cp*cy,cp*sy,sp};
        const float up[3]={(-sp*cy)*cr+sy*sr,(-sp*sy)*cr-cy*sr,cp*cr};
        const float left[3]={up[1]*forward[2]-up[2]*forward[1],
                             up[2]*forward[0]-up[0]*forward[2],
                             up[0]*forward[1]-up[1]*forward[0]};
        memcpy(basis,forward,sizeof(forward));
        memcpy(basis+3,left,sizeof(left));
        memcpy(basis+6,up,sizeof(up));
    }

    // Column-basis product out = left * right. Element (row,column) is stored
    // at [column*3+row]. Keeping this in one helper makes the otherwise subtle
    // H^-1*C order explicit at the weapon-bank call site.
    void MultiplyBases(const float left[9], const float right[9], float out[9])
    {
        for (int c=0;c<3;++c)
            for (int r=0;r<3;++r)
            {
                float v=0.0f;
                for (int k=0;k<3;++k)
                    v+=left[k*3+r]*right[c*3+k];
                out[c*3+r]=v;
            }
    }

    // Shortest-arc rotation (column-major 3x3) taking unit vector `from` onto
    // unit vector `to`. Used to swing a bone's rest orientation onto its new
    // IK-solved child direction while carrying the authored twist.
    void ShortestArcRotation(const float from[3], const float to[3], float out[9])
    {
        const float c=from[0]*to[0]+from[1]*to[1]+from[2]*to[2];
        float v[3]={from[1]*to[2]-from[2]*to[1],
                    from[2]*to[0]-from[0]*to[2],
                    from[0]*to[1]-from[1]*to[0]};
        if (c>0.99999f)
        { out[0]=out[4]=out[8]=1; out[1]=out[2]=out[3]=out[5]=out[6]=out[7]=0; return; }
        if (c<-0.99999f)
        {
            // 180 degrees: rotate about any axis perpendicular to `from`.
            float ax[3]={1,0,0};
            if (fabsf(from[0])>0.9f) { ax[0]=0; ax[1]=1; }
            float p[3]={from[1]*ax[2]-from[2]*ax[1],
                        from[2]*ax[0]-from[0]*ax[2],
                        from[0]*ax[1]-from[1]*ax[0]};
            const float pl=sqrtf(p[0]*p[0]+p[1]*p[1]+p[2]*p[2]);
            if (pl<1e-5f){ out[0]=out[4]=out[8]=1; out[1]=out[2]=out[3]=out[5]=out[6]=out[7]=0; return; }
            p[0]/=pl; p[1]/=pl; p[2]/=pl;
            // R = 2*p*p^T - I (180 deg about p), stored column-major.
            for (int col=0;col<3;++col)
                for (int row=0;row<3;++row)
                    out[col*3+row]=2.0f*p[row]*p[col]-(row==col?1.0f:0.0f);
            return;
        }
        // Rodrigues: R = I + [v]x + [v]x^2 / (1+c). Column-major m[col*3+row].
        const float k=1.0f/(1.0f+c);
        const float vx=v[0],vy=v[1],vz=v[2];
        // rows of R:
        const float r00=1-(vy*vy+vz*vz)*k, r01=-vz+vx*vy*k, r02=vy+vx*vz*k;
        const float r10=vz+vx*vy*k,        r11=1-(vx*vx+vz*vz)*k, r12=-vx+vy*vz*k;
        const float r20=-vy+vx*vz*k,       r21=vx+vy*vz*k,    r22=1-(vx*vx+vy*vy)*k;
        out[0]=r00; out[1]=r10; out[2]=r20; // column 0 (rows 0,1,2)
        out[3]=r01; out[4]=r11; out[5]=r21; // column 1
        out[6]=r02; out[7]=r12; out[8]=r22; // column 2
    }

    // Column-basis product out = orthonormalLeft^-1 * right = left^T * right.
    void MultiplyInverseBasis(const float orthonormalLeft[9], const float right[9], float out[9])
    {
        for (int c=0;c<3;++c)
            for (int r=0;r<3;++r)
            {
                float v=0.0f;
                for (int k=0;k<3;++k)
                    v+=orthonormalLeft[r*3+k]*right[c*3+k];
                out[c*3+r]=v;
        }
    }

    bool NormalizedBasis(const BoneMatrix& matrix, float out[9])
    {
        for (int c=0;c<3;++c)
        {
            float lengthSquared=0.0f;
            for (int r=0;r<3;++r)
            {
                out[c*3+r]=matrix.rotation[c*3+r];
                lengthSquared+=out[c*3+r]*out[c*3+r];
            }
            const float length=sqrtf(lengthSquared);
            if (!isfinite(length) || length<0.001f) return false;
            for (int r=0;r<3;++r) out[c*3+r]/=length;
        }
        // A corrupt/non-orthogonal pose is much more dangerous than dropping
        // one weapon palette. Normal animation matrices are orthonormal to
        // floating-point noise.
        for (int a=0;a<3;++a)
            for (int b=a+1;b<3;++b)
            {
                float dot=0.0f;
                for (int r=0;r<3;++r) dot+=out[a*3+r]*out[b*3+r];
                if (!isfinite(dot) || fabsf(dot)>0.02f) return false;
            }
        return true;
    }

    bool ComposeBoneMatrices(const BoneMatrix& left, const BoneMatrix& right,
                             BoneMatrix& output)
    {
        if (!isfinite(left.scale) || !isfinite(right.scale) ||
            fabsf(left.scale)<0.001f || fabsf(right.scale)<0.001f) return false;
        float leftBasis[9],rightBasis[9];
        if (!NormalizedBasis(left,leftBasis) || !NormalizedBasis(right,rightBasis)) return false;
        BoneMatrix result{};
        result.scale=left.scale*right.scale;
        MultiplyBases(leftBasis,rightBasis,result.rotation);
        for (int r=0;r<3;++r)
        {
            float rotated=0.0f;
            for (int c=0;c<3;++c)
                rotated+=leftBasis[c*3+r]*right.translation[c];
            result.translation[r]=left.translation[r]+left.scale*rotated;
            if (!isfinite(result.translation[r])) return false;
        }
        output=result;
        return true;
    }

    bool InvertBoneMatrix(const BoneMatrix& input, BoneMatrix& output)
    {
        if (!isfinite(input.scale) || fabsf(input.scale)<0.001f) return false;
        float basis[9];
        if (!NormalizedBasis(input,basis)) return false;
        BoneMatrix result{};
        result.scale=1.0f/input.scale;
        for (int c=0;c<3;++c)
            for (int r=0;r<3;++r)
                result.rotation[c*3+r]=basis[r*3+c];
        for (int r=0;r<3;++r)
        {
            float v=0.0f;
            for (int c=0;c<3;++c) v+=result.rotation[c*3+r]*input.translation[c];
            result.translation[r]=-v/input.scale;
            if (!isfinite(result.translation[r])) return false;
        }
        output=result;
        return true;
    }

    void StoreAtomicBoneMatrix(AtomicBoneMatrix& destination, const BoneMatrix& value)
    {
        destination.sequence.fetch_add(1,std::memory_order_acq_rel); // writer active (odd)
        const float* source=reinterpret_cast<const float*>(&value);
        for (int i=0;i<13;++i)
            destination.value[i].store(source[i],std::memory_order_relaxed);
        destination.sequence.fetch_add(1,std::memory_order_release); // complete (even)
    }

    bool LoadAtomicBoneMatrix(const AtomicBoneMatrix& source, BoneMatrix& value)
    {
        for (int attempt=0;attempt<4;++attempt)
        {
            const uint32_t before=source.sequence.load(std::memory_order_acquire);
            if (!before || (before&1)) continue;
            float* destination=reinterpret_cast<float*>(&value);
            for (int i=0;i<13;++i)
                destination[i]=source.value[i].load(std::memory_order_relaxed);
            const uint32_t after=source.sequence.load(std::memory_order_acquire);
            if (before==after && !(after&1)) return true;
        }
        return false;
    }

    bool LoadCameraBasis(float basis[9])
    {
        if (!g_camValid.load()) return false;
        for(int j=0;j<3;++j)
        {
            basis[j]=g_camFwd[j].load();
            basis[6+j]=g_camUp[j].load();
        }
        basis[3]=basis[7]*basis[2]-basis[8]*basis[1];
        basis[4]=basis[8]*basis[0]-basis[6]*basis[2];
        basis[5]=basis[6]*basis[1]-basis[7]*basis[0];
        return true;
    }

    bool GetControllerFirstPersonTransform(int slot, float target[3], float desired[9])
    {
        // Slot 0 is Halo's right-hand first-person weapon. Do not move a
        // second/left-hand slot as part of the right-controller override.
        if (slot!=0 || !g_vrAim.load() || !g_enabled.load()) return false;
        if (!g_aimSeen.load()) return false;
        float hq[4],hp[3],cq[4],cp[3];
        if (!VR_GetHeadPose(hq,hp) || !VR_GetRightControllerPose(cq,cp)) return false;

        float mount[9];
        BasisFromAngles(g_config.gun_yaw_deg*0.0174533f,
                        g_config.gun_pitch_deg*0.0174533f,
                        g_config.gun_roll_deg*0.0174533f,mount);

        // The visible renderer supplies the head camera as the skeleton root:
        //     World = Head * record
        // Therefore the replacement record (not a delta composed onto the
        // engine's already head-baked record) must be:
        //     record = Head^-1 * Controller
        // so World = Head * Head^-1 * Controller = Controller.
        const float ih[4]={-hq[0],-hq[1],-hq[2],hq[3]};
        const float dp[3]={cp[0]-hp[0],cp[1]-hp[1],cp[2]-hp[2]};
        float rp[3]; RotateByQuat(ih,dp,rp);
        const float s=g_worldScale.load();
        // Head basis = the EXACT vectors the engine's camera consumed this
        // tick (captured in CamCopyHook), not a re-derivation. left = up x fwd.
        float headBasis[9],controllerBasis[9];
        if (!LoadCameraBasis(headBasis)) return false;
        BuildTrackedGameBasis(cq,false,controllerBasis);
        // The renderer draws World = Root * record, Root = (Head, camPos).
        // We want World = Controller, so record must be Head^-1 * Controller,
        // and the offset must be expressed IN the head frame (Head^-1 * world
        // offset == the head-frame components themselves).
        target[0]=-rp[2]*s; target[1]=-rp[0]*s; target[2]=rp[1]*s; // (fwd,left,up)
        // Head-relative with the engine's EXACT camera floats. The two
        // bracketing experiments (2026-07-15 ~05:1x): zero head terms -> the
        // gun FOLLOWS the head (proves the render root carries the camera
        // orientation); head-cancellation -> counter-wobble DURING head
        // motion only (the renderer samples the camera on its own interpolated
        // 120Hz clock; our 60Hz sim-side write is stale by up to a tick).
        // This form is exactly right whenever the head is not mid-motion; the
        // residual is a velocity-proportional wobble + a one-tick flick on
        // snap turns. Fixing THAT requires cancelling on the renderer's clock
        // = intercepting the render-side FP root build (not yet located; see
        // CONTINUATION). Do not retry frame-algebra variants: both directions
        // are already falsified.
        // rel = Head^-1 * Controller. ORDER IS THE WHOLE BUG (2026-07-15 05:2x):
        //   rel = Controller * Head^T  -> World = H*C*H^T = CONJUGATION: the
        //     head rotation is applied TWICE -> "gun tracks inverted".
        //   rel = 0 head terms         -> World = H*C -> "gun follows head".
        //   rel = Head^T * Controller  -> World = H*H^T*C = C. Correct.
        // Storage is column-major (m[c*3+j] = component j of column c), so
        // (H^T C)(r,c) = sum_k H(k,r)C(k,c) => rel[c*3+r] = sum_k H[r*3+k]*C[c*3+k].
        float rel[9];
        MultiplyInverseBasis(headBasis,controllerBasis,rel);

        // Verify the storage/order invariant in the live build. If an engine
        // update ever changes the basis convention, do not write a plausible
        // but wrong quaternion into the animation bank.
        float reconstructedController[9];
        MultiplyBases(headBasis,rel,reconstructedController);
        float maxError=0.0f;
        for(int i=0;i<9;++i)
            maxError=fmaxf(maxError,fabsf(reconstructedController[i]-controllerBasis[i]));
        if (!isfinite(maxError) || maxError>0.002f) return false;
        MultiplyBases(rel,mount,desired);

        return true;
    }

    bool GetControllerTransformForRoot(const BoneMatrix& root, float target[3],
                                       float desiredCameraControl[9], float& meshScale)
    {
        if (!isfinite(root.scale) || fabsf(root.scale)<0.001f) return false;
        float rootBasis[9];
        if (!NormalizedBasis(root,rootBasis)) return false;

        float controllerBasis[9],controllerPosition[3];
        if (!ControllerWorldPose(controllerBasis,controllerPosition,meshScale)) return false;
        float mountedController[9],mount[9];
        BasisFromAngles(g_config.gun_yaw_deg*0.0174533f,
                        g_config.gun_pitch_deg*0.0174533f,
                        g_config.gun_roll_deg*0.0174533f,mount);
        MultiplyBases(controllerBasis,mount,mountedController);
        // This is the actual root pointer passed to the visible-palette
        // consumer, not a TLS re-read or a separately sampled head matrix.
        MultiplyInverseBasis(rootBasis,mountedController,desiredCameraControl);

        const float delta[3]={controllerPosition[0]-root.translation[0],
                              controllerPosition[1]-root.translation[1],
                              controllerPosition[2]-root.translation[2]};
        for (int c=0;c<3;++c)
        {
            target[c]=0.0f;
            for (int r=0;r<3;++r) target[c]+=rootBasis[c*3+r]*delta[r];
            target[c]/=root.scale;
            if (!isfinite(target[c])) return false;
        }
        for (int i=0;i<9;++i)
            if (!isfinite(desiredCameraControl[i])) return false;
        return isfinite(meshScale) && meshScale>0.0f;
    }

    // THE LEVER, HaloCEVR's pattern (root fed INTO composition) applied to the
    // input the mesh actually reads. Proven by elimination across headset
    // tests + the composer disassembly (0x23200C):
    //   output[0] = defaultsRoot * sourceRecord[0]            (0x23203B)
    //   output[i] = output[parent[i]] * sourceRecord[i]       (0x232099)
    // - Writing `defaults` (03:27 build) moved ONLY the muzzle flash: the
    //   composed output feeds markers/effects, and nothing else.
    // - The visible MESH recomposes from the 0x20-byte orientation bank — the
    //   composers' `source` argument — with its own camera-derived root
    //   (that is how it stays head-glued in vanilla). Editing the bank root
    //   is the only lever that has ever moved the actual gun in a headset.
    // So replace the wrist ancestor's bank record (quaternion + translation)
    // with the controller pose expressed in the head-camera frame; the mesh's
    // own camera root then cancels the head and lands the gun on the hand.
    // The composed output inherits the same pose through that child, so the
    // muzzle flash stays correct WITHOUT touching `defaults` (writing both
    // would double-apply the transform).
    //
    // No new hooks: 0x20 bytes into data the engine hands us and immediately
    // consumes. Detouring additional engine functions is what crashed the game.
    // Write the controller pose into the first bank record on the wrist's
    // ancestry chain BELOW the root (found by walking the tag node table's
    // parent words, never guessed). Rationale, from tonight's falsifications:
    // the renderer rebuilds the mesh from the bank's CHILD records under its
    // own camera-derived root — record 0 is replaced by that root (why the
    // record-0 test moved only the camera feedback, never the mesh), children
    // are kept. The game-thread composition consumes the same record, so the
    // markers/flash inherit the pose with no separate camera_control edit.
    bool ApplyControllerToBankChild(void* model, BoneMatrix* output, float* bank)
    {
        if (!model || !bank || !output || !g_animationTagData || !*g_animationTagData) return false;
        int slot=-1; unsigned char* weapon=nullptr;
        if (!FindFirstPersonWeapon(output,slot,weapon)) return false;
        const int count=*reinterpret_cast<int*>(weapon+0x49C);
        if (count<=0 || count>64 ||
            *reinterpret_cast<int*>(reinterpret_cast<unsigned char*>(model)+0x14)!=count) return false;
        const int recordOffset=*reinterpret_cast<int*>(reinterpret_cast<unsigned char*>(model)+0x18);
        if (!recordOffset) return false;
        auto* records=*g_animationTagData+static_cast<ptrdiff_t>(recordOffset)*4;
        constexpr uint32_t kRightHandStringIndex=0xA6;
        int wristIndex=-1;
        for(int i=0;i<count;++i)
            if (*reinterpret_cast<const uint32_t*>(records+i*0x20)==kRightHandStringIndex)
            { wristIndex=i; break; }
        if (wristIndex<0) return false;
        // Walk wrist -> root; stop at the child whose parent IS the root (0).
        int child=wristIndex;
        for(int guard=0; guard<16; ++guard)
        {
            const int parent=*reinterpret_cast<const int16_t*>(records+child*0x20+8);
            if (parent<0 || parent>=count) return false;
            if (parent==0) break;
            child=parent;
        }
        // Cache the authored alignment nodes for the render-thread
        // interpolator. Publish the count last so an acquiring reader never
        // observes indices from a half-updated skeleton.
        const int selectedIndex=*reinterpret_cast<int*>(weapon+0x11A4);
        const int orientationIndex=
            selectedIndex>=0&&selectedIndex<count?selectedIndex:wristIndex;
        // Arm-IK chain: elbow = parent(wrist), shoulder = parent(elbow).
        auto parentOf=[&](int node)->int{
            if (node<0||node>=count) return -1;
            const int p=*reinterpret_cast<const int16_t*>(records+node*0x20+8);
            return (p>=0&&p<count)?p:-1;
        };
        const int elbowIndex=parentOf(wristIndex);
        const int shoulderIndex=parentOf(elbowIndex);
        // Descendant mask: every bone whose parent chain passes through the
        // given wrist (plus the wrist itself). Capped at 64 bones (mask width).
        auto subtreeOf=[&](int wrist)->uint64_t{
            if (wrist<0) return 0;
            uint64_t mask=(wrist<64)?(1ull<<wrist):0;
            for(int i=0;i<count && i<64;++i)
            {
                int n=i;
                for(int g=0;g<16;++g)
                {
                    if (n==wrist) { mask|=(1ull<<i); break; }
                    n=parentOf(n);
                    if (n<0) break;
                }
            }
            return mask;
        };
        const uint64_t descendants=subtreeOf(wristIndex);
        // Left arm: l_hand global string id 0xA2 — LIVE-PROVEN (2026-07-19
        // skeleton dump: index 5 = id 0xA2, parent chain 5<-3<-1, subtree 16;
        // right mirror 6=0xA6<-4<-2, subtree 21 incl. the 5 gun bones 37-41).
        // Both offline derivations (0xA1, then 0x9E from the disk pointer
        // table) were falsified live — trust only the runtime skeleton dump.
        constexpr uint32_t kLeftHandStringIndex=0xA2;
        int lWristIndex=-1;
        for(int i=0;i<count;++i)
            if (*reinterpret_cast<const uint32_t*>(records+i*0x20)==kLeftHandStringIndex)
            { lWristIndex=i; break; }
        // TOPOLOGICAL FALLBACK (2026-07-19): every offline id derivation for
        // the left hand has been falsified live (0xA1, then 0x9E: "L wrist
        // -1"), so stop depending on the id at all. The left wrist is
        // structurally unmistakable: the DEEPEST node with a hand-sized
        // subtree (>=10 bones) that neither contains the right wrist nor
        // lies inside its subtree. Deepest == smallest qualifying subtree
        // (l_hand < l_radius < l_humerus). Log the id found there so the
        // true value becomes a recorded fact.
        if (lWristIndex<0)
        {
            int bestPop=count;
            for(int i=0;i<count && i<64;++i)
            {
                if ((descendants>>i)&1) continue;          // inside right subtree
                const uint64_t sub=subtreeOf(i);
                if ((sub>>wristIndex)&1) continue;         // ancestor of right wrist
                const int pop=(int)__popcnt64(sub);
                if (pop>=10 && pop<=count/2 && pop<bestPop)
                { bestPop=pop; lWristIndex=i; }
            }
            static std::atomic<bool> loggedTopo{false};
            if (lWristIndex>=0 && !loggedTopo.exchange(true))
                LOG("M3 VRIK: left wrist found TOPOLOGICALLY: index %d, subtree %d, "
                    "record string id 0x%08X (record this id!)",
                    lWristIndex,bestPop,
                    *reinterpret_cast<const uint32_t*>(records+lWristIndex*0x20));
        }
        const int lElbowIndex=parentOf(lWristIndex);
        const int lShoulderIndex=parentOf(lElbowIndex);
        const uint64_t lDescendants=subtreeOf(lWristIndex);
        g_fpWristIndex[slot].store(wristIndex,std::memory_order_relaxed);
        g_fpOrientationIndex[slot].store(orientationIndex,std::memory_order_relaxed);
        g_fpElbowIndex[slot].store(elbowIndex,std::memory_order_relaxed);
        g_fpShoulderIndex[slot].store(shoulderIndex,std::memory_order_relaxed);
        g_fpWristDescendants[slot].store(descendants,std::memory_order_relaxed);
        g_fpLWristIndex[slot].store(lWristIndex,std::memory_order_relaxed);
        g_fpLElbowIndex[slot].store(lElbowIndex,std::memory_order_relaxed);
        g_fpLShoulderIndex[slot].store(lShoulderIndex,std::memory_order_relaxed);
        g_fpLWristDescendants[slot].store(lDescendants,std::memory_order_relaxed);
        g_fpBoneCount[slot].store(count,std::memory_order_release);
        // PROBE (2026-07-19, shotgun left-hand-stuck): the analysis above
        // re-runs every frame for whatever weapon is up, but this log was
        // once-per-session, so only the FIRST weapon's skeleton was ever
        // recorded. Key it on the skeleton's identity instead: every weapon
        // SWITCH dumps its chain + skeleton, and a missing left wrist is
        // called out loudly. Rate-limited (dual-wield could alternate
        // skeletons per frame). Log-only — behavior unchanged.
        uint64_t skelKey=(uint64_t)count;
        for(int i=0;i<count && i<64;++i)
            skelKey=skelKey*31+*reinterpret_cast<const uint32_t*>(records+i*0x20);
        g_fpSkeletonKey.store(skelKey,std::memory_order_release);
        static std::atomic<uint64_t> loggedSkelKey{0};
        static std::atomic<DWORD> lastSkelLogMs{0};
        const DWORD skelNowMs=GetTickCount();
        if (loggedSkelKey.load(std::memory_order_relaxed)!=skelKey &&
            skelNowMs-lastSkelLogMs.load(std::memory_order_relaxed)>=2000)
        {
            loggedSkelKey.store(skelKey,std::memory_order_relaxed);
            lastSkelLogMs.store(skelNowMs,std::memory_order_relaxed);
            if (lWristIndex<0)
                LOG("M3 VRIK PROBE: LEFT WRIST NOT FOUND on this skeleton — left "
                    "arm stays game-animated (the 'hand stuck on gun' symptom)");
            LOG("M3 VRIK: arm chains — R wrist %d/elbow %d/shoulder %d (subtree %d) | "
                "L wrist %d/elbow %d/shoulder %d (subtree %d)",
                wristIndex,elbowIndex,shoulderIndex,(int)__popcnt64(descendants),
                lWristIndex,lElbowIndex,lShoulderIndex,(int)__popcnt64(lDescendants));
            // Full skeleton dump, per weapon: index=id/parent for every record.
            // This is the ground truth every offline id derivation failed to
            // reproduce — keep it in every log.
            char line[512]; int pos=0; int from=0;
            for(int i=0;i<count;++i)
            {
                const uint32_t id=*reinterpret_cast<const uint32_t*>(records+i*0x20);
                const int par=*reinterpret_cast<const int16_t*>(records+i*0x20+8);
                const int n=snprintf(line+pos,sizeof(line)-pos,"%d=%X/%d ",i,id,par);
                if (n<0 || pos+n>=(int)sizeof(line)-1)
                {
                    line[pos]=0;
                    LOG("M3 VRIK: skeleton[%d..%d]: %s",from,i-1,line);
                    pos=0; from=i; --i; continue;
                }
                pos+=n;
            }
            line[pos]=0;
            LOG("M3 VRIK: skeleton[%d..%d]: %s",from,count-1,line);
        }

        // The CE probe proved the interpolated render packet is the first safe
        // gun/arms-only boundary. Once that hook is live, this sim-bank path
        // becomes topology discovery only: writing here would apply the hand
        // twice and can leak through camera_control into the gameplay camera.
        if (g_fpInterpolatorHooked.load(std::memory_order_acquire)) return true;

        float target[3],desired[9];
        if (!GetControllerFirstPersonTransform(slot,target,desired)) return false;
        // Never hand the engine a non-finite value: that is how a bad frame
        // becomes a crash deep inside the renderer instead of a visible glitch.
        for(float v : target) if (!isfinite(v)) return false;
        for(float v : desired) if (!isfinite(v)) return false;
        const float meshScale=Clamp(g_config.gun_scale,0.3f,3.0f);
        if (!isfinite(meshScale) || meshScale<=0.0f) return false;

        // Column-basis matrix (desired[c*3+r], columns = forward/left/up) ->
        // quaternion, robust in all four trace branches.
        const float m00=desired[0],m10=desired[1],m20=desired[2];
        const float m01=desired[3],m11=desired[4],m21=desired[5];
        const float m02=desired[6],m12=desired[7],m22=desired[8];
        float qx,qy,qz,qw;
        const float tr=m00+m11+m22;
        if (tr>0.0f)
        { const float s=sqrtf(tr+1.0f)*2.0f; qw=0.25f*s; qx=(m21-m12)/s; qy=(m02-m20)/s; qz=(m10-m01)/s; }
        else if (m00>m11 && m00>m22)
        { const float s=sqrtf(1.0f+m00-m11-m22)*2.0f; qw=(m21-m12)/s; qx=0.25f*s; qy=(m01+m10)/s; qz=(m02+m20)/s; }
        else if (m11>m22)
        { const float s=sqrtf(1.0f+m11-m00-m22)*2.0f; qw=(m02-m20)/s; qx=(m01+m10)/s; qy=0.25f*s; qz=(m12+m21)/s; }
        else
        { const float s=sqrtf(1.0f+m22-m00-m11)*2.0f; qw=(m10-m01)/s; qx=(m02+m20)/s; qy=(m12+m21)/s; qz=0.25f*s; }
        if (!isfinite(qx)||!isfinite(qy)||!isfinite(qz)||!isfinite(qw)) return false;
        const float qLength=sqrtf(qx*qx+qy*qy+qz*qz+qw*qw);
        if (!isfinite(qLength) || qLength<0.001f) return false;
        qx/=qLength; qy/=qLength; qz/=qLength; qw/=qLength;

        // The engine record is not composed with anymore: its content is the
        // per-tick camera bake (rest pose identity), and composing with it
        // re-imports the head at whatever phase the animator sampled — the
        // dual-tracking / snap-turn fling. The record is replaced outright.

        // This target record's scale is deliberately NOT written: camera_control
        // descends from this node, and changing it zooms the game camera —
        // the user's "scale control scales the entire world" report. The
        // engine's animated value is preserved; a mesh-only size lever is an
        // open follow-up (gun_scale currently has no effect on the mesh).
        // HaloCEVR's actual anti-contortion mechanism (WeaponHandler.cpp):
        //     if (bone.Parent == 0 || boneArray[bone.Parent].Parent == 0)
        //         outBoneTransforms[i].scale = 0.0f;   // hide the arms
        // The span from the camera-anchored body to the hand-anchored wrist
        // CANNOT be posed away — the shipped Halo VR mod hides the geometry
        // that spans it. Ours: apply the same parent-or-grandparent criterion
        // only outside the controller and camera root branches, i.e. to the
        // body/other-arm geometry stretching between the two
        // anchors. Our own branch (child) keeps its scale, and the branch
        // camera_control descends from is never touched — a scale there zooms
        // the game camera (the "scale moves the whole world" report).
        const int cc=selectedIndex;
        int camBranch=-1;
        if (cc>=0 && cc<count)
        {
            int n=cc;
            for(int g=0; g<16; ++g)
            {
                const int p=*reinterpret_cast<const int16_t*>(records+n*0x20+8);
                if (p<0||p>=count) break;
                if (p==0) { camBranch=n; break; }
                n=p;
            }
        }
        auto rootBranchOf=[&](int node)
        {
            for(int g=0; g<16 && node>0 && node<count; ++g)
            {
                const int p=*reinterpret_cast<const int16_t*>(records+node*0x20+8);
                if (p==0) return node;
                if (p<0 || p>=count) break;
                node=p;
            }
            return -1;
        };
        auto writeRecord=[&](float* base)
        {
            float* r=base+static_cast<ptrdiff_t>(child)*8;
            r[0]=qx; r[1]=qy; r[2]=qz; r[3]=qw; // i,j,k,w
            r[4]=target[0]; r[5]=target[1]; r[6]=target[2];
            for(int i=1;i<count;++i)
            {
                const int branch=rootBranchOf(i);
                if (branch==child || branch==camBranch) continue;
                const int parent=*reinterpret_cast<const int16_t*>(records+i*0x20+8);
                const int grandparent=(parent>0 && parent<count)
                    ? *reinterpret_cast<const int16_t*>(records+parent*0x20+8) : -1;
                if (parent==0 || grandparent==0)
                    base[static_cast<ptrdiff_t>(i)*8+7]=0.0f;
            }
        };
        writeRecord(bank);
        // The renderer interpolates TWO sim snapshots of these records (the
        // engine's 60Hz-sim -> 120Hz-render path). Writing only the half being
        // composed leaves the other half head-glued and the visible gun lands
        // midway between head and hand — the reported "weird in-between
        // state". Write the sibling half too (banks at TLS+0x560, one 0x1000
        // bank per slot, two 0x800 halves of 64 records each).
        auto** tlsSlots=reinterpret_cast<void**>(__readgsqword(0x58));
        auto* tls2=tlsSlots?reinterpret_cast<unsigned char*>(tlsSlots[*g_engineTlsIndex]):nullptr;
        auto* banks=tls2?*reinterpret_cast<unsigned char**>(tls2+0x560):nullptr;
        if (banks)
        {
            auto* slotBank=banks+static_cast<size_t>(slot)*0x1000;
            auto* half=reinterpret_cast<unsigned char*>(bank);
            const bool match=(half==slotBank || half==slotBank+0x800);
            static std::atomic<bool> loggedHalf{false};
            if (!loggedHalf.exchange(true))
                LOG("M3 DIAG: bank source=%p slotBank=%p sibling-write=%s",
                    half,slotBank,match?"ACTIVE":"MISSED (blend may persist)");
            if (match)
                writeRecord(reinterpret_cast<float*>(slotBank+((half-slotBank)^0x800)));
        }
        static std::atomic<unsigned> logged{0};
        const unsigned bit=1u<<slot;
        if (!(logged.fetch_or(bit)&bit))
            LOG("M3: slot %d bank CHILD node %d (wrist ancestor under root) bound to the %s controller (scale %.2f)",
                slot,child,slot==0?"right":"left",meshScale);
        return true;
    }

    void CacheAuthoredFirstPersonAlignment(BoneMatrix* bones, int first, int composedCount)
    {
        if (!bones || first!=0) return;
        int slot=-1;
        unsigned char* weapon=nullptr;
        if (!FindFirstPersonWeapon(bones,slot,weapon) || slot<0 || slot>1) return;
        const int count=*reinterpret_cast<int*>(weapon+0x49C);
        const int wrist=g_fpWristIndex[slot].load(std::memory_order_acquire);
        const int cameraControl=g_fpOrientationIndex[slot].load(std::memory_order_acquire);
        if (count<=0 || count>64 || composedCount<count ||
            wrist<0 || wrist>=count || cameraControl<0 || cameraControl>=count) return;

        BoneMatrix inverseWrist{},wristToCamera{};
        if (!InvertBoneMatrix(bones[wrist],inverseWrist) ||
            !ComposeBoneMatrices(inverseWrist,bones[cameraControl],wristToCamera)) return;
        StoreAtomicBoneMatrix(g_fpWristToCamera[slot],wristToCamera);
        static std::atomic<unsigned> loggedSlots{0};
        const unsigned bit=1u<<slot;
        if (!(loggedSlots.fetch_or(bit)&bit))
            LOG("M3: cached authored wrist->camera_control relation for FP slot %d "
                "(wrist %d, camera_control %d)",slot,wrist,cameraControl);
    }

    bool ApplyControllerToComposedBones(void* model, BoneMatrix* bones)
    {
        if (g_fpInterpolatorHooked.load(std::memory_order_acquire)) return false;
        if (!model || !bones || !g_engineTlsIndex || !g_animationTagData || !*g_animationTagData)
            return false;
        auto** slots=reinterpret_cast<void**>(__readgsqword(0x58));
        if (!slots) return false;
        auto* tls=reinterpret_cast<unsigned char*>(slots[*g_engineTlsIndex]);
        if (!tls) return false;
        auto* weapons=*reinterpret_cast<unsigned char**>(tls+0x568);
        if (!weapons) return false;
        int slot=-1;
        unsigned char* weapon=nullptr;
        for(int candidate=0;candidate<2;++candidate)
        {
            auto* w=weapons+candidate*0x11BC;
            if (reinterpret_cast<BoneMatrix*>(w+0x4A4)==bones) { slot=candidate; weapon=w; break; }
        }
        if (!weapon) return false;
        const int count=*reinterpret_cast<int*>(weapon+0x49C);
        if (count<=0 || count>64 ||
            *reinterpret_cast<int*>(reinterpret_cast<unsigned char*>(model)+0x14)!=count) return false;
        const int recordOffset=*reinterpret_cast<int*>(reinterpret_cast<unsigned char*>(model)+0x18);
        if (!recordOffset) return false;
        auto* records=*g_animationTagData+static_cast<ptrdiff_t>(recordOffset)*4;
        // Runtime animation node records use the raw global string index, not
        // the packed map StringId.  This is also how the engine calls its own
        // node finder at 0x2323AC (for example, edx=0xD9/0x1D9).  r_hand is
        // global string index 0xA6; using 0x0C0000A6 made every pose silently
        // miss the wrist and left the view model attached to the camera.
        constexpr uint32_t kRightHandStringIndex=0xA6;
        int wristIndex=-1;
        for(int i=0;i<count;++i)
            if (*reinterpret_cast<const uint32_t*>(records+i*0x20)==kRightHandStringIndex)
            {
                wristIndex=i;
                break;
            }
        if (wristIndex<0)
        {
            static std::atomic<bool> loggedMissing{false};
            if (!loggedMissing.exchange(true))
            {
                LOG("M3: r_hand node 0x%X absent from first-person skeleton (%d bones; first nodes %X,%X,%X,%X)",
                    kRightHandStringIndex,count,
                    count>0?*reinterpret_cast<const uint32_t*>(records):0,
                    count>1?*reinterpret_cast<const uint32_t*>(records+0x20):0,
                    count>2?*reinterpret_cast<const uint32_t*>(records+0x40):0,
                    count>3?*reinterpret_cast<const uint32_t*>(records+0x60):0);
            }
            return false;
        }
        // Halo resolves the per-weapon camera_control node (string index 0xD9)
        // itself and stores its bone index here. Unlike a guessed `gun` name,
        // this exists in the live 43-node skeleton and follows each weapon's
        // authored barrel orientation.
        const int selectedIndex=*reinterpret_cast<int*>(weapon+0x11A4);
        const int orientationIndex=selectedIndex>=0&&selectedIndex<count?selectedIndex:wristIndex;

        // Proven offline (2026-07-15): the root transform handed to the composer
        // at halo3+0x2C4626 is scale 1, rotation IDENTITY, translation ~0, so
        // these composed bones are CAMERA-space. Log the real values once to
        // confirm the magnitudes match that reading in the live game.
        static std::atomic<bool> loggedBones{false};
        if (!loggedBones.exchange(true))
            LOG("M3 PROBE: composed bones camera-space check: wrist[%d] t=(%.4f,%.4f,%.4f) "
                "scale=%.3f | camera_control[%d] t=(%.4f,%.4f,%.4f) fwd=(%.3f,%.3f,%.3f)",
                wristIndex,bones[wristIndex].translation[0],bones[wristIndex].translation[1],
                bones[wristIndex].translation[2],bones[wristIndex].scale,
                selectedIndex,bones[orientationIndex].translation[0],
                bones[orientationIndex].translation[1],bones[orientationIndex].translation[2],
                bones[orientationIndex].rotation[0],bones[orientationIndex].rotation[1],
                bones[orientationIndex].rotation[2]);

        if (g_config.weapon_probe)
        {
            // DECISIVE PROBE. No controller, no head, no rotation: shove the
            // whole composed assembly a fixed 0.3 world units (~1 m) to the
            // LEFT (camera space: +x forward, +y left, +z up). The visible gun
            // either moves or it does not, and that single bit tells us whether
            // the mesh reads these matrices at all — which the disassembly
            // cannot, because our edits provably reach both the effects anchor
            // and the mesh's own render packet.
            for(int i=0;i<count;++i) bones[i].translation[1]+=0.3f;
            static std::atomic<bool> loggedProbe{false};
            if (!loggedProbe.exchange(true))
                LOG("M3 PROBE ACTIVE: all %d bones of slot %d pushed +0.3 left; "
                    "if the GUN MESH does not move, it does not read weapon+0x4A4",
                    count,slot);
            return true;
        }

        float target[3],desired[9];
        if (!GetControllerFirstPersonTransform(slot,target,desired)) return false;
        const float anchor[3]={bones[wristIndex].translation[0],bones[wristIndex].translation[1],
                               bones[wristIndex].translation[2]};
        float current[9];
        for(int column=0;column<3;++column)
        {
            float len=0;
            for(int j=0;j<3;++j)
            {
                current[column*3+j]=bones[orientationIndex].rotation[column*3+j];
                len+=current[column*3+j]*current[column*3+j];
            }
            len=sqrtf(len);
            if (len<0.001f) return false;
            for(int j=0;j<3;++j) current[column*3+j]/=len;
        }
        auto rotateDelta=[&](const float in[3],float out[3])
        {
            float component[3]{};
            for(int column=0;column<3;++column)
                for(int j=0;j<3;++j)
                    component[column]+=in[j]*current[column*3+j];
            for(int j=0;j<3;++j)
                out[j]=desired[j]*component[0]+desired[3+j]*component[1]+desired[6+j]*component[2];
        };

        // Halo 3's weapon vertices are weighted across r_hand, camera_control,
        // root and weapon-specific nodes. Moving only the wrist descendants
        // leaves some weights head-driven, producing the reported dual
        // head+hand motion. Apply one rigid delta and one uniform mesh scale
        // to the complete composed assembly so every influence agrees. The
        // bones are camera-space world units and the overlay frustum matches
        // the world projection, so 1.0 draws the weapon at authored size.
        const float meshScale=Clamp(g_config.gun_scale,0.3f,3.0f);
        for(int i=0;i<count;++i)
        {
            const float d[3]={bones[i].translation[0]-anchor[0],bones[i].translation[1]-anchor[1],
                              bones[i].translation[2]-anchor[2]};
            float rt[3]; rotateDelta(d,rt);
            for(int j=0;j<3;++j) bones[i].translation[j]=target[j]+rt[j]*meshScale;
            bones[i].scale*=meshScale;
            for(int column=0;column<3;++column)
            {
                float rotated[3]; rotateDelta(&bones[i].rotation[column*3],rotated);
                for(int j=0;j<3;++j) bones[i].rotation[column*3+j]=rotated[j];
            }
        }
        static std::atomic<unsigned> logged{0};
        const unsigned bit=1u<<slot;
        if (!(logged.fetch_or(bit)&bit))
            LOG("M3: complete first-person slot %d bound to %s controller (%d bones, wrist %d, camera_control %d, scale %.2f)",
                slot,slot==0?"right":"left",count,wristIndex,selectedIndex,meshScale);
        return true;
    }

    bool LoadCachedRenderRoot(int player, BoneMatrix& root)
    {
        if (player<0 || player>=4 || !g_engineTlsIndex) return false;
        auto** tlsSlots=reinterpret_cast<void**>(__readgsqword(0x58));
        if (!tlsSlots) return false;
        auto* tls=reinterpret_cast<unsigned char*>(tlsSlots[*g_engineTlsIndex]);
        if (!tls) return false;
        auto* renderPlayers=*reinterpret_cast<unsigned char**>(tls+0x568);
        if (!renderPlayers) return false;
        root=*reinterpret_cast<const BoneMatrix*>(
            renderPlayers+static_cast<uintptr_t>(player)*0x2430+0x23F0);
        return isfinite(root.scale) && fabsf(root.scale)>0.001f;
    }

    // AUTO BARREL ALIGNMENT source data. In the authored first-person pose the
    // barrel lies on the camera-forward axis (Halo aims the viewmodel at the
    // center reticle), and the render root IS the camera, so in record space
    // the authored barrel direction expressed in the wrist frame is
    // invRot(wrist) * (1,0,0). That is a rig constant per weapon (the gun is
    // glued to the hand); a slow EMA rides out idle sway and reload swings.
    // Written by the game thread (FpInterpolateHook), read by DesiredWristWorld.
    // Authored barrel-in-wrist direction per held-weapon slot: the primary
    // (slot 0) aligns to the right controller ray, the dual-wield secondary
    // (slot 1) to the left. Each skeleton's own animated wrist is measured —
    // the secondary's authored pose differs from the primary's.
    std::atomic<float> g_barrelInWrist[2][3];
    std::atomic<bool> g_barrelInWristValid[2]={{false},{false}};

    // Marker/effect packet builders also consume 0x184B08 directly. Preserve
    // their already headset-verified controller registration on the live
    // interpolation buffer. The visible palette never consumes this mutation:
    // it receives the private unmodified copy reconstructed below.
    // Same single same-frame rigid transform as the visible palette (wrist ->
    // controller), applied to the live interpolation buffer that feeds
    // markers/muzzle effects, so the flash and the gun cannot diverge.
    bool ApplyControllerToMarkerBones(int player, BoneMatrix* bones, int count,
                                      int wrist, int cameraControl, bool dual)
    {
        (void)cameraControl;
        if (!bones || count<=0 || count>64 || wrist<0 || wrist>=count) return false;
        BoneMatrix root{};
        if (!LoadCachedRenderRoot(player,root)) return false;
        // Root POSE from the LIVE center camera, not the TLS cache: the cache
        // can hold a stale or per-eye root, and the transform baked against it
        // put an IPD-sized static offset on the flash and made it follow head
        // motion (2026-07-19 report: "flash follows my head and hand, offset
        // from the gun; bullets spawn further out"). The old headset-proven
        // flash lever used these same camera atomics. TLS root keeps only its
        // scale.
        {
            float camBasis[9];
            if (g_camValid.load() && LoadCameraBasis(camBasis))
            {
                memcpy(root.rotation,camBasis,sizeof(camBasis));
                root.translation[0]=g_camX.load();
                root.translation[1]=g_camY.load();
                root.translation[2]=g_camZ.load();
            }
        }
        // IDENTICAL hand target as the visible gun (one shared definition) so
        // muzzle flash and weapon can never diverge. A dual-wield secondary is
        // carried by the slot's LEFT hand; the hand follows the controller and
        // the weapon/marker assembly inherits that same rigid hand delta.
        const int transformAnchor=dual
            ? g_fpLWristIndex[1].load(std::memory_order_acquire)
            : wrist;
        if (transformAnchor<0 || transformAnchor>=count) return false;
        float meshScale=1.0f;
        BoneMatrix desiredWristWorld{};
        if (!DesiredWristWorld(dual,desiredWristWorld,meshScale)) return false;
        BoneMatrix wristWorld{},inverseWristWorld{},t{},inverseRoot{},tRoot{},m{};
        if (!ComposeBoneMatrices(root,bones[transformAnchor],wristWorld) ||
            !InvertBoneMatrix(wristWorld,inverseWristWorld) ||
            !ComposeBoneMatrices(desiredWristWorld,inverseWristWorld,t) ||
            !InvertBoneMatrix(root,inverseRoot) ||
            !ComposeBoneMatrices(t,root,tRoot) ||
            !ComposeBoneMatrices(inverseRoot,tRoot,m)) return false;
        for (int i=0;i<count;++i)
        {
            BoneMatrix transformed{};
            if (!ComposeBoneMatrices(m,bones[i],transformed)) return false;
            bones[i]=transformed;
        }
        if (meshScale!=1.0f)
        {
            const float anchor[3]={bones[transformAnchor].translation[0],
                                   bones[transformAnchor].translation[1],
                                   bones[transformAnchor].translation[2]};
            for (int i=0;i<count;++i)
            {
                for (int r=0;r<3;++r)
                    bones[i].translation[r]=anchor[r]+
                        (bones[i].translation[r]-anchor[r])*meshScale;
                bones[i].scale*=meshScale;
            }
        }
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true))
            LOG("M3: marker/muzzle bones rigid-parented with the same transform as the gun");
        return true;
    }

    bool __fastcall FpInterpolateHook(int view,int id,int slot,
                                      BoneMatrix** outBones,int* outCount)
    {
        const bool result=g_origFpInterpolate(view,id,slot,outBones,outCount);
        if (slot==0 || slot==1)
        {
        g_fpInterpolationContexts[slot]={};
        if (result && outBones && outCount && *outBones)
        {
            const int count=*outCount;
            const int cached=g_fpBoneCount[slot].load(std::memory_order_acquire);
            const int wrist=g_fpWristIndex[slot].load(std::memory_order_acquire);
            const int cameraControl=g_fpOrientationIndex[slot].load(std::memory_order_acquire);
            const int elbow=g_fpElbowIndex[slot].load(std::memory_order_acquire);
            const int shoulder=g_fpShoulderIndex[slot].load(std::memory_order_acquire);
            if (count>0 && count<=64 && cached==count &&
                wrist>=0 && wrist<count && cameraControl>=0 && cameraControl<count)
            {
                auto& context=g_fpInterpolationContexts[slot];
                context.source=*outBones;
                context.count=count;
                context.player=view;
                context.slot=slot;
                context.wrist=wrist;
                context.cameraControl=cameraControl;
                context.elbow=elbow;
                context.shoulder=shoulder;
                context.wristDescendants=
                    g_fpWristDescendants[slot].load(std::memory_order_acquire);
                context.lWrist=g_fpLWristIndex[slot].load(std::memory_order_acquire);
                context.lElbow=g_fpLElbowIndex[slot].load(std::memory_order_acquire);
                context.lShoulder=g_fpLShoulderIndex[slot].load(std::memory_order_acquire);
                context.lWristDescendants=
                    g_fpLWristDescendants[slot].load(std::memory_order_acquire);
                context.valid=true;
                memcpy(g_fpUnmodifiedInterpolations[slot],*outBones,
                       static_cast<size_t>(count)*sizeof(BoneMatrix));
                if (slot==1)
                {
                    static std::atomic<bool> loggedDual{false};
                    if (!loggedDual.exchange(true))
                        LOG("DUAL: slot 1 FP context captured (%d bones, wrist %d, "
                            "camera_control %d) — left-hand weapon path live",
                            count,wrist,cameraControl);
                }
                // Measure the authored barrel-in-wrist direction (row 0 of the
                // wrist rotation = invRot * camera-forward; storage m[c*3+r]).
                // Per slot: each weapon hand auto-aligns to its own ray.
                {
                    const float* wr=g_fpUnmodifiedInterpolations[slot][wrist].rotation;
                    float b[3]={wr[0],wr[3],wr[6]};
                    const float bl=sqrtf(b[0]*b[0]+b[1]*b[1]+b[2]*b[2]);
                    if (bl>1e-4f && isfinite(bl))
                    {
                        b[0]/=bl; b[1]/=bl; b[2]/=bl;
                        if (g_barrelInWristValid[slot].load(std::memory_order_relaxed))
                        {
                            const float a=0.02f;
                            for(int j=0;j<3;++j)
                                b[j]=g_barrelInWrist[slot][j].load(std::memory_order_relaxed)
                                     *(1.0f-a)+b[j]*a;
                            const float l2=sqrtf(b[0]*b[0]+b[1]*b[1]+b[2]*b[2]);
                            if (l2>1e-4f){ b[0]/=l2; b[1]/=l2; b[2]/=l2; }
                        }
                        for(int j=0;j<3;++j)
                            g_barrelInWrist[slot][j].store(b[j],std::memory_order_relaxed);
                        g_barrelInWristValid[slot].store(true,std::memory_order_release);
                        static std::atomic<bool> loggedBarrel{false};
                        if (slot==0 && !loggedBarrel.exchange(true))
                            LOG("M3 VRIK: authored barrel-in-wrist measured "
                                "(%.3f, %.3f, %.3f) — expect ~(1,0,0)+cant if the "
                                "camera-forward invariant holds",b[0],b[1],b[2]);
                    }
                }
                ApplyControllerToMarkerBones(view,*outBones,count,wrist,
                                             cameraControl,slot==1);
            }
        }
        }
        return result;
    }

    // TRUE RIGID PARENTING (2026-07-19, replacing the cached-relation
    // reconstruction). Everything below is computed from THIS frame only:
    // the untouched interpolated bones, the actual render root passed to the
    // palette consumer, and a live controller read. One rigid transform T
    // snaps the untouched wrist bone's world pose onto the controller
    // (rotation AND position, all axes), and the same T is applied to every
    // bone, so the assembly stays exactly as authored/animated. No cached
    // wrist->camera_control relation, no synthesized bones, no sim-clock
    // state — the layer the user correctly called a "mask" is gone. Barrel
    // mounting is a CONSTANT user trim (gun_pitch/yaw/roll + gun_forward_m),
    // not a per-frame estimate.
    // ONE shared definition of "where a wrist glued to its controller belongs
    // in the world" — used by the visible palette, the arm IK, AND the
    // muzzle/marker path so the gun, flash, and hands can never diverge. The
    // right hand carries the weapon mount trim + forward standoff; the left
    // hand mirrors the yaw/roll trim and has no standoff.
    bool DesiredWristWorld(bool left, BoneMatrix& out, float& meshScale)
    {
        float basisC[9], posC[3];
        if (!ControllerWorldPoseEx(left, basisC, posC, meshScale))
            return false;
        const float sign = left ? -1.0f : 1.0f;
        float mount[9], mounted[9];
        BasisFromAngles(sign * g_config.gun_yaw_deg * 0.0174533f,
                        g_config.gun_pitch_deg * 0.0174533f,
                        sign * g_config.gun_roll_deg * 0.0174533f, mount);
        MultiplyBases(basisC, mount, mounted);
        // AUTO BARREL ALIGNMENT (weapon hands only): swing the mounted hand by
        // the minimal world rotation that puts the measured authored barrel
        // axis exactly on the controller ray (basis column 0) — the SAME ray
        // the cursor and bullet steering use. The barrel therefore sits on the
        // cursor line by construction; the trim sliders adjust hand posture
        // and roll about that fixed line, they can no longer misalign it.
        // Weapon hand = the right hand only: the dual-wield secondary is
        // seated by its weapon NODE directly at the palm point, so the
        // wrist-row-0 swing heuristic must not fight it (23:04 headset
        // result: the heuristic mis-seated the left gun).
        const int barrelSlot=left?-1:0;
        if (barrelSlot>=0 &&
            g_barrelInWristValid[barrelSlot].load(std::memory_order_acquire))
        {
            const float b[3]={g_barrelInWrist[barrelSlot][0].load(std::memory_order_relaxed),
                              g_barrelInWrist[barrelSlot][1].load(std::memory_order_relaxed),
                              g_barrelInWrist[barrelSlot][2].load(std::memory_order_relaxed)};
            float worldBarrel[3]={0,0,0};
            for(int c=0;c<3;++c)
                for(int r=0;r<3;++r)
                    worldBarrel[r]+=mounted[c*3+r]*b[c];
            const float ray[3]={basisC[0],basisC[1],basisC[2]};
            float swing[9],aligned[9];
            ShortestArcRotation(worldBarrel,ray,swing);
            MultiplyBases(swing,mounted,aligned);
            bool ok=true;
            for(int j=0;j<9;++j) if (!isfinite(aligned[j])) { ok=false; break; }
            if (ok)
            {
                memcpy(mounted,aligned,sizeof(aligned));
                static std::atomic<bool> loggedSwing{false};
                if (!loggedSwing.exchange(true))
                {
                    const float d=Clamp(worldBarrel[0]*ray[0]+worldBarrel[1]*ray[1]+
                                        worldBarrel[2]*ray[2],-1.0f,1.0f);
                    LOG("M3 VRIK: auto barrel alignment ACTIVE, first swing %.1f deg",
                        acosf(d)*57.2958f);
                }
            }
        }
        out = BoneMatrix{};
        out.scale = 1.0f;
        memcpy(out.rotation, mounted, sizeof(mounted));
        memcpy(out.translation, posC, sizeof(posC));
        return true;
    }

    bool ReconstructVisiblePaletteSource(const FpInterpolationContext& context,
                                         const BoneMatrix& root,
                                         const BoneMatrix* source,
                                         const BoneMatrix*& replacement)
    {
        if (!context.valid || context.slot<0 || context.slot>1 ||
            source!=context.source ||
            context.count<=0 || context.count>64 ||
            context.wrist<0 || context.wrist>=context.count) return false;

        // Dual-wield secondary (slot 1): its LEFT hand follows the left
        // controller. The separate weapon subtree then inherits that solved
        // hand's rigid delta; it is never independently seated on a controller.
        // Slot-1 failures never touch the primary arm diagnostics.
        const bool dual=(context.slot==1);
        const BoneMatrix* const unmodified=g_fpUnmodifiedInterpolations[context.slot];

        float meshScale=1.0f;
        BoneMatrix desiredWristWorld{};
        if (!DesiredWristWorld(dual,desiredWristWorld,meshScale))
        {
            if (dual) return false;
            g_armFailurePublished.store("right-controller-pose",std::memory_order_relaxed);
            g_armFailureSide.store(1,std::memory_order_release);
            return false;
        }
        // CENTER-ROOT WORLD SOLVE (2026-07-19): this function runs once per
        // EYE, and the palette consumer's `root` is that eye's camera. Any
        // world position built from it (the planted shoulder, the solved
        // elbow) shifts by the eye offset, so the two eyes solved DIFFERENT
        // arms — the user saw the bare left arm split between eyes. Build all
        // WORLD-space poses from the live CENTER camera instead; only the
        // final record conversion (invRoot) may use the eye root, which makes
        // the rendered world pose eye-independent and the stereo fuse.
        BoneMatrix centerRoot=root;
        {
            float camBasis[9];
            if (g_camValid.load() && LoadCameraBasis(camBasis))
            {
                memcpy(centerRoot.rotation,camBasis,sizeof(camBasis));
                centerRoot.translation[0]=g_camX.load();
                centerRoot.translation[1]=g_camY.load();
                centerRoot.translation[2]=g_camZ.load();
            }
        }

        // SHOULDER LEVELING (2026-07-19): the arm rest pose is composed from the
        // camera frame, so its FULL pitch/roll rides the head — look down and the
        // shoulder swings up into your face (user: "the shoulders follow my head,
        // they don't stay in place"). Build a torso frame with the camera's
        // HEADING only (yaw about world-up +Z; Blam is a Z-up engine, and the
        // camera cols are fwd/left/up per below), level in pitch/roll, at the
        // camera position. Anchor the arm to THAT instead of the pitching camera.
        // The hand + gun ride the controller target and are root-invariant (the
        // root cancels in desired*invWristRest*bone), so this levels the visible
        // UPPER ARM without moving the hand/gun the user already likes. Falls
        // back to the camera frame when looking near-vertical or when disabled.
        BoneMatrix torsoRoot=centerRoot;
        if (g_config.shoulder_level)
        {
            float cb[9];
            if (NormalizedBasis(centerRoot,cb))
            {
                // MEASURED world-up (not assumed) — see g_worldUp / CamCopyHook.
                const float U[3]={g_worldUp[0].load(),g_worldUp[1].load(),
                                  g_worldUp[2].load()};
                float fwd[3]={cb[0],cb[1],cb[2]};           // camera forward, world
                const float d=fwd[0]*U[0]+fwd[1]*U[1]+fwd[2]*U[2];
                float fH[3]={fwd[0]-U[0]*d,fwd[1]-U[1]*d,fwd[2]-U[2]*d};
                const float fl=sqrtf(fH[0]*fH[0]+fH[1]*fH[1]+fH[2]*fH[2]);
                if (fl>1e-3f)                                // not looking straight up/down
                {
                    fH[0]/=fl; fH[1]/=fl; fH[2]/=fl;
                    float L[3]={U[1]*fH[2]-U[2]*fH[1],       // left = up x fwd
                                U[2]*fH[0]-U[0]*fH[2],
                                U[0]*fH[1]-U[1]*fH[0]};
                    const float ll=sqrtf(L[0]*L[0]+L[1]*L[1]+L[2]*L[2]);
                    if (ll>1e-3f)
                    {
                        L[0]/=ll; L[1]/=ll; L[2]/=ll;
                        torsoRoot.rotation[0]=fH[0]; torsoRoot.rotation[1]=fH[1]; torsoRoot.rotation[2]=fH[2];
                        torsoRoot.rotation[3]=L[0];  torsoRoot.rotation[4]=L[1];  torsoRoot.rotation[5]=L[2];
                        torsoRoot.rotation[6]=U[0];  torsoRoot.rotation[7]=U[1];  torsoRoot.rotation[8]=U[2];
                    }
                }
            }
        }
        // The arm rest pose and elbow pole are built from this frame; when
        // shoulder_level is off it equals centerRoot, so behavior is unchanged.
        const BoneMatrix& armRoot = torsoRoot;

        // VRIK ARM IK: keep the body exactly where the game posed it and bend
        // ONLY the arms so each wrist reaches its controller — shoulders
        // planted, elbows solved analytically (ik.cpp), hand + gun ride the
        // wrist rigidly. Right arm is required; the left arm applies when its
        // chain resolved and the left controller is tracked.
        const bool carrierChainValid=dual
            ? (context.lShoulder>=0 && context.lShoulder<context.count &&
               context.lElbow>=0 && context.lElbow<context.count &&
               context.lWrist>=0 && context.lWrist<context.count)
            : (context.shoulder>=0 && context.shoulder<context.count &&
               context.elbow>=0 && context.elbow<context.count);
        if (g_config.arm_ik && carrierChainValid)
        {
            const BoneMatrix* unmod=unmodified;
            memcpy(g_fpPaletteScratch,unmod,
                   static_cast<size_t>(context.count)*sizeof(BoneMatrix));
            BoneMatrix invRoot{};
            if (InvertBoneMatrix(root,invRoot))
            {
                // IK divergence probe (log-only): capture the per-eye solve
                // inputs so a single desktop/headset session names WHICH input
                // differs between the two eye passes (root by design; anything
                // else is the left-arm-splits-between-eyes bug). Filled by
                // applyArm, compared when eye 1 lands against eye 0.
                struct ArmProbe { float S[3]; float E[3]; float T[3];
                                  float upperLen; float lowerLen; };
                ArmProbe probeLeft{};
                bool probeLeftValid=false;
                auto applyArm=[&](int shoulder,int elbow,int wrist,uint64_t mask,
                                  const BoneMatrix& desired,float outSign,
                                  ArmProbe* probeOut,float shoulderDrop)->bool
                {
                    g_armFailWhy="compose-rest";
                    BoneMatrix shW{},elW{},wrW{};
                    if (!ComposeBoneMatrices(armRoot,unmod[shoulder],shW) ||
                        !ComposeBoneMatrices(armRoot,unmod[elbow],elW) ||
                        !ComposeBoneMatrices(armRoot,unmod[wrist],wrW)) return false;
                    // Lower the shoulder anchor along the view-DOWN axis (camera
                    // up column, negated) so Chief's arm sits lower and stops
                    // clipping the face. Right arm only (shoulderDrop>0).
                    if (shoulderDrop>0.0f)
                    {
                        float rb[9];
                        if (NormalizedBasis(armRoot,rb))
                            for (int j=0;j<3;++j)
                                shW.translation[j]-=rb[6+j]*shoulderDrop;
                    }
                    const float* S=shW.translation; const float* Er=elW.translation;
                    const float* Wr=wrW.translation;
                    auto dist=[](const float* a,const float* b){
                        const float dx=a[0]-b[0],dy=a[1]-b[1],dz=a[2]-b[2];
                        return sqrtf(dx*dx+dy*dy+dz*dz); };
                    const float upperLen=dist(S,Er), lowerLen=dist(Er,Wr);
                    const float* T=desired.translation;
                    float mid[3]={(S[0]+Wr[0])*0.5f,(S[1]+Wr[1])*0.5f,(S[2]+Wr[2])*0.5f};
                    float pole[3]={Er[0]-mid[0],Er[1]-mid[1],Er[2]-mid[2]};
                    if (pole[0]*pole[0]+pole[1]*pole[1]+pole[2]*pole[2]<1e-6f)
                    { pole[0]=0; pole[1]=0; pole[2]=-1; }
                    // Elbow pole bias: OUT (away from the torso, camera left/
                    // right column) and DOWN, 75/25 over the authored pole
                    // (user: elbows "feel inward — I want them to stick
                    // outward"). Root is the camera: cols fwd/left/up.
                    {
                        float rootB[9];
                        if (NormalizedBasis(armRoot,rootB))
                        {
                            const float outDir[3]={
                                rootB[3]*outSign-rootB[6]*0.6f,
                                rootB[4]*outSign-rootB[7]*0.6f,
                                rootB[5]*outSign-rootB[8]*0.6f};
                            const float ol=sqrtf(outDir[0]*outDir[0]+
                                outDir[1]*outDir[1]+outDir[2]*outDir[2]);
                            const float pl=sqrtf(pole[0]*pole[0]+
                                pole[1]*pole[1]+pole[2]*pole[2]);
                            if (ol>1e-4f && pl>1e-4f)
                                for (int j=0;j<3;++j)
                                    pole[j]=pole[j]/pl*0.25f+outDir[j]/ol*0.75f;
                        }
                    }
                    // FULL EXTENSION: when the controller is beyond the arm's
                    // natural reach, the two-bone solver clamps to a straight
                    // chain that stops SHORT of the hand, so the hand visibly
                    // detaches from the forearm. Stretch both bones so the arm
                    // always reaches the wrist (skinning stretches the mesh with
                    // it) — capped so it never looks rubbery. Right arm only
                    // (the user's left is perfect and untouched here anyway).
                    float solveUpper=upperLen, solveLower=lowerLen;
                    {
                        const float reach=upperLen+lowerLen;
                        const float td=sqrtf((T[0]-S[0])*(T[0]-S[0])+
                                             (T[1]-S[1])*(T[1]-S[1])+
                                             (T[2]-S[2])*(T[2]-S[2]));
                        if (reach>1e-4f && td>reach)
                        {
                            const float k=fminf(td/reach,1.8f);
                            solveUpper*=k; solveLower*=k;
                        }
                    }
                    float E[3];
                    g_armFailWhy="two-bone-solve";
                    if (!IK_SolveTwoBone(S,T,solveUpper,solveLower,pole,E)) return false;
                    if (probeOut)
                    {
                        for(int j=0;j<3;++j){ probeOut->S[j]=S[j]; probeOut->E[j]=E[j];
                                              probeOut->T[j]=T[j]; }
                        probeOut->upperLen=upperLen; probeOut->lowerLen=lowerLen;
                    }
                    auto unit=[](const float* a,const float* b,float* o){
                        o[0]=a[0]-b[0];o[1]=a[1]-b[1];o[2]=a[2]-b[2];
                        const float l=sqrtf(o[0]*o[0]+o[1]*o[1]+o[2]*o[2]);
                        if (l>1e-6f){o[0]/=l;o[1]/=l;o[2]/=l;} };
                    float restU[3],restF[3],newU[3],newF[3];
                    unit(Er,S,restU); unit(Wr,Er,restF);
                    unit(E,S,newU);   unit(T,E,newF);
                    float Ru[9],Rf[9];
                    ShortestArcRotation(restU,newU,Ru);
                    ShortestArcRotation(restF,newF,Rf);
                    BoneMatrix newSh=shW; MultiplyBases(Ru,shW.rotation,newSh.rotation);
                    BoneMatrix newEl=elW; MultiplyBases(Rf,elW.rotation,newEl.rotation);
                    newEl.translation[0]=E[0]; newEl.translation[1]=E[1]; newEl.translation[2]=E[2];
                    BoneMatrix invWrRest{},D{};
                    g_armFailWhy="invert-wrist";
                    if (!InvertBoneMatrix(wrW,invWrRest)) return false;
                    ComposeBoneMatrices(desired,invWrRest,D);
                    ComposeBoneMatrices(invRoot,newSh,g_fpPaletteScratch[shoulder]);
                    ComposeBoneMatrices(invRoot,newEl,g_fpPaletteScratch[elbow]);
                    for (int i=0;i<context.count && i<64;++i)
                    {
                        if (!(mask&(1ull<<i))) continue;
                        BoneMatrix boneW{},newW{};
                        if (ComposeBoneMatrices(armRoot,unmod[i],boneW) &&
                            ComposeBoneMatrices(D,boneW,newW))
                            ComposeBoneMatrices(invRoot,newW,g_fpPaletteScratch[i]);
                    }
                    g_armFailWhy=nullptr;
                    return true;
                };
                // Primary keeps its right-hand path. A dual weapon is owned by
                // the actual LEFT hand, which follows the left controller.
                const bool handApplied=dual
                    ? applyArm(context.lShoulder,context.lElbow,context.lWrist,
                               context.lWristDescendants,desiredWristWorld,
                               1.0f,nullptr,0.0f)
                    : applyArm(context.shoulder,context.elbow,context.wrist,
                               context.wristDescendants,desiredWristWorld,
                               -1.0f,nullptr,g_config.right_shoulder_drop);
                if (handApplied)
                {
                    if (dual)
                    {
                        BoneMatrix authoredLeftWorld{},invAuthoredLeft{},handDelta{};
                        if (!ComposeBoneMatrices(armRoot,unmod[context.lWrist],
                                                 authoredLeftWorld) ||
                            !InvertBoneMatrix(authoredLeftWorld,invAuthoredLeft) ||
                            !ComposeBoneMatrices(desiredWristWorld,invAuthoredLeft,
                                                 handDelta)) return false;
                        for (int i=0;i<context.count && i<64;++i)
                        {
                            if (!(context.wristDescendants&(1ull<<i))) continue;
                            BoneMatrix authoredWorld{},lockedWorld{};
                            if (!ComposeBoneMatrices(armRoot,unmod[i],authoredWorld) ||
                                !ComposeBoneMatrices(handDelta,authoredWorld,lockedWorld) ||
                                !ComposeBoneMatrices(invRoot,lockedWorld,
                                                     g_fpPaletteScratch[i])) return false;
                        }
                        static std::atomic<bool> loggedDualIk{false};
                        if (!loggedDualIk.exchange(true))
                            LOG("DUAL VRIK: slot 1 arm IK active on the LEFT "
                                "controller (wrist %d, elbow %d, shoulder %d)",
                                context.lWrist,context.lElbow,context.lShoulder);
                    }
                    // Left SUPPORT arm (primary weapon only): same treatment
                    // onto the left controller. During dual wield the left
                    // hand holds slot 1's weapon instead, and slot 1's own
                    // mirror-side chain stays game-animated.
                    auto publishLeftFailure=[](const char* why){
                        g_armFailurePublished.store(why,std::memory_order_relaxed);
                        g_armFailureSide.store(2,std::memory_order_release);
                    };
                    if (dual)
                    {
                        // HEADSET RESULTS: 22:51 build — the visible arm is
                        // skinned to the lWrist/lElbow/lShoulder chain, not the
                        // gun chain. 22:59 build — carrying that arm at its
                        // AUTHORED offset from the gun put the hand ~1 m left:
                        // with the stock weapon-IK branch patched off, the
                        // secondary's animation never poses l_hand on the grip,
                        // so there is no authored grip relation to preserve.
                        // Target the visible hand at the LEFT CONTROLLER itself
                        // — the identical, user-tuned support-hand treatment
                        // (palm correction + mirrored trim, F1 slider applies).
                        if (context.lShoulder>=0 && context.lShoulder<context.count &&
                            context.lElbow>=0 && context.lElbow<context.count &&
                            context.lWrist>=0 && context.lWrist<context.count)
                        {
                            BoneMatrix desiredL{}; float leftScale=1.0f;
                            if (DesiredWristWorld(true,desiredL,leftScale))
                            {
                                static std::atomic<bool> loggedDualArm{false};
                                if (applyArm(context.lShoulder,context.lElbow,
                                             context.lWrist,context.lWristDescendants,
                                             desiredL,1.0f,nullptr,0.0f) &&
                                    !loggedDualArm.exchange(true))
                                    LOG("DUAL VRIK: secondary VISIBLE hand bound to "
                                        "the left controller (wrist %d, elbow %d, "
                                        "shoulder %d)",
                                        context.lWrist,context.lElbow,context.lShoulder);
                            }
                        }
                    }
                    else if (context.lShoulder>=0 && context.lShoulder<context.count &&
                        context.lElbow>=0 && context.lElbow<context.count &&
                        context.lWrist>=0 && context.lWrist<context.count)
                    {
                        BoneMatrix desiredLeft{}; float leftScale=1.0f;
                        if (DesiredWristWorld(true,desiredLeft,leftScale))
                        {
                            static std::atomic<bool> loggedLeft{false};
                            if (applyArm(context.lShoulder,context.lElbow,context.lWrist,
                                         context.lWristDescendants,desiredLeft,1.0f,
                                         &probeLeft,0.0f))
                            {
                                probeLeftValid=true;
                                g_armFailurePublished.store(nullptr,std::memory_order_relaxed);
                                g_armFailureSide.store(0,std::memory_order_release);
                                if (!loggedLeft.exchange(true))
                                    LOG("M3 VRIK: LEFT arm on the left controller "
                                        "(wrist %d, elbow %d, shoulder %d)",
                                        context.lWrist,context.lElbow,context.lShoulder);
                            }
                            else publishLeftFailure(g_armFailWhy?g_armFailWhy:"apply-arm");
                        }
                        else publishLeftFailure("left-controller-pose");
                    }
                    else publishLeftFailure("left-chain-indices");
                    // Compare this eye's LEFT-arm solve inputs to the other
                    // eye's. dRoot large is expected (eye offset). Any nonzero
                    // dCenterRoot / dWrist / dLens / dDesired names the leaking
                    // per-eye input behind the left-arm split. Rate-limited.
                    if (probeLeftValid)
                    {
                        static ArmProbe eyeProbe[2]{};
                        static BoneMatrix eyeRoot[2]{}, eyeCenterRoot[2]{};
                        static bool eyeHave[2]={false,false};
                        const int eye=g_stereoEye.load();
                        if (eye==0 || eye==1)
                        {
                            eyeProbe[eye]=probeLeft; eyeRoot[eye]=root;
                            eyeCenterRoot[eye]=centerRoot; eyeHave[eye]=true;
                            static std::atomic<uint64_t> lastMs{0};
                            const uint64_t now=GetTickCount64();
                            if (eye==1 && eyeHave[0] &&
                                now-lastMs.load()>2000)
                            {
                                lastMs.store(now);
                                auto d3=[](const float* a,const float* b){
                                    float m=0; for(int j=0;j<3;++j){
                                        const float e=fabsf(a[j]-b[j]); if(e>m)m=e;} return m;};
                                const float dRoot=d3(eyeRoot[0].translation,eyeRoot[1].translation);
                                const float dCenter=d3(eyeCenterRoot[0].translation,
                                                       eyeCenterRoot[1].translation);
                                const float dWrist=d3(eyeProbe[0].T,eyeProbe[1].T);
                                const float dElbow=d3(eyeProbe[0].E,eyeProbe[1].E);
                                const float dLens=fabsf(eyeProbe[0].upperLen-eyeProbe[1].upperLen)+
                                                  fabsf(eyeProbe[0].lowerLen-eyeProbe[1].lowerLen);
                                LOG("IK-PROBE dRoot=%.4f dCenterRoot=%.4f dDesiredWrist=%.4f "
                                    "dElbow=%.4f dLens=%.4f (dRoot big=OK; others should be ~0)",
                                    dRoot,dCenter,dWrist,dElbow,dLens);
                            }
                        }
                    }
                    // Uniform size trim about the gripping hand (grip stays put).
                    if (meshScale!=1.0f)
                    {
                        const int scaleBone=dual?context.lWrist:context.wrist;
                        const float anchor[3]={
                            g_fpPaletteScratch[scaleBone].translation[0],
                            g_fpPaletteScratch[scaleBone].translation[1],
                            g_fpPaletteScratch[scaleBone].translation[2]};
                        const uint64_t mask=context.wristDescendants;
                        for (int i=0;i<context.count && i<64;++i)
                        {
                            if (!(mask&(1ull<<i))) continue;
                            BoneMatrix& bone=g_fpPaletteScratch[i];
                            for (int r=0;r<3;++r)
                                bone.translation[r]=anchor[r]+
                                    (bone.translation[r]-anchor[r])*meshScale;
                            bone.scale*=meshScale;
                        }
                    }
                    // NOTE: a "length squash along the barrel" is NOT possible
                    // here and must not be re-attempted: the weapon mesh is
                    // rigid geometry on essentially one bone, and BoneMatrix
                    // carries a single UNIFORM scale. Squashing bone ORIGINS
                    // only translates the mesh (user-confirmed 2026-07-19:
                    // "the length just moves the gun"). Size trims are
                    // gun_scale (uniform) and gun_forward_m (seat depth).
                    replacement=g_fpPaletteScratch;
                    static std::atomic<bool> loggedIk{false};
                    if (!loggedIk.exchange(true))
                        LOG("M3 VRIK: arm IK active — shoulder %d planted, elbow %d solved, "
                            "wrist %d + %lld subtree bones to controller",
                            context.shoulder,context.elbow,context.wrist,
                            (long long)__popcnt64(context.wristDescendants));
                    // Full-solve rate (both arms). ~2x fps today (once per eye);
                    // the Session B once-per-frame cache should halve this to ~fps.
                    {
                        static std::atomic<uint32_t> solves{0};
                        static std::atomic<DWORD> lastLog{GetTickCount()};
                        solves.fetch_add(1);
                        const DWORD now=GetTickCount(); DWORD last=lastLog.load();
                        if (now-last>=10000 && lastLog.compare_exchange_strong(last,now))
                            LOG("PERF: FP palette full solves %.0f/sec "
                                "(compare to fps; ~2x fps = once per eye)",
                                solves.exchange(0)*1000.0/(now-last));
                    }
                    return true;
                }
                if (!dual)
                {
                    g_armFailurePublished.store(g_armFailWhy?g_armFailWhy:"right-apply-arm",
                                                std::memory_order_relaxed);
                    g_armFailureSide.store(1,std::memory_order_release);
                }
            }
            else if (!dual)
            {
                g_armFailurePublished.store("invert-root",std::memory_order_relaxed);
                g_armFailureSide.store(1,std::memory_order_release);
            }
            // IK could not solve this frame — fall through to rigid parent.
        }

        // M = rootEye^-1 * T * rootCenter applied per record: the WORLD result
        else if (g_config.arm_ik && !dual)
        {
            g_armFailurePublished.store("right-chain-indices",std::memory_order_relaxed);
            g_armFailureSide.store(1,std::memory_order_release);
        }
        // T * rootCenter * record is built from the center camera (identical
        // in both eyes), and only the record conversion uses the eye root.
        BoneMatrix wristWorld{},inverseWristWorld{},t{},inverseRoot{},m{},tRoot{};
        if (!ComposeBoneMatrices(centerRoot,unmodified[context.wrist],wristWorld) ||
            !InvertBoneMatrix(wristWorld,inverseWristWorld) ||
            !ComposeBoneMatrices(desiredWristWorld,inverseWristWorld,t) ||
            !InvertBoneMatrix(root,inverseRoot) ||
            !ComposeBoneMatrices(t,centerRoot,tRoot) ||
            !ComposeBoneMatrices(inverseRoot,tRoot,m)) return false;

        for (int i=0;i<context.count;++i)
            if (!ComposeBoneMatrices(m,unmodified[i],g_fpPaletteScratch[i])) return false;

        // Uniform user size trim, applied about the wrist so the grip stays on
        // the controller.
        if (meshScale!=1.0f)
        {
            const float* wristT=g_fpPaletteScratch[context.wrist].translation;
            const float anchor[3]={wristT[0],wristT[1],wristT[2]};
            for (int i=0;i<context.count;++i)
            {
                BoneMatrix& bone=g_fpPaletteScratch[i];
                for (int r=0;r<3;++r)
                    bone.translation[r]=anchor[r]+(bone.translation[r]-anchor[r])*meshScale;
                bone.scale*=meshScale;
                if (!isfinite(bone.scale) || !isfinite(bone.translation[0])) return false;
            }
        }

        replacement=g_fpPaletteScratch;
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true))
            LOG("M3: FP palette rigid-parented to the controller "
                "(player %d, %d bones, wrist %d, single same-frame transform)",
                context.player,context.count,context.wrist);
        return true;
    }

    void __fastcall FpVisiblePaletteHook(uint16_t tag, const BoneMatrix* root,
                                         BoneMatrix* destination, uintptr_t unused,
                                         const BoneMatrix* source, const int32_t* boneMap)
    {
        // Match this palette submission to its slot's interpolation context by
        // the source pointer (each slot interpolates into its own bank), then
        // consume that context so a stale frame can never be re-applied.
        FpInterpolationContext context{};
        for (auto& candidate : g_fpInterpolationContexts)
            if (candidate.valid && source && candidate.source==source)
            {
                context=candidate;
                candidate.valid=false;
                break;
            }
        const BoneMatrix* selectedSource=source;
        // Never let the visible palette consume the live buffer after the
        // marker/effect preservation rewrite. If the final reconstruction is
        // temporarily unavailable (for example while a new weapon's authored
        // relation is publishing), use the untouched snapshot for this frame.
        if (context.valid && source==context.source)
            selectedSource=g_fpUnmodifiedInterpolations[context.slot];
        bool reconstructed=false;
        if (root && source)
            reconstructed=ReconstructVisiblePaletteSource(
                context,*root,source,selectedSource);
        g_origFpVisiblePalette(tag,root,destination,unused,selectedSource,boneMap);

        // Collect every UNIQUE final-palette submission, not just the first
        // one for a weapon. A shotgun-only secondary arm palette can otherwise
        // render the authored pump grip over the correctly solved fp_body.
        // Atomic publication only; Present owns all formatting and logging.
        const uint64_t key=g_fpSkeletonKey.load(std::memory_order_acquire);
        if (key)
        {
            uint64_t signature=key;
            signature=signature*31+tag;
            signature=signature*31+(context.valid?1:0);
            signature=signature*31+(reconstructed?1:0);
            static thread_local uint64_t seen[16]{};
            static thread_local int seenCount=0;
            bool known=false;
            for(int i=0;i<seenCount;++i) if(seen[i]==signature){known=true;break;}
            if(!known && seenCount<16)
            {
                seen[seenCount++]=signature;
                const int slot=g_fpBoneMapSnapshotCount.fetch_add(
                    1,std::memory_order_acq_rel);
                if(slot<16)
                {
                    auto& snap=g_fpBoneMapSnapshots[slot];
                    const int n=(context.valid && boneMap)
                        ? (context.count>64?64:context.count) : 0;
                    snap.sequence.fetch_add(1,std::memory_order_acq_rel);
                    snap.skeletonKey.store(key,std::memory_order_relaxed);
                    snap.tag.store(tag,std::memory_order_relaxed);
                    snap.count.store(n,std::memory_order_relaxed);
                    snap.reconstructed.store(reconstructed?1:0,
                                             std::memory_order_relaxed);
                    for(int i=0;i<n;++i)
                        snap.map[i].store(boneMap[i],std::memory_order_relaxed);
                    snap.sequence.fetch_add(1,std::memory_order_release);
                }
            }
        }
    }

    void __fastcall FpCameraRebuildHook(void* view, unsigned char flag)
    {
        // Let the engine do its full rebuild (including publishing
        // render_first_person_fov_scale for whatever else reads it) ...
        g_origFpCameraRebuild(view, flag);
        // 0x279BEC just built a compressed viewmodel camera + projection at
        // {view+0x08, view+0x1E8} (its argument's own offsets). While one of
        // OUR eye renders is active, overwrite BOTH with this eye's WORLD
        // camera and WORLD derived block (position, orientation, FOV AND the
        // depth terms), then re-run the engine's own constant uploader. This
        // is the last writer before the FP draw, so the gun/HUD render in true
        // world perspective instead of the crushed viewmodel depth slab.
        // No +0x6C8 gate: the earlier gate never matched (proven by the
        // diagnostic) and the layer kept its flattened depth. Applying to
        // whatever 0x279BEC rebuilds during our eye pass is exactly the set of
        // FP overlays that need world depth.
        char* eyeView = static_cast<char*>(g_eyeFpView.load(std::memory_order_acquire));
        if (!view || !eyeView) return;
        char* base = static_cast<char*>(view);
        memcpy(base + 0x08, g_eyeCompactCamera, sizeof(g_eyeCompactCamera));
        memcpy(base + 0x1E8, g_eyeDerivedBlock, sizeof(g_eyeDerivedBlock));
        if (g_fpCameraUpload)
            g_fpCameraUpload(base + 0x08, base + 0x1E8);
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true))
            LOG("M3: per-eye FP camera active — gun/HUD now render in full world "
                "projection (depth uncrushed)");
    }

    // (EnforceHudElements + ChudStateCopyHook REMOVED 2026-07-19 evening. They
    // force-wrote chud+0x144..0x14A every frame with an offset map the headset
    // DISPROVED (0x146 was a nav dot, not the crosshair; 0x32F97C copies only
    // 0x144..0x147) — the stomping suppressed the whole HUD except the objective
    // text and the F1 element checkboxes did nothing. The CHUD struct is now
    // fully game-managed; the only element control is the class-gated
    // crosshair predicate in 0x2EDF24. Do not write into
    // the chud byte block again without headset-verified offsets.)

    // DIAGNOSTIC (hud_probe): log the bytes in the CHUD struct that CHANGE, to
    // locate (a) the enemy target-lock state that turns the OG reticle red and
    // (b) the per-element visibility flags (health / motion sensor / ammo).
    // Aim at an enemy, then away, then toggle HUD elements — the flipped
    // offsets appear in the log. Log-only; changes nothing. Called from
    // CamCopyHook where the CHUD pointer is already resolved.
    void ChudProbe()
    {
        if (!g_config.hud_probe || !g_engineTlsIndex) return;
        auto** slots=reinterpret_cast<void**>(__readgsqword(0x58));
        if (!slots) return;
        auto* tls=reinterpret_cast<unsigned char*>(slots[*g_engineTlsIndex]);
        if (!tls) return;
        auto* chud=*reinterpret_cast<unsigned char**>(tls+0x220);
        if (!chud) return;
        // A window generous enough to span the known crosshair/show bytes
        // (0x144/0x146) plus nearby state. We force 0x144/0x146 every frame, so
        // ignore those two offsets in the diff.
        constexpr int kStart=0x100, kEnd=0x260;
        static unsigned char prev[kEnd-kStart];
        static bool have=false;
        if (!have) { memcpy(prev,chud+kStart,sizeof(prev)); have=true;
            LOG("HUD-PROBE armed: watching CHUD +0x%03X..+0x%03X for changes "
                "(aim at an enemy to find the red-reticle byte)",kStart,kEnd); return; }
        static std::atomic<uint64_t> lastMs{0};
        const uint64_t now=GetTickCount64();
        char line[512]; int n=0; int changes=0;
        for (int off=kStart; off<kEnd; ++off)
        {
            const int i=off-kStart;
            const unsigned char cur=chud[off];
            if (cur==prev[i]) continue;
            if (off==0x144 || off==0x146) { prev[i]=cur; continue; } // we drive these
            if (changes<24 && n<(int)sizeof(line)-24)
                n+=snprintf(line+n,sizeof(line)-n,"+0x%03X:%02X->%02X ",off,prev[i],cur);
            prev[i]=cur;
            ++changes;
        }
        if (changes && now-lastMs.load()>250)
        {
            lastMs.store(now);
            LOG("HUD-PROBE %d byte(s) changed: %s",changes,line);
        }
    }

    thread_local bool g_insideSpecialCompose=false;

    // Place the assembly by writing the root BEFORE the engine composes.
    // Post-editing the composed output is retired: it is downstream of the
    // engine's own weapon-lag pass (0x2C484B), which rotates every bone except
    // camera_control and so overwrote the mesh while leaving the muzzle flash
    // on our pose — exactly the reported split. `weapon_probe` still drives the
    // old output path, and only that, as a diagnostic.
    // THE MESH FIX — a call-site patch, not a detour (2026-07-15 ~04:00).
    // Proven chain, all read offline: the visible first-person mesh is built by
    // the object-node recomposer at halo3+0x341768 (single caller 0x3424DD),
    // which dequantizes the object's compressed animation and roots the chain
    // with `call 0x3453DC` at +0x341A5B — a generic object-root getter with 56
    // callers that fabricates {MakeTransformFromXZ(fwd@+0x5C, up@+0x68),
    // pos@+0x50} from the object datum. The FP arms/weapon objects sit exactly
    // at the camera every frame; that collocation IS the head-glue, and it is
    // also how the shim identifies them without knowing their handles.
    //
    // We patch the 4 aligned displacement bytes of that ONE call (atomic
    // InterlockedExchange, installed at DLL load before any level runs) to a
    // 12-byte trampoline that reaches FpRootShim. The shim calls the REAL
    // getter, and only if the returned root is camera-collocated replaces it
    // with the controller's world pose — a write into a STACK buffer the
    // renderer consumes immediately. No engine function is detoured, none of
    // the other 55 callers are affected, and no simulation state is touched.
    // Failure mode if MCC updates: signature miss -> log + gun stays glued.
    using FpRootFn = BoneMatrix*(__fastcall*)(uint16_t index, BoneMatrix* out);
    FpRootFn g_realFpRoot = nullptr;


    // THE HEAD-GLUE, finally acted on safely: right after composition, the FP
    // evaluator loops every bone EXCEPT camera_control (cmp r9d,[rdi+0x11A4])
    // and rotates it via `call 0x120DF8` at halo3+0x2C485B with a camera-
    // pitch/turn matrix. That is the exact flash-vs-mesh partition observed in
    // every headset test tonight. Detouring 0x120DF8 crashes on level load
    // (proven, banned); patching THIS ONE CALL SITE affects no other caller —
    // the same aligned-disp32 technique that survived a full session at
    // 0x341A5B. The shim skips the rotation only for bone addresses inside an
    // assembly we re-rooted this frame (thread_local ranges, same thread that
    // composes), and forwards everything else to the real function untouched.
    using SwayApplyFn = void(__fastcall*)(void* sway, BoneMatrix* bone);
    SwayApplyFn g_realSwayApply = nullptr;

    // CRASH LESSON (both fatal errors tonight, same root cause): halo3.dll is
    // LTCG-optimized — the sway loop keeps its counter (r9d), bone pointer
    // (r8) and count (r10d) LIVE IN VOLATILE REGISTERS across `call 0x120DF8`,
    // because the compiler knows that function never touches them. ANY
    // compiled C/C++ interposition (a MinHook detour or a C++ shim) clobbers
    // those registers and corrupts the caller -> wild writes -> fatal error on
    // level load. Interposing engine-internal calls therefore requires a
    // hand-assembled shim restricted to registers the caller provably treats
    // as dead — here, only RAX (verified: reloaded/unused after the call).
    //
    // The emitted shim (see InstallHook) compares rdx (the bone) against
    // these bounds and returns without rotating when it lies inside an
    // assembly ApplyControllerToRoot re-rooted this frame; everything else
    // tail-jumps to the real rotator. Same game thread writes and reads the
    // bounds (compose -> sway loop), so there is no race.
    alignas(8) volatile uintptr_t g_fpSkipBounds[4] = {0, 0, 0, 0}; // lo0,hi0,lo1,hi1

    bool ControllerWorldPoseEx(bool left, float basis[9], float pos[3], float& scale)
    {
        if (!g_vrAim.load() || !g_enabled.load() || !g_camValid.load() ||
            !g_baseCamValid.load()) return false;
        float cq[4], cp[3];
        // Right/weapon hand uses the shared aim pose (two-hand-adjusted); the
        // left hand uses its own controller for the support-arm IK target.
        if (left ? !VR_GetLeftControllerPose(cq, cp)
                 : !VR_GetAimPose(cq, cp)) return false;
        BuildTrackedGameBasis(cq, false, basis); // controller basis, game world axes
        // Full controller displacement from the recentered room origin. The
        // old camera + (controller-currentHead) expression combined a camera
        // position containing an older head-lean sample with a fresh relative
        // hand sample. That subtraction shortened forward reach and leaked
        // head motion. The gameplay base below was captured before head lean.
        const float dx=cp[0]-g_headPosRef[0];
        const float dy=cp[1]-g_headPosRef[1];
        const float dz=cp[2]-g_headPosRef[2];
        const float sh=sinf(g_headYawRef), ch=cosf(g_headYawRef);
        const float roomForward=dx*sh-dz*ch;
        const float roomRight=dx*ch+dz*sh;
        const float cg=cosf(g_gameYawRef), sg=sinf(g_gameYawRef);
        const float s = g_worldScale.load();
        const float off[3] = {
            (cg*roomForward+sg*roomRight)*s,
            (sg*roomForward-cg*roomRight)*s,
            dy*s};
        const float cam[3] = {g_baseCamX.load(),g_baseCamY.load(),g_baseCamZ.load()};
        // Forward standoff along the controller's own aim direction (basis
        // column 0 = forward). EVERY left-hand use — support hand AND the
        // dual-wield gun seat — is the same wrist-to-palm PALM point (23:17
        // headset result: seating the dual gun by the weapon depth put it on
        // the wrist, ~12 cm behind the rendered hand). Right keeps its
        // independent weapon offset.
        const float standoff = (left
            ? Clamp(g_config.left_hand_forward_m, -0.15f, 0.30f)
            : Clamp(g_config.gun_forward_m, -0.3f, 0.5f)) * s;
        for (int j = 0; j < 3; ++j)
            pos[j] = cam[j] + off[j] + basis[j] * standoff;
        scale = Clamp(g_config.gun_scale, 0.3f, 3.0f);
        for (int j = 0; j < 9; ++j) if (!isfinite(basis[j])) return false;
        for (int j = 0; j < 3; ++j) if (!isfinite(pos[j])) return false;
        return true;
    }

    // Legacy name: the right-hand pose (existing call sites).
    bool ControllerWorldPose(float basis[9], float pos[3], float& scale)
    {
        return ControllerWorldPoseEx(false, basis, pos, scale);
    }

    // Repurposed 2026-07-19 as the VRIK Stage A2 probe. This call-site patch
    // sits inside the OBJECT-node recomposer (0x341768) — the pipeline that
    // animates every visible biped, including the player's own natively
    // visible legs. With the Bone-probe checkbox on, every recomposed object's
    // root is pushed 0.3 wu left. Legs/NPCs visibly shifting = this boundary
    // reaches biped pixels = Stage B (arm IK) has a real write path.
    // The old camera-collocated re-anchor is retired (the FP camera anchor in
    // RenderViewHook owns gun placement now).
    // Retired to a clean passthrough (2026-07-19). This object recomposer's
    // root output was proven NOT to drive the visible body (the +0.6 lift test
    // moved nothing on screen), and the FP camera anchor owns gun placement.
    // Kept only so the call-site patch remains a stable, harmless no-op rather
    // than reintroducing an unpatch path. See docs/VRIK-ROADMAP.md.
    BoneMatrix* __fastcall FpRootShim(uint16_t index, BoneMatrix* out)
    {
        return g_realFpRoot(index, out);
    }

    // The 03:27 lever, restored: write the composers' `defaults` root. Proven
    // side-effect-free in the headset and it puts the muzzle flash/markers on
    // the controller. Does NOT move the mesh (that consumer is still unfound).
    bool ApplyControllerToRoot(BoneMatrix* output, BoneMatrix* root)
    {
        if (!root || !output) return false;
        int slot=-1; unsigned char* weapon=nullptr;
        if (!FindFirstPersonWeapon(output,slot,weapon)) return false;
        float target[3],desired[9];
        if (!GetControllerFirstPersonTransform(slot,target,desired)) return false;
        for(float v : target) if (!isfinite(v)) return false;
        for(float v : desired) if (!isfinite(v)) return false;
        const float meshScale=Clamp(g_config.gun_scale,0.3f,3.0f);
        if (!isfinite(meshScale) || meshScale<=0.0f) return false;
        root->scale=meshScale;
        memcpy(root->rotation,desired,sizeof(desired));
        memcpy(root->translation,target,sizeof(target));
        // This assembly now carries the controller pose; exempt exactly these
        // bones from the engine's camera pitch/turn rotation (emitted shim).
        const int count=*reinterpret_cast<int*>(weapon+0x49C);
        if (count>0 && count<=64)
        {
            g_fpSkipBounds[slot*2+0]=reinterpret_cast<uintptr_t>(output);
            g_fpSkipBounds[slot*2+1]=g_fpSkipBounds[slot*2+0]+static_cast<size_t>(count)*sizeof(BoneMatrix);
        }
        static std::atomic<unsigned> logged{0};
        const unsigned bit=1u<<slot;
        if (!(logged.fetch_or(bit)&bit))
            LOG("M3: first-person slot %d rooted to the %s controller (markers/flash lever, scale %.2f)",
                slot,slot==0?"right":"left",meshScale);
        return true;
    }

    // CENSUS RESULT (2026-07-19, retired): the two composer hooks we install
    // process ONLY first-person weapon/arm skeletons (42-45 bones, camera-space
    // at origin) — never world bipeds. The biped skeleton lives in the render
    // pool at RVA ~0x468xxxx (found live via camscan; see docs/VRIK-ROADMAP.md).
    // The census + biped probe that proved this are removed.

    // BANK WRITES ARE BANNED (2026-07-15, 03:4x headset result): writing the
    // controller pose into bank record 0 did NOT move the mesh, but it DID
    // bleed the wrist into the body/camera — record 0 propagates into
    // camera_control, which the game reads back to drive the camera. The
    // ApplyControllerToBankRoot helper is intentionally no longer called;
    // kept only as documentation of the falsified lever.
    // BULLET-SPAWN FIX (2026-07-19): the projectile spawn / effect origins
    // read the COMPOSED output, which stayed AUTHORED (head-glued) once the
    // bank write went dormant — bullets emerged at the authored center-screen
    // muzzle, "slightly left and ahead of the gun". Composed output is WORLD
    // space (output[0] = defaultsRoot * record0), so snap ONLY the right-wrist
    // subtree onto the shared controller wrist target. Everything else —
    // especially record 0 and camera_control — is left untouched: rewriting
    // those is the falsified "wrist moves the world" camera feedback.
    bool ApplyControllerToComposedWristSubtree(BoneMatrix* output)
    {
        int slot=-1; unsigned char* weapon=nullptr;
        if (!FindFirstPersonWeapon(output,slot,weapon)) return false;
        const int count=g_fpBoneCount[slot].load(std::memory_order_acquire);
        const int wrist=g_fpWristIndex[slot].load(std::memory_order_acquire);
        const int lWrist=g_fpLWristIndex[slot].load(std::memory_order_acquire);
        const uint64_t mask=g_fpWristDescendants[slot].load(std::memory_order_acquire);
        if (count<=0 || count>64 || wrist<0 || wrist>=count ||
            *reinterpret_cast<int*>(weapon+0x49C)!=count) return false;
        float meshScale=1.0f;
        BoneMatrix desired{},invWrist{},t{};
        // Slot 1 (dual-wield secondary): effect origins belong on the LEFT
        // controller, matching its visible weapon — anchored on the carrier
        // hand bone exactly like the visible seat.
        const bool dual=(slot==1);
        const int transformAnchor=dual?lWrist:wrist;
        if (transformAnchor<0 || transformAnchor>=count) return false;
        if (!DesiredWristWorld(dual,desired,meshScale)) return false;
        if (!InvertBoneMatrix(output[transformAnchor],invWrist) ||
            !ComposeBoneMatrices(desired,invWrist,t)) return false;
        for (int i=0;i<count;++i)
        {
            if (!(mask&(1ull<<i))) continue;
            BoneMatrix moved{};
            if (!ComposeBoneMatrices(t,output[i],moved)) return false;
            output[i]=moved;
        }
        if (meshScale!=1.0f)
        {
            const float* a=desired.translation;
            for (int i=0;i<count;++i)
            {
                if (!(mask&(1ull<<i))) continue;
                for (int r=0;r<3;++r)
                    output[i].translation[r]=a[r]+
                        (output[i].translation[r]-a[r])*meshScale;
                output[i].scale*=meshScale;
            }
        }
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true))
            LOG("M3: composed wrist subtree snapped to the controller "
                "(bullet spawn / effect origins now on the gun)");
        return true;
    }

    void __fastcall ComposeBonesHook(void* model, int start, int count, BoneMatrix* output,
                                     void* source, void* defaults)
    {
        if (!g_insideSpecialCompose)
        {
            g_fpSkipBounds[0]=g_fpSkipBounds[1]=g_fpSkipBounds[2]=g_fpSkipBounds[3]=0;
            ApplyControllerToBankChild(model,output,reinterpret_cast<float*>(source));
        }
        g_origComposeBones(model,start,count,output,source,defaults);
        // This is the last full authored pose before Halo's caller applies the
        // camera-lag transform (camera_control alone is exempt). Preserve that
        // relation for exact render-side lag removal.
        CacheAuthoredFirstPersonAlignment(output,start,count);
        // NOTE (2026-07-18): wiring ApplyControllerToComposedWristSubtree here
        // (the "bullet_snap" experiment) caused the RIGHT HAND to spin
        // uncontrollably and pushed bullets to stage-left — the composed output
        // is downstream of the engine weapon-lag pass and re-snapping the wrist
        // fights it. Reverted to the known-good M3 gun tracking. Bullet origin
        // will be fixed via a weapon-fire hook (origin+direction swap) instead,
        // not by editing composed bones. The call is intentionally gone.
        // 04:17 falsified the output rewrite as a mesh lever for good (43-bone
        // rewrite confirmed running; mesh unmoved; "wrist moves the world" =
        // camera_control feedback). Probe-only again. The defaults-root write
        // is retired for the same reason: both only fed markers + the camera.
        if (!g_insideSpecialCompose && g_config.weapon_probe)
            ApplyControllerToComposedBones(model,output);
    }
    void __fastcall ComposeSpecialBonesHook(void* model, BoneMatrix* output, void* source,
                                            void* defaults, int firstSpecial, int secondSpecial)
    {
        g_fpSkipBounds[0]=g_fpSkipBounds[1]=g_fpSkipBounds[2]=g_fpSkipBounds[3]=0;
        ApplyControllerToBankChild(model,output,reinterpret_cast<float*>(source));
        g_insideSpecialCompose=true;
        g_origComposeSpecialBones(model,output,source,defaults,firstSpecial,secondSpecial);
        g_insideSpecialCompose=false;
        int slot=-1; unsigned char* weapon=nullptr;
        if (FindFirstPersonWeapon(output,slot,weapon))
            CacheAuthoredFirstPersonAlignment(
                output,0,*reinterpret_cast<int*>(weapon+0x49C));
        // (bullet_snap reverted here too — see ComposeBonesHook note.)
        if (g_config.weapon_probe) ApplyControllerToComposedBones(model,output);
    }

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
        // Smooth turn needs a sub-frame timebase. GetTickCount only updates on
        // the ~15.6 ms system tick, but this runs several times per 11 ms (90 Hz)
        // frame from CamCopyHook, so a GetTickCount delta was 0 on most frames
        // and ~15 ms in a lump on others -> a visible ~5 Hz yaw stutter. The
        // performance counter gives the true elapsed time between calls, so the
        // yaw advances evenly regardless of how many calls land in a frame.
        static LARGE_INTEGER freq{}, last{};
        if (freq.QuadPart == 0)
            QueryPerformanceFrequency(&freq);
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        float dt = last.QuadPart == 0 ? 0.0f
                       : (float)(now.QuadPart - last.QuadPart) / (float)freq.QuadPart;
        last = now;
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
        g_lastCamCopyMs.store(GetTickCount64(), std::memory_order_relaxed);
        ChudProbe();
        ApplyMotionBlurSetting();
        ProbeBulletOrigin();
        ApplyBodySetting();
        // M2 tracing: this function copies src+0x68/+0x6C into the compact
        // render camera at dst+0x28/+0x2C. Record only the first few calls so
        // we can distinguish world, weapon, and other camera passes without
        // producing a frame-sized log forever.
        static std::atomic<unsigned> traceCount{0};
        if (src)
        {
            const float srcTanX=*reinterpret_cast<const float*>(
                reinterpret_cast<const char*>(src) + kSrcProjX);
            g_projectionTanX.store(srcTanX);
            g_projectionTanY.store(*reinterpret_cast<const float*>(
                reinterpret_cast<const char*>(src) + kSrcProjY));
            // ZOOM DETECTION (for the weapon scope): Halo narrows the camera's
            // projection tangent when the player zooms. Track the widest
            // (unzoomed) tangent as the baseline; a meaningfully smaller current
            // tangent = zoomed, with magnification = baseline/current. Logged so
            // the scope render can be built on a confirmed signal.
            if (srcTanX>0.2f && srcTanX<3.0f)
            {
                static float baseTan=0.0f;
                if (srcTanX>baseTan) baseTan=srcTanX; // widest seen = hip FOV
                const float factor = (baseTan>1e-3f)? baseTan/srcTanX : 1.0f;
                const bool zoomed = factor>1.15f;
                g_zoomFactor.store(zoomed?factor:1.0f);
                static bool wasZoomed=false;
                if (zoomed!=wasZoomed)
                {
                    wasZoomed=zoomed;
                    LOG("M3 ZOOM: %s (tan %.3f vs base %.3f => %.2fx)",
                        zoomed?"zoomed IN":"unzoomed",srcTanX,baseTan,factor);
                }
            }
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
            if (src)
            {
                // Snapshot the engine-owned locomotion/body origin before
                // ApplyHeadLook adds this frame's headset lean. The visible
                // weapon reads this origin but never writes it.
                const float* p=reinterpret_cast<const float*>(
                    reinterpret_cast<const char*>(src)+kSrcPos);
                g_baseCamX.store(p[0]); g_baseCamY.store(p[1]); g_baseCamZ.store(p[2]);
                g_baseCamValid.store(true,std::memory_order_release);
            }
            // The head pose LIVES in this authoritative camera (the proven M3
            // regime). A 07-15 experiment saved/restored the original values
            // around the copy so gameplay would keep the aim pose — but the
            // first-person bone frame is head-camera-relative, and splitting
            // the two frames made the hand-anchored weapon visibly pick up
            // both head and aim motion. Do not scope this write again.
            ApplyHeadLook(src);
            if (src)
            {
                // Post-head-look camera position (includes leaning): the
                // reference FpRootShim uses to recognize camera-glued FP
                // objects and to place the controller in world space.
                const float* p = reinterpret_cast<const float*>(
                    reinterpret_cast<const char*>(src) + kSrcPos);
                g_camX.store(p[0]); g_camY.store(p[1]); g_camZ.store(p[2]);
                const float* f = reinterpret_cast<const float*>(
                    reinterpret_cast<const char*>(src) + kSrcFwd);
                const float* u = reinterpret_cast<const float*>(
                    reinterpret_cast<const char*>(src) + kSrcUp);
                for (int j = 0; j < 3; ++j)
                { g_camFwd[j].store(f[j]); g_camUp[j].store(u[j]); }
                g_camValid.store(true);

                // Measure the world-up axis from the engine's camera-up (see the
                // g_worldUp declaration). Bootstrap from the first sample, then EMA
                // slowly toward each tick's up — but ONLY while looking roughly
                // level (camera-fwd within ~25 deg of horizontal), so staring up or
                // down a slope for a while can't drag the estimate off vertical.
                {
                    if (!g_worldUpInit.load())
                    {
                        const float l=sqrtf(u[0]*u[0]+u[1]*u[1]+u[2]*u[2]);
                        if (l>1e-4f)
                            for (int j=0;j<3;++j) g_worldUp[j].store(u[j]/l);
                        g_worldUpInit.store(true);
                    }
                    else
                    {
                        float wu[3]={g_worldUp[0].load(),g_worldUp[1].load(),
                                     g_worldUp[2].load()};
                        const float lookLevel=fabsf(f[0]*wu[0]+f[1]*wu[1]+f[2]*wu[2]);
                        if (lookLevel<0.42f)   // forward within ~25 deg of horizontal
                        {
                            const float a=0.01f;
                            for (int j=0;j<3;++j) wu[j]+=(u[j]-wu[j])*a;
                            const float l=sqrtf(wu[0]*wu[0]+wu[1]*wu[1]+wu[2]*wu[2]);
                            if (l>1e-4f)
                                for (int j=0;j<3;++j) g_worldUp[j].store(wu[j]/l);
                        }
                    }
                    static std::atomic<int> wuFrames{0};
                    const int n=wuFrames.fetch_add(1);
                    if (n==600)
                        LOG("M3: measured world-up = (%.3f, %.3f, %.3f)",
                            g_worldUp[0].load(),g_worldUp[1].load(),g_worldUp[2].load());
                }
            }
        }
        else
        {
            g_camValid.store(false);
            g_baseCamValid.store(false);
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

        // STEREO GHOSTING — root cause finally OBSERVED (2026-07-14, the
        // CopyResource probe): between the eye passes, the engine snapshots
        // the full-resolution scene into a sampleable texture
        // (M2 COPY eye=-1, 2912x2100 fmt29 -> fmt29) — its "last frame"
        // source for temporal effects. In stereo that snapshot is made from
        // whichever eye rendered LAST, and BOTH eyes sample it next frame:
        // the last eye reads itself (clean), the first eye reads the other
        // eye (trailing after-images offset by the eye separation). This
        // explains every earlier result: fixed order -> steady first-eye
        // ghost; alternation -> flicker; a discarded warm-up render -> clean
        // (it flushed the foreign snapshot through the effect chain) at the
        // cost of a third render (60 fps).
        //
        // The fix (vr.cpp, VR_Begin/EndRasterEye + the CopyResource hook):
        // keep a per-eye copy of that snapshot — captured after each eye's
        // own render, substituted into the game's snapshot texture right
        // before that eye renders again. Each eye then always samples its own
        // previous frame. Three texture copies per frame instead of a third
        // world render, so this runs at the full two-render rate.
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
        for (int pass = 0; pass < 2; ++pass)
        {
            const int eye = pass == 0 ? firstEye : 1 - firstEye;
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
                float eyeFov[4];
                float halfX=1.07338f,halfY=0.92502f;
                if(VR_GetEyeFov(eye,eyeFov))
                {
                    halfX=fmaxf(-eyeFov[0],eyeFov[1]);
                    halfY=fmaxf(eyeFov[2],-eyeFov[3]);
                }
                float* vrProjection = reinterpret_cast<float*>(
                    reinterpret_cast<char*>(view) + 0x98 + 0x78);
                vrProjection[0]=1.0f/tanf(halfX);
                vrProjection[5]=1.0f/tanf(halfY);
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
                // {view+0x158, view+0x1E8} is a SECOND camera+derived pair on
                // the main view, consumed by the vtable render method 0x28331C
                // via 0x295DC0. It is NOT the first-person pair (that is the
                // sub-view at view+0x6C8: compact +0x6D0, derived +0x8B0 —
                // corrected 2026-07-19). Its exact role is OPEN again; the
                // previous-frame/temporal-reprojection reading from the ghost
                // notes is back on the table. These copies keep it coherent
                // with the current eye camera, as shipped since bd2254e.
                memcpy(reinterpret_cast<char*>(view) + 0x158, camera, 0x90);
                memcpy(reinterpret_cast<char*>(view) + 0x1E8,
                       reinterpret_cast<char*>(view) + 0x98, 0x90);
            }
            // Match the gun/HUD overlay frustum EXACTLY to the widened world
            // raster, otherwise the ~81 deg overlay stretched across the
            // ~123 deg frame magnifies the first-person weapon and HUD ~2x —
            // and any deliberate mismatch would also shift where the
            // hand-anchored weapon projects, breaking controller registration.
            // Written BEFORE the per-view preparation below so the tangents are
            // in place for everything preparation triggers.
            if (const uintptr_t gunCam = g_gunCamera.load())
            {
                float* gunTan = reinterpret_cast<float*>(gunCam + kGunProjX);
                static std::atomic<bool> loggedGunTan{false};
                if (!loggedGunTan.exchange(true))
                    LOG("M2 gun overlay tangents: game (%.4f, %.4f) -> world match (%.4f, %.4f)",
                        gunTan[0], gunTan[1],
                        tanf(g_renderHalfFovX.load()), tanf(g_renderHalfFovY.load()));
                gunTan[0] = tanf(g_renderHalfFovX.load());
                gunTan[1] = tanf(g_renderHalfFovY.load());
                // Experimental HUD sizing: the other three overlay cameras get
                // a scaled frustum (>1 = smaller HUD). Element 0 (the weapon)
                // is never scaled — its projection must match the world for
                // controller registration. Default 1.0 = byte-identical no-op.
                // Elements 1-3 are inactive split-screen player cameras, not
                // independent HUD layers. HUD placement must be solved at the
                // CHUD draw itself; never move this shared camera to the hand.
            }
            // THE DEPTH FIX (2026-07-19). The gun/HUD first-person layer is
            // drawn through the FP camera pair {view+0x158, view+0x1E8}, which
            // the engine builds with a CRUSHED viewmodel depth range (a thin
            // near-far slab so the gun never clips walls on a flat screen). In
            // VR that reads as an orthographic, flattened space: the gun looks
            // squashed, warps when twisted, and cannot move forward — the
            // user's exact report. The fix is to render the FP layer through
            // this eye's FULL WORLD camera + projection (position, orientation,
            // FOV AND depth terms), so the weapon lives in true world
            // perspective. The authoritative override happens during the
            // render in FpCameraRebuildHook (the engine rebuilds this pair
            // mid-render); this pre-render copy keeps it coherent beforehand.
            memcpy(reinterpret_cast<char*>(view) + 0x158, camera, 0x90);
            memcpy(reinterpret_cast<char*>(view) + 0x1E8,
                   reinterpret_cast<char*>(view) + 0x98, 0x90);
            if (g_fpCameraUpload)
                g_fpCameraUpload(reinterpret_cast<char*>(view) + 0x158,
                                 reinterpret_cast<char*>(view) + 0x1E8);
            // Snapshot this eye's finished camera + derived block and ARM the
            // FP hooks BEFORE the per-view preparation: the measured in-eye FP
            // driver runs (~3 per eye per frame, exactly-equal histogram
            // buckets) are triggered BY g_prepareView below — arming after it
            // meant every stamp silently missed (2026-07-19 evening logs).
            memcpy(g_eyeCompactCamera, camera, sizeof(g_eyeCompactCamera));
            memcpy(g_eyeDerivedBlock, reinterpret_cast<char*>(view) + 0x98,
                   sizeof(g_eyeDerivedBlock));
            g_eyeFpView.store(view, std::memory_order_release);
            // The draw routine consumes camera state uploaded to engine globals
            // by this per-view preparation stage, not the view structure
            // directly. Re-run it after each eye matrix rebuild.
            if (g_prepareView)
                g_prepareView(view, 0);
            g_origRenderView(view);
            g_eyeFpView.store(nullptr, std::memory_order_release);
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
    // Bone composition boundaries called by the first-person animator at
    // 0x2C4663/0x2C4633. The render packet copy happens immediately afterward.
    const char* kComposeBonesSig =
        "45 85 C0 0F 8E ?? ?? ?? ?? 48 89 5C 24 08 57 48 83 EC 20 45 8B D0 49 8B F9 4C 8B C9";
    const char* kComposeSpecialBonesSig =
        "48 8B C4 48 89 58 08 48 89 70 10 48 89 78 18 4C 89 60 20 55 41 55 41 56 48 8D 68 B8";
    const char* kFpInterpolateSig =
        "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 54 41 55 41 56 41 57 48 83 EC 20 33 DB 49 63 E8";
    // Final visible first-person skin palette consumer. Unlike 0x2C13B8
    // (marker/effect packets), this function maps interpolated bones into the
    // actual render palette and receives the exact root as argument 2.
    const char* kFpVisiblePaletteSig =
        "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 48 83 EC 20 48 8B 05 ?? ?? ?? ?? 49 8B F0 0F B7 C9 4C 8B F2";
    // First-person camera rebuild (0x279BEC in build 1.3528). Re-copies the FP
    // compact camera from [view+0x2A8] (center pose), forces the viewmodel FOV,
    // derives view+0x1E8, then tail-jumps into the constant uploader. Hooked so
    // each eye render substitutes its own world camera afterwards.
    const char* kFpCameraRebuildSig =
        "48 8B C4 48 89 58 08 48 89 70 10 57 48 83 EC 50 48 8D 79 08 0F 29 78 E8 F3 0F 10 3D ?? ?? ?? ?? 48 8B F1";
    // The uploader that rebuild tail-jumps into (0x2770F0):
    // fastcall(compactCamera, derivedBlock). The hook calls it again after the
    // substitution so the constants the original already pushed are redone.
    const char* kFpCameraUploadSig =
        "48 8B C4 48 89 58 08 48 89 70 10 48 89 78 18 55 48 8D 68 A1 48 81 EC C0 00 00 00 0F 29 70 E8 48 8B FA F3 0F 10 35 ?? ?? ?? ?? 48 8D 55 B7";

    // Native pause state owner. The HaloScript external global named
    // game_paused is only a developer override and does not change when MCC's
    // real pause menu opens. Four alternating live snapshots plus a 2 ms
    // transition trace identified the engine flag written by this unique code
    // path: it changes before MCC's generic game-thread suspension flags on
    // both entry and exit. The final mov's RIP target is the pause byte.
    const char* kNativePauseOwnerSig =
        "E8 ?? ?? ?? ?? 84 C0 74 18 B9 03 00 00 00 "
        "E8 ?? ?? ?? ?? 84 C0 75 0A 8B D1 40 8A CE "
        "E8 ?? ?? ?? ?? 40 88 35 ?? ?? ?? ?? E9 ?? ?? ?? ??";

    void LocateNativePauseFlag(uintptr_t base, size_t size)
    {
        const uintptr_t hit = sig::Find(base, size, kNativePauseOwnerSig);
        if (!hit || sig::Find(hit + 1, base + size - hit - 1,
                              kNativePauseOwnerSig))
        {
            g_nativePauseFlag = 0;
            g_enginePauseValidated = false;
            LOG("pause state: native pause signature missing or ambiguous; "
                "using transition fallback");
            return;
        }

        // Signature starts at halo3.dll+0xB682 in build 1.3528. The target
        // instruction begins at +33, its disp32 is +36, and RIP after it is
        // +40. Resolve rather than retaining the observed +0xA3CA9A offset.
        const int32_t displacement =
            *reinterpret_cast<const int32_t*>(hit + 36);
        const uintptr_t flag = hit + 40 + displacement;
        if (flag < base || flag >= base + size)
        {
            g_nativePauseFlag = 0;
            g_enginePauseValidated = false;
            LOG("pause state: native pause signature resolved outside halo3.dll; "
                "using transition fallback");
            return;
        }

        const uint8_t initial = *reinterpret_cast<const uint8_t*>(flag);
        if (initial > 1)
        {
            g_nativePauseFlag = 0;
            g_enginePauseValidated = false;
            LOG("pause state: native pause flag failed boolean validation (%u); "
                "using transition fallback", static_cast<unsigned>(initial));
            return;
        }

        g_nativePauseFlag = flag;
        g_enginePauseValidated = true;
        LOG("pause state: authoritative native flag at halo3.dll+0x%llX "
            "(initial=%u, owner signature +0x%llX)",
            (unsigned long long)(flag - base), static_cast<unsigned>(initial),
            (unsigned long long)(hit - base));
    }

    bool ReadEnginePaused(bool& paused)
    {
        const uintptr_t flag = g_nativePauseFlag.load(std::memory_order_acquire);
        if (!flag)
            return false;
        __try
        {
            const uint8_t value = *reinterpret_cast<const uint8_t*>(flag);
            if (value > 1)
                return false;
            paused = value != 0;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    void InstallHook(uintptr_t base, size_t size)
    {
        LocateNativePauseFlag(base, size);
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

        uintptr_t composeHit=sig::Find(base,size,kComposeBonesSig);
        uintptr_t specialHit=sig::Find(base,size,kComposeSpecialBonesSig);
        if (composeHit && specialHit &&
            !sig::Find(composeHit+1,base+size-composeHit-1,kComposeBonesSig) &&
            !sig::Find(specialHit+1,base+size-specialHit-1,kComposeSpecialBonesSig))
        {
            // Both compositor callers and the allocator use the engine TLS
            // index global at halo3.dll+0xA39F9C. Resolve it through the proven
            // first-person update signature instead of baking that RVA.
            uintptr_t fp=sig::Find(base,size,
                "48 8B C4 44 88 40 18 89 50 10 89 48 08 55 53 56 57 41 54 41 55 41 56 41 57");
            if (fp)
            {
                const int32_t tlsDisp=*reinterpret_cast<const int32_t*>(fp+0x62);
                g_engineTlsIndex=reinterpret_cast<uint32_t*>(fp+0x66+tlsDisp);
            }
            // composeHit+0x50 is mov rax,[rip+tag-data-base]; its displacement
            // resolves the node-record storage used for names and parents.
            const int32_t tagDataDisp=*reinterpret_cast<const int32_t*>(composeHit+0x53);
            g_animationTagData=reinterpret_cast<unsigned char**>(composeHit+0x57+tagDataDisp);
            if (g_engineTlsIndex && g_animationTagData &&
                MH_CreateHook(reinterpret_cast<void*>(composeHit),reinterpret_cast<void*>(&ComposeBonesHook),
                              reinterpret_cast<void**>(&g_origComposeBones)) == MH_OK &&
                MH_CreateHook(reinterpret_cast<void*>(specialHit),reinterpret_cast<void*>(&ComposeSpecialBonesHook),
                              reinterpret_cast<void**>(&g_origComposeSpecialBones)) == MH_OK &&
                MH_EnableHook(reinterpret_cast<void*>(composeHit)) == MH_OK &&
                MH_EnableHook(reinterpret_cast<void*>(specialHit)) == MH_OK)
                LOG("M3: first-person marker/muzzle matrix hooks installed at halo3.dll+0x%llX/+0x%llX",
                    (unsigned long long)(composeHit-base),(unsigned long long)(specialHit-base));
            else
                LOG("M3: FAILED to hook first-person hand/weapon matrix composers");
        }
        else
            LOG("M3: first-person hand/weapon matrix signatures missing or ambiguous");

        uintptr_t interpolateHit=sig::Find(base,size,kFpInterpolateSig);
        uintptr_t visiblePaletteHit=sig::Find(base,size,kFpVisiblePaletteSig);
        const bool uniqueInterpolate=interpolateHit &&
            !sig::Find(interpolateHit+1,base+size-interpolateHit-1,kFpInterpolateSig);
        const bool uniqueVisiblePalette=visiblePaletteHit &&
            !sig::Find(visiblePaletteHit+1,base+size-visiblePaletteHit-1,
                       kFpVisiblePaletteSig);
        bool visiblePathOk=false;
        if (uniqueInterpolate && uniqueVisiblePalette)
        {
            const MH_STATUS createInterpolate=MH_CreateHook(
                reinterpret_cast<void*>(interpolateHit),
                reinterpret_cast<void*>(&FpInterpolateHook),
                reinterpret_cast<void**>(&g_origFpInterpolate));
            const MH_STATUS createPalette=MH_CreateHook(
                reinterpret_cast<void*>(visiblePaletteHit),
                reinterpret_cast<void*>(&FpVisiblePaletteHook),
                reinterpret_cast<void**>(&g_origFpVisiblePalette));
            if (createInterpolate==MH_OK && createPalette==MH_OK)
                visiblePathOk=
                    MH_EnableHook(reinterpret_cast<void*>(interpolateHit))==MH_OK &&
                    MH_EnableHook(reinterpret_cast<void*>(visiblePaletteHit))==MH_OK;
            if (!visiblePathOk)
            {
                MH_DisableHook(reinterpret_cast<void*>(interpolateHit));
                MH_DisableHook(reinterpret_cast<void*>(visiblePaletteHit));
                MH_RemoveHook(reinterpret_cast<void*>(interpolateHit));
                MH_RemoveHook(reinterpret_cast<void*>(visiblePaletteHit));
                g_origFpInterpolate=nullptr;
                g_origFpVisiblePalette=nullptr;
            }
        }
        if (visiblePathOk)
        {
            g_fpInterpolatorHooked.store(true,std::memory_order_release);
            LOG("M3: visible FP reconstruction hooked at halo3.dll+0x%llX -> +0x%llX "
                "(interpolation identity + final palette root; gun/arms only)",
                (unsigned long long)(interpolateHit-base),
                (unsigned long long)(visiblePaletteHit-base));
        }
        else
            LOG("M3: complete visible FP path missing/ambiguous or hook failed; using sim fallback");

        // Native first-person weapon IK is a flat-screen support-hand system:
        // the animation graph can attach the arm's marker to a marker on the
        // weapon. H3EK proves the shotgun enables exactly
        // left_hand -> left_hand, with the target marker parented to pump node
        // 4; the AR's corresponding weapon-IK block is empty. Our controller
        // solver must own that arm instead. Redirect the one unique decision
        // to Halo's EXISTING no-weapon-IK state (mode 1/3), before any level is
        // evaluated. This is a two-byte startup patch, not a per-frame detour,
        // and preserves all authored fire/reload/melee animation channels.
        {
            const char* kFpNativeWeaponIkDecisionSig =
                "40 84 ED 74 05 45 84 FF 75 04 84 DB 74 0F BA 03 00 00 00 "
                "41 0F 28 D8 44 8D 42 FF EB 11";
            uintptr_t decision=sig::Find(base,size,kFpNativeWeaponIkDecisionSig);
            const bool uniqueDecision=decision &&
                !sig::Find(decision+1,base+size-decision-1,
                           kFpNativeWeaponIkDecisionSig);
            if (uniqueDecision)
            {
                unsigned char* branch=reinterpret_cast<unsigned char*>(decision+3);
                if (branch[0]==0x74 && branch[1]==0x05)
                {
                    DWORD oldProtect=0;
                    if (VirtualProtect(branch,2,PAGE_EXECUTE_READWRITE,&oldProtect))
                    {
                        // JE +5 (conditional native-IK decision) -> JMP +0x18
                        // (the stock no-weapon-IK branch at halo3+0x2C3959).
                        branch[0]=0xEB;
                        branch[1]=0x18;
                        FlushInstructionCache(GetCurrentProcess(),branch,2);
                        DWORD ignored=0;
                        VirtualProtect(branch,2,oldProtect,&ignored);
                        LOG("M3 VRIK: native first-person weapon IK disabled at "
                            "halo3.dll+0x%llX; controller solver owns support arms",
                            (unsigned long long)(decision-base));
                    }
                    else
                        LOG("M3 VRIK: native weapon-IK branch protection failed; "
                            "shotgun support hand may stay attached to pump");
                }
                else
                    LOG("M3 VRIK: native weapon-IK branch bytes changed; patch skipped");
            }
            else
                LOG("M3 VRIK: native weapon-IK decision missing/ambiguous; patch skipped");
        }

        // The CHUD steal-and-requad machinery is GONE (2026-07-18): its three
        // CHUD hooks + draw-call classifier removed the native HUD from both
        // eyes, never displayed the hand quad, and its retry loop cost ~30 fps.
        // The native HUD renders untouched again; only the stock crosshair is
        // suppressed (our floating reticle replaces it).
        uintptr_t fpCamHit=sig::Find(base,size,kFpCameraRebuildSig);
        uintptr_t fpUploadHit=sig::Find(base,size,kFpCameraUploadSig);
        const bool uniqueFpCam=fpCamHit &&
            !sig::Find(fpCamHit+1,base+size-fpCamHit-1,kFpCameraRebuildSig);
        const bool uniqueFpUpload=fpUploadHit &&
            !sig::Find(fpUploadHit+1,base+size-fpUploadHit-1,kFpCameraUploadSig);
        bool fpCameraOk=false;
        if (uniqueFpCam && uniqueFpUpload)
        {
            if (MH_CreateHook(reinterpret_cast<void*>(fpCamHit),
                              reinterpret_cast<void*>(&FpCameraRebuildHook),
                              reinterpret_cast<void**>(&g_origFpCameraRebuild))==MH_OK)
            {
                fpCameraOk=MH_EnableHook(reinterpret_cast<void*>(fpCamHit))==MH_OK;
                if (!fpCameraOk)
                {
                    MH_RemoveHook(reinterpret_cast<void*>(fpCamHit));
                    g_origFpCameraRebuild=nullptr;
                }
            }
        }
        if (fpCameraOk)
        {
            g_fpCameraUpload=reinterpret_cast<FpCameraUploadFn>(fpUploadHit);
            LOG("M3: FP camera rebuild hooked at halo3.dll+0x%llX (uploader +0x%llX); "
                "gun/HUD layer renders per-eye",
                (unsigned long long)(fpCamHit-base),
                (unsigned long long)(fpUploadHit-base));
        }
        else
            LOG("M3: FP camera rebuild signature missing/ambiguous; gun/HUD stay a mono flat layer");

        // GAME BRIGHTNESS: hook 0x278EE0 (once thought to size the HUD; the
        // headset proved it drives brightness). See HudXformHook — it scales the
        // two screen color/gamma floats by game_brightness. MinHook on the
        // function installs reliably. Prologue verified unique on disk (push rbp;
        // mov rbp,rsp; sub rsp,0x50; save xmm6/xmm7; the movaps arg shuffle).
        {
            const char* kHudXformSig =
                "40 55 48 8B EC 48 83 EC 50 0F 29 74 24 40 0F 28 F1 "
                "0F 29 7C 24 30 0F 28 CA 0F 28 F8";
            uintptr_t hudXform = sig::Find(base, size, kHudXformSig);
            if (hudXform && !sig::Find(hudXform+1, base+size-hudXform-1, kHudXformSig))
            {
                if (MH_CreateHook(reinterpret_cast<void*>(hudXform),
                                  reinterpret_cast<void*>(&HudXformHook),
                                  reinterpret_cast<void**>(&g_realHudXform)) == MH_OK &&
                    MH_EnableHook(reinterpret_cast<void*>(hudXform)) == MH_OK)
                    LOG("M3: brightness hook installed at halo3.dll+0x%llX "
                        "(game_brightness)",
                        (unsigned long long)(hudXform - base));
                else
                    LOG("M3: brightness MH_CreateHook failed; brightness fixed at 1.0");
            }
            else
                LOG("M3: brightness signature missing/ambiguous; brightness fixed at 1.0");
        }

        // HUD CROSSHAIR CLASS HIDER. H3EK and ManagedDonkey agree that class 2
        // is the authoritative crosshair marker. chud_draw_widget already checks
        // that class, but game_is_playback short-circuits the check during normal
        // play. NOP only that short-circuit and hook the existing class-gated
        // predicate; no tag-table reads are added to the render hook.
        {
            const char* kHudElemSig =
                "48 89 5C 24 10 57 48 83 EC 50 48 8B 05 ?? ?? ?? ?? 41 8A F9 45 0F B7 C0";
            uintptr_t hudElem = sig::Find(base, size, kHudElemSig);
            bool uniqueHudElem = hudElem != 0;
            if (uniqueHudElem)
                uniqueHudElem = sig::Find(hudElem + 1, base + size - hudElem - 1,
                                          kHudElemSig) == 0;
            if (uniqueHudElem)
            {
                const unsigned char expectedClassGate[] = {
                    0x74, 0x17, 0xB8, 0x02, 0x00, 0x00, 0x00,
                    0x66, 0x41, 0x3B, 0x42, 0x04, 0x75, 0x0B,
                    0x8B, 0xCB, 0xE8
                };
                unsigned char* classGate =
                    reinterpret_cast<unsigned char*>(hudElem + 0x84);
                bool layoutMatches =
                    memcmp(classGate, expectedClassGate,
                           sizeof(expectedClassGate)) == 0;
                if (reinterpret_cast<unsigned char*>(hudElem)[0x7D] != 0xE8)
                    layoutMatches = false;
                if (layoutMatches)
                {
                    const uintptr_t playbackTarget =
                        sig::RipTarget(hudElem + 0x7E, hudElem + 0x82);
                    const uintptr_t visibleTarget =
                        sig::RipTarget(hudElem + 0x95, hudElem + 0x99);
                    bool targetsInModule = playbackTarget >= base;
                    if (playbackTarget >= base + size)
                        targetsInModule = false;
                    if (visibleTarget < base)
                        targetsInModule = false;
                    if (visibleTarget >= base + size)
                        targetsInModule = false;
                    if (targetsInModule)
                    {
                        g_gameIsPlayback =
                            reinterpret_cast<GameIsPlaybackFn>(playbackTarget);
                        const MH_STATUS createStatus = MH_CreateHook(
                            reinterpret_cast<void*>(visibleTarget),
                            reinterpret_cast<void*>(&HudCrosshairVisibleHook),
                            reinterpret_cast<void**>(&g_realHudCrosshairVisible));
                        bool hookReady = createStatus == MH_OK;
                        if (hookReady)
                            hookReady = MH_EnableHook(
                                reinterpret_cast<void*>(visibleTarget)) == MH_OK;
                        if (hookReady)
                        {
                            DWORD oldProtect = 0;
                            if (VirtualProtect(classGate, 2, PAGE_EXECUTE_READWRITE,
                                               &oldProtect))
                            {
                                classGate[0] = 0x90;
                                classGate[1] = 0x90;
                                FlushInstructionCache(GetCurrentProcess(),
                                                      classGate, 2);
                                DWORD ignored = 0;
                                VirtualProtect(classGate, 2, oldProtect, &ignored);
                                LOG("M3: native CHUD crosshair class hider active "
                                    "at halo3.dll+0x%llX",
                                    (unsigned long long)(hudElem - base));
                            }
                            else
                            {
                                MH_DisableHook(reinterpret_cast<void*>(visibleTarget));
                                MH_RemoveHook(reinterpret_cast<void*>(visibleTarget));
                                LOG("M3: CHUD class gate protection failed; "
                                    "game reticle stays visible");
                            }
                        }
                        else
                        {
                            if (createStatus == MH_OK)
                                MH_RemoveHook(reinterpret_cast<void*>(visibleTarget));
                            LOG("M3: CHUD crosshair predicate hook failed; "
                                "game reticle stays visible");
                        }
                    }
                    else
                        LOG("M3: CHUD class targets outside halo3.dll; "
                            "game reticle stays visible");
                }
                else
                    LOG("M3: CHUD class-gate layout mismatch; "
                        "game reticle stays visible");
            }
            else
                LOG("M3: HUD element signature missing/ambiguous; "
                    "game reticle stays visible");
        }

        // (0x2EEFC8 placement hook removed — measured: no coordinates there.)

        // DO NOT HOOK halo3+0x120DF8. Tried 2026-07-15: it crashes the game on
        // level load, on contact, even as a pure pass-through (proven — the
        // skip range was never armed, the unconditional probe log never
        // printed, and it still died). Surviving the menus proves nothing:
        // halo3.dll's model pipeline does not run there, so the first real call
        // IS the level load. The weapon-lag diagnosis in RE-notes stands; the
        // mechanism for acting on it must not be a detour on this function.

        // FP mesh re-anchor: patch the single root-fetch call inside the object
        // node recomposer (see FpRootShim). lea rdx,[rsp+20]; mov ecx,ebx;
        // call <root>; then the 0x1205AC multiply — unique on disk, verified.
        const char* kFpRootCallSig =
            "48 8D 54 24 20 8B CB E8 ?? ?? ?? ?? 4D 8B C4 48 8D 4C 24 20 49 8B D7 E8";
        uintptr_t callSite = sig::Find(base, size, kFpRootCallSig);
        if (callSite &&
            !sig::Find(callSite+1, base+size-callSite-1, kFpRootCallSig))
        {
            const uintptr_t callInstr = callSite + 7;    // the E8
            const uintptr_t relAt = callInstr + 1;       // 4-byte aligned disp32
            const int32_t origRel = *reinterpret_cast<const int32_t*>(relAt);
            g_realFpRoot = reinterpret_cast<FpRootFn>(callInstr + 5 + origRel);
            // 12-byte trampoline (mov rax, imm64; jmp rax) within rel32 range
            // of the call site, since our DLL may sit >2GB away.
            unsigned char* tramp = nullptr;
            for (uintptr_t probe = callInstr & ~0xFFFFull;
                 probe > callInstr - 0x40000000ull && !tramp; probe -= 0x100000)
                tramp = static_cast<unsigned char*>(VirtualAlloc(
                    reinterpret_cast<void*>(probe), 0x1000,
                    MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
            const intptr_t newRel = tramp
                ? reinterpret_cast<intptr_t>(tramp) - static_cast<intptr_t>(callInstr + 5) : INT64_MAX;
            if (tramp && newRel >= INT32_MIN && newRel <= INT32_MAX && (relAt & 3) == 0)
            {
                tramp[0]=0x48; tramp[1]=0xB8;                       // mov rax, imm64
                *reinterpret_cast<void**>(tramp+2) =
                    reinterpret_cast<void*>(&FpRootShim);
                tramp[10]=0xFF; tramp[11]=0xE0;                     // jmp rax
                DWORD old;
                if (VirtualProtect(reinterpret_cast<void*>(relAt), 4,
                                   PAGE_EXECUTE_READWRITE, &old))
                {
                    InterlockedExchange(reinterpret_cast<volatile LONG*>(relAt),
                                        static_cast<LONG>(newRel));
                    VirtualProtect(reinterpret_cast<void*>(relAt), 4, old, &old);
                    FlushInstructionCache(GetCurrentProcess(),
                                          reinterpret_cast<void*>(callInstr), 5);
                    LOG("M3: FP mesh root call-site patched at halo3.dll+0x%llX "
                        "(real getter halo3.dll+0x%llX; atomic disp32 swap)",
                        (unsigned long long)(callInstr-base),
                        (unsigned long long)(reinterpret_cast<uintptr_t>(g_realFpRoot)-base));
                }
                else
                    LOG("M3: FP mesh call-site VirtualProtect failed; gun stays camera-glued");
            }
            else
                LOG("M3: FP mesh trampoline allocation failed (rel %lld); gun stays camera-glued",
                    (long long)newRel);
        }
        else
            LOG("VRIK: object-root call-site signature missing/ambiguous; A2 probe unavailable");

        // Second call-site patch: the camera pitch/turn rotation applied to
        // every FP bone but camera_control (the head-glue; see the emitted
        // rax-only shim below and the LTCG note at g_fpSkipBounds).
        const char* kSwayCallSig = "44 3B 8F A4 11 00 00 74 0C 49 8B D0 48 8D 4D C8 E8";
        uintptr_t swaySite = sig::Find(base, size, kSwayCallSig);
        if (!g_fpInterpolatorHooked.load() && swaySite &&
            !sig::Find(swaySite+1, base+size-swaySite-1, kSwayCallSig))
        {
            const uintptr_t callInstr = swaySite + 16;   // the E8
            const uintptr_t relAt = callInstr + 1;       // 4-byte aligned disp32
            const int32_t origRel = *reinterpret_cast<const int32_t*>(relAt);
            g_realSwayApply = reinterpret_cast<SwayApplyFn>(callInstr + 5 + origRel);
            unsigned char* tramp = nullptr;
            for (uintptr_t probe = callInstr & ~0xFFFFull;
                 probe > callInstr - 0x40000000ull && !tramp; probe -= 0x100000)
                tramp = static_cast<unsigned char*>(VirtualAlloc(
                    reinterpret_cast<void*>(probe), 0x1000,
                    MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
            const intptr_t newRel = tramp
                ? reinterpret_cast<intptr_t>(tramp) - static_cast<intptr_t>(callInstr + 5) : INT64_MAX;
            if (tramp && newRel >= INT32_MIN && newRel <= INT32_MAX && (relAt & 3) == 0)
            {
                // Hand-assembled shim, clobbers ONLY rax (the caller keeps its
                // loop state in r8/r9/r10 across this call — LTCG contract; a
                // compiled C++ shim here IS the fatal-error bug). Layout:
                //   [0x00] mov rax,[lo0]; cmp rdx,rax; jb +0x0F (-> lo1 test)
                //   [0x0F] mov rax,[hi0]; cmp rdx,rax; jb +0x2A (-> ret)
                //   [0x1E] mov rax,[lo1]; cmp rdx,rax; jb +0x0F (-> tail)
                //   [0x2D] mov rax,[hi1]; cmp rdx,rax; jb +0x0C (-> ret)
                //   [0x3C] mov rax, real; jmp rax
                //   [0x48] ret
                unsigned char shim[0x49];
                int o = 0;
                auto movRaxAbs = [&](volatile uintptr_t* a) {
                    shim[o++]=0x48; shim[o++]=0xA1;         // mov rax, moffs64
                    const void* p = const_cast<uintptr_t*>(a);
                    memcpy(shim+o, &p, 8); o += 8;
                };
                auto cmpJb = [&](unsigned char disp) {
                    shim[o++]=0x48; shim[o++]=0x39; shim[o++]=0xC2; // cmp rdx, rax
                    shim[o++]=0x72; shim[o++]=disp;                 // jb rel8
                };
                movRaxAbs(&g_fpSkipBounds[0]); cmpJb(0x0F);
                movRaxAbs(&g_fpSkipBounds[1]); cmpJb(0x2A);
                movRaxAbs(&g_fpSkipBounds[2]); cmpJb(0x0F);
                movRaxAbs(&g_fpSkipBounds[3]); cmpJb(0x0C);
                shim[o++]=0x48; shim[o++]=0xB8;             // mov rax, imm64
                const void* real = reinterpret_cast<const void*>(g_realSwayApply);
                memcpy(shim+o, &real, 8); o += 8;
                shim[o++]=0xFF; shim[o++]=0xE0;             // jmp rax
                shim[o++]=0xC3;                             // ret (skip path)
                memcpy(tramp, shim, o);
                DWORD old;
                if (VirtualProtect(reinterpret_cast<void*>(relAt), 4,
                                   PAGE_EXECUTE_READWRITE, &old))
                {
                    InterlockedExchange(reinterpret_cast<volatile LONG*>(relAt),
                                        static_cast<LONG>(newRel));
                    VirtualProtect(reinterpret_cast<void*>(relAt), 4, old, &old);
                    FlushInstructionCache(GetCurrentProcess(),
                                          reinterpret_cast<void*>(callInstr), 5);
                    LOG("M3: camera pitch/turn call-site patched at halo3.dll+0x%llX "
                        "(rotator halo3.dll+0x%llX)",
                        (unsigned long long)(callInstr-base),
                        (unsigned long long)(reinterpret_cast<uintptr_t>(g_realSwayApply)-base));
                }
                else
                    LOG("M3: pitch/turn call-site VirtualProtect failed; gun stays head-rotated");
            }
            else
                LOG("M3: pitch/turn trampoline allocation failed; gun stays head-rotated");
        }
        else if (!g_fpInterpolatorHooked.load())
            LOG("M3: pitch/turn call-site signature missing/ambiguous; gun stays head-rotated");

        ResolveMotionBlurVars(base, size);
        ResolveBodyVars(base, size);
        DumpHudDebugVars(base, size);

        // (CHUD visibility-snapshot hook removed 2026-07-19 evening — its forced
        // byte writes used a disproven offset map and suppressed the HUD. The
        // reticle kill uses Halo's class-gated path inside 0x2EDF24 only.)

        // RECONSTRUCTION Phase 0: hook the engine's FP render driver and
        // resolve the guard global that gates its first in-window call site.
        // Driver prologue (0x2835D4 in 1.3528); guard cmp site (0x28599D).
        const char* kFpDriverSig =
            "48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 48 89 78 20 41 55 41 56 41 57 "
            "48 83 EC 20 48 8B D9 40 8A F2 8B 89 F4 27 00 00";
        const char* kFpDriverGuardSig = "44 39 2D ?? ?? ?? ?? 75 0A 33 D2 48 8B CF E8";
        uintptr_t driverHit = sig::Find(base, size, kFpDriverSig);
        uintptr_t guardHit = sig::Find(base, size, kFpDriverGuardSig);
        if (guardHit && !sig::Find(guardHit+1, base+size-guardHit-1, kFpDriverGuardSig))
        {
            const int32_t disp = *reinterpret_cast<const int32_t*>(guardHit + 3);
            g_fpDriverGuard = reinterpret_cast<int32_t*>(guardHit + 7 + disp);
            LOG("P0: FP driver guard global at halo3.dll+0x%llX (value now %d)",
                (unsigned long long)(reinterpret_cast<uintptr_t>(g_fpDriverGuard) - base),
                *g_fpDriverGuard);
        }
        else
            LOG("P0: FP driver guard site missing/ambiguous");
        if (driverHit && !sig::Find(driverHit+1, base+size-driverHit-1, kFpDriverSig))
        {
            if (MH_CreateHook(reinterpret_cast<void*>(driverHit),
                              reinterpret_cast<void*>(&FpDriverHook),
                              reinterpret_cast<void**>(&g_origFpDriver)) == MH_OK &&
                MH_EnableHook(reinterpret_cast<void*>(driverHit)) == MH_OK)
                LOG("P0: FP driver hooked at halo3.dll+0x%llX",
                    (unsigned long long)(driverHit - base));
            else
                LOG("P0: FP driver hook FAILED");
        }
        else
            LOG("P0: FP driver signature missing/ambiguous");

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
            const TitleDescriptor* activeTitle = TitleAdapter_PollLoaded();
            if (!gameHooked && activeTitle && activeTitle->title == GameTitle::Halo3)
            {
                uintptr_t base = 0;
                size_t size = 0;
                if (sig::ModuleRange(activeTitle->moduleName, base, size))
                {
                    LOG("%ls loaded at %p, size 0x%zX",
                        activeTitle->moduleName, (void*)base, size);
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
bool Game_HasAuthoritativePauseState() { return g_enginePauseValidated.load(); }

// HUD size (F1 menu): manual rescan + status. The scan normally starts itself
// (HudSizeAutoTick) whenever hud_size is non-stock and no slots are located.
void Game_LocateHudSafeFrames()
{
    LaunchSafeFrameScan("manual rescan from the menu");
}

void Game_GetHudSafeFrameStatus(int& matches, bool& scanning)
{
    const int c = g_safeFrameHitCount.load(std::memory_order_acquire);
    scanning = (c == -2);
    matches = (c > 0) ? c : 0;
}

namespace { std::atomic<bool> g_autoVrUserVeto{false}; std::atomic<bool> g_autoVrOwned{false}; }

void Game_ToggleHeadTracking()
{
    const bool on = !g_enabled.load();
    g_enabled = on;
    if (on)
        g_needRecenter = true;
    else
        g_autoVrUserVeto = true; // user turned VR off by hand; don't auto re-arm
    LOG("head tracking %s", on ? "ON" : "OFF");
}

// Called every frame from VR_OnPresent. Turns head tracking + stereo ON shortly
// after a level starts driving the camera, and back OFF when you return to the
// menu — so the mod behaves like a normal VR game (no F2/F11). Manual F2 off
// while in a level vetoes auto-arm until the next level load; F2/F11 still work.
void Game_AutoVrTick()
{
    HudSizeAutoTick(); // HUD size: (re)locate the tag slots when needed
    // Render-thread diagnostics are reported here, on Present. Log only a
    // stable state transition; never log from the palette or HUD hot hooks.
    {
        static int loggedSide=-2;
        static const char* loggedWhy=reinterpret_cast<const char*>(1);
        static uint64_t lastLogMs=0;
        const int side=g_armFailureSide.load(std::memory_order_acquire);
        const char* why=g_armFailurePublished.load(std::memory_order_relaxed);
        const uint64_t diagNow=GetTickCount64();
        if ((side!=loggedSide || why!=loggedWhy) && diagNow-lastLogMs>=500)
        {
            loggedSide=side; loggedWhy=why; lastLogMs=diagNow;
            if (side==0)
                LOG("M3 VRIK SAFE-DIAG: both arms applied to controllers");
            else if (side==1)
                LOG("M3 VRIK SAFE-DIAG: right-arm solve fell back (%s); authored "
                    "support hand remains on weapon",why?why:"pre-solve");
            else if (side==2)
                LOG("M3 VRIK SAFE-DIAG: left arm not applied (%s)",
                    why?why:"pre-solve");
        }
    }
    {
        static int loggedCount=0;
        int available=g_fpBoneMapSnapshotCount.load(std::memory_order_acquire);
        if(available>16) available=16;
        while(loggedCount<available)
        {
            auto& snap=g_fpBoneMapSnapshots[loggedCount];
            const uint32_t begin=snap.sequence.load(std::memory_order_acquire);
            if((begin&1) || begin==0) break;
            const uint64_t key=snap.skeletonKey.load(std::memory_order_relaxed);
            const uint32_t tag=snap.tag.load(std::memory_order_relaxed);
            const int count=snap.count.load(std::memory_order_relaxed);
            const int reconstructed=snap.reconstructed.load(std::memory_order_relaxed);
            int32_t map[64]{};
            for(int i=0;i<count && i<64;++i)
                map[i]=snap.map[i].load(std::memory_order_relaxed);
            const uint32_t end=snap.sequence.load(std::memory_order_acquire);
            if(begin!=end) break;
            int shoulderDest=-1,elbowDest=-1,wristDest=-1;
            for(int i=0;i<count && i<64;++i)
            {
                if(map[i]==1) shoulderDest=i;
                if(map[i]==3) elbowDest=i;
                if(map[i]==5) wristDest=i;
            }
            LOG("M3 VRIK PALETTE #%d: skeleton %016llX tag 0x%04X "
                "reconstructed=%d count=%d; left 1/3/5 -> %d/%d/%d",
                loggedCount,static_cast<unsigned long long>(key),tag,
                reconstructed,count,shoulderDest,elbowDest,wristDest);
            for(int from=0;from<count;from+=16)
            {
                char line[512]; int pos=0;
                const int to=(from+16<count)?from+16:count;
                for(int i=from;i<to && pos<(int)sizeof(line)-24;++i)
                    pos+=snprintf(line+pos,sizeof(line)-pos,"%d=%d ",i,map[i]);
                LOG("M3 VRIK PALETTE #%d MAP[%d..%d]: %s",
                    loggedCount,from,to-1,line);
            }
            ++loggedCount;
        }
    }
    const uint64_t now = GetTickCount64();
    const uint64_t last = g_lastCamCopyMs.load(std::memory_order_relaxed);
    const bool cameraFresh = last != 0 && (now - last) < 500;    // camera driving now
    const bool cameraStale = last == 0 || (now - last) > 2000;   // menu / loading

    // Debounce entry: require the camera to have been fresh continuously for a
    // short spell before arming, so a single stray frame doesn't flip us.
    static uint64_t freshSince = 0;
    if (cameraFresh) { if (freshSince == 0) freshSince = now; }
    else freshSince = 0;
    const bool inLevelStable = freshSince != 0 && (now - freshSince) > 1000;

    const TitleDescriptor* activeTitle = TitleAdapter_GetActive();
    const bool pausePresentation = VR_IsPausePresentation();
    bool enginePaused = false;
    static bool previousEnginePaused = false;
    static bool enginePauseLogged = false;
    static uint64_t pauseMismatchSince = 0;
    static bool pauseMismatchValue = false;
    if (ReadEnginePaused(enginePaused))
    {
        if (!enginePauseLogged || enginePaused != previousEnginePaused)
        {
            LOG("pause state: native engine flag=%d, presentation target=%d",
                enginePaused ? 1 : 0,
                VR_IsPausePresentationTarget() ? 1 : 0);
            previousEnginePaused = enginePaused;
            enginePauseLogged = true;
        }
        const bool targetPaused = VR_IsPausePresentationTarget();
        if (g_enginePauseValidated.load() && enginePaused != targetPaused)
        {
            if (pauseMismatchSince == 0 || pauseMismatchValue != enginePaused)
            {
                pauseMismatchSince = now;
                pauseMismatchValue = enginePaused;
            }
            else if (now - pauseMismatchSince >= 50)
            {
                VR_RequestPausePresentation(enginePaused);
                LOG("pause state: authoritative engine value corrected "
                    "presentation to %s",
                    enginePaused ? "head-locked 2D" : "stereo 3D");
                pauseMismatchSince = 0;
            }
        }
        else
            pauseMismatchSince = 0;
    }
    static PauseLevelRecovery pauseLevelRecovery;
    if (!g_enginePauseValidated.load() &&
        pauseLevelRecovery.Update(pausePresentation, cameraStale, inLevelStable))
    {
        // Restart Level leaves Halo's native pause screen without producing a
        // second Start edge. Re-enter stereo only after the replacement
        // level's camera has been stable for the normal debounce interval.
        VR_RequestPausePresentation(false);
        LOG("pause transition: restarted level is stable, restoring stereo 3D");
    }
    static bool pauseExitClearRequested = false;
    if (pausePresentation &&
        (!activeTitle || activeTitle->title != GameTitle::Halo3))
    {
        // Leaving the title through Halo's pause menu must not strand the next
        // level in the pause override. This changes presentation only; it does
        // not inject another Start press into the MCC shell.
        if (!pauseExitClearRequested)
        {
            pauseExitClearRequested = true;
            VR_RequestPausePresentation(false);
            LOG("pause transition: title left, clearing 2D pause override");
        }
    }
    else
        pauseExitClearRequested = false;
    if (activeTitle && activeTitle->title == GameTitle::Halo3)
        TitleAdapter_SetRuntimeMode(pausePresentation
            ? RuntimeMode::Paused
            : (inLevelStable ? RuntimeMode::Gameplay : RuntimeMode::Loading));
    else if (activeTitle && !activeTitle->runtimeSupported)
        TitleAdapter_SetRuntimeMode(RuntimeMode::Unsupported);

    if (!g_config.auto_vr || pausePresentation) return;

    if (inLevelStable)
    {
        if (!g_enabled.load() && !g_autoVrUserVeto.load())
        {
            g_enabled = true;
            g_needRecenter = true;
            if (!VR_IsStereoEnabled()) VR_ToggleStereo();
            g_autoVrOwned = true;
            LOG("auto-VR: level detected — head tracking + stereo ON");
        }
    }
    else if (cameraStale)
    {
        g_autoVrUserVeto = false; // reset veto on leaving the level
        if (g_autoVrOwned.load() && g_enabled.load())
        {
            g_enabled = false;
            if (VR_IsStereoEnabled()) VR_ToggleStereo();
            g_autoVrOwned = false;
            LOG("auto-VR: left the level — back to the flat menu screen");
        }
    }
}

void Game_Recenter()
{
    // One public recenter action owns both references: Halo's camera/position
    // origin and the OpenXR head-locked screen origin. This keeps keyboard F3,
    // the F1 button, and transition-triggered recentering behavior identical.
    g_needRecenter = true;
    VR_RequestRecenter();
}
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
    if (VR_IsPausePresentation())
        return blocked(6, "Halo pause presentation active");
    if (!g_vrAim.load())
        return blocked(1, "VR aim toggled OFF (press Insert)");
    if (!g_enabled.load())
        return blocked(2, "head tracking OFF (press F2)");
    if (!g_aimSeen.load())
        return blocked(3, "camera hook not running (not in a level?)");
    float q[4], p[3];
    if (!VR_GetAimPose(q, p)) // two-hand-adjusted weapon aim (falls back to right hand)
        return blocked(4, "right controller not tracked");
    float hq[4], hp[3];
    if (!VR_GetHeadPose(hq, hp))
        return blocked(5, "headset not tracked");
    lastAimBlock = 0;

    // Controller forward: the RAW aim-pose -Z, deliberately NOT mount-trimmed.
    // Bullets and the reticle share this fixed "laser" ray; the mount trim
    // (gun_pitch/yaw/roll) rotates only the visible gun + flash, so the user
    // can turn the mesh until its barrel lies on the cursor line. Coupling the
    // ray to the trim made that tuning non-convergent (2026-07-19: "the
    // rotation moves the cursor too").
    const float localDir[3] = {0.0f, 0.0f, -1.0f};
    float f3[3];
    RotateByQuat(q, localDir, f3);
    const float fx = f3[0], fy = f3[1], fz = f3[2];

    // Halo spawns first-person projectiles at the CAMERA — the head — and no
    // steering can move that origin. Aiming the bullet ray PARALLEL to the
    // hand ray therefore leaves a permanent head-to-hand parallax miss (the
    // 07-15 report "bullets shoot from my head"). Instead steer the head-
    // origin ray through the point the hand ray reaches at the crosshair
    // distance: every shot then passes exactly through the floating reticle,
    // and beyond it the two rays are effectively identical.
    const float d = Clamp(g_config.crosshair_distance_m, 2.0f, 50.0f);
    float tx = p[0] + fx * d - hp[0];
    float ty = p[1] + fy * d - hp[1];
    float tz = p[2] + fz * d - hp[2];
    const float tl = sqrtf(tx * tx + ty * ty + tz * tz);
    if (tl > 1e-3f) { tx /= tl; ty /= tl; tz /= tl; }
    const float cy = atan2f(tx, -tz);
    const float cp = asinf(Clamp(ty, -1.0f, 1.0f));
    const float desiredYaw = g_gameYawRef + g_yawSign.load() * WrapPi(cy - g_headYawRef);
    const float desiredPitch = Clamp(g_pitchSign.load() * cp, -1.45f, 1.45f);

    const float ax = g_aimFwdX.load(), ay = g_aimFwdY.load(), az = g_aimFwdZ.load();
    const float aimYaw = atan2f(ay, ax);
    const float aimPitch = asinf(Clamp(az, -1.0f, 1.0f));

    const float errYaw = WrapPi(desiredYaw - aimYaw);
    const float errPitch = desiredPitch - aimPitch;

    // Full deflection at ~4.8 deg of error (was ~10; user: vertical follow too
    // slow). The ceiling is the game's own turn rate — raising in-game look
    // sensitivity raises it further.
    const float k = 12.0f;
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

void Game_GunScale(int dir)
{
    // Uniform mesh scale of the hand-anchored arms+gun assembly around the
    // wrist. Home = bigger, End = smaller. Persisted next time settings save.
    const float s = Clamp(g_config.gun_scale * (dir > 0 ? 1.05f : 1.0f / 1.05f),
                          0.3f, 3.0f);
    g_config.gun_scale = s;
    LOG("weapon size %.2fx", s);
}

float Game_GetWorldScale() { return g_worldScale.load(); }
float Game_GetZoomFactor() { return g_zoomFactor.load(); }

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
