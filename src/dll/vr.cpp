#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <algorithm>
#include <vector>
#include <array>
#include <string>
#include <atomic>
#include <mutex>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "vr.h"
#include "menu.h"
#include "game.h"
#include "d3d11_hook.h"
#include "d3d_state.h"
#include "title_adapter.h"
#include "../common/log.h"
#include "../common/config.h"
#include "../common/input_logic.h"
#include "../common/scope_logic.h"

// M0 "virtual cinema": every frame the game presents, we copy its backbuffer
// into an OpenXR swapchain and submit it as a world-locked quad layer (a flat
// rectangle floating in space). The ImGui menu is a second, smaller quad.
// The headset compositor does all the reprojection; there is no stereo yet.

namespace
{
    enum class State { Uninitialized, Ready, Failed };

    State g_state = State::Uninitialized;

    // The OpenXR instance/system are created on a background thread (see
    // VR_InitInstance) because xrCreateInstance can take 20+ seconds while
    // SteamVR spins up — doing that on the game's render thread freezes the
    // game and it gets killed as unresponsive. These flags let the render
    // thread know when the instance is ready without ever blocking on it.
    std::atomic<bool> g_instanceReady{false};
    std::atomic<bool> g_instanceFailed{false};

    // OpenXR core
    XrInstance g_instance = XR_NULL_HANDLE;
    XrSystemId g_systemId = XR_NULL_SYSTEM_ID;
    XrSession g_session = XR_NULL_HANDLE;
    XrSpace g_localSpace = XR_NULL_HANDLE; // world-fixed, origin = headset pose at session start
    XrSpace g_viewSpace = XR_NULL_HANDLE;  // follows the headset; used for recentering
    XrActionSet g_gameplayActions = XR_NULL_HANDLE;
    XrAction g_rightAimAction = XR_NULL_HANDLE;
    XrSpace g_rightAimSpace = XR_NULL_HANDLE;
    XrAction g_leftAimAction = XR_NULL_HANDLE;
    XrSpace g_leftAimSpace = XR_NULL_HANDLE;
    XrAction g_hapticAction = XR_NULL_HANDLE;
    XrAction g_actMenu = XR_NULL_HANDLE;
    XrPath g_leftHandPath = XR_NULL_PATH;
    XrPath g_rightHandPath = XR_NULL_PATH;
    bool g_touchProProfileEnabled = false;
    XrEnvironmentBlendMode g_blendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    bool g_sessionRunning = false;
    XrSessionState g_sessionState = XR_SESSION_STATE_UNKNOWN;
    std::atomic<float> g_requestedHaptics{0.0f};
    void StopControllerHaptics();

    // M2 stereo: per-eye recommended render size and per-eye pose/FOV.
    std::vector<XrViewConfigurationView> g_viewConfigs;
    std::vector<XrView> g_views;

    // D3D11 (the game's device — we never create our own for rendering)
    ID3D11Device* g_device = nullptr;
    ID3D11DeviceContext* g_context = nullptr;

    // XR swapchains + cached render target views of their images
    int64_t g_xrFormat = 0;
    XrSwapchain g_screenChain = XR_NULL_HANDLE;
    uint32_t g_screenW = 0, g_screenH = 0;
    std::vector<ID3D11Texture2D*> g_screenImages;
    std::vector<ID3D11RenderTargetView*> g_screenRtvs;
    XrSwapchain g_menuChain = XR_NULL_HANDLE;
    std::vector<ID3D11Texture2D*> g_menuImages;
    std::vector<ID3D11RenderTargetView*> g_menuRtvs;
    XrSwapchain g_fadeChain = XR_NULL_HANDLE;
    std::vector<ID3D11Texture2D*> g_fadeImages;
    std::vector<ID3D11RenderTargetView*> g_fadeRtvs;

    // M3 aim crosshair: a tiny static reticle image floated as a quad layer
    // along the weapon's true aim ray. Drawn once; the compositor keeps
    // re-showing the last released image, so it costs nothing per frame.
    XrSwapchain g_reticleChain = XR_NULL_HANDLE;
    std::vector<ID3D11Texture2D*> g_reticleImages;
    std::vector<ID3D11RenderTargetView*> g_reticleRtvs;
    constexpr uint32_t kReticleSize = 512;

    // Universal gun scope. A private scene cache receives the refresh-limited
    // third render; an isolated upload pipeline center-crops it to the fixed
    // 1024x768 screen and paints the aiming mark without touching either eye.
    XrSwapchain g_scopeScreenChain = XR_NULL_HANDLE;
    std::vector<ID3D11Texture2D*> g_scopeScreenImages;
    std::vector<ID3D11RenderTargetView*> g_scopeScreenRtvs;
    constexpr uint32_t kScopeScreenWidth = 1024;
    constexpr uint32_t kScopeScreenHeight = 768;
    std::atomic<bool> g_scopeActive{false};
    ID3D11Texture2D* g_scopeCache = nullptr;
    ID3D11RenderTargetView* g_scopeCacheRtv = nullptr;
    ID3D11ShaderResourceView* g_scopeCacheSrv = nullptr;
    D3D11_TEXTURE2D_DESC g_scopeCacheDesc{};
    std::atomic<bool> g_rasterScope{false};
    bool g_scopeRedirected = false;
    std::atomic<bool> g_scopeHasImage{false};
    ScopeRefreshScheduler g_scopeRefreshScheduler;
    ScopeZoomResolver g_scopeZoomResolver;
    ScopeZoomController g_scopeZoomController;
    std::atomic<float> g_scopeRuntimeZoom{3.39f};
    std::atomic<float> g_scopeZoomStickY{0.0f};
    uint64_t g_scopeZoomLastMs = 0;
    std::atomic<uint64_t> g_scopeToggleSerial{0};
    uint64_t g_scopeToggleObserved = 0;
    std::atomic<bool> g_scopeResetRequested{false};
    // Color the reticle was last painted with, so we repaint only when the
    // user changes it (not every frame). Sentinel forces the first paint.
    float g_reticlePaintedColor[3] = {-1.0f, -1.0f, -1.0f};
    float g_reticlePaintedOpacity = -1.0f; // last painted opacity; sentinel forces first paint
    bool g_reticleEnemyPainted = false; // which color is currently on the image
    // Set by the game layer when the crosshair is over an enemy (the engine's
    // target-lock state). While set, the reticle repaints red like the OG HUD.
    std::atomic<bool> g_reticleEnemy{false};

    // Halo's class-2 CHUD widget is rendered into this small transparent
    // target instead of either eye. The resulting game-owned, per-weapon
    // artwork is uploaded to the existing controller-ray OpenXR quad.
    ID3D11Texture2D* g_authoredReticleTexture = nullptr;
    ID3D11RenderTargetView* g_authoredReticleRtv = nullptr;
    bool g_authoredReticleReady = false;
    uint64_t g_authoredReticleSerial = 0;
    uint64_t g_authoredReticleUploadedSerial = 0;
    bool g_reticleContainsAuthored = false;
    struct ReticleCaptureState
    {
        ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
        ID3D11DepthStencilView* dsv = nullptr;
        D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
        UINT viewportCount = 0;
        D3D11_RECT scissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
        UINT scissorCount = 0;
        bool active = false;
    };
    ReticleCaptureState g_reticleCaptureState{};

    // The CHUD steal-and-requad machinery (capture texture, shader
    // classifier, hand-HUD swapchain) was removed 2026-07-18: it removed the
    // native HUD from both eyes, never displayed its quad, and its
    // calibration retry loop cost ~30 fps. The native HUD renders untouched;
    // per-eye FP camera substitution in game.cpp gives it (and the gun)
    // stereo-correct rendering.

    // M2 eye targets. Each eye is a separate swapchain because the headset may
    // recommend a different size for each view. They are allocated now so the
    // later render hook can draw directly into them; the mono quad remains the
    // active layer until both contain genuine per-eye game renders.
    struct EyeChain
    {
        XrSwapchain chain = XR_NULL_HANDLE;
        uint32_t width = 0;
        uint32_t height = 0;
        std::vector<ID3D11Texture2D*> images;
        std::vector<ID3D11RenderTargetView*> rtvs;
    };
    std::vector<EyeChain> g_eyeChains;
    XrSwapchain g_stereoChain = XR_NULL_HANDLE;
    uint32_t g_stereoW = 0, g_stereoH = 0;
    std::vector<ID3D11Texture2D*> g_stereoImages;
    std::vector<std::array<ID3D11RenderTargetView*, 2>> g_stereoRtvs;
    ID3D11Texture2D* g_eyeCache[2] = {nullptr, nullptr};
    ID3D11RenderTargetView* g_eyeCacheRtvs[2] = {nullptr, nullptr};
    D3D11_TEXTURE2D_DESC g_eyeCacheDesc{};

    // Removed: the per-eye post-process history machinery (cross-pass
    // discovery, frame-level blanking, and the learned scene-snapshot
    // pairs). Every one of them was disproven in a headset session, and
    // together they held two full-resolution shadow textures per learned
    // pair (~25 MB each) plus 96 AddRef'd candidate textures, and re-copied
    // them on every eye pass. See docs/CONTINUATION.md for what each probe
    // ruled out; do not rebuild them without new evidence.
    std::atomic<bool> g_stereoEnabled{false};
    int g_renderEye = 0;
    bool g_eyeHasImage[2] = {false, false};
    bool g_stereoValidationDone = false;
    std::atomic<int> g_rasterEye{-1};
    bool g_rasterRedirected[2] = {false, false};
    IDXGISwapChain* g_gameSwapchain = nullptr; // borrowed; owned by the game
    // The internal scene-color RTV is stable after the first eye render. Keep
    // one reference and use a pointer comparison in the OMSetRenderTargets
    // hook. The retired census path performed GetBuffer/GetResource/QI/GetDesc
    // plus a 128-entry linear scan on nearly every RTV bind and could collapse
    // stereo from 90 fps into the 20s.
    ID3D11RenderTargetView* g_sceneColorRtv = nullptr;
#if HALOMCCVR_EXPERIMENTAL_ODST_BRINGUP
    ID3D11Resource* g_sceneColorResource = nullptr;
    struct NativeHudEyeRouteState
    {
        ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
        ID3D11DepthStencilView* dsv = nullptr;
        ID3D11RenderTargetView* phaseOutputRtv = nullptr;
        D3D11_VIEWPORT viewports[
            D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
        D3D11_RECT scissors[
            D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
        UINT viewportCount = 0;
        UINT scissorCount = 0;
        int eye = -1;
        bool active = false;
        bool bypassOmRedirect = false;
        bool targetCopy = false;
    };
    thread_local NativeHudEyeRouteState g_nativeHudEyeRoute{};
    std::atomic<unsigned> g_nativeHudPhaseScopes{0};
    std::atomic<unsigned> g_nativeHudProvenOmMatches{0};
    std::atomic<unsigned> g_nativeHudExactCopyScopes{0};
    std::atomic<unsigned> g_nativeHudCopySubstitutions{0};
#endif
    D3D11_TEXTURE2D_DESC g_gameBackbufferDesc{};
    bool g_gameBackbufferDescValid = false;
    // Retained immediately after Present using the flip chain's current buffer
    // index. ODST can copy it after each death-camera eye draw without COM
    // discovery in the hot render path.
    std::atomic<ID3D11Texture2D*> g_nextGameBackbuffer{nullptr};
    IDXGISwapChain* g_flipIndexOwner = nullptr;
    IDXGISwapChain3* g_flipIndexChain = nullptr;

    // Where the virtual screen sits: yaw-only orientation + head position,
    // captured once at start (and again on "re-center").
    XrQuaternionf g_centerRot{0, 0, 0, 1};
    XrVector3f g_centerPos{0, 0, 0};
    bool g_haveCenter = false;

    // Screen placement while head tracking is on. World-locked (default) reads
    // as natural because turning your head shifts the screen in your view to
    // match your head motion; head-locked keeps it pinned in front but feels
    // disconnected. Toggle with F10.
    std::atomic<bool> g_screenFollow{false};
    // Input threads request a target; the render thread owns the 200 ms
    // fade-out, presentation switch, and fade-in.
    std::atomic<int> g_pauseRequest{-1};
    std::atomic<bool> g_pausePresentation{false};
    std::atomic<bool> g_pauseTarget{false};

    // Latest head pose in the LOCAL space, captured every frame on the render
    // thread and read by the game camera hook (M1) on the game thread — hence
    // the lock. Orientation is a quaternion, position is in meters.
    CRITICAL_SECTION g_headCs;
    bool g_headCsInit = false;
    XrPosef g_headPose{{0, 0, 0, 1}, {0, 0, 0}};
    bool g_headPoseValid = false;
    XrPosef g_rightAimPose{{0, 0, 0, 1}, {0, 0, 0}};
    bool g_rightAimPoseValid = false;
    XrPosef g_leftAimPose{{0, 0, 0, 1}, {0, 0, 0}};
    bool g_leftAimPoseValid = false;
    // Render-thread-only filtered copy for the compositor crosshair. Keeping it
    // separate is intentional: weapon steering and bullets stay on raw aim.
    XrPosef g_reticleAimPose{{0, 0, 0, 1}, {0, 0, 0}};
    bool g_reticleAimPoseValid = false;

    // Blit (copy-with-format-conversion) resources, created on demand
    ID3D11VertexShader* g_blitVs = nullptr;
    ID3D11PixelShader* g_blitPsLinearize = nullptr; // sRGB-decodes in the shader
    ID3D11PixelShader* g_blitPsPass = nullptr;
    ID3D11SamplerState* g_blitSampler = nullptr;
    ID3D11RasterizerState* g_blitRasterizer = nullptr;
    ID3D11DepthStencilState* g_blitDepthOff = nullptr;
    ID3D11Texture2D* g_intermediate = nullptr; // SRV-capable copy of the backbuffer
    ID3D11ShaderResourceView* g_intermediateSrv = nullptr;
    D3D11_TEXTURE2D_DESC g_intermediateDesc{};
    ID3D11ShaderResourceView* g_srcSrv = nullptr; // direct SRV of the backbuffer, when allowed
    ID3D11Texture2D* g_srcSrvKey = nullptr;

    // Deliberately separate from the eye/menu blitter: scope upload failures
    // cannot poison the proven stereo presentation pipeline.
    ID3D11VertexShader* g_scopeUploadVs = nullptr;
    ID3D11PixelShader* g_scopeUploadPs = nullptr;
    ID3D11PixelShader* g_scopeUploadPsLinearize = nullptr;
    ID3D11SamplerState* g_scopeUploadSampler = nullptr;
    ID3D11RasterizerState* g_scopeUploadRasterizer = nullptr;
    ID3D11DepthStencilState* g_scopeUploadDepthOff = nullptr;

    // Status shown in the menu (only touched on the render thread)
    VrStatus g_status{};
    LARGE_INTEGER g_fpsTimer{};
    int g_fpsFrames = 0;

    // One OpenXR frame stays begun while Halo renders. VR_BeforePresent
    // submits it; after DXGI Present returns, VR_AfterPresent obtains the
    // runtime's exact prediction for the next Halo render.
    struct PreparedFrame
    {
        XrFrameState state{XR_TYPE_FRAME_STATE};
        bool begun = false;
        bool viewsValid = false;
        uint32_t viewCount = 0;
        uint64_t serial = 0;
    };
    PreparedFrame g_preparedFrame{};
    uint64_t g_nextPreparedSerial = 0;
    std::atomic<bool> g_preparedShouldRender{false};

    template <size_t N>
    struct TimingRing
    {
        std::array<double, N> values{};
        size_t next = 0;
        size_t count = 0;
        void Add(double value)
        {
            if (!std::isfinite(value) || value < 0.0)
                return;
            values[next] = value;
            next = (next + 1) % N;
            if (count < N) ++count;
        }
    };
    TimingRing<512> g_presentIntervalsMs;
    TimingRing<512> g_presentDurationsMs;
    TimingRing<512> g_waitDurationsMs;
    TimingRing<512> g_predictionErrorMs;
    LARGE_INTEGER g_qpcFrequency{};
    LARGE_INTEGER g_lastBeforePresentQpc{};
    LARGE_INTEGER g_dxgiPresentStartQpc{};
    uint64_t g_timingLogStartMs = 0;
    XrTime g_lastPredictedDisplayTime = 0;
    uint64_t g_missedPredictions = 0;
    uint64_t g_duplicatePredictions = 0;
    uint64_t g_frameOrderFailures = 0;
    std::atomic<uint64_t> g_preparedSerialPublished{0};
    std::atomic<uint64_t> g_prepareQpcPublished{0};
    std::atomic<uint64_t> g_cameraSerialObserved{0};
    std::atomic<uint64_t> g_firstCameraDelayUs{0};

    // ---------------------------------------------------------------- utils

    const char* XrStr(XrResult r)
    {
        static char buf[XR_MAX_RESULT_STRING_SIZE];
        if (g_instance != XR_NULL_HANDLE && XR_SUCCEEDED(xrResultToString(g_instance, r, buf)))
            return buf;
        snprintf(buf, sizeof(buf), "XrResult(%d)", (int)r);
        return buf;
    }

    double QpcMs(LONGLONG ticks)
    {
        if (!g_qpcFrequency.QuadPart)
            QueryPerformanceFrequency(&g_qpcFrequency);
        return ticks * 1000.0 / static_cast<double>(g_qpcFrequency.QuadPart);
    }

    template <size_t N>
    double TimingPercentile(const TimingRing<N>& ring, double percentile)
    {
        if (!ring.count)
            return 0.0;
        std::array<double, N> sorted{};
        for (size_t i = 0; i < ring.count; ++i)
            sorted[i] = ring.values[i];
        std::sort(sorted.begin(), sorted.begin() + ring.count);
        const size_t index = static_cast<size_t>(
            std::clamp(percentile, 0.0, 1.0) * static_cast<double>(ring.count - 1));
        return sorted[index];
    }

    // Tell the user VR failed without freezing the game (own thread) and let
    // the game keep running flat.
    void Fail(const char* what, XrResult r = XR_SUCCESS)
    {
        char msg[512];
        if (r != XR_SUCCESS)
            snprintf(msg, sizeof(msg), "%s (%s)", what, XrStr(r));
        else
            snprintf(msg, sizeof(msg), "%s", what);
        LOG("VR FAILED: %s", msg);
        g_state = State::Failed;

        static char popupText[640];
        snprintf(popupText, sizeof(popupText),
                 "Halo MCC VR mod could not start VR:\n\n%s\n\n"
                 "The game will keep running flat on the monitor.\n"
                 "Details are in halo3xr.log next to the mod DLL.", msg);
        CreateThread(nullptr, 0,
                     [](LPVOID p) -> DWORD {
                         MessageBoxA(nullptr, (const char*)p, "Halo MCC VR mod", MB_OK | MB_ICONWARNING | MB_TOPMOST);
                         return 0;
                     },
                     popupText, 0, nullptr);
    }

    DXGI_FORMAT FormatFamily(DXGI_FORMAT f)
    {
        switch (f)
        {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_R8G8B8A8_UINT:
        case DXGI_FORMAT_R8G8B8A8_SNORM:
        case DXGI_FORMAT_R8G8B8A8_SINT:
            return DXGI_FORMAT_R8G8B8A8_TYPELESS;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            return DXGI_FORMAT_B8G8R8A8_TYPELESS;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        case DXGI_FORMAT_R10G10B10A2_UNORM:
        case DXGI_FORMAT_R10G10B10A2_UINT:
            return DXGI_FORMAT_R10G10B10A2_TYPELESS;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R16G16B16A16_UNORM:
            return DXGI_FORMAT_R16G16B16A16_TYPELESS;
        default:
            return f;
        }
    }

    bool IsSrgb(DXGI_FORMAT f)
    {
        return f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    }

    DXGI_FORMAT UnormSibling(DXGI_FORMAT f)
    {
        if (f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) return DXGI_FORMAT_R8G8B8A8_UNORM;
        if (f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) return DXGI_FORMAT_B8G8R8A8_UNORM;
        return f;
    }

    XrVector3f Rotate(const XrQuaternionf& q, const XrVector3f& v)
    {
        const XrVector3f u{q.x, q.y, q.z};
        auto cross = [](const XrVector3f& a, const XrVector3f& b) {
            return XrVector3f{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
        };
        XrVector3f c1 = cross(u, v);
        c1.x += q.w * v.x; c1.y += q.w * v.y; c1.z += q.w * v.z;
        const XrVector3f c2 = cross(u, c1);
        return {v.x + 2 * c2.x, v.y + 2 * c2.y, v.z + 2 * c2.z};
    }

    bool NormalizeTrackedPose(XrPosef& pose)
    {
        const float ql2 = pose.orientation.x * pose.orientation.x +
                          pose.orientation.y * pose.orientation.y +
                          pose.orientation.z * pose.orientation.z +
                          pose.orientation.w * pose.orientation.w;
        if (!std::isfinite(ql2) || ql2 < 1e-8f ||
            !std::isfinite(pose.position.x) || !std::isfinite(pose.position.y) ||
            !std::isfinite(pose.position.z))
            return false;
        const float inv = 1.0f / sqrtf(ql2);
        pose.orientation.x *= inv;
        pose.orientation.y *= inv;
        pose.orientation.z *= inv;
        pose.orientation.w *= inv;
        return true;
    }

    // One-pole previous-frame blend. `historyWeight` is deliberately expressed
    // as the UI percentage: 0 = raw current pose, .05 = 5% previous + 95% current.
    // Quaternion sign correction keeps equivalent q/-q samples from cancelling.
    XrPosef SmoothTrackedPose(const XrPosef& current, const XrPosef& previous,
                              float historyWeight)
    {
        const float h = std::clamp(historyWeight, 0.0f, 0.95f);
        const float n = 1.0f - h;
        float sign = current.orientation.x * previous.orientation.x +
                     current.orientation.y * previous.orientation.y +
                     current.orientation.z * previous.orientation.z +
                     current.orientation.w * previous.orientation.w < 0.0f ? -1.0f : 1.0f;
        XrPosef result{};
        result.orientation = {
            previous.orientation.x * h + current.orientation.x * n * sign,
            previous.orientation.y * h + current.orientation.y * n * sign,
            previous.orientation.z * h + current.orientation.z * n * sign,
            previous.orientation.w * h + current.orientation.w * n * sign};
        result.position = {
            previous.position.x * h + current.position.x * n,
            previous.position.y * h + current.position.y * n,
            previous.position.z * h + current.position.z * n};
        NormalizeTrackedPose(result);
        return result;
    }

    const char* SessionStateName(XrSessionState s)
    {
        switch (s)
        {
        case XR_SESSION_STATE_IDLE: return "idle";
        case XR_SESSION_STATE_READY: return "ready";
        case XR_SESSION_STATE_SYNCHRONIZED: return "synchronized";
        case XR_SESSION_STATE_VISIBLE: return "visible";
        case XR_SESSION_STATE_FOCUSED: return "focused";
        case XR_SESSION_STATE_STOPPING: return "stopping";
        case XR_SESSION_STATE_LOSS_PENDING: return "loss pending";
        case XR_SESSION_STATE_EXITING: return "exiting";
        default: return "unknown";
        }
    }

    // ------------------------------------------------------------- blitting

    bool EnsureBlitPipeline()
    {
        if (g_blitVs && g_blitPsLinearize && g_blitPsPass &&
            g_blitSampler && g_blitRasterizer && g_blitDepthOff)
            return true;
        auto release=[&](auto*& object)
        {
            if (object) object->Release();
            object=nullptr;
        };
        release(g_blitVs);
        release(g_blitPsLinearize);
        release(g_blitPsPass);
        release(g_blitSampler);
        release(g_blitRasterizer);
        release(g_blitDepthOff);

        static const char* src = R"(
Texture2D srcTex : register(t0);
SamplerState smp : register(s0);
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut vs_main(uint id : SV_VertexID)
{
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = uv;
    return o;
}
float lin(float c) { return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }
float4 ps_linearize(VSOut i) : SV_Target
{
    float4 c = srcTex.Sample(smp, i.uv);
    return float4(lin(c.r), lin(c.g), lin(c.b), c.a);
}
float4 ps_pass(VSOut i) : SV_Target
{
    return srcTex.Sample(smp, i.uv);
}
)";
        ID3DBlob* blob = nullptr;
        ID3DBlob* err = nullptr;
        auto compile = [&](const char* entry, const char* target) -> ID3DBlob* {
            ID3DBlob* out = nullptr;
            if (FAILED(D3DCompile(src, strlen(src), nullptr, nullptr, nullptr, entry, target, 0, 0, &out, &err)))
            {
                LOG("blit shader '%s' failed to compile: %s", entry, err ? (const char*)err->GetBufferPointer() : "?");
                if (err) { err->Release(); err = nullptr; }
                return nullptr;
            }
            return out;
        };

        blob = compile("vs_main", "vs_5_0");
        if (!blob) return false;
        HRESULT hr = g_device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &g_blitVs);
        blob->Release();
        if (FAILED(hr)) return false;

        blob = compile("ps_linearize", "ps_5_0");
        if (!blob) return false;
        hr = g_device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &g_blitPsLinearize);
        blob->Release();
        if (FAILED(hr)) return false;

        blob = compile("ps_pass", "ps_5_0");
        if (!blob) return false;
        hr = g_device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &g_blitPsPass);
        blob->Release();
        if (FAILED(hr)) return false;

        D3D11_SAMPLER_DESC smp{};
        smp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        if (FAILED(g_device->CreateSamplerState(&smp, &g_blitSampler))) return false;

        D3D11_RASTERIZER_DESC rs{};
        rs.FillMode = D3D11_FILL_SOLID;
        rs.CullMode = D3D11_CULL_NONE;
        rs.DepthClipEnable = TRUE;
        if (FAILED(g_device->CreateRasterizerState(&rs, &g_blitRasterizer))) return false;

        D3D11_DEPTH_STENCIL_DESC ds{};
        ds.DepthEnable = FALSE;
        if (FAILED(g_device->CreateDepthStencilState(&ds, &g_blitDepthOff))) return false;

        return true;
    }

    void ReleaseSourceViews()
    {
        if (g_srcSrv) { g_srcSrv->Release(); g_srcSrv = nullptr; }
        g_srcSrvKey = nullptr;
        if (g_intermediateSrv) { g_intermediateSrv->Release(); g_intermediateSrv = nullptr; }
        if (g_intermediate) { g_intermediate->Release(); g_intermediate = nullptr; }
        g_intermediateDesc = {};
    }

    // Copy src into dst (an XR swapchain image). Uses a plain GPU copy when
    // the formats/sizes allow it, otherwise draws a fullscreen quad, fixing
    // gamma along the way.
    bool Blit(ID3D11Texture2D* src, const D3D11_TEXTURE2D_DESC& srcDesc,
              ID3D11Texture2D* dst, uint32_t dstW, uint32_t dstH,
              ID3D11RenderTargetView* dstRtv)
    {
        const bool sameSize = srcDesc.Width == dstW && srcDesc.Height == dstH;
        const bool sameFamily = FormatFamily(srcDesc.Format) == FormatFamily((DXGI_FORMAT)g_xrFormat);
        const bool fastPath = sameSize && sameFamily && srcDesc.SampleDesc.Count <= 1;
        // One-time: confirm the cheap CopyResource path is taken (the slow path
        // makes an intermediate texture + full-screen draw every eye blit).
        static bool loggedPath=false;
        if (!loggedPath)
        {
            loggedPath=true;
            LOG("PERF: eye blit uses %s path (src %ux%u fmt %d -> dst %ux%u xrfmt %d)",
                fastPath?"FAST CopyResource":"SLOW shader",
                srcDesc.Width,srcDesc.Height,(int)srcDesc.Format,
                dstW,dstH,(int)g_xrFormat);
        }
        if (fastPath)
        {
            g_context->CopyResource(dst, src);
            return true;
        }

        if (!EnsureBlitPipeline())
            return false;

        // Find something we can sample from. Backbuffers usually can't be
        // used as shader input directly, so we may need an intermediate copy.
        ID3D11ShaderResourceView* srv = nullptr;
        if ((srcDesc.BindFlags & D3D11_BIND_SHADER_RESOURCE) && srcDesc.SampleDesc.Count <= 1)
        {
            if (g_srcSrvKey != src)
            {
                if (g_srcSrv) { g_srcSrv->Release(); g_srcSrv = nullptr; }
                if (FAILED(g_device->CreateShaderResourceView(src, nullptr, &g_srcSrv)))
                    g_srcSrv = nullptr;
                g_srcSrvKey = g_srcSrv ? src : nullptr;
            }
            srv = g_srcSrv;
        }
        if (!srv)
        {
            if (!g_intermediate || g_intermediateDesc.Width != srcDesc.Width ||
                g_intermediateDesc.Height != srcDesc.Height || g_intermediateDesc.Format != srcDesc.Format)
            {
                if (g_intermediateSrv) { g_intermediateSrv->Release(); g_intermediateSrv = nullptr; }
                if (g_intermediate) { g_intermediate->Release(); g_intermediate = nullptr; }
                D3D11_TEXTURE2D_DESC d{};
                d.Width = srcDesc.Width;
                d.Height = srcDesc.Height;
                d.MipLevels = 1;
                d.ArraySize = 1;
                d.Format = srcDesc.Format;
                d.SampleDesc.Count = 1;
                d.Usage = D3D11_USAGE_DEFAULT;
                d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                if (FAILED(g_device->CreateTexture2D(&d, nullptr, &g_intermediate)) ||
                    FAILED(g_device->CreateShaderResourceView(g_intermediate, nullptr, &g_intermediateSrv)))
                {
                    LOG("blit: intermediate texture creation failed (fmt %d)", (int)srcDesc.Format);
                    ReleaseSourceViews();
                    return false;
                }
                g_intermediateDesc = d;
            }
            if (srcDesc.SampleDesc.Count > 1)
                g_context->ResolveSubresource(g_intermediate, 0, src, 0, srcDesc.Format);
            else
                g_context->CopyResource(g_intermediate, src);
            srv = g_intermediateSrv;
        }

        // If the source is already an sRGB view (sampling gives linear) or the
        // destination isn't sRGB, a raw copy through the shader is correct.
        // Otherwise decode gamma in the shader so the sRGB target re-encodes it.
        const bool linearize=!IsSrgb(srcDesc.Format) && IsSrgb((DXGI_FORMAT)g_xrFormat);
        ID3D11PixelShader* ps=linearize?g_blitPsLinearize:g_blitPsPass;

        D3DStateBackup backup;
        backup.Capture(g_context);

        g_context->OMSetRenderTargets(1, &dstRtv, nullptr);
        D3D11_VIEWPORT vp{0, 0, (float)dstW, (float)dstH, 0, 1};
        g_context->RSSetViewports(1, &vp);
        g_context->RSSetState(g_blitRasterizer);
        g_context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
        g_context->OMSetDepthStencilState(g_blitDepthOff, 0);
        g_context->IASetInputLayout(nullptr);
        g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g_context->VSSetShader(g_blitVs, nullptr, 0);
        // Halo may leave a geometry shader bound at the end of an eye pass.
        // The fullscreen triangle has no compatible GS stage; clear it for the
        // blit and let D3DStateBackup restore the game's shader afterward.
        g_context->GSSetShader(nullptr, nullptr, 0);
        g_context->PSSetShader(ps, nullptr, 0);
        g_context->PSSetShaderResources(0, 1, &srv);
        g_context->PSSetSamplers(0, 1, &g_blitSampler);
        g_context->Draw(3, 0);

        backup.Restore(g_context);
        return true;
    }

    // (The HUD "capture-diff panel" machinery that lived here — per-eye pre-HUD
    // snapshots, ps_huddiff extraction, union blend, head-locked panel quad,
    // native-HUD erase — was headset-DISPROVEN 2026-07-19: the diff carried only
    // the objective text, the rest of the HUD vanished, and the capture copies
    // cost real GPU time every frame. Removed at the user's direction. The HUD
    // ships native and full-size; only the reticle element is hidden via the
    // verified 0x2EDF24 element hook. See docs/RE-notes.md HUD dead ends.)

    // ------------------------------------------------------- XR swapchains

    void DestroyChain(XrSwapchain& chain, std::vector<ID3D11Texture2D*>& images,
                      std::vector<ID3D11RenderTargetView*>& rtvs)
    {
        for (auto* rtv : rtvs)
            if (rtv) rtv->Release();
        rtvs.clear();
        images.clear(); // owned by the runtime, not AddRef'd
        if (chain != XR_NULL_HANDLE)
        {
            xrDestroySwapchain(chain);
            chain = XR_NULL_HANDLE;
        }
    }

    bool CreateChain(uint32_t w, uint32_t h, XrSwapchain& chain,
                     std::vector<ID3D11Texture2D*>& images, std::vector<ID3D11RenderTargetView*>& rtvs,
                     const char* what)
    {
        XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
                        XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        ci.format = g_xrFormat;
        ci.sampleCount = 1;
        ci.width = w;
        ci.height = h;
        ci.faceCount = 1;
        ci.arraySize = 1;
        ci.mipCount = 1;
        XrResult r = xrCreateSwapchain(g_session, &ci, &chain);
        if (XR_FAILED(r))
        {
            LOG("xrCreateSwapchain(%s, %ux%u) failed: %s", what, w, h, XrStr(r));
            return false;
        }
        uint32_t count = 0;
        r=xrEnumerateSwapchainImages(chain,0,&count,nullptr);
        if (XR_FAILED(r) || !count)
        {
            LOG("xrEnumerateSwapchainImages(%s) count failed: %s",what,XrStr(r));
            DestroyChain(chain,images,rtvs);
            return false;
        }
        std::vector<XrSwapchainImageD3D11KHR> xrImages(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        r = xrEnumerateSwapchainImages(chain, count, &count,
                                       reinterpret_cast<XrSwapchainImageBaseHeader*>(xrImages.data()));
        if (XR_FAILED(r))
        {
            LOG("xrEnumerateSwapchainImages(%s) failed: %s", what, XrStr(r));
            DestroyChain(chain,images,rtvs);
            return false;
        }
        images.clear();
        rtvs.assign(count, nullptr);
        for (auto& img : xrImages)
            images.push_back(img.texture);
        LOG("XR swapchain '%s' created: %ux%u, %u images", what, w, h, count);
        return true;
    }

    ID3D11RenderTargetView* GetRtv(std::vector<ID3D11Texture2D*>& images,
                                   std::vector<ID3D11RenderTargetView*>& rtvs, uint32_t idx)
    {
        if (idx >= images.size())
            return nullptr;
        if (!rtvs[idx])
            g_device->CreateRenderTargetView(images[idx], nullptr, &rtvs[idx]);
        return rtvs[idx];
    }

    void ReleaseScopeCache()
    {
        if (g_scopeCacheSrv) { g_scopeCacheSrv->Release(); g_scopeCacheSrv = nullptr; }
        if (g_scopeCacheRtv) { g_scopeCacheRtv->Release(); g_scopeCacheRtv = nullptr; }
        if (g_scopeCache) { g_scopeCache->Release(); g_scopeCache = nullptr; }
        g_scopeCacheDesc = {};
        g_scopeHasImage.store(false);
    }

    bool EnsureScopeCache()
    {
        if (!g_device || !g_eyeCacheDesc.Width || !g_eyeCacheDesc.Height)
            return false;
        if (g_scopeCache && g_scopeCacheRtv && g_scopeCacheSrv &&
            g_scopeCacheDesc.Width == g_eyeCacheDesc.Width &&
            g_scopeCacheDesc.Height == g_eyeCacheDesc.Height &&
            g_scopeCacheDesc.Format == g_eyeCacheDesc.Format)
            return true;
        ReleaseScopeCache();
        D3D11_TEXTURE2D_DESC desc = g_eyeCacheDesc;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = 0;
        desc.MiscFlags = 0;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        HRESULT hr = g_device->CreateTexture2D(&desc, nullptr, &g_scopeCache);
        if (SUCCEEDED(hr))
            hr = g_device->CreateRenderTargetView(g_scopeCache, nullptr, &g_scopeCacheRtv);
        if (SUCCEEDED(hr))
            hr = g_device->CreateShaderResourceView(g_scopeCache, nullptr, &g_scopeCacheSrv);
        if (FAILED(hr))
        {
            static bool logged = false;
            if (!logged)
            {
                logged = true;
                LOG("scope cache creation failed: HRESULT 0x%08X (%ux%u format %d)",
                    (unsigned)hr, desc.Width, desc.Height, (int)desc.Format);
            }
            ReleaseScopeCache();
            return false;
        }
        g_scopeCacheDesc = desc;
        LOG("scope private render cache created: %ux%u format %d",
            desc.Width, desc.Height, (int)desc.Format);
        return true;
    }

    bool EnsureScopeUploadPipeline()
    {
        if (g_scopeUploadVs && g_scopeUploadPs && g_scopeUploadPsLinearize &&
            g_scopeUploadSampler && g_scopeUploadRasterizer && g_scopeUploadDepthOff)
            return true;
        static bool failed = false;
        if (failed || !g_device) return false;
        static const char* source = R"(
Texture2D srcTex : register(t0);
SamplerState smp : register(s0);
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut vs_main(uint id : SV_VertexID) {
    VSOut o; float2 uv=float2((id<<1)&2,id&2);
    o.pos=float4(uv*float2(2,-2)+float2(-1,1),0,1); o.uv=uv; return o;
}
float lin(float c) { return c<=0.04045 ? c/12.92 : pow((c+0.055)/1.055,2.4); }
float4 paint(float2 uv, bool decode) {
    uint sw,sh; srcTex.GetDimensions(sw,sh);
    float sa=(float)sw/max(1.0,(float)sh), da=4.0/3.0;
    float2 scale=sa>da ? float2(da/sa,1) : float2(1,sa/da);
    float3 rgb=srcTex.Sample(smp,0.5+(uv-0.5)*scale).rgb;
    if(decode) rgb=float3(lin(rgb.r),lin(rgb.g),lin(rgb.b));
    float2 px=abs((uv-0.5)*float2(1024,768));
    float outer=max((1-step(3,px.x))*(1-step(16,px.y)),
                    (1-step(3,px.y))*(1-step(16,px.x)));
    float inner=max((1-step(1.2,px.x))*(1-step(12,px.y)),
                    (1-step(1.2,px.y))*(1-step(12,px.x)));
    rgb=lerp(rgb,float3(0,0,0),outer);
    rgb=lerp(rgb,float3(0.35,1,0.35),inner);
    return float4(rgb,1);
}
float4 ps_scope(VSOut i):SV_Target { return paint(i.uv,false); }
float4 ps_scope_linearize(VSOut i):SV_Target { return paint(i.uv,true); }
)";
        HRESULT hr = S_OK;
        auto compile = [&](const char* entry, const char* target)->ID3DBlob* {
            ID3DBlob *blob=nullptr,*errors=nullptr;
            hr=D3DCompile(source,strlen(source),nullptr,nullptr,nullptr,entry,target,0,0,&blob,&errors);
            if(FAILED(hr)) LOG("scope shader %s failed: HRESULT 0x%08X: %s",entry,
                (unsigned)hr,errors?(const char*)errors->GetBufferPointer():"no compiler text");
            if(errors) errors->Release(); return blob;
        };
        ID3DBlob* blob=compile("vs_main","vs_5_0");
        if(blob) { hr=g_device->CreateVertexShader(blob->GetBufferPointer(),blob->GetBufferSize(),
                                                   nullptr,&g_scopeUploadVs); blob->Release(); }
        if(SUCCEEDED(hr)) blob=compile("ps_scope","ps_5_0");
        if(SUCCEEDED(hr) && blob) { hr=g_device->CreatePixelShader(blob->GetBufferPointer(),blob->GetBufferSize(),
                                                            nullptr,&g_scopeUploadPs); blob->Release(); }
        if(SUCCEEDED(hr)) blob=compile("ps_scope_linearize","ps_5_0");
        if(SUCCEEDED(hr) && blob) { hr=g_device->CreatePixelShader(blob->GetBufferPointer(),blob->GetBufferSize(),
                                                            nullptr,&g_scopeUploadPsLinearize); blob->Release(); }
        D3D11_SAMPLER_DESC sampler{}; sampler.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;
        if(SUCCEEDED(hr)) hr=g_device->CreateSamplerState(&sampler,&g_scopeUploadSampler);
        D3D11_RASTERIZER_DESC raster{}; raster.FillMode=D3D11_FILL_SOLID;
        raster.CullMode=D3D11_CULL_NONE; raster.DepthClipEnable=TRUE;
        if(SUCCEEDED(hr)) hr=g_device->CreateRasterizerState(&raster,&g_scopeUploadRasterizer);
        D3D11_DEPTH_STENCIL_DESC depth{}; depth.DepthEnable=FALSE;
        if(SUCCEEDED(hr)) hr=g_device->CreateDepthStencilState(&depth,&g_scopeUploadDepthOff);
        if(FAILED(hr))
        {
            failed=true;
            LOG("scope isolated upload pipeline failed: HRESULT 0x%08X",(unsigned)hr);
            auto release=[](auto*& p){if(p)p->Release();p=nullptr;};
            release(g_scopeUploadVs); release(g_scopeUploadPs); release(g_scopeUploadPsLinearize);
            release(g_scopeUploadSampler); release(g_scopeUploadRasterizer); release(g_scopeUploadDepthOff);
            return false;
        }
        return true;
    }

    bool UploadScopeToRtv(ID3D11RenderTargetView* rtv)
    {
        if(!rtv || !EnsureScopeUploadPipeline()) return false;
        const bool linearize=!IsSrgb(g_scopeCacheDesc.Format) &&
                             IsSrgb((DXGI_FORMAT)g_xrFormat);
        ID3D11PixelShader* ps=linearize?g_scopeUploadPsLinearize:g_scopeUploadPs;
        D3DStateBackup backup; backup.Capture(g_context);
        g_context->OMSetRenderTargets(1,&rtv,nullptr);
        D3D11_VIEWPORT viewport{0,0,(float)kScopeScreenWidth,
                                (float)kScopeScreenHeight,0,1};
        g_context->RSSetViewports(1,&viewport);
        g_context->RSSetState(g_scopeUploadRasterizer);
        g_context->OMSetBlendState(nullptr,nullptr,0xFFFFFFFF);
        g_context->OMSetDepthStencilState(g_scopeUploadDepthOff,0);
        g_context->IASetInputLayout(nullptr);
        g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g_context->VSSetShader(g_scopeUploadVs,nullptr,0);
        g_context->GSSetShader(nullptr,nullptr,0);
        g_context->PSSetShader(ps,nullptr,0);
        g_context->PSSetShaderResources(0,1,&g_scopeCacheSrv);
        g_context->PSSetSamplers(0,1,&g_scopeUploadSampler);
        g_context->Draw(3,0);
        backup.Restore(g_context);
        return true;
    }

    bool PrepareScopeImageDelivery()
    {
        static bool creationFailed = false;
        if (creationFailed || !g_scopeHasImage.load() || !g_scopeCache ||
            !g_scopeCacheSrv || !EnsureScopeUploadPipeline())
            return false;
        if (g_scopeScreenChain == XR_NULL_HANDLE &&
            !CreateChain(kScopeScreenWidth, kScopeScreenHeight, g_scopeScreenChain,
                         g_scopeScreenImages, g_scopeScreenRtvs, "scope 4:3 screen"))
        {
            creationFailed = true;
            return false;
        }

        uint32_t index = 0;
        XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wait.timeout = 1000000000;
        XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};

        XrResult result = xrAcquireSwapchainImage(g_scopeScreenChain, &acquire, &index);
        if (XR_FAILED(result))
        {
            static bool logged = false;
            if (!logged) { logged = true; LOG("scope image acquire failed: %s", XrStr(result)); }
            return false;
        }
        result = xrWaitSwapchainImage(g_scopeScreenChain, &wait);
        if (XR_FAILED(result))
        {
            static bool logged = false;
            if (!logged) { logged = true; LOG("scope image wait failed: %s", XrStr(result)); }
            xrReleaseSwapchainImage(g_scopeScreenChain, &release);
            return false;
        }

        ID3D11RenderTargetView* rtv = nullptr;
        if (index < g_scopeScreenImages.size() && index < g_scopeScreenRtvs.size())
        {
            rtv = g_scopeScreenRtvs[index];
            if (!rtv)
            {
                D3D11_RENDER_TARGET_VIEW_DESC desc{};
                desc.Format = (DXGI_FORMAT)g_xrFormat;
                desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
                desc.Texture2D.MipSlice = 0;
                const HRESULT hr = g_device->CreateRenderTargetView(
                    g_scopeScreenImages[index], &desc, &g_scopeScreenRtvs[index]);
                if (FAILED(hr))
                {
                    static bool logged = false;
                    if (!logged)
                    {
                        logged = true;
                        LOG("scope image RTV creation failed: HRESULT 0x%08X format %d",
                            (unsigned)hr, (int)g_xrFormat);
                    }
                }
                rtv = g_scopeScreenRtvs[index];
            }
        }

        const bool uploaded = UploadScopeToRtv(rtv);
        if (!uploaded)
        {
            static bool logged = false;
            if (!logged) { logged=true; LOG("scope image upload failed: no valid output RTV"); }
        }

        const XrResult releaseResult = xrReleaseSwapchainImage(g_scopeScreenChain, &release);
        if (XR_FAILED(releaseResult))
        {
            static bool logged = false;
            if (!logged) { logged = true; LOG("scope image release failed: %s", XrStr(releaseResult)); }
            return false;
        }
        return uploaded;
    }

    bool EnsureScreenChain(uint32_t w, uint32_t h)
    {
        if (w == 0 || h == 0) // game momentarily has a 0x0 backbuffer (e.g. intro video / mode switch)
            return false;
        if (g_screenChain != XR_NULL_HANDLE && g_screenW == w && g_screenH == h)
            return true;
        DestroyChain(g_screenChain, g_screenImages, g_screenRtvs);
        if (!CreateChain(w, h, g_screenChain, g_screenImages, g_screenRtvs, "screen"))
            return false;
        g_screenW = w;
        g_screenH = h;
        g_status.gameWidth = w;
        g_status.gameHeight = h;
        return true;
    }

    bool CreateEyeChains(uint32_t testWidth = 0, uint32_t testHeight = 0)
    {
        if (g_viewConfigs.size() != 2)
        {
            LOG("M2: expected 2 stereo views, runtime reported %u; eye targets disabled",
                (unsigned)g_viewConfigs.size());
            return false;
        }

        g_eyeChains.resize(g_viewConfigs.size());
        for (uint32_t i = 0; i < (uint32_t)g_eyeChains.size(); ++i)
        {
            EyeChain& eye = g_eyeChains[i];
            // During bring-up, allow the known-good game backbuffer shape to
            // isolate a SteamVR/D3D11 issue with the recommended near-square
            // eye size from a multiple-swapchain issue.
            eye.width = testWidth ? testWidth : g_viewConfigs[i].recommendedImageRectWidth;
            eye.height = testHeight ? testHeight : g_viewConfigs[i].recommendedImageRectHeight;
            char name[32];
            snprintf(name, sizeof(name), "eye %u", i);
            if (!CreateChain(eye.width, eye.height, eye.chain, eye.images, eye.rtvs, name))
            {
                for (EyeChain& made : g_eyeChains)
                    DestroyChain(made.chain, made.images, made.rtvs);
                g_eyeChains.clear();
                return false;
            }
        }
        LOG("M2: stereo eye swapchains ready (projection submission held until per-eye rendering is ready)");
        return true;
    }

    bool CreateStereoArrayChain()
    {
        if (g_viewConfigs.size() != 2)
            return false;
        g_stereoW = g_viewConfigs[0].recommendedImageRectWidth;
        g_stereoH = g_viewConfigs[0].recommendedImageRectHeight;
        XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        ci.format = g_xrFormat;
        ci.sampleCount = 1;
        ci.width = g_stereoW;
        ci.height = g_stereoH;
        ci.faceCount = 1;
        ci.arraySize = 2;
        ci.mipCount = 1;
        XrResult r = xrCreateSwapchain(g_session, &ci, &g_stereoChain);
        if (XR_FAILED(r))
        {
            LOG("M2: xrCreateSwapchain(stereo array) failed: %s", XrStr(r));
            return false;
        }
        uint32_t count = 0;
        xrEnumerateSwapchainImages(g_stereoChain, 0, &count, nullptr);
        std::vector<XrSwapchainImageD3D11KHR> xrImages(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        r = xrEnumerateSwapchainImages(g_stereoChain, count, &count,
            reinterpret_cast<XrSwapchainImageBaseHeader*>(xrImages.data()));
        if (XR_FAILED(r))
            return false;
        g_stereoImages.clear();
        for (auto& image : xrImages)
            g_stereoImages.push_back(image.texture);
        g_stereoRtvs.resize(count);
        for (auto& pair : g_stereoRtvs)
            pair = {nullptr, nullptr};
        LOG("M2: stereo 2-slice array swapchain created: %ux%u, %u images",
            g_stereoW, g_stereoH, count);
        return true;
    }

    ID3D11RenderTargetView* GetStereoRtv(uint32_t image, uint32_t eye)
    {
        if (image >= g_stereoImages.size() || eye >= 2)
            return nullptr;
        if (!g_stereoRtvs[image][eye])
        {
            D3D11_RENDER_TARGET_VIEW_DESC desc{};
            desc.Format = (DXGI_FORMAT)g_xrFormat;
            desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
            desc.Texture2DArray.MipSlice = 0;
            desc.Texture2DArray.FirstArraySlice = eye;
            desc.Texture2DArray.ArraySize = 1;
            if (FAILED(g_device->CreateRenderTargetView(g_stereoImages[image], &desc,
                                                        &g_stereoRtvs[image][eye])))
                return nullptr;
        }
        return g_stereoRtvs[image][eye];
    }

    bool EnsureEyeCaches(const D3D11_TEXTURE2D_DESC& source)
    {
        if (g_eyeCache[0] && g_eyeCacheDesc.Width == source.Width &&
            g_eyeCacheDesc.Height == source.Height && g_eyeCacheDesc.Format == source.Format)
            return true;
        for (auto*& texture : g_eyeCache)
        {
            if (texture) texture->Release();
            texture = nullptr;
        }
        for (auto*& rtv : g_eyeCacheRtvs)
        {
            if (rtv) rtv->Release();
            rtv = nullptr;
        }
        D3D11_TEXTURE2D_DESC desc = source;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        desc.CPUAccessFlags = 0;
        desc.MiscFlags = 0;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        if (FAILED(g_device->CreateTexture2D(&desc, nullptr, &g_eyeCache[0])) ||
            FAILED(g_device->CreateTexture2D(&desc, nullptr, &g_eyeCache[1])) ||
            FAILED(g_device->CreateRenderTargetView(g_eyeCache[0], nullptr, &g_eyeCacheRtvs[0])) ||
            FAILED(g_device->CreateRenderTargetView(g_eyeCache[1], nullptr, &g_eyeCacheRtvs[1])))
        {
            LOG("M2: failed to create persistent eye frame caches");
            return false;
        }
        g_eyeCacheDesc = desc;
        g_eyeHasImage[0] = g_eyeHasImage[1] = false;
        g_stereoValidationDone = false;
        LOG("M2: persistent eye frame caches created: %ux%u", desc.Width, desc.Height);
        return true;
    }

    void ValidateStereoImagesOnce()
    {
        if (g_stereoValidationDone || !g_eyeHasImage[0] || !g_eyeHasImage[1] ||
            !g_eyeCache[0] || !g_eyeCache[1])
            return;
        if (g_eyeCacheDesc.Format != DXGI_FORMAT_R8G8B8A8_UNORM &&
            g_eyeCacheDesc.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
        {
            LOG("M2 VALIDATION: unsupported eye format %u", (unsigned)g_eyeCacheDesc.Format);
            g_stereoValidationDone = true;
            return;
        }

        D3D11_TEXTURE2D_DESC stagingDesc = g_eyeCacheDesc;
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.BindFlags = 0;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        stagingDesc.MiscFlags = 0;
        ID3D11Texture2D* staging[2]{};
        if (FAILED(g_device->CreateTexture2D(&stagingDesc, nullptr, &staging[0])) ||
            FAILED(g_device->CreateTexture2D(&stagingDesc, nullptr, &staging[1])))
        {
            if (staging[0]) staging[0]->Release();
            if (staging[1]) staging[1]->Release();
            LOG("M2 VALIDATION: staging allocation failed");
            g_stereoValidationDone = true;
            return;
        }

        g_context->CopyResource(staging[0], g_eyeCache[0]);
        g_context->CopyResource(staging[1], g_eyeCache[1]);
        D3D11_MAPPED_SUBRESOURCE mapped[2]{};
        const HRESULT hr0 = g_context->Map(staging[0], 0, D3D11_MAP_READ, 0, &mapped[0]);
        const HRESULT hr1 = g_context->Map(staging[1], 0, D3D11_MAP_READ, 0, &mapped[1]);
        if (SUCCEEDED(hr0) && SUCCEEDED(hr1))
        {
            unsigned long long rgbDelta = 0;
            unsigned samples = 0, changed = 0;
            const unsigned step = 16;
            for (unsigned y = step / 2; y < g_eyeCacheDesc.Height; y += step)
            {
                const auto* left = static_cast<const unsigned char*>(mapped[0].pData) + y * mapped[0].RowPitch;
                const auto* right = static_cast<const unsigned char*>(mapped[1].pData) + y * mapped[1].RowPitch;
                for (unsigned x = step / 2; x < g_eyeCacheDesc.Width; x += step)
                {
                    const unsigned o = x * 4;
                    unsigned pixelDelta = 0;
                    for (unsigned c = 0; c < 3; ++c)
                    {
                        const int d = (int)left[o + c] - (int)right[o + c];
                        pixelDelta += (unsigned)(d < 0 ? -d : d);
                    }
                    rgbDelta += pixelDelta;
                    if (pixelDelta > 12) ++changed;
                    ++samples;
                }
            }
            const double meanChannelDelta = samples ? (double)rgbDelta / (samples * 3.0) : 0.0;
            const double changedPercent = samples ? (100.0 * changed / samples) : 0.0;
            LOG("M2 VALIDATION: distinct eye pixels mean RGB delta=%.3f, changed samples=%.1f%% (%u/%u)",
                meanChannelDelta, changedPercent, changed, samples);
        }
        else
        {
            LOG("M2 VALIDATION: staging map failed (0x%08X, 0x%08X)",
                (unsigned)hr0, (unsigned)hr1);
        }
        if (SUCCEEDED(hr0)) g_context->Unmap(staging[0], 0);
        if (SUCCEEDED(hr1)) g_context->Unmap(staging[1], 0);
        staging[0]->Release();
        staging[1]->Release();
        g_stereoValidationDone = true;
    }

    void ResetPreparedFrame()
    {
        g_preparedShouldRender.store(false, std::memory_order_release);
        g_preparedFrame.begun = false;
        g_preparedFrame.viewsValid = false;
        g_preparedFrame.viewCount = 0;
    }

    void EndPreparedFrameWithoutLayers(const char* reason)
    {
        if (!g_preparedFrame.begun || g_session == XR_NULL_HANDLE)
        {
            ResetPreparedFrame();
            return;
        }
        XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
        end.displayTime = g_preparedFrame.state.predictedDisplayTime;
        end.environmentBlendMode = g_blendMode;
        const XrResult r = xrEndFrame(g_session, &end);
        if (XR_FAILED(r))
        {
            ++g_frameOrderFailures;
            LOG("timing: empty xrEndFrame during %s failed: %s", reason, XrStr(r));
        }
        ResetPreparedFrame();
    }

    // -------------------------------------------------------------- events

    void PollEvents()
    {
        XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
        while (xrPollEvent(g_instance, &ev) == XR_SUCCESS)
        {
            switch (ev.type)
            {
            case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED:
            {
                auto& sc = *reinterpret_cast<XrEventDataSessionStateChanged*>(&ev);
                g_sessionState = sc.state;
                strcpy_s(g_status.sessionState, SessionStateName(sc.state));
                LOG("XR session state -> %s", SessionStateName(sc.state));
                if (sc.state != XR_SESSION_STATE_FOCUSED)
                    StopControllerHaptics();
                if (sc.state == XR_SESSION_STATE_READY)
                {
                    XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
                    bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    XrResult r = xrBeginSession(g_session, &bi);
                    if (XR_SUCCEEDED(r))
                        g_sessionRunning = true;
                    else
                        LOG("xrBeginSession failed: %s", XrStr(r));
                }
                else if (sc.state == XR_SESSION_STATE_STOPPING)
                {
                    StopControllerHaptics();
                    EndPreparedFrameWithoutLayers("session stopping");
                    xrEndSession(g_session);
                    g_sessionRunning = false;
                }
                else if (sc.state == XR_SESSION_STATE_EXITING || sc.state == XR_SESSION_STATE_LOSS_PENDING)
                {
                    StopControllerHaptics();
                    ResetPreparedFrame();
                    g_sessionRunning = false;
                    Fail("The VR runtime ended the session (headset off / SteamVR closed?)");
                }
                break;
            }
            case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
                StopControllerHaptics();
                ResetPreparedFrame();
                g_sessionRunning = false;
                Fail("The OpenXR runtime is shutting down");
                break;
            case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED:
            {
                auto logProfile = [](XrPath hand, const char* label) {
                    XrInteractionProfileState state{XR_TYPE_INTERACTION_PROFILE_STATE};
                    if (XR_FAILED(xrGetCurrentInteractionProfile(g_session, hand, &state)) ||
                        state.interactionProfile == XR_NULL_PATH)
                    {
                        LOG("controller profile %s: unavailable", label);
                        return;
                    }
                    char path[XR_MAX_PATH_LENGTH]{};
                    uint32_t written = 0;
                    if (XR_SUCCEEDED(xrPathToString(g_instance, state.interactionProfile,
                        (uint32_t)sizeof(path), &written, path)))
                        LOG("controller profile %s: %s", label, path);
                };
                logProfile(g_leftHandPath, "left");
                logProfile(g_rightHandPath, "right");
                if (g_actMenu != XR_NULL_HANDLE)
                {
                    XrBoundSourcesForActionEnumerateInfo info{
                        XR_TYPE_BOUND_SOURCES_FOR_ACTION_ENUMERATE_INFO};
                    info.action = g_actMenu;
                    uint32_t count = 0;
                    if (XR_SUCCEEDED(xrEnumerateBoundSourcesForAction(
                            g_session, &info, 0, &count, nullptr)) && count > 0)
                    {
                        std::vector<XrPath> sources(count);
                        if (XR_SUCCEEDED(xrEnumerateBoundSourcesForAction(
                                g_session, &info, count, &count, sources.data())))
                        {
                            for (XrPath source : sources)
                            {
                                char path[XR_MAX_PATH_LENGTH]{};
                                uint32_t written = 0;
                                if (XR_SUCCEEDED(xrPathToString(g_instance, source,
                                        (uint32_t)sizeof(path), &written, path)))
                                    LOG("Menu/Start bound source: %s", path);
                            }
                        }
                    }
                    else
                        LOG("Menu/Start bound source: none");
                }
                break;
            }
            default:
                break;
            }
            ev = {XR_TYPE_EVENT_DATA_BUFFER};
        }
    }

    bool TryRecenter(XrTime time)
    {
        XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
        if (XR_FAILED(xrLocateSpace(g_viewSpace, g_localSpace, time, &loc)))
            return false;
        constexpr XrSpaceLocationFlags need =
            XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
        if ((loc.locationFlags & need) != need)
            return false;
        // Keep only the yaw (left/right) part of the head orientation so the
        // screen is level, straight ahead of wherever the user is facing.
        const XrVector3f fwd = Rotate(loc.pose.orientation, {0, 0, -1});
        float yaw = 0.0f;
        if (fabsf(fwd.x) > 1e-4f || fabsf(fwd.z) > 1e-4f)
            yaw = atan2f(-fwd.x, -fwd.z);
        g_centerRot = {0, sinf(yaw * 0.5f), 0, cosf(yaw * 0.5f)};
        g_centerPos = loc.pose.position;
        g_haveCenter = true;
        LOG("screen recentered (yaw %.1f deg)", yaw * 57.2958f);
        return true;
    }

    // Store the head pose for the game camera hook to read. Called once near
    // the end of Present with the NEXT frame's predicted display time, so Halo
    // renders the upcoming image from its matching pose instead of a stale one.
    void CaptureHeadPose(XrTime time)
    {
        XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
        if (XR_FAILED(xrLocateSpace(g_viewSpace, g_localSpace, time, &loc)))
            return;
        constexpr XrSpaceLocationFlags need =
            XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
        if ((loc.locationFlags & need) != need)
            return;
        if (!NormalizeTrackedPose(loc.pose))
            return;
        EnterCriticalSection(&g_headCs);
        // Filter exactly once per OpenXR frame. CamCopyHook can run several
        // times inside that frame, so smoothing there would compound and vary
        // with Halo's number of camera passes.
        const float smoothing = std::clamp(g_config.headset_smoothing, 0.0f, 0.10f);
        g_headPose = g_headPoseValid && smoothing > 0.0f
            ? SmoothTrackedPose(loc.pose, g_headPose, smoothing)
            : loc.pose;
        g_headPoseValid = true;
        LeaveCriticalSection(&g_headCs);

        // Runtime proof for headset logs: successful pose sampling must equal
        // the OpenXR/game presentation rate. Camera-copy transforms are logged
        // independently in game.cpp and normally exceed this count.
        static uint64_t rateStartMs = 0;
        static unsigned samples = 0;
        ++samples;
        const uint64_t now = GetTickCount64();
        if (!rateStartMs) rateStartMs = now;
        else if (now - rateStartMs >= 10000)
        {
            LOG("M1 timing: HMD pose samples %.1f/sec (next-display prediction for each game frame)",
                samples * 1000.0 / (now - rateStartMs));
            samples = 0;
            rateStartMs = now;
        }
    }

    // Create the crosshair swapchain on first use (lazily, once the session is
    // running — SteamVR presented pre-session eye chains black) and paint the
    // reticle into it a single time.  Four cyan arc segments and short inner
    // ticks echo Halo 3's original blue CHUD reticle, but at a VR-friendly
    // angular size. A dark-blue outline keeps it legible without a black disc.
    // Paint the reticle image in the given color (0-1 per channel). The bright
    // arcs/ticks take the color; the outline is a darkened version of the same
    // hue so it reads at any color. Called on first use and whenever the color
    // changes (user edit, or the enemy-red switch below) — not per frame.
    bool PaintReticle(float cr, float cg, float cb, float opacity)
    {
        std::vector<uint32_t> px(kReticleSize * kReticleSize);
        const float c = (kReticleSize - 1) * 0.5f;
        const float scale = kReticleSize / 64.0f;
        // coverage: 1 inside the shape, 0 outside, ~1px linear edge for AA
        auto cov = [scale](float d, float halfWidth) {
            const float v = (halfWidth - d) * scale + 0.5f;
            return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
        };
        // Bright fill = the requested color at full brightness; outline = the
        // same color at ~28% (a legible dark rim without a black disc).
        const float brR=fminf(cr,1.0f)*255.0f, brG=fminf(cg,1.0f)*255.0f,
                    brB=fminf(cb,1.0f)*255.0f;
        const float olR=brR*0.28f, olG=brG*0.28f, olB=brB*0.28f;
        for (uint32_t y = 0; y < kReticleSize; ++y)
            for (uint32_t x = 0; x < kReticleSize; ++x)
            {
                const float dx = (x - c) / scale, dy = (y - c) / scale;
                const float r = sqrtf(dx * dx + dy * dy);
                const float ax=fabsf(dx), ay=fabsf(dy);
                const bool cardinal=ax>ay*1.22f || ay>ax*1.22f;
                const float dRing=fabsf(r-19.0f);
                const float arc=cardinal?cov(dRing,1.55f):0.0f;
                const float arcOutline=cardinal?cov(dRing,3.0f):0.0f;
                const float tickDistance=(ax>ay)?ax:ay;
                const float tickWidth=(ax>ay)?ay:ax;
                const float tick=(tickDistance>=7.0f && tickDistance<=11.5f)?cov(tickWidth,1.2f):0.0f;
                const float tickOutline=(tickDistance>=5.8f && tickDistance<=12.7f)?cov(tickWidth,2.6f):0.0f;
                const float bright=fmaxf(arc,tick);
                const float outline=fmaxf(arcOutline,tickOutline);
                const uint32_t r8=(uint32_t)(opacity*
                    fminf(255.0f,olR*outline+brR*bright)+0.5f);
                const uint32_t g8=(uint32_t)(opacity*
                    fminf(255.0f,olG*outline+brG*bright)+0.5f);
                const uint32_t b8=(uint32_t)(opacity*
                    fminf(255.0f,olB*outline+brB*bright)+0.5f);
                const uint32_t a8=(uint32_t)(opacity*outline*255.0f+0.5f);
                // OpenXR's preferred swapchain is RGBA8 on this runtime.
                px[y*kReticleSize+x]=(a8<<24)|(b8<<16)|(g8<<8)|r8;
            }
        uint32_t idx = 0;
        XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wi.timeout = 1000000000;
        XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        if (XR_FAILED(xrAcquireSwapchainImage(g_reticleChain, &ai, &idx)) ||
            XR_FAILED(xrWaitSwapchainImage(g_reticleChain, &wi)))
            return false;
        g_context->UpdateSubresource(g_reticleImages[idx], 0, nullptr, px.data(),
                                     kReticleSize * 4, 0);
        xrReleaseSwapchainImage(g_reticleChain, &ri);
        return true;
    }

    bool EnsureAuthoredReticleTexture()
    {
        if (g_authoredReticleTexture && g_authoredReticleRtv)
            return true;
        if (!g_device)
            return false;

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = kReticleSize;
        desc.Height = kReticleSize;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(g_device->CreateTexture2D(&desc, nullptr,
                                             &g_authoredReticleTexture)) ||
            FAILED(g_device->CreateRenderTargetView(g_authoredReticleTexture,
                                                     nullptr,
                                                     &g_authoredReticleRtv)))
        {
            if (g_authoredReticleRtv)
            {
                g_authoredReticleRtv->Release();
                g_authoredReticleRtv = nullptr;
            }
            if (g_authoredReticleTexture)
            {
                g_authoredReticleTexture->Release();
                g_authoredReticleTexture = nullptr;
            }
            return false;
        }
        LOG("M3: authored crosshair capture target ready (%ux%u)",
            kReticleSize, kReticleSize);
        return true;
    }

    bool EnsureReticleChain()
    {
        static bool failed = false;
        if (failed)
            return false;
        if (g_reticleChain == XR_NULL_HANDLE)
        {
            if (!CreateChain(kReticleSize, kReticleSize, g_reticleChain, g_reticleImages,
                             g_reticleRtvs, "crosshair"))
            {
                failed = true;
                return false;
            }
        }
        const bool authoredThisFrame =
            g_authoredReticleReady &&
            g_authoredReticleSerial == g_preparedFrame.serial;
        if (authoredThisFrame)
            return true;

        // Halo can omit the authored widget in some of the repeated FP passes
        // of one displayed frame. Retain the last authored image across that
        // short gap; otherwise the render thread alternates upload/clear and
        // pays a swapchain repaint every frame. A genuine death/loading gap
        // still clears once after this small grace window.
        constexpr uint64_t kAuthoredReticleGraceFrames = 2;
        const bool authoredCaptureRecent = g_authoredReticleSerial != 0 &&
            g_preparedFrame.serial >= g_authoredReticleSerial &&
            g_preparedFrame.serial - g_authoredReticleSerial <=
                kAuthoredReticleGraceFrames;
        if (g_reticleContainsAuthored && authoredCaptureRecent)
            return true;

        // Halo can stop drawing its authored widget during death and other
        // non-gameplay states. Keep the old procedural fallback fully
        // transparent so it cannot appear close to the viewer; authored
        // crosshairs use UploadAuthoredReticle below and are unaffected.
        // The procedural reticle is normally transparent: titles with the
        // authored CHUD capture (Halo 3) get their visible crosshair from
        // UploadAuthoredReticle, and a visible procedural fallback could flash
        // during death/loading gaps. The private ODST camera core installs no
        // authored capture yet, so there the procedural reticle IS the
        // crosshair and must be opaque to be seen at all.
        const float kProceduralOpacity =
            Game_IsCameraOnlyBringup() ? 1.0f : 0.0f;
        const bool enemy = g_reticleEnemy.load(std::memory_order_relaxed);
        const float wantR = enemy ? 1.0f : g_config.reticle_r;
        const float wantG = enemy ? 0.18f : g_config.reticle_g;
        const float wantB = enemy ? 0.14f : g_config.reticle_b;
        // Repaint only when the desired color OR opacity changed (compositor
        // keeps showing the last released image, so a static reticle costs
        // nothing per frame). Opacity is included so a Halo 3 -> ODST transition
        // repaints the swapchain from transparent to visible.
        const bool colorChanged = g_reticleContainsAuthored ||
            g_reticleEnemyPainted != enemy ||
            g_reticlePaintedOpacity != kProceduralOpacity ||
            (!enemy && (g_reticlePaintedColor[0] != g_config.reticle_r ||
                        g_reticlePaintedColor[1] != g_config.reticle_g ||
                        g_reticlePaintedColor[2] != g_config.reticle_b));
        if (!colorChanged)
            return true;
        const bool clearingAuthored = g_reticleContainsAuthored;
        if (!PaintReticle(wantR, wantG, wantB, kProceduralOpacity))
        {
            failed = true;
            return false;
        }
        g_reticlePaintedColor[0]=wantR;
        g_reticlePaintedColor[1]=wantG;
        g_reticlePaintedColor[2]=wantB;
        g_reticlePaintedOpacity=kProceduralOpacity;
        g_reticleEnemyPainted=enemy;
        g_reticleContainsAuthored=false;
        if (clearingAuthored)
            LOG("M3: authored crosshair cleared after capture stopped");
        return true;
    }

    bool UploadAuthoredReticle()
    {
        if (!g_authoredReticleReady ||
            g_authoredReticleSerial != g_preparedFrame.serial ||
            !g_authoredReticleTexture ||
            g_reticleChain == XR_NULL_HANDLE)
            return false;
        if (g_authoredReticleUploadedSerial == g_authoredReticleSerial)
            return true;

        uint32_t index = 0;
        XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wait.timeout = 1000000000;
        XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        if (XR_FAILED(xrAcquireSwapchainImage(g_reticleChain, &acquire, &index)) ||
            XR_FAILED(xrWaitSwapchainImage(g_reticleChain, &wait)))
            return false;

        D3D11_TEXTURE2D_DESC sourceDesc{};
        g_authoredReticleTexture->GetDesc(&sourceDesc);
        const bool copied = Blit(g_authoredReticleTexture, sourceDesc,
                                 g_reticleImages[index], kReticleSize,
                                 kReticleSize,
                                 GetRtv(g_reticleImages, g_reticleRtvs, index));
        xrReleaseSwapchainImage(g_reticleChain, &release);
        if (!copied)
            return false;
        g_authoredReticleUploadedSerial = g_authoredReticleSerial;
        g_reticleContainsAuthored = true;
        return true;
    }

    XrCompositionLayerQuad MakeQuad(XrSwapchain chain, int32_t imgW, int32_t imgH,
                                    float widthMeters, float distMeters, float yOffset,
                                    XrCompositionLayerFlags flags, bool headLocked)
    {
        XrCompositionLayerQuad q{XR_TYPE_COMPOSITION_LAYER_QUAD};
        q.layerFlags = flags;
        q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        q.subImage.swapchain = chain;
        q.subImage.imageRect = {{0, 0}, {imgW, imgH}};
        q.subImage.imageArrayIndex = 0;
        if (headLocked)
        {
            // Pinned in front of the head (VIEW space); the game camera looks around.
            q.space = g_viewSpace;
            q.pose.orientation = {0, 0, 0, 1};
            q.pose.position = {0, yOffset, -distMeters};
        }
        else
        {
            q.space = g_localSpace;
            q.pose.orientation = g_centerRot;
            const XrVector3f off = Rotate(g_centerRot, {0, yOffset, -distMeters});
            q.pose.position = {g_centerPos.x + off.x, g_centerPos.y + off.y, g_centerPos.z + off.z};
        }
        q.size = {widthMeters, widthMeters * (float)imgH / (float)imgW};
        return q;
    }

    float UpdatePauseTransition()
    {
        enum class Phase { Idle, FadeOut, FadeIn };
        static Phase phase = Phase::Idle;
        static bool targetPaused = false;
        static uint64_t phaseStartMs = 0;
        const uint64_t now = GetTickCount64();
        const int requested = g_pauseRequest.exchange(-1);
        if (requested >= 0)
        {
            targetPaused = requested != 0;
            phase = Phase::FadeOut;
            phaseStartMs = now;
            g_requestedHaptics = 0.0f;
            LOG("pause transition: fade out -> %s",
                targetPaused ? "head-locked 2D" : "stereo 3D");
        }
        if (phase == Phase::Idle)
            return 0.0f;

        constexpr float fadeMs = 200.0f;
        float t = static_cast<float>(now - phaseStartMs) / fadeMs;
        if (phase == Phase::FadeOut)
        {
            if (t < 1.0f)
                return t;
            g_pausePresentation = targetPaused;
            if (!targetPaused)
            {
                Game_Recenter();
                g_haveCenter = false;
            }
            phase = Phase::FadeIn;
            phaseStartMs = now;
            LOG("pause transition: presentation switched to %s",
                targetPaused ? "head-locked 2D" : "stereo 3D");
            return 1.0f;
        }
        if (t < 1.0f)
            return 1.0f - t;
        phase = Phase::Idle;
        LOG("pause transition: comfort fade complete");
        return 0.0f;
    }

    bool AppendComfortFade(float alpha, XrCompositionLayerQuad& quad,
                           std::vector<XrCompositionLayerBaseHeader*>& layers)
    {
        if (alpha <= 0.001f)
            return true;
        if (g_fadeChain == XR_NULL_HANDLE &&
            !CreateChain(4, 4, g_fadeChain, g_fadeImages, g_fadeRtvs, "comfort fade"))
            return false;

        uint32_t idx = 0;
        XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wait.timeout = 1000000000;
        XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        if (XR_FAILED(xrAcquireSwapchainImage(g_fadeChain, &acquire, &idx)) ||
            XR_FAILED(xrWaitSwapchainImage(g_fadeChain, &wait)))
            return false;
        const float black[4] = {0.0f, 0.0f, 0.0f,
            std::clamp(alpha, 0.0f, 1.0f)};
        g_context->ClearRenderTargetView(GetRtv(g_fadeImages, g_fadeRtvs, idx), black);
        xrReleaseSwapchainImage(g_fadeChain, &release);
        quad = MakeQuad(g_fadeChain, 4, 4, 20.0f, 0.25f, 0.0f,
            XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
                XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT,
            true);
        layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&quad));
        return true;
    }

    // ---------------------------------------------------------------- init

    // Part 1 (background thread): create the OpenXR instance and find the
    // headset. No D3D device needed here, so it can run while the game loads.
    bool InitInstance()
    {
        uint32_t extensionCount = 0;
        std::vector<XrExtensionProperties> availableExtensions;
        if (XR_SUCCEEDED(xrEnumerateInstanceExtensionProperties(
                nullptr, 0, &extensionCount, nullptr)) && extensionCount > 0)
        {
            availableExtensions.resize(extensionCount);
            for (auto& extension : availableExtensions)
                extension.type = XR_TYPE_EXTENSION_PROPERTIES;
            xrEnumerateInstanceExtensionProperties(nullptr, extensionCount,
                &extensionCount, availableExtensions.data());
        }
        auto hasExtension = [&](const char* name) {
            return std::any_of(availableExtensions.begin(), availableExtensions.end(),
                [&](const XrExtensionProperties& extension) {
                    return strcmp(extension.extensionName, name) == 0;
                });
        };

        std::vector<const char*> enabledExtensions{
            XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
        g_touchProProfileEnabled =
            hasExtension(XR_FB_TOUCH_CONTROLLER_PRO_EXTENSION_NAME);
        if (g_touchProProfileEnabled)
            enabledExtensions.push_back(XR_FB_TOUCH_CONTROLLER_PRO_EXTENSION_NAME);

        XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
        strcpy_s(ici.applicationInfo.applicationName, "HaloMCCVR");
        ici.applicationInfo.applicationVersion = 1;
        strcpy_s(ici.applicationInfo.engineName, "halo3xr");
        ici.applicationInfo.apiVersion = XR_API_VERSION_1_0;
        ici.enabledExtensionCount = (uint32_t)enabledExtensions.size();
        ici.enabledExtensionNames = enabledExtensions.data();
        LOG("creating OpenXR instance (this can take a while as SteamVR starts)...");
        XrResult r = xrCreateInstance(&ici, &g_instance);
        if (XR_FAILED(r))
        {
            Fail("No OpenXR runtime available. Is SteamVR installed and set as the\n"
                 "default OpenXR runtime? (SteamVR -> Settings -> OpenXR)", r);
            return false;
        }

        XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
        xrGetInstanceProperties(g_instance, &ip);
        snprintf(g_status.runtime, sizeof(g_status.runtime), "%s %u.%u.%u", ip.runtimeName,
                 XR_VERSION_MAJOR(ip.runtimeVersion), XR_VERSION_MINOR(ip.runtimeVersion),
                 XR_VERSION_PATCH(ip.runtimeVersion));
        LOG("OpenXR runtime: %s", g_status.runtime);
        LOG("Quest Touch Pro interaction profile: %s",
            g_touchProProfileEnabled ? "enabled" : "not advertised by runtime");

        XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
        sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        r = xrGetSystem(g_instance, &sgi, &g_systemId);
        if (XR_FAILED(r))
        {
            Fail("No headset found. Make sure the headset is connected and SteamVR is running", r);
            return false;
        }
        LOG("OpenXR instance ready; headset found");
        return true;
    }

    // M3 gamepad-replacement actions: sticks, buttons, triggers, grips.
    XrAction g_actMove = XR_NULL_HANDLE, g_actTurn = XR_NULL_HANDLE;
    XrAction g_actTrigL = XR_NULL_HANDLE, g_actTrigR = XR_NULL_HANDLE;
    XrAction g_actGripL = XR_NULL_HANDLE, g_actGripR = XR_NULL_HANDLE;
    XrAction g_actA = XR_NULL_HANDLE, g_actB = XR_NULL_HANDLE;
    XrAction g_actX = XR_NULL_HANDLE, g_actY = XR_NULL_HANDLE;
    XrAction g_actClickL = XR_NULL_HANDLE, g_actClickR = XR_NULL_HANDLE;
    VrPadState g_padState{};

    bool CreateControllerActions()
    {
        XrActionSetCreateInfo setInfo{XR_TYPE_ACTION_SET_CREATE_INFO};
        strcpy_s(setInfo.actionSetName, "gameplay");
        strcpy_s(setInfo.localizedActionSetName, "Halo MCC VR Gameplay");
        setInfo.priority = 0;
        if (XR_FAILED(xrCreateActionSet(g_instance, &setInfo, &g_gameplayActions)))
        {
            LOG("M3: failed to create OpenXR gameplay action set");
            return false;
        }
        if (XR_FAILED(xrStringToPath(g_instance, "/user/hand/right", &g_rightHandPath)) ||
            XR_FAILED(xrStringToPath(g_instance, "/user/hand/left", &g_leftHandPath)))
            return false;

        XrActionCreateInfo actionInfo{XR_TYPE_ACTION_CREATE_INFO};
        actionInfo.actionType = XR_ACTION_TYPE_POSE_INPUT;
        strcpy_s(actionInfo.actionName, "right_aim_pose");
        strcpy_s(actionInfo.localizedActionName, "Right Hand Aim Pose");
        actionInfo.countSubactionPaths = 1;
        actionInfo.subactionPaths = &g_rightHandPath;
        if (XR_FAILED(xrCreateAction(g_gameplayActions, &actionInfo, &g_rightAimAction)))
        {
            LOG("M3: failed to create right-controller pose action");
            return false;
        }
        strcpy_s(actionInfo.actionName, "left_aim_pose");
        strcpy_s(actionInfo.localizedActionName, "Left Hand Aim Pose");
        actionInfo.subactionPaths = &g_leftHandPath;
        if (XR_FAILED(xrCreateAction(g_gameplayActions, &actionInfo, &g_leftAimAction)))
            g_leftAimAction = XR_NULL_HANDLE; // non-fatal: D-pad gesture falls back to right

        auto makeAction = [&](XrAction& out, XrActionType type, const char* name,
                              const char* label) {
            XrActionCreateInfo ai{XR_TYPE_ACTION_CREATE_INFO};
            ai.actionType = type;
            strcpy_s(ai.actionName, name);
            strcpy_s(ai.localizedActionName, label);
            if (XR_FAILED(xrCreateAction(g_gameplayActions, &ai, &out)))
                out = XR_NULL_HANDLE;
        };
        makeAction(g_actMove,   XR_ACTION_TYPE_VECTOR2F_INPUT, "move",       "Move (left stick)");
        makeAction(g_actTurn,   XR_ACTION_TYPE_VECTOR2F_INPUT, "turn",       "Turn (right stick)");
        makeAction(g_actTrigL,  XR_ACTION_TYPE_FLOAT_INPUT,    "trigger_l",  "Left Trigger");
        makeAction(g_actTrigR,  XR_ACTION_TYPE_FLOAT_INPUT,    "trigger_r",  "Right Trigger");
        makeAction(g_actGripL,  XR_ACTION_TYPE_FLOAT_INPUT,    "grip_l",     "Left Grip");
        makeAction(g_actGripR,  XR_ACTION_TYPE_FLOAT_INPUT,    "grip_r",     "Right Grip");
        makeAction(g_actA,      XR_ACTION_TYPE_BOOLEAN_INPUT,  "btn_a",      "A (right lower)");
        makeAction(g_actB,      XR_ACTION_TYPE_BOOLEAN_INPUT,  "btn_b",      "B (right upper)");
        makeAction(g_actX,      XR_ACTION_TYPE_BOOLEAN_INPUT,  "btn_x",      "X (left lower)");
        makeAction(g_actY,      XR_ACTION_TYPE_BOOLEAN_INPUT,  "btn_y",      "Y (left upper)");
        makeAction(g_actClickL, XR_ACTION_TYPE_BOOLEAN_INPUT,  "click_l",    "Left Stick Click");
        makeAction(g_actClickR, XR_ACTION_TYPE_BOOLEAN_INPUT,  "click_r",    "Right Stick Click");
        makeAction(g_actMenu,   XR_ACTION_TYPE_BOOLEAN_INPUT,  "menu",       "Menu / Start");

        XrPath hapticPaths[2] = {g_leftHandPath, g_rightHandPath};
        XrActionCreateInfo hapticInfo{XR_TYPE_ACTION_CREATE_INFO};
        hapticInfo.actionType = XR_ACTION_TYPE_VIBRATION_OUTPUT;
        strcpy_s(hapticInfo.actionName, "game_haptics");
        strcpy_s(hapticInfo.localizedActionName, "Game Haptics");
        hapticInfo.countSubactionPaths = 2;
        hapticInfo.subactionPaths = hapticPaths;
        if (XR_FAILED(xrCreateAction(g_gameplayActions, &hapticInfo, &g_hapticAction)))
        {
            g_hapticAction = XR_NULL_HANDLE;
            LOG("M3: portable haptic output unavailable; controller input remains active");
        }

        // Per-profile suggested bindings. SteamVR remaps these onto PSVR2
        // Sense automatically (users can rebind in SteamVR controller
        // settings); the Touch/Index layouts are the closest templates.
        struct Bind { XrAction action; const char* path; };
        auto suggest = [&](const char* profile, const Bind* binds, size_t count) -> bool {
            XrPath profilePath = XR_NULL_PATH;
            if (XR_FAILED(xrStringToPath(g_instance, profile, &profilePath)))
                return false;
            std::vector<XrActionSuggestedBinding> out;
            for (size_t i = 0; i < count; ++i)
            {
                XrPath p = XR_NULL_PATH;
                if (binds[i].action != XR_NULL_HANDLE &&
                    XR_SUCCEEDED(xrStringToPath(g_instance, binds[i].path, &p)))
                    out.push_back({binds[i].action, p});
            }
            if (out.empty())
                return false;
            XrInteractionProfileSuggestedBinding suggestion{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
            suggestion.interactionProfile = profilePath;
            suggestion.suggestedBindings = out.data();
            suggestion.countSuggestedBindings = (uint32_t)out.size();
            return XR_SUCCEEDED(xrSuggestInteractionProfileBindings(g_instance, &suggestion));
        };

        const Bind touch[] = {
            {g_rightAimAction, "/user/hand/right/input/aim/pose"},
            {g_leftAimAction, "/user/hand/left/input/aim/pose"},
            {g_actMove,   "/user/hand/left/input/thumbstick"},
            {g_actTurn,   "/user/hand/right/input/thumbstick"},
            {g_actTrigL,  "/user/hand/left/input/trigger/value"},
            {g_actTrigR,  "/user/hand/right/input/trigger/value"},
            {g_actGripL,  "/user/hand/left/input/squeeze/value"},
            {g_actGripR,  "/user/hand/right/input/squeeze/value"},
            {g_actA,      "/user/hand/right/input/a/click"},
            {g_actB,      "/user/hand/right/input/b/click"},
            {g_actX,      "/user/hand/left/input/x/click"},
            {g_actY,      "/user/hand/left/input/y/click"},
            {g_actClickL, "/user/hand/left/input/thumbstick/click"},
            {g_actClickR, "/user/hand/right/input/thumbstick/click"},
            {g_actMenu,   "/user/hand/left/input/menu/click"},
            {g_actMenu,   "/user/hand/right/input/system/click"},
            {g_hapticAction, "/user/hand/left/output/haptic"},
            {g_hapticAction, "/user/hand/right/output/haptic"},
        };
        const Bind index[] = {
            {g_rightAimAction, "/user/hand/right/input/aim/pose"},
            {g_leftAimAction, "/user/hand/left/input/aim/pose"},
            {g_actMove,   "/user/hand/left/input/thumbstick"},
            {g_actTurn,   "/user/hand/right/input/thumbstick"},
            {g_actTrigL,  "/user/hand/left/input/trigger/value"},
            {g_actTrigR,  "/user/hand/right/input/trigger/value"},
            {g_actGripL,  "/user/hand/left/input/squeeze/value"},
            {g_actGripR,  "/user/hand/right/input/squeeze/value"},
            {g_actA,      "/user/hand/right/input/a/click"},
            {g_actB,      "/user/hand/right/input/b/click"},
            {g_actX,      "/user/hand/left/input/a/click"},
            {g_actY,      "/user/hand/left/input/b/click"},
            {g_actClickL, "/user/hand/left/input/thumbstick/click"},
            {g_actClickR, "/user/hand/right/input/thumbstick/click"},
            {g_actMenu,   "/user/hand/left/input/system/click"},
            {g_hapticAction, "/user/hand/left/output/haptic"},
            {g_hapticAction, "/user/hand/right/output/haptic"},
        };
        const Bind wmr[] = {
            {g_rightAimAction, "/user/hand/right/input/aim/pose"},
            {g_leftAimAction, "/user/hand/left/input/aim/pose"},
            {g_actMove,   "/user/hand/left/input/thumbstick"},
            {g_actTurn,   "/user/hand/right/input/thumbstick"},
            {g_actTrigL,  "/user/hand/left/input/trigger/value"},
            {g_actTrigR,  "/user/hand/right/input/trigger/value"},
            {g_actGripL,  "/user/hand/left/input/squeeze/click"},
            {g_actGripR,  "/user/hand/right/input/squeeze/click"},
            {g_actClickL, "/user/hand/left/input/thumbstick/click"},
            {g_actClickR, "/user/hand/right/input/thumbstick/click"},
            {g_actMenu,   "/user/hand/left/input/menu/click"},
            {g_hapticAction, "/user/hand/left/output/haptic"},
            {g_hapticAction, "/user/hand/right/output/haptic"},
        };
        const Bind vive[] = {
            {g_rightAimAction, "/user/hand/right/input/aim/pose"},
            {g_leftAimAction, "/user/hand/left/input/aim/pose"},
            {g_actMove,   "/user/hand/left/input/trackpad"},
            {g_actTurn,   "/user/hand/right/input/trackpad"},
            {g_actTrigL,  "/user/hand/left/input/trigger/value"},
            {g_actTrigR,  "/user/hand/right/input/trigger/value"},
            {g_actGripL,  "/user/hand/left/input/squeeze/click"},
            {g_actGripR,  "/user/hand/right/input/squeeze/click"},
            {g_actClickL, "/user/hand/left/input/trackpad/click"},
            {g_actClickR, "/user/hand/right/input/trackpad/click"},
            {g_actMenu,   "/user/hand/left/input/menu/click"},
            {g_hapticAction, "/user/hand/left/output/haptic"},
            {g_hapticAction, "/user/hand/right/output/haptic"},
        };
        const Bind simple[] = {
            {g_rightAimAction, "/user/hand/right/input/grip/pose"},
            {g_leftAimAction, "/user/hand/left/input/grip/pose"},
            {g_actTrigR,  "/user/hand/right/input/select/click"},
            {g_actMenu,   "/user/hand/left/input/menu/click"},
            {g_hapticAction, "/user/hand/left/output/haptic"},
            {g_hapticAction, "/user/hand/right/output/haptic"},
        };
        unsigned accepted = 0;
        if (g_touchProProfileEnabled)
            accepted += suggest("/interaction_profiles/facebook/touch_controller_pro",
                touch, _countof(touch));
        accepted += suggest("/interaction_profiles/oculus/touch_controller", touch, _countof(touch));
        accepted += suggest("/interaction_profiles/valve/index_controller", index, _countof(index));
        accepted += suggest("/interaction_profiles/microsoft/motion_controller", wmr, _countof(wmr));
        accepted += suggest("/interaction_profiles/htc/vive_controller", vive, _countof(vive));
        accepted += suggest("/interaction_profiles/khr/simple_controller", simple, _countof(simple));

        XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
        attach.countActionSets = 1;
        attach.actionSets = &g_gameplayActions;
        if (XR_FAILED(xrAttachSessionActionSets(g_session, &attach)))
        {
            LOG("M3: failed to attach OpenXR gameplay action set");
            return false;
        }
        XrActionSpaceCreateInfo spaceInfo{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        spaceInfo.action = g_rightAimAction;
        spaceInfo.subactionPath = g_rightHandPath;
        spaceInfo.poseInActionSpace.orientation.w = 1.0f;
        if (XR_FAILED(xrCreateActionSpace(g_session, &spaceInfo, &g_rightAimSpace)))
        {
            LOG("M3: failed to create right-controller aim space");
            return false;
        }
        if (g_leftAimAction != XR_NULL_HANDLE)
        {
            spaceInfo.action = g_leftAimAction;
            spaceInfo.subactionPath = g_leftHandPath;
            if (XR_FAILED(xrCreateActionSpace(g_session, &spaceInfo, &g_leftAimSpace)))
                g_leftAimSpace = XR_NULL_HANDLE;
        }
        LOG("M3: right-controller aim action ready (%u interaction profiles accepted)", accepted);
        return true;
    }

    // Two-handed aim state. `latched` = two-hand engaged this frame; decided
    // ONCE per frame in UpdateTwoHandLatch (edge detection can't live in the
    // multi-call aim getter). `active` mirrors it for the menu indicator.
    std::atomic<bool> g_twoHandLatched{false};
    std::atomic<bool> g_twoHandActive{false};

    // Called once per frame from the pose capture. Toggle mode (default): a
    // left-grip press while the left hand is inside the thin/long barrel zone
    // flips two-hand ON; the next left-grip press flips it OFF (anywhere). Hold
    // mode: the zone acquires the hold, which stays engaged until grip release.
    // The OpenXR aim pose sits back at the wrist. Shift the left-hand sample to
    // the palm so activation and the two-hand line are measured at the rendered
    // support hand. The same configured correction is used by game.cpp's left
    // arm target, keeping the visible wrist and the aiming point together.
    XrVector3f LeftHandPoint(const XrPosef& lpose)
    {
        const XrVector3f lfwd = Rotate(lpose.orientation, {0,0,-1});
        // Hand-target correction PLUS the rendered wrist-to-palm depth: the
        // two-hand line and grab zone meet the visible PALM, not the wrist
        // bone the hand target anchors (23:26 headset result).
        const float k = std::clamp(g_config.left_hand_forward_m, -0.15f, 0.30f)
                      + std::clamp(g_config.left_grip_forward_m, -0.05f, 0.25f);
        return {lpose.position.x + lfwd.x*k,
                lpose.position.y + lfwd.y*k,
                lpose.position.z + lfwd.z*k};
    }

    void UpdateTwoHandLatch(bool rightValid, const XrPosef& rpose,
                            bool leftValid, const XrPosef& lpose, float gripL)
    {
        if (!g_config.two_handed_aim || !rightValid || !leftValid)
        { g_twoHandLatched.store(false); return; }
        const XrVector3f rfwd = Rotate(rpose.orientation, {0,0,-1});
        // Grab-zone side nudge: the visible barrel can sit beside the raw aim
        // ray (headset report: the AR's barrel was right of the zone), so the
        // zone axis shifts along the controller's +X by the F1-tuned amount.
        const float zr = std::clamp(g_config.two_hand_zone_right_m, -0.10f, 0.10f);
        const XrVector3f rright = Rotate(rpose.orientation, {1,0,0});
        const XrVector3f origin{rpose.position.x+rright.x*zr,
                                rpose.position.y+rright.y*zr,
                                rpose.position.z+rright.z*zr};
        auto inZoneAt = [&](const XrVector3f& p) -> bool {
            const XrVector3f v{p.x-origin.x, p.y-origin.y, p.z-origin.z};
            const float along = v.x*rfwd.x + v.y*rfwd.y + v.z*rfwd.z;
            const XrVector3f perp{v.x-along*rfwd.x, v.y-along*rfwd.y, v.z-along*rfwd.z};
            const float lateral = sqrtf(perp.x*perp.x+perp.y*perp.y+perp.z*perp.z);
            return along>0.08f && along<0.80f && lateral<0.09f;
        };
        // Register the grab at the PALM point or at the RAW hand position —
        // whichever touches the line. In a cross-body grip the forward palm
        // shift overshoots the barrel (23:17 headset report: the click zone
        // sat past the hand), so the raw sample must also count.
        const bool inZone = inZoneAt(LeftHandPoint(lpose)) || inZoneAt(lpose.position);
        const bool gripHeld = gripL > 0.5f;

        if (g_config.two_hand_toggle)
        {
            static bool prevGrip=false;
            const bool rising = gripHeld && !prevGrip;
            prevGrip = gripHeld;
            if (rising)
            {
                if (g_twoHandLatched.load()) g_twoHandLatched.store(false);      // toggle off
                else if (inZone)             g_twoHandLatched.store(true);       // toggle on
            }
        }
        else // hold mode
        {
            g_twoHandLatched.store(UpdateTwoHandHold(
                g_twoHandLatched.load(), gripHeld, inZone));
        }
    }

    void StopControllerHaptics()
    {
        if (g_session == XR_NULL_HANDLE || g_hapticAction == XR_NULL_HANDLE)
            return;
        XrHapticActionInfo info{XR_TYPE_HAPTIC_ACTION_INFO};
        info.action = g_hapticAction;
        info.subactionPath = g_leftHandPath;
        xrStopHapticFeedback(g_session, &info);
        info.subactionPath = g_rightHandPath;
        xrStopHapticFeedback(g_session, &info);
    }

    void ApplyControllerHaptics(bool trackingValid)
    {
        static bool active = false;
        static uint64_t lastApplyMs = 0;
        static RuntimeMode previousMode = RuntimeMode::Shell;
        const RuntimeMode mode = TitleAdapter_GetRuntimeMode();
        const bool modeAllows = mode == RuntimeMode::Gameplay ||
            mode == RuntimeMode::Vehicle || mode == RuntimeMode::Turret;
        float amplitude = std::clamp(g_requestedHaptics.load(std::memory_order_acquire), 0.0f, 1.0f);
        amplitude *= std::clamp(g_config.haptic_intensity, 0.0f, 1.0f);
        const bool mustStop = amplitude <= 0.0f || !trackingValid || !modeAllows ||
            Menu_IsOpen() || g_sessionState != XR_SESSION_STATE_FOCUSED;
        if (mustStop)
        {
            if (active || mode != previousMode)
                StopControllerHaptics();
            active = false;
            previousMode = mode;
            return;
        }
        previousMode = mode;

        const uint64_t now = GetTickCount64();
        if (active && now - lastApplyMs < 40)
            return;
        XrHapticVibration vibration{XR_TYPE_HAPTIC_VIBRATION};
        vibration.amplitude = amplitude;
        vibration.duration = 50000000;
        vibration.frequency = XR_FREQUENCY_UNSPECIFIED;
        XrHapticActionInfo info{XR_TYPE_HAPTIC_ACTION_INFO};
        info.action = g_hapticAction;
        info.subactionPath = g_leftHandPath;
        xrApplyHapticFeedback(g_session, &info,
            reinterpret_cast<const XrHapticBaseHeader*>(&vibration));
        info.subactionPath = g_rightHandPath;
        xrApplyHapticFeedback(g_session, &info,
            reinterpret_cast<const XrHapticBaseHeader*>(&vibration));
        active = true;
        lastApplyMs = now;
    }

    void CaptureRightControllerPose(XrTime time)
    {
        if (g_gameplayActions == XR_NULL_HANDLE || g_rightAimAction == XR_NULL_HANDLE ||
            g_rightAimSpace == XR_NULL_HANDLE)
            return;
        XrActiveActionSet active{g_gameplayActions, XR_NULL_PATH};
        XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
        sync.countActiveActionSets = 1;
        sync.activeActionSets = &active;
        if (XR_FAILED(xrSyncActions(g_session, &sync)))
            return;
        XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO};
        get.action = g_rightAimAction;
        get.subactionPath = g_rightHandPath;
        XrActionStatePose state{XR_TYPE_ACTION_STATE_POSE};
        bool valid = false;
        XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
        if (XR_SUCCEEDED(xrGetActionStatePose(g_session, &get, &state)) && state.isActive &&
            XR_SUCCEEDED(xrLocateSpace(g_rightAimSpace, g_localSpace, time, &location)))
        {
            constexpr XrSpaceLocationFlags required =
                XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
            valid = (location.locationFlags & required) == required &&
                    NormalizeTrackedPose(location.pose);
        }
        // Left hand: position only matters (D-pad gesture), same locate path.
        bool leftValid = false;
        XrSpaceLocation leftLocation{XR_TYPE_SPACE_LOCATION};
        if (g_leftAimAction != XR_NULL_HANDLE && g_leftAimSpace != XR_NULL_HANDLE)
        {
            get.action = g_leftAimAction;
            get.subactionPath = g_leftHandPath;
            XrActionStatePose leftState{XR_TYPE_ACTION_STATE_POSE};
            if (XR_SUCCEEDED(xrGetActionStatePose(g_session, &get, &leftState)) &&
                leftState.isActive &&
                XR_SUCCEEDED(xrLocateSpace(g_leftAimSpace, g_localSpace, time, &leftLocation)))
            {
                constexpr XrSpaceLocationFlags required =
                    XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
                leftValid = (leftLocation.locationFlags & required) == required &&
                            NormalizeTrackedPose(leftLocation.pose);
            }
        }

        EnterCriticalSection(&g_headCs);
        g_rightAimPoseValid = valid;
        if (valid)
            g_rightAimPose = location.pose;
        g_leftAimPoseValid = leftValid;
        if (leftValid)
            g_leftAimPose = leftLocation.pose;
        LeaveCriticalSection(&g_headCs);
        static bool logged = false;
        if (valid && !logged)
        {
            LOG("M3: right-controller tracking active pose=(%.3f,%.3f,%.3f)",
                location.pose.position.x, location.pose.position.y, location.pose.position.z);
            logged = true;
        }

        // Read the gamepad-replacement actions (already synced above).
        VrPadState pad{};
        auto getV2 = [&](XrAction action, float& outX, float& outY) {
            if (action == XR_NULL_HANDLE) return;
            XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
            gi.action = action;
            XrActionStateVector2f st{XR_TYPE_ACTION_STATE_VECTOR2F};
            if (XR_SUCCEEDED(xrGetActionStateVector2f(g_session, &gi, &st)) && st.isActive)
            { outX = st.currentState.x; outY = st.currentState.y; pad.valid = true; }
        };
        auto getF = [&](XrAction action, float& out) {
            if (action == XR_NULL_HANDLE) return;
            XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
            gi.action = action;
            XrActionStateFloat st{XR_TYPE_ACTION_STATE_FLOAT};
            if (XR_SUCCEEDED(xrGetActionStateFloat(g_session, &gi, &st)) && st.isActive)
            { out = st.currentState; pad.valid = true; }
        };
        auto getB = [&](XrAction action, bool& out) {
            if (action == XR_NULL_HANDLE) return;
            XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
            gi.action = action;
            XrActionStateBoolean st{XR_TYPE_ACTION_STATE_BOOLEAN};
            if (XR_SUCCEEDED(xrGetActionStateBoolean(g_session, &gi, &st)) && st.isActive)
            { out = st.currentState == XR_TRUE; pad.valid = true; }
        };
        getV2(g_actMove, pad.moveX, pad.moveY);
        getV2(g_actTurn, pad.turnX, pad.turnY);
        getF(g_actTrigL, pad.trigL);
        getF(g_actTrigR, pad.trigR);
        getF(g_actGripL, pad.gripL);
        getF(g_actGripR, pad.gripR);
        getB(g_actA, pad.a);
        getB(g_actB, pad.b);
        getB(g_actX, pad.x);
        getB(g_actY, pad.y);
        getB(g_actClickL, pad.clickL);
        getB(g_actClickR, pad.clickR);
        getB(g_actMenu, pad.menu);
        static VrPadState previousPad{};
        static bool previousRawMenu = false;
        static uint64_t odstMenuPulseUntil = 0;
        const uint64_t inputNow = GetTickCount64();
        const bool rawMenuEdge = pad.menu && !previousRawMenu;
        if (rawMenuEdge)
        {
            LOG("controller edge: Menu/Start");
            if (Game_IsCameraOnlyBringup())
                odstMenuPulseUntil = inputNow + 350;
        }
        previousRawMenu = pad.menu;
        if (Game_IsCameraOnlyBringup() && inputNow < odstMenuPulseUntil)
            pad.menu = true;
        if (pad.a && !previousPad.a) LOG("controller edge: A");
        if (pad.b && !previousPad.b) LOG("controller edge: B");
        if (pad.x && !previousPad.x) LOG("controller edge: X");
        if (pad.y && !previousPad.y) LOG("controller edge: Y");
        previousPad = pad;
        g_scopeZoomStickY.store(pad.valid?pad.turnY:0.0f,
                                std::memory_order_release);
        EnterCriticalSection(&g_headCs);
        g_padState = pad;
        LeaveCriticalSection(&g_headCs);
        UpdateTwoHandLatch(valid, location.pose, leftValid, leftLocation.pose, pad.gripL);
        ApplyControllerHaptics(valid && leftValid);
        static bool padLogged = false;
        if (pad.valid && !padLogged)
        {
            LOG("M3: controller inputs active (sticks/buttons feeding the virtual gamepad)");
            padLogged = true;
        }
    }

    void UpdateMenuPointer(bool headLocked)
    {
        static bool triggerPressed = false;
        static bool hadHit = false;
        static float smoothU = 0.5f, smoothV = 0.5f;
        static uint64_t lastDiagMs = 0;
        if (!g_rightAimPoseValid || (headLocked && !g_headPoseValid) ||
            (!headLocked && !g_haveCenter))
        {
            triggerPressed = false;
            hadHit = false;
            Menu_ClearVrPointer();
            return;
        }

        const XrPosef anchor = headLocked
            ? g_headPose
            : XrPosef{g_centerRot, g_centerPos};
        const XrQuaternionf inverseAnchor{
            -anchor.orientation.x, -anchor.orientation.y,
            -anchor.orientation.z, anchor.orientation.w};
        const XrVector3f relative{
            g_rightAimPose.position.x - anchor.position.x,
            g_rightAimPose.position.y - anchor.position.y,
            g_rightAimPose.position.z - anchor.position.z};
        const XrVector3f localOrigin = Rotate(inverseAnchor, relative);
        const XrVector3f worldDirection =
            Rotate(g_rightAimPose.orientation, {0.0f, 0.0f, -1.0f});
        const XrVector3f localDirection = Rotate(inverseAnchor, worldDirection);
        const float origin[3] = {localOrigin.x, localOrigin.y, localOrigin.z};
        const float direction[3] = {localDirection.x, localDirection.y, localDirection.z};
        const MenuPointerHit hit = IntersectMenuQuad(origin, direction,
            1.2f, 1.1f, 1.1f * MENU_H / MENU_W, -0.08f);

        if (!triggerPressed && g_padState.trigR >= 0.65f)
            triggerPressed = true;
        else if (triggerPressed && g_padState.trigR <= 0.35f)
            triggerPressed = false;
        const float scroll = std::fabs(g_padState.turnY) > 0.25f
            ? g_padState.turnY * 0.12f
            : 0.0f;
        if (hit.hit)
        {
            if (!hadHit)
            {
                smoothU = hit.u;
                smoothV = hit.v;
            }
            else
            {
                smoothU += (hit.u - smoothU) * 0.35f;
                smoothV += (hit.v - smoothV) * 0.35f;
            }
        }
        hadHit = hit.hit;
        Menu_SetVrPointer(hit.hit, smoothU, smoothV, triggerPressed, scroll);

        const uint64_t now = GetTickCount64();
        if (now - lastDiagMs >= 2000)
        {
            lastDiagMs = now;
            LOG("menu pointer: hit=%d uv=(%.3f,%.3f) origin=(%.2f,%.2f,%.2f) "
                "dir=(%.2f,%.2f,%.2f) trigger=%.2f stickY=%.2f headLocked=%d",
                hit.hit ? 1 : 0, hit.u, hit.v,
                origin[0], origin[1], origin[2], direction[0], direction[1], direction[2],
                g_padState.trigR, g_padState.turnY, headLocked ? 1 : 0);
        }
    }

    // Part 2 (render thread, first frame): now that we have the game's D3D
    // device, create the session and everything that hangs off it. This is
    // fast (~100 ms), so running it on the render thread is fine.
    bool InitSession(IDXGISwapChain* sc)
    {
        // The game's device is the one we hand to OpenXR: the runtime then
        // reads our textures without any cross-device copying.
        if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&g_device)))
        {
            Fail("Could not get the game's D3D11 device");
            return false;
        }
        g_device->GetImmediateContext(&g_context);

        DXGI_SWAP_CHAIN_DESC scd{};
        sc->GetDesc(&scd);
        LOG("game swapchain: %ux%u fmt %d windowed=%d swapeffect=%d bufcount=%u hwnd %p",
            scd.BufferDesc.Width, scd.BufferDesc.Height, (int)scd.BufferDesc.Format,
            (int)scd.Windowed, (int)scd.SwapEffect, scd.BufferCount, (void*)scd.OutputWindow);

        XrResult r;
        // Required call before creating a D3D11 session; also tells us which
        // GPU the runtime wants (must match the game's).
        PFN_xrGetD3D11GraphicsRequirementsKHR pfnReq = nullptr;
        xrGetInstanceProcAddr(g_instance, "xrGetD3D11GraphicsRequirementsKHR",
                              reinterpret_cast<PFN_xrVoidFunction*>(&pfnReq));
        XrGraphicsRequirementsD3D11KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
        if (!pfnReq || XR_FAILED(pfnReq(g_instance, g_systemId, &req)))
        {
            Fail("The OpenXR runtime does not support D3D11");
            return false;
        }
        IDXGIDevice* dxgiDev = nullptr;
        if (SUCCEEDED(g_device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDev)))
        {
            IDXGIAdapter* adapter = nullptr;
            if (SUCCEEDED(dxgiDev->GetAdapter(&adapter)))
            {
                DXGI_ADAPTER_DESC ad{};
                adapter->GetDesc(&ad);
                if (memcmp(&ad.AdapterLuid, &req.adapterLuid, sizeof(LUID)) != 0)
                    LOG("WARNING: game GPU differs from the headset's GPU; session may fail");
                adapter->Release();
            }
            dxgiDev->Release();
        }

        XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
        binding.device = g_device;
        XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
        sci.next = &binding;
        sci.systemId = g_systemId;
        r = xrCreateSession(g_instance, &sci, &g_session);
        if (XR_FAILED(r))
        {
            Fail("Could not create the VR session", r);
            return false;
        }

        XrReferenceSpaceCreateInfo rsci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        rsci.poseInReferenceSpace.orientation.w = 1.0f;
        if (XR_FAILED(xrCreateReferenceSpace(g_session, &rsci, &g_localSpace)))
        {
            Fail("Could not create the LOCAL reference space");
            return false;
        }
        rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
        if (XR_FAILED(xrCreateReferenceSpace(g_session, &rsci, &g_viewSpace)))
        {
            Fail("Could not create the VIEW reference space");
            return false;
        }

        // Controller actions are optional for M0-M2: failure must never take
        // down the already-working headset/stereo path.
        if (!CreateControllerActions())
            LOG("M3: controller tracking unavailable; head/stereo remain enabled");

        uint32_t modeCount = 0;
        xrEnumerateEnvironmentBlendModes(g_instance, g_systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                         1, &modeCount, &g_blendMode);

        // M2: the headset's two eyes — recommended per-eye render size. We'll
        // render the game once per eye into swapchains of this size.
        uint32_t viewCount = 0;
        xrEnumerateViewConfigurationViews(g_instance, g_systemId,
                                          XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount, nullptr);
        g_viewConfigs.assign(viewCount, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
        xrEnumerateViewConfigurationViews(g_instance, g_systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                          viewCount, &viewCount, g_viewConfigs.data());
        g_views.assign(viewCount, {XR_TYPE_VIEW});
        for (uint32_t i = 0; i < viewCount; i++)
            LOG("M2: eye %u recommended render size %ux%u", i,
                g_viewConfigs[i].recommendedImageRectWidth, g_viewConfigs[i].recommendedImageRectHeight);

        // Pick the image format for our XR swapchains, preferring sRGB
        // variants so colors in the headset match the monitor.
        uint32_t fmtCount = 0;
        xrEnumerateSwapchainFormats(g_session, 0, &fmtCount, nullptr);
        std::vector<int64_t> formats(fmtCount);
        xrEnumerateSwapchainFormats(g_session, fmtCount, &fmtCount, formats.data());
        const int64_t preferred[] = {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
                                     DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM};
        g_xrFormat = 0;
        for (int64_t want : preferred)
        {
            for (int64_t have : formats)
                if (have == want) { g_xrFormat = want; break; }
            if (g_xrFormat) break;
        }
        if (!g_xrFormat && fmtCount > 0)
            g_xrFormat = formats[0];
        if (!g_xrFormat)
        {
            Fail("The runtime offered no usable swapchain formats");
            return false;
        }
        LOG("XR swapchain format: %d", (int)g_xrFormat);

        // Menu: fixed-size texture on its own quad. Its render target uses the
        // non-sRGB sibling format so a raw GPU copy lands with correct gamma.
        if (!CreateChain(MENU_W, MENU_H, g_menuChain, g_menuImages, g_menuRtvs, "menu"))
        {
            Fail("Could not create the menu swapchain");
            return false;
        }
        if (!Menu_Init(scd.OutputWindow, g_device, g_context, UnormSibling((DXGI_FORMAT)g_xrFormat)))
            LOG("WARNING: menu failed to initialize; F1 menu unavailable");

        strcpy_s(g_status.sessionState, "starting");
        LOG("OpenXR session created");
        return true;
    }

    // --------------------------------------------------------------- frame

    // Log the first few frames step by step so if anything dies on the render
    // thread we can see the exact call it died on. Silent afterward.
    int g_frameNo = 0;
    inline void FLog(const char* step)
    {
        if (g_frameNo <= 3)
            LOG("frame %d: %s", g_frameNo, step);
    }

    void LocateViewsForUpcomingRender(XrTime displayTime);

    void PrepareNextFrame()
    {
        if (g_preparedFrame.begun)
            return;

        ++g_frameNo;
        FLog("xrWaitFrame after DXGI Present");
        XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
        XrFrameState frameState{XR_TYPE_FRAME_STATE};
        LARGE_INTEGER waitStart{}, waitEnd{};
        QueryPerformanceCounter(&waitStart);
        const XrResult waitResult = xrWaitFrame(g_session, &waitInfo, &frameState);
        QueryPerformanceCounter(&waitEnd);
        g_waitDurationsMs.Add(QpcMs(waitEnd.QuadPart - waitStart.QuadPart));
        if (XR_FAILED(waitResult))
        {
            ++g_frameOrderFailures;
            LOG("timing: xrWaitFrame failed: %s", XrStr(waitResult));
            return;
        }

        FLog("xrBeginFrame before Halo render");
        XrFrameBeginInfo beginInfo{XR_TYPE_FRAME_BEGIN_INFO};
        const XrResult beginResult = xrBeginFrame(g_session, &beginInfo);
        if (XR_FAILED(beginResult))
        {
            ++g_frameOrderFailures;
            LOG("timing: xrBeginFrame failed: %s", XrStr(beginResult));
            return;
        }

        g_preparedFrame.state = frameState;
        g_preparedFrame.begun = true;
        g_preparedFrame.serial = ++g_nextPreparedSerial;
        g_preparedShouldRender.store(
            frameState.shouldRender == XR_TRUE, std::memory_order_release);

        if (g_lastPredictedDisplayTime)
        {
            const XrDuration delta =
                frameState.predictedDisplayTime - g_lastPredictedDisplayTime;
            const XrDuration period = frameState.predictedDisplayPeriod;
            if (delta <= 0)
                ++g_duplicatePredictions;
            else if (period > 0)
            {
                g_predictionErrorMs.Add(
                    std::fabs(static_cast<double>(delta - period)) / 1000000.0);
                if (delta > period + period / 2)
                    g_missedPredictions += static_cast<uint64_t>(
                        std::max<XrDuration>(1, delta / period - 1));
            }
        }
        g_lastPredictedDisplayTime = frameState.predictedDisplayTime;

        // Input first, then views/head as late as possible. Every locate uses
        // the exact predicted time associated with this begun frame.
        CaptureRightControllerPose(frameState.predictedDisplayTime);
        LocateViewsForUpcomingRender(frameState.predictedDisplayTime);
        CaptureHeadPose(frameState.predictedDisplayTime);

        LARGE_INTEGER preparedAt{};
        QueryPerformanceCounter(&preparedAt);
        g_prepareQpcPublished.store(static_cast<uint64_t>(preparedAt.QuadPart),
                                    std::memory_order_release);
        g_preparedSerialPublished.store(g_preparedFrame.serial,
                                        std::memory_order_release);
        if (g_frameNo == 1)
            LOG("timing: exact OpenXR pipeline active; headset smoothing %.1f%%",
                std::clamp(g_config.headset_smoothing, 0.0f, 0.10f) * 100.0f);
    }

    void LocateViewsForUpcomingRender(XrTime displayTime)
    {
        if (g_views.empty())
            return;
        XrViewLocateInfo info{XR_TYPE_VIEW_LOCATE_INFO};
        info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        info.displayTime = displayTime;
        info.space = g_localSpace;
        XrViewState state{XR_TYPE_VIEW_STATE};
        uint32_t count = 0;
        xrLocateViews(g_session, &info, &state, static_cast<uint32_t>(g_views.size()),
                      &count, g_views.data());
    }

    void SubmitPreparedFrame(IDXGISwapChain* sc)
    {
        if (!g_preparedFrame.begun)
            return;
        const XrFrameState fs = g_preparedFrame.state;
        const float comfortFadeAlpha = UpdatePauseTransition();

        // M2: per-eye pose + field of view for this frame (foundation for
        // stereo rendering — not used to render yet).
        bool viewsValid = false;
        uint32_t locatedViewCount = 0;
        if (!g_views.empty())
        {
            XrViewLocateInfo vli{XR_TYPE_VIEW_LOCATE_INFO};
            vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            vli.displayTime = fs.predictedDisplayTime;
            vli.space = g_localSpace;
            XrViewState vs{XR_TYPE_VIEW_STATE};
            if (XR_SUCCEEDED(xrLocateViews(g_session, &vli, &vs, (uint32_t)g_views.size(),
                                           &locatedViewCount, g_views.data())) &&
                locatedViewCount == g_views.size() &&
                (vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) &&
                (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT))
            {
                viewsValid = true;
                static bool loggedEyes = false;
                if (!loggedEyes)
                {
                    for (uint32_t i = 0; i < locatedViewCount; i++)
                        LOG("M2: eye %u pose(%.3f,%.3f,%.3f) fov L%.1f R%.1f U%.1f D%.1f deg", i,
                            g_views[i].pose.position.x, g_views[i].pose.position.y, g_views[i].pose.position.z,
                            g_views[i].fov.angleLeft * 57.2958f, g_views[i].fov.angleRight * 57.2958f,
                            g_views[i].fov.angleUp * 57.2958f, g_views[i].fov.angleDown * 57.2958f);
                    // Interpupillary distance = horizontal gap between the eye poses.
                    if (locatedViewCount >= 2)
                        LOG("M2: eye separation (IPD) = %.1f mm",
                            (g_views[1].pose.position.x - g_views[0].pose.position.x) * 1000.0f);
                    loggedEyes = true;
                }
            }
        }

        XrCompositionLayerQuad screenQuad, menuQuad, reticleQuad, scopeQuad, fadeQuad;
        XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        // Reused across frames (Frame runs only on the render thread) so the
        // per-frame layer assembly allocates nothing in steady state.
        static std::vector<XrCompositionLayerProjectionView> projectionViews;
        static std::vector<XrCompositionLayerBaseHeader*> layers;
        projectionViews.clear();
        layers.clear();

        // Build the descriptors every frame from the predicted eye poses/FOV.
        // A later M2 render hook only needs to fill/release both swapchain
        // images and replace the mono quad in `layers` with `projection`.
        if (viewsValid && g_stereoChain != XR_NULL_HANDLE && locatedViewCount == 2)
        {
            projection.space = g_localSpace;
            projectionViews.assign(locatedViewCount, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW});

            // RenderViewHook positions and rotates each raster camera from the
            // same per-eye view offsets (VR_GetEyeViewOffset), so the images
            // are canted the way PSVR2 reports its views — submit the real
            // per-eye orientations. Submitting a shared midpoint orientation
            // here under-covers the outward-angled lens edge and shows as a
            // black border at the outer edge of each eye.
            // The FOV submitted must be the FOV Halo actually rastered with
            // (symmetric, widened by RenderViewHook to cover the headset's
            // per-eye angles). Fixed headset angles warp during head turns
            // whenever Halo's internal projection produces different scales.
            float haloHalfX = atanf(1.091595f);
            float haloHalfY = atanf(1.114286f);
            for (uint32_t i = 0; i < locatedViewCount; ++i)
            {
                Game_GetRenderHalfFov(static_cast<int>(i), haloHalfX, haloHalfY);
                projectionViews[i].pose = g_views[i].pose;
                projectionViews[i].fov = {-haloHalfX, haloHalfX, haloHalfY, -haloHalfY};
                projectionViews[i].subImage.swapchain = g_stereoChain;
                projectionViews[i].subImage.imageRect = {
                    {0, 0}, {(int32_t)g_stereoW, (int32_t)g_stereoH}};
                projectionViews[i].subImage.imageArrayIndex = i;
            }
            projection.viewCount = locatedViewCount;
            projection.views = projectionViews.data();
        }

        if (fs.shouldRender)
        {
            ID3D11Texture2D* backbuffer = nullptr;
            sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backbuffer);
            if (backbuffer)
            {
                D3D11_TEXTURE2D_DESC bd{};
                backbuffer->GetDesc(&bd);
                if (!g_haveCenter)
                    TryRecenter(fs.predictedDisplayTime);
                // SteamVR accepted eye swapchains made before xrBeginSession
                // but presented them black. Create them lazily only once the
                // session is running, matching the known-good screen chain.
                if (g_stereoEnabled.load() && g_stereoChain == XR_NULL_HANDLE)
                {
                    if (!CreateStereoArrayChain())
                    {
                        LOG("M2: eye swapchain allocation failed; returning to mono screen");
                        g_stereoEnabled = false;
                        Game_SetStereoEye(-1);
                    }
                }
                const bool pausedPresentation = g_pausePresentation.load();
                const bool stereo = !pausedPresentation && g_stereoEnabled.load() && viewsValid &&
                                    g_stereoChain != XR_NULL_HANDLE && Game_IsHeadTracking();
                if (stereo)
                {
                    ValidateStereoImagesOnce();
                    uint32_t idx = 0;
                    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
                    XrSwapchainImageWaitInfo swi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                    swi.timeout = 1000000000;
                    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                    if (XR_SUCCEEDED(xrAcquireSwapchainImage(g_stereoChain, &ai, &idx)) &&
                        XR_SUCCEEDED(xrWaitSwapchainImage(g_stereoChain, &swi)))
                    {
                        for (uint32_t eye = 0; eye < 2; ++eye)
                        {
                            if (g_eyeHasImage[eye])
                                if (ID3D11RenderTargetView* rtv = GetStereoRtv(idx, eye))
                                    Blit(g_eyeCache[eye], g_eyeCacheDesc, g_stereoImages[idx],
                                         g_stereoW, g_stereoH, rtv);
                        }
                        xrReleaseSwapchainImage(g_stereoChain, &ri);
                    }

                    if (g_eyeHasImage[0] && g_eyeHasImage[1] && projection.viewCount == 2)
                    {
                        layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&projection));

                        // Use the same predicted OpenXR controller pose captured
                        // for this displayed frame. Routing the quad through
                        // Halo's virtual-stick aim introduced visible catch-up
                        // lag and placed its ray origin at the player's head.
                        // Start from the same aim pose as bullet steering, then
                        // optionally stabilize ONLY the displayed reticle. At
                        // 0% it is the exact current bullet ray; higher values
                        // deliberately trade visual reticle response for calm.
                        float aimQ[4], aimP[3];
                        const bool haveAim = VR_GetAimPose(aimQ, aimP);
                        if ((Game_AllowsSharedGameplayFeatures() ||
                             Game_AllowsOdstMotionAim()) &&
                            g_config.crosshair &&
                            haveAim && EnsureReticleChain())
                        {
                            if (g_authoredReticleReady &&
                                g_authoredReticleSerial == g_preparedFrame.serial &&
                                !UploadAuthoredReticle())
                            {
                                // Never expose stale or undefined swapchain
                                // contents if the authored upload fails.
                                g_authoredReticleReady = false;
                                EnsureReticleChain();
                            }
                            XrPosef rawAim{{aimQ[0],aimQ[1],aimQ[2],aimQ[3]},
                                           {aimP[0],aimP[1],aimP[2]}};
                            const float smoothing =
                                std::clamp(g_config.aim_stabilization, 0.0f, 0.95f);
                            g_reticleAimPose = g_reticleAimPoseValid && smoothing > 0.0f
                                ? SmoothTrackedPose(rawAim, g_reticleAimPose, smoothing)
                                : rawAim;
                            g_reticleAimPoseValid = true;
                            const XrVector3f aimRay = Rotate(
                                g_reticleAimPose.orientation, {0.0f,0.0f,-1.0f});
                            const float aimDir[3] = {aimRay.x,aimRay.y,aimRay.z};
                            const float dist = g_config.crosshair_distance_m;
                            const float yaw = atan2f(aimDir[0], -aimDir[2]);
                            const float sp = fminf(fmaxf(aimDir[1], -1.0f), 1.0f);
                            const float pitch = asinf(sp);
                            // Orientation whose local -Z runs along the ray
                            // (quad faces the player): global yaw about +Y
                            // (angle -yaw, same convention as TryRecenter),
                            // then local pitch about +X.
                            const XrQuaternionf qy{0, sinf(-yaw * 0.5f), 0, cosf(-yaw * 0.5f)};
                            const XrQuaternionf qp{sinf(pitch * 0.5f), 0, 0, cosf(pitch * 0.5f)};
                            const XrQuaternionf q{
                                qy.w * qp.x + qy.x * qp.w + qy.y * qp.z - qy.z * qp.y,
                                qy.w * qp.y - qy.x * qp.z + qy.y * qp.w + qy.z * qp.x,
                                qy.w * qp.z + qy.x * qp.y - qy.y * qp.x + qy.z * qp.w,
                                qy.w * qp.w - qy.x * qp.x - qy.y * qp.y - qy.z * qp.z};
                            reticleQuad = {XR_TYPE_COMPOSITION_LAYER_QUAD};
                            reticleQuad.layerFlags =
                                XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
                                XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
                            reticleQuad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                            reticleQuad.space = g_localSpace;
                            reticleQuad.subImage.swapchain = g_reticleChain;
                            reticleQuad.subImage.imageRect =
                                {{0, 0}, {(int32_t)kReticleSize, (int32_t)kReticleSize}};
                            reticleQuad.subImage.imageArrayIndex = 0;
                            reticleQuad.pose.orientation = q;
                            reticleQuad.pose.position = {
                                g_reticleAimPose.position.x + aimDir[0] * dist,
                                g_reticleAimPose.position.y + aimDir[1] * dist,
                                g_reticleAimPose.position.z + aimDir[2] * dist};
                            const float w = 2.0f * dist *
                                tanf(g_config.crosshair_size_deg * 0.5f * 0.0174533f);
                            reticleQuad.size = {w, w};
                            layers.push_back(
                                reinterpret_cast<XrCompositionLayerBaseHeader*>(&reticleQuad));
                        }
                        else
                        {
                            // Never blend from a stale pose after tracking or
                            // the crosshair is restored.
                            g_reticleAimPoseValid = false;
                        }

                        if (Game_AllowsSharedGameplayFeatures() &&
                            g_config.scope_enabled &&
                            g_scopeActive.load() &&
                            !Menu_IsOpen() && haveAim && PrepareScopeImageDelivery())
                        {
                            const ScopeQuadTransform transform = ComputeScopeQuadTransform(
                                aimQ, aimP,
                                g_config.scope_screen_right_m,
                                g_config.scope_screen_up_m,
                                g_config.scope_screen_forward_m,
                                g_config.scope_screen_width_m);
                            scopeQuad = {XR_TYPE_COMPOSITION_LAYER_QUAD};
                            scopeQuad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                            scopeQuad.space = g_localSpace;
                            scopeQuad.subImage.swapchain = g_scopeScreenChain;
                            scopeQuad.subImage.imageRect = {
                                {0, 0}, {(int32_t)kScopeScreenWidth, (int32_t)kScopeScreenHeight}};
                            scopeQuad.subImage.imageArrayIndex = 0;
                            scopeQuad.pose.orientation = {aimQ[0], aimQ[1], aimQ[2], aimQ[3]};
                            scopeQuad.pose.position = {
                                transform.position[0], transform.position[1], transform.position[2]};
                            scopeQuad.size = {transform.width, transform.height};
                            layers.push_back(
                                reinterpret_cast<XrCompositionLayerBaseHeader*>(&scopeQuad));
                            static bool logged = false;
                            if (!logged)
                            {
                                logged = true;
                                LOG("scope 4:3 zoom screen submitted: %.3fm x %.3fm at local offsets %.3f/%.3f/%.3f",
                                    transform.width, transform.height,
                                    g_config.scope_screen_right_m,
                                    g_config.scope_screen_up_m,
                                    g_config.scope_screen_forward_m);
                            }
                        }
                    }
                }
                else if (g_haveCenter && EnsureScreenChain(bd.Width, bd.Height))
                {
                    uint32_t idx = 0;
                    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
                    XrSwapchainImageWaitInfo wi2{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                    wi2.timeout = 1000000000; // 1 second in ns; never hang the render thread
                    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                    FLog("acquire+wait screen image");
                    if (XR_SUCCEEDED(xrAcquireSwapchainImage(g_screenChain, &ai, &idx)) &&
                        XR_SUCCEEDED(xrWaitSwapchainImage(g_screenChain, &wi2)))
                    {
                        FLog("blit backbuffer -> screen");
                        Blit(backbuffer, bd, g_screenImages[idx], g_screenW, g_screenH,
                             GetRtv(g_screenImages, g_screenRtvs, idx));
                        xrReleaseSwapchainImage(g_screenChain, &ri);
                        FLog("screen image released");
                        const bool headLock = pausedPresentation ||
                            (g_screenFollow.load() && Game_IsHeadTracking());
                        screenQuad = MakeQuad(g_screenChain, (int32_t)g_screenW, (int32_t)g_screenH,
                                              g_config.screen_width_m, g_config.screen_distance_m, 0.0f, 0,
                                              headLock);
                        layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&screenQuad));
                    }
                }

                // The menu is submitted in BOTH modes (it used to live only in
                // the mono-screen branch, which made F1 invisible in stereo).
                // In stereo it head-locks so it is always in front of you.
                if (Menu_IsOpen())
                {
                    const bool menuHeadLocked =
                        pausedPresentation || stereo ||
                        (g_screenFollow.load() && Game_IsHeadTracking());
                    UpdateMenuPointer(menuHeadLocked);
                    if (ID3D11Texture2D* menuTex = Menu_Render())
                    {
                        uint32_t idx = 0;
                        XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
                        XrSwapchainImageWaitInfo wi2{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                        wi2.timeout = 1000000000;
                        XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                        D3D11_TEXTURE2D_DESC md{};
                        menuTex->GetDesc(&md);
                        if (XR_SUCCEEDED(xrAcquireSwapchainImage(g_menuChain, &ai, &idx)) &&
                            XR_SUCCEEDED(xrWaitSwapchainImage(g_menuChain, &wi2)))
                        {
                            Blit(menuTex, md, g_menuImages[idx], MENU_W, MENU_H,
                                 GetRtv(g_menuImages, g_menuRtvs, idx));
                            xrReleaseSwapchainImage(g_menuChain, &ri);
                            menuQuad = MakeQuad(g_menuChain, MENU_W, MENU_H, 1.1f, 1.2f, -0.08f,
                                                XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
                                                    XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT,
                                                menuHeadLocked);
                            layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&menuQuad));
                        }
                    }
                }
                else
                {
                    Menu_ClearVrPointer();
                }
                backbuffer->Release();
            }
        }
        if (fs.shouldRender)
            AppendComfortFade(comfortFadeAlpha, fadeQuad, layers);

        // Heartbeat: log the session state + whether the runtime wants us to
        // render + how many layers we submitted, on any change and at least
        // every couple of seconds. This shows whether we ever go VISIBLE.
        {
            static XrSessionState lastState = XR_SESSION_STATE_UNKNOWN;
            static int lastShould = -1;
            static LARGE_INTEGER last{}, freq{};
            LARGE_INTEGER now;
            if (freq.QuadPart == 0)
                QueryPerformanceFrequency(&freq);
            QueryPerformanceCounter(&now);
            const bool changed = g_sessionState != lastState || (int)fs.shouldRender != lastShould;
            const bool tick = last.QuadPart == 0 || (now.QuadPart - last.QuadPart) >= 2 * freq.QuadPart;
            if (changed || tick)
            {
                LOG("status: session=%s shouldRender=%d layers=%u", SessionStateName(g_sessionState),
                    (int)fs.shouldRender, (unsigned)layers.size());
                lastState = g_sessionState;
                lastShould = (int)fs.shouldRender;
                last = now;
            }
        }

        FLog("xrEndFrame before DXGI Present");
        XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
        ei.displayTime = fs.predictedDisplayTime;
        ei.environmentBlendMode = g_blendMode;
        ei.layerCount = (uint32_t)layers.size();
        ei.layers = layers.data();
        XrResult r = xrEndFrame(g_session, &ei);
        ResetPreparedFrame();
        if (XR_FAILED(r))
        {
            ++g_frameOrderFailures;
            static bool logged = false;
            if (!logged)
            {
                LOG("xrEndFrame failed: %s", XrStr(r));
                logged = true;
            }
        }
        FLog("frame complete");
        if (g_frameNo == 3)
            LOG("first 3 frames submitted OK; going quiet now");
    }
} // namespace

void VR_InitInstance()
{
    if (!g_headCsInit)
    {
        InitializeCriticalSection(&g_headCs);
        g_headCsInit = true;
    }
    // Runs on the DLL's background init thread, in parallel with the game
    // loading. Never touches the render thread or the game's D3D device.
    if (InitInstance())
        g_instanceReady = true;
    else
        g_instanceFailed = true; // Fail() already showed a message
}

void VR_BeforePresent(IDXGISwapChain* sc)
{
    g_gameSwapchain = sc;
#if HALOMCCVR_EXPERIMENTAL_ODST_BRINGUP
    // Presentation cleanup must not depend on a healthy/running OpenXR session.
    // The D3D Present hook still reaches this path when instance/session setup
    // failed, so it can acknowledge ODST teardown and restore shared title
    // policy without leaving camera-only ownership latched indefinitely.
    Game_ProcessPresentationDetachRequest();
#endif
    if (!g_gameBackbufferDescValid && sc)
    {
        ID3D11Texture2D* backbuffer = nullptr;
        if (SUCCEEDED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                    reinterpret_cast<void**>(&backbuffer))) && backbuffer)
        {
            backbuffer->GetDesc(&g_gameBackbufferDesc);
            g_gameBackbufferDescValid = true;
            backbuffer->Release();
        }
    }
    if (g_state == State::Failed)
        return;
    if (g_state == State::Uninitialized)
    {
        // Wait for the background thread to finish creating the OpenXR
        // instance. Until then, do nothing so the game renders normally to
        // the monitor instead of freezing.
        if (g_instanceFailed)
        {
            g_state = State::Failed;
            return;
        }
        if (!g_instanceReady)
            return;
        static bool announced = false;
        if (!announced)
        {
            LOG("instance ready; creating VR session on the render thread");
            announced = true;
        }
        if (!InitSession(sc))
            return; // state is Failed, message shown
        g_state = State::Ready;
    }

    // FPS counter for the menu status line
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    if (g_lastBeforePresentQpc.QuadPart)
        g_presentIntervalsMs.Add(
            QpcMs(now.QuadPart - g_lastBeforePresentQpc.QuadPart));
    g_lastBeforePresentQpc = now;
    g_fpsFrames++;
    if (g_fpsTimer.QuadPart == 0)
        g_fpsTimer = now;
    else if (now.QuadPart - g_fpsTimer.QuadPart >= freq.QuadPart)
    {
        g_status.fps = (float)g_fpsFrames * freq.QuadPart / (float)(now.QuadPart - g_fpsTimer.QuadPart);
        g_fpsFrames = 0;
        g_fpsTimer = now;
        // Every 10 s, put the frame rate in the log so performance reports
        // ("fps has been lower") can be tied to a session and a build.
        static int fpsLogCountdown = 10;
        if (--fpsLogCountdown <= 0)
        {
            fpsLogCountdown = 10;
            LOG("fps %.0f (stereo %s)", g_status.fps,
                g_stereoEnabled.load() ? "on" : "off");
        }
    }

    const uint64_t timingNowMs = GetTickCount64();
    if (!g_timingLogStartMs)
        g_timingLogStartMs = timingNowMs;
    else if (timingNowMs - g_timingLogStartMs >= 10000)
    {
        LOG("timing: frame interval p95 %.2fms p99 %.2fms; "
            "DXGI Present p95 %.2fms; xrWait p95 %.2fms; "
            "prediction error p95 %.3fms; missed=%llu duplicate=%llu "
            "orderFailures=%llu firstCamera=%.3fms",
            TimingPercentile(g_presentIntervalsMs, 0.95),
            TimingPercentile(g_presentIntervalsMs, 0.99),
            TimingPercentile(g_presentDurationsMs, 0.95),
            TimingPercentile(g_waitDurationsMs, 0.95),
            TimingPercentile(g_predictionErrorMs, 0.95),
            static_cast<unsigned long long>(g_missedPredictions),
            static_cast<unsigned long long>(g_duplicatePredictions),
            static_cast<unsigned long long>(g_frameOrderFailures),
            g_firstCameraDelayUs.load(std::memory_order_relaxed) / 1000.0);
        g_timingLogStartMs = timingNowMs;
    }

    PollEvents();
    if (g_state != State::Ready || !g_sessionRunning)
        return;

    // Auto-enter/exit VR when a level loads/unloads (no F2/F11 needed).
    Game_AutoVrTick();
    if (!g_config.scope_enabled || !Game_IsHeadTracking())
        VR_SetScopeActive(false);

    SubmitPreparedFrame(sc);
    QueryPerformanceCounter(&g_dxgiPresentStartQpc);
}

void VR_AfterPresent(IDXGISwapChain* sc)
{
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    if (g_dxgiPresentStartQpc.QuadPart)
        g_presentDurationsMs.Add(
            QpcMs(now.QuadPart - g_dxgiPresentStartQpc.QuadPart));
    g_dxgiPresentStartQpc = {};

    // Present has advanced the flip-model chain. Retain its reported current
    // buffer outside all camera/render hooks for ODST's direct death capture.
    if (sc)
    {
        UINT currentIndex = 0;
        if (g_flipIndexOwner != sc)
        {
            if (g_flipIndexChain)
                g_flipIndexChain->Release();
            g_flipIndexChain = nullptr;
            g_flipIndexOwner = sc;
            sc->QueryInterface(__uuidof(IDXGISwapChain3),
                reinterpret_cast<void**>(&g_flipIndexChain));
        }
        if (g_flipIndexChain)
            currentIndex = g_flipIndexChain->GetCurrentBackBufferIndex();
        ID3D11Texture2D* next = nullptr;
        if (SUCCEEDED(sc->GetBuffer(currentIndex, __uuidof(ID3D11Texture2D),
                                    reinterpret_cast<void**>(&next))) && next)
        {
            ID3D11Texture2D* previous =
                g_nextGameBackbuffer.exchange(next, std::memory_order_acq_rel);
            if (previous)
                previous->Release();
        }
    }

    if (g_state != State::Ready || !g_sessionRunning)
        return;
    PrepareNextFrame();
}

void VR_NotifyCameraTransform()
{
    const uint64_t serial =
        g_preparedSerialPublished.load(std::memory_order_acquire);
    if (!serial)
        return;
    uint64_t observed =
        g_cameraSerialObserved.load(std::memory_order_relaxed);
    if (observed == serial ||
        !g_cameraSerialObserved.compare_exchange_strong(
            observed, serial, std::memory_order_acq_rel))
        return;

    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    const uint64_t prepared =
        g_prepareQpcPublished.load(std::memory_order_acquire);
    if (prepared && static_cast<uint64_t>(now.QuadPart) >= prepared)
    {
        const double delayMs =
            QpcMs(now.QuadPart - static_cast<LONGLONG>(prepared));
        g_firstCameraDelayUs.store(
            static_cast<uint64_t>(delayMs * 1000.0),
            std::memory_order_relaxed);
    }
}

void VR_OnResizeBuffers(IDXGISwapChain*)
{
    // The game is about to destroy its backbuffer; anything of ours that
    // references it must go first or the resize fails. The tracked history
    // targets are resolution-dependent too — drop and re-learn them.
    ReleaseSourceViews();
    if (g_sceneColorRtv)
    {
        g_sceneColorRtv->Release();
        g_sceneColorRtv = nullptr;
    }
#if HALOMCCVR_EXPERIMENTAL_ODST_BRINGUP
    if (g_sceneColorResource)
    {
        g_sceneColorResource->Release();
        g_sceneColorResource = nullptr;
    }
#endif
    g_gameBackbufferDesc = {};
    g_gameBackbufferDescValid = false;
    if (ID3D11Texture2D* retained =
            g_nextGameBackbuffer.exchange(nullptr, std::memory_order_acq_rel))
        retained->Release();
}

void VR_RequestRecenter()
{
    g_haveCenter = false;
}

void VR_RequestPausePresentation(bool paused)
{
    const bool previous = g_pauseTarget.exchange(paused);
    if (previous != paused || g_pausePresentation.load() != paused)
        g_pauseRequest = paused ? 1 : 0;
}

bool VR_IsPausePresentation()
{
    return g_pausePresentation.load();
}

bool VR_IsPausePresentationTarget()
{
    return g_pauseTarget.load();
}

void VR_ToggleScreenFollow()
{
    const bool on = !g_screenFollow.load();
    g_screenFollow = on;
    LOG("screen-follow %s", on ? "on (screen follows head)" : "off (screen world-locked)");
}

void VR_ToggleStereo()
{
    const bool on = !g_stereoEnabled.load();
    g_stereoEnabled = on;
    if (on)
        Game_ForcePositional();
    g_renderEye = 0;
    g_eyeHasImage[0] = g_eyeHasImage[1] = false;
    Game_SetStereoEye(on ? 0 : -1);
    LOG("M2 alternate-eye stereo %s%s", on ? "ON" : "OFF",
        on && !Game_IsHeadTracking() ? " (enable head tracking with F2)" : "");
}

bool VR_IsStereoEnabled()
{
    return g_stereoEnabled.load();
}

bool VR_ShouldRenderPreparedFrame()
{
    return g_preparedShouldRender.load(std::memory_order_acquire);
}

void VR_DetachGamePresentation()
{
    // Game_AutoVrTick calls this from Present, after Halo has stopped issuing
    // camera renders and before this frame is submitted to OpenXR. Do not tear
    // down the session or shared MCC D3D hooks: the flat shell still needs
    // them. Only disarm Halo's per-eye work and release its retained render
    // target so a different MCC engine can own the shared device cleanly.
    if (g_stereoEnabled.load())
        VR_ToggleStereo();
    else
        Game_SetStereoEye(-1);
    g_renderEye = 0;
    g_eyeHasImage[0] = g_eyeHasImage[1] = false;
    g_stereoValidationDone = false;
    g_rasterEye = -1;
    g_rasterRedirected[0] = g_rasterRedirected[1] = false;
    g_rasterScope = false;
    g_scopeRedirected = false;
    g_scopeActive = false;
    g_scopeHasImage = false;
    g_scopeResetRequested = true;
    ReleaseSourceViews();
    if (g_sceneColorRtv)
    {
        g_sceneColorRtv->Release();
        g_sceneColorRtv = nullptr;
    }
#if HALOMCCVR_EXPERIMENTAL_ODST_BRINGUP
    if (g_sceneColorResource)
    {
        g_sceneColorResource->Release();
        g_sceneColorResource = nullptr;
    }
#endif
}

bool VR_CaptureRenderedEye(int eye)
{
    if (eye < 0 || eye > 1)
        return false;
    if (g_rasterRedirected[eye] && g_eyeCache[eye])
    {
        g_eyeHasImage[eye] = true;
        return true;
    }
    static bool loggedMissing = false;
    if (!loggedMissing)
    {
        LOG("M2 RASTER: no internal scene-color RTV redirect occurred; refusing fake eye copy");
        loggedMissing = true;
    }
    return false;
}

bool VR_CaptureBackbufferEye(int eye)
{
    if (eye < 0 || eye > 1 || !g_context || !g_gameBackbufferDescValid ||
        !g_eyeCache[eye] || !g_eyeCacheRtvs[eye])
        return false;
    ID3D11Texture2D* backbuffer =
        g_nextGameBackbuffer.load(std::memory_order_acquire);
    if (!backbuffer ||
        g_eyeCacheDesc.Width != g_gameBackbufferDesc.Width ||
        g_eyeCacheDesc.Height != g_gameBackbufferDesc.Height)
        return false;
    if (!Blit(backbuffer, g_gameBackbufferDesc, g_eyeCache[eye],
              g_eyeCacheDesc.Width, g_eyeCacheDesc.Height,
              g_eyeCacheRtvs[eye]))
        return false;
    g_eyeHasImage[eye] = true;
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true, std::memory_order_relaxed))
        LOG("M2 RASTER: ODST direct backbuffer death-camera capture active");
    return true;
}

void VR_TraceEvent(const char* tag, int a, int b)
{
    // Arms ~8s after first call (a level is up), then logs the next 60 events
    // and disarms forever. One burst, zero steady-state cost beyond two loads.
    static std::atomic<DWORD> firstMs{0};
    static std::atomic<int> budget{-1};
    DWORD f = firstMs.load();
    if (f == 0 && firstMs.compare_exchange_strong(f, GetTickCount())) f = firstMs.load();
    int have = budget.load();
    if (have == -1)
    {
        if (GetTickCount() - f < 8000) return;
        int expect = -1;
        if (!budget.compare_exchange_strong(expect, 60)) {}
        have = budget.load();
    }
    if (have <= 0) return;
    if (budget.fetch_sub(1) <= 0) return;
    LOG("TRACE %s a=%d b=%d rasterEye=%d redir0=%d", tag, a, b,
        g_rasterEye.load(), g_rasterRedirected[0] ? 1 : 0);
}

void VR_BeginRasterEye(int eye)
{
    if (eye < 0 || eye > 1 || !g_gameSwapchain || !g_device)
        return;
    // Eye caches are created lazily when Halo binds its final scene-color RTV.
    // That RTV's typed view format (not the swapchain resource format) controls
    // the required sRGB conversion.
    g_rasterRedirected[eye] = false;
    g_rasterEye = eye;
    VR_TraceEvent("eye-begin", eye, 0);
}


void VR_EndRasterEye()
{
    VR_TraceEvent("eye-end", g_rasterEye.load(), 0);
    // Promote any newly identified history, then save this eye's copies of
    // every tracked target before the other eye (or next frame) overwrites
    // them. A ping-pong pair only reveals its read side a frame after its
    // write side, so discovery stays open for a fixed window (~2 s of
    // stereo) instead of closing at the first find.
    g_rasterEye = -1;
}

#if HALOMCCVR_EXPERIMENTAL_ODST_BRINGUP
bool VR_BeginNativeHudEyeDraw(int eye)
{
    auto& route = g_nativeHudEyeRoute;
    if (!g_context || eye < 0 || eye > 1 || !g_eyeCacheRtvs[eye] || route.active)
        return false;

    g_context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT,
                                  route.rtvs, &route.dsv);
    route.viewportCount =
        D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    g_context->RSGetViewports(&route.viewportCount, route.viewports);
    route.scissorCount =
        D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    g_context->RSGetScissorRects(&route.scissorCount, route.scissors);
    // ODST's unique prepare callback owns slot 0 at both native CHUD boundaries:
    // it establishes target 1 immediately before secondary, then performs its
    // engine-owned transition before primary. A non-null slot-0 RTV captured
    // only at these TLS-scoped hooks is therefore the title-proven phase output,
    // even when its view pointer differs from the later scene-color view.
    if (!route.rtvs[0])
    {
        for (auto*& rtv : route.rtvs)
        {
            if (rtv) rtv->Release();
            rtv = nullptr;
        }
        if (route.dsv)
        {
            route.dsv->Release();
            route.dsv = nullptr;
        }
        route.viewportCount = 0;
        route.scissorCount = 0;
        return false;
    }

    route.phaseOutputRtv = route.rtvs[0];
    route.eye = eye;
    ID3D11RenderTargetView* routed[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
        routed[i] = route.rtvs[i];
    routed[0] = g_eyeCacheRtvs[eye];
    route.bypassOmRedirect = true;
    g_context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT,
                                  routed, route.dsv);
    route.bypassOmRedirect = false;
    route.targetCopy = false;
    route.active = true;
    return true;
}

void VR_EndNativeHudEyeDraw()
{
    auto& route = g_nativeHudEyeRoute;
    if (!route.active || !g_context)
        return;

    route.active = false;
    route.targetCopy = false;
    route.bypassOmRedirect = true;
    g_context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT,
                                  route.rtvs, route.dsv);
    route.bypassOmRedirect = false;
    g_context->RSSetViewports(route.viewportCount,
                              route.viewportCount ? route.viewports : nullptr);
    g_context->RSSetScissorRects(route.scissorCount,
                                 route.scissorCount ? route.scissors : nullptr);
    for (auto*& rtv : route.rtvs)
    {
        if (rtv) rtv->Release();
        rtv = nullptr;
    }
    if (route.dsv)
    {
        route.dsv->Release();
        route.dsv = nullptr;
    }
    route.phaseOutputRtv = nullptr;
    route.viewportCount = 0;
    route.scissorCount = 0;
    route.eye = -1;
    g_nativeHudPhaseScopes.fetch_add(1, std::memory_order_release);
}

void VR_BeginNativeHudTargetCopy()
{
    auto& route = g_nativeHudEyeRoute;
    route.targetCopy = route.active;
}

void VR_EndNativeHudTargetCopy()
{
    auto& route = g_nativeHudEyeRoute;
    if (!route.targetCopy)
        return;
    route.targetCopy = false;
    g_nativeHudExactCopyScopes.fetch_add(1, std::memory_order_relaxed);
}

ID3D11Resource* VR_RedirectNativeHudCopySource(ID3D11Resource* source)
{
    const auto& route = g_nativeHudEyeRoute;
    if (!route.active || !route.targetCopy || route.eye < 0 || route.eye > 1 ||
        !g_eyeCache[route.eye] || !g_sceneColorResource ||
        source != g_sceneColorResource)
        return source;

    g_nativeHudCopySubstitutions.fetch_add(1, std::memory_order_relaxed);
    return g_eyeCache[route.eye];
}

void VR_GetNativeHudRouteStats(unsigned& completedPhaseScopes,
                               unsigned& provenOmMatches,
                               unsigned& exactCopyScopes,
                               unsigned& copySubstitutions)
{
    completedPhaseScopes =
        g_nativeHudPhaseScopes.load(std::memory_order_acquire);
    provenOmMatches = g_nativeHudProvenOmMatches.load(std::memory_order_relaxed);
    exactCopyScopes =
        g_nativeHudExactCopyScopes.load(std::memory_order_relaxed);
    copySubstitutions =
        g_nativeHudCopySubstitutions.load(std::memory_order_relaxed);
}

#endif

bool VR_ScopeShouldRenderThisFrame()
{
    const bool enabled=g_config.scope_enabled && !Menu_IsOpen() &&
                       Game_IsHeadTracking();
    if(g_scopeResetRequested.exchange(false,std::memory_order_acq_rel))
    {
        g_scopeZoomResolver.Reset();
        g_scopeToggleObserved=g_scopeToggleSerial.load(std::memory_order_acquire);
    }
    const uint64_t requested=g_scopeToggleSerial.load(std::memory_order_acquire);
    if(requested!=g_scopeToggleObserved)
    {
        g_scopeToggleObserved=requested;
        g_scopeZoomResolver.RequestToggle();
    }
    // Native Halo zoom is deliberately suppressed by the input layer: it hides
    // the VR body/viewmodel. This resolver now supplies only the delayed R3
    // toggle that keeps input and render-thread ownership race-free.
    const bool active=g_scopeZoomResolver.Update(enabled,false);
    const bool previous=g_scopeActive.exchange(active,std::memory_order_acq_rel);
    if(previous && !active)
        g_scopeHasImage.store(false,std::memory_order_release);
    const uint64_t now=GetTickCount64();
    const float deltaSeconds=active && g_scopeZoomLastMs
        ? static_cast<float>(now-g_scopeZoomLastMs)/1000.0f : 0.0f;
    g_scopeZoomLastMs=active?now:0;
    const float zoom=g_scopeZoomController.Update(
        active,g_scopeZoomStickY.load(std::memory_order_acquire),
        deltaSeconds,g_config.scope_zoom);
    g_scopeRuntimeZoom.store(zoom,std::memory_order_release);
    return g_scopeRefreshScheduler.Advance(active,g_config.scope_refresh_divisor);
}

bool VR_BeginScopeRaster()
{
    if(!g_gameSwapchain || !g_sceneColorRtv || !EnsureScopeCache())
        return false;
    g_scopeRedirected=false;
    g_rasterScope.store(true,std::memory_order_release);
    return true;
}

void VR_CaptureScope()
{
    if(g_scopeRedirected && g_scopeCache)
        g_scopeHasImage.store(true,std::memory_order_release);
}

void VR_EndScopeRaster()
{
    g_rasterScope.store(false,std::memory_order_release);
}

bool VR_GetScopeRenderAspect(float& outAspect)
{
    if (!g_scopeCacheDesc.Width || !g_scopeCacheDesc.Height)
        return false;
    outAspect = static_cast<float>(g_scopeCacheDesc.Width) /
                static_cast<float>(g_scopeCacheDesc.Height);
    return std::isfinite(outAspect) && outAspect > 0.0f;
}


bool VR_RedirectRenderTargets(ID3D11DeviceContext* context, UINT count,
                              ID3D11RenderTargetView* const* input,
                              ID3D11RenderTargetView** output)
{
#if HALOMCCVR_EXPERIMENTAL_ODST_BRINGUP
    auto& hudRoute = g_nativeHudEyeRoute;
    if (hudRoute.bypassOmRedirect)
        return false;
#endif
    const int eye = g_rasterEye.load();
    const bool scope=g_rasterScope.load(std::memory_order_acquire);
#if HALOMCCVR_EXPERIMENTAL_ODST_BRINGUP
    const bool nativeHud = hudRoute.active && hudRoute.eye >= 0 &&
        hudRoute.eye <= 1;
#else
    constexpr bool nativeHud = false;
#endif
    if ((!scope && !nativeHud && (eye < 0 || eye > 1)) || !input || !output ||
        !g_gameSwapchain)
        return false;
    int targetEye = eye;
#if HALOMCCVR_EXPERIMENTAL_ODST_BRINGUP
    if (nativeHud)
        targetEye = hudRoute.eye;
#endif
    ID3D11RenderTargetView* target = scope ? g_scopeCacheRtv :
        g_eyeCacheRtvs[targetEye];
    bool changed = false;      // any slot rewritten (scene color or sun shaft)
    bool sceneChanged = false; // scene-color redirect only: marks the eye image valid
    for (UINT i = 0; i < count; ++i)
    {
        output[i] = input[i];
        if (!input[i]) continue;

        // ODST's CHUD phase is logically inside this eye render just like Halo
        // 3, but its recompiled phase can bind the flat output RTV. Redirect
        // only the exact target saved at the proven CHUD phase boundary. This
        // is a pointer comparison in the OM hot hook; all COM work occurs once
        // at phase entry/exit.
#if HALOMCCVR_EXPERIMENTAL_ODST_BRINGUP
        if (nativeHud)
        {
            if (target && input[i] == hudRoute.phaseOutputRtv)
            {
                output[i] = target;
                g_nativeHudProvenOmMatches.fetch_add(1, std::memory_order_relaxed);
                changed = true;
            }
            // A phase-local target which is not the captured proven pointer
            // stays stock. In particular, never enter scene-target discovery
            // while a CHUD phase is active.
            continue;
        }
#endif

        // Normal steady-state path: two pointer comparisons and no COM calls.
        if (g_sceneColorRtv)
        {
            if (input[i] == g_sceneColorRtv && target)
            {
                output[i] = target;
                changed = true;
                sceneChanged = true;
            }
            continue;
        }

        // One-time discovery, before the exact scene RTV has been learned.
        ID3D11Resource* resource = nullptr;
        input[i]->GetResource(&resource);
        ID3D11Texture2D* candidate = nullptr;
        D3D11_TEXTURE2D_DESC candidateDesc{};
        const bool isTexture = resource &&
            SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Texture2D),
                                               reinterpret_cast<void**>(&candidate)));
        if (isTexture)
            candidate->GetDesc(&candidateDesc);

        // Halo 3's completed frame is the unique full-resolution typeless RGBA
        // resource with RTV+SRV+UAV bindings at the end of the inner render.  The
        // preceding typed RGBA RT is an intermediate and can remain black.  Keep
        // our eye caches typed (from the game backbuffer) so they can be sampled
        // directly by the OpenXR blit.
        const bool isInternalSceneColor = i == 0 && candidate &&
            g_gameBackbufferDescValid &&
            candidateDesc.Width == g_gameBackbufferDesc.Width &&
            candidateDesc.Height == g_gameBackbufferDesc.Height &&
            candidateDesc.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS &&
            (candidateDesc.BindFlags & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE |
                                        D3D11_BIND_UNORDERED_ACCESS)) ==
                (D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE |
                 D3D11_BIND_UNORDERED_ACCESS);

        D3D11_RENDER_TARGET_VIEW_DESC sceneViewDesc{};
        if (isInternalSceneColor)
            input[i]->GetDesc(&sceneViewDesc);
        D3D11_TEXTURE2D_DESC eyeDesc = candidateDesc;
        if (isInternalSceneColor)
            eyeDesc.Format = sceneViewDesc.Format;

        if (isInternalSceneColor && EnsureEyeCaches(eyeDesc) && g_eyeCacheRtvs[eye])
        {
            input[i]->AddRef();
            g_sceneColorRtv = input[i];
#if HALOMCCVR_EXPERIMENTAL_ODST_BRINGUP
            g_sceneColorResource = resource;
            resource = nullptr;
#endif
            output[i] = g_eyeCacheRtvs[eye];
            changed = true;
            sceneChanged = true;
            LOG("M2 RASTER: learned scene-color RTV %p; steady-state redirect is pointer-only",
                g_sceneColorRtv);
        }
        if (candidate) candidate->Release();
        if (resource) resource->Release();
    }
    if (sceneChanged)
    {
        if(scope)
        {
            g_scopeRedirected=true;
        }
        else
        {
            g_rasterRedirected[eye] = true;
            VR_TraceEvent("rtv-redirect", eye, 0);
            static std::atomic<unsigned> logged{0};
            if (logged.fetch_add(1) < 4)
                LOG("M2 RASTER: redirected internal scene-color RTV to eye %d target", eye);
        }
    }
    return changed;
}

bool VR_GetHeadPose(float outQuat[4], float outPos[3])
{
    if (!g_headCsInit)
        return false;
    EnterCriticalSection(&g_headCs);
    const bool ok = g_headPoseValid;
    if (ok)
    {
        outQuat[0] = g_headPose.orientation.x;
        outQuat[1] = g_headPose.orientation.y;
        outQuat[2] = g_headPose.orientation.z;
        outQuat[3] = g_headPose.orientation.w;
        outPos[0] = g_headPose.position.x;
        outPos[1] = g_headPose.position.y;
        outPos[2] = g_headPose.position.z;
    }
    LeaveCriticalSection(&g_headCs);
    return ok;
}

void VR_GetPadState(VrPadState& out)
{
    if (!g_headCsInit)
    {
        out = {};
        return;
    }
    EnterCriticalSection(&g_headCs);
    out = g_padState;
    LeaveCriticalSection(&g_headCs);
}

void VR_SetScopeActive(bool active)
{
    g_scopeActive.store(active,std::memory_order_release);
    if(active) return;
    g_scopeResetRequested.store(true,std::memory_order_release);
    g_scopeHasImage.store(false,std::memory_order_release);
}

bool VR_IsScopeActive()
{
    return g_scopeActive.load(std::memory_order_acquire);
}

void VR_RequestScopeToggle()
{
    g_scopeToggleSerial.fetch_add(1,std::memory_order_release);
}

void VR_SetGameHaptics(float amplitude)
{
    g_requestedHaptics.store(std::clamp(amplitude, 0.0f, 1.0f),
        std::memory_order_release);
}

bool VR_GetRightControllerPose(float outQuat[4], float outPos[3])
{
    if (!g_headCsInit)
        return false;
    EnterCriticalSection(&g_headCs);
    const bool ok = g_rightAimPoseValid;
    if (ok)
    {
        outQuat[0] = g_rightAimPose.orientation.x;
        outQuat[1] = g_rightAimPose.orientation.y;
        outQuat[2] = g_rightAimPose.orientation.z;
        outQuat[3] = g_rightAimPose.orientation.w;
        outPos[0] = g_rightAimPose.position.x;
        outPos[1] = g_rightAimPose.position.y;
        outPos[2] = g_rightAimPose.position.z;
    }
    LeaveCriticalSection(&g_headCs);
    return ok;
}

bool VR_GetEyeViewOffset(int eye, float outPosition[3], float outQuat[4])
{
    if (eye < 0 || eye > 1 || !outPosition || !outQuat || g_views.size() < 2)
        return false;
    const XrQuaternionf& a = g_views[0].pose.orientation;
    const XrQuaternionf& b = g_views[1].pose.orientation;
    // Quaternions q and -q encode the same rotation. Align their signs before
    // averaging so a runtime choosing opposite representations cannot collapse
    // the midpoint to zero.
    const float dot = a.x*b.x + a.y*b.y + a.z*b.z + a.w*b.w;
    const float sign = dot < 0.0f ? -1.0f : 1.0f;
    float cx = a.x + b.x*sign, cy = a.y + b.y*sign;
    float cz = a.z + b.z*sign, cw = a.w + b.w*sign;
    const float len = sqrtf(cx * cx + cy * cy + cz * cz + cw * cw);
    if (!std::isfinite(len) || len < 1e-5f)
        return false;
    cx /= len; cy /= len; cz /= len; cw /= len;

    // The VIEW reference origin is the centroid of the stereo view origins.
    // Reconstruct the same midpoint from this atomic xrLocateViews result, then
    // express each eye's position in that midpoint's local axes. This preserves
    // the runtime's actual, possibly adjustable IPD and any non-horizontal eye
    // offset instead of imposing the PSVR2 baseline on every headset.
    const XrVector3f& ap = g_views[0].pose.position;
    const XrVector3f& bp = g_views[1].pose.position;
    const float separationX = bp.x-ap.x;
    const float separationY = bp.y-ap.y;
    const float separationZ = bp.z-ap.z;
    const float separation = sqrtf(separationX*separationX +
                                   separationY*separationY +
                                   separationZ*separationZ);
    if (!std::isfinite(separation) || separation < 0.03f || separation > 0.10f)
        return false;
    const XrVector3f& ep = g_views[eye].pose.position;
    const XrVector3f delta{
        ep.x - (ap.x+bp.x)*0.5f,
        ep.y - (ap.y+bp.y)*0.5f,
        ep.z - (ap.z+bp.z)*0.5f};
    const XrVector3f local = Rotate({-cx,-cy,-cz,cw},delta);
    if (!std::isfinite(local.x) || !std::isfinite(local.y) ||
        !std::isfinite(local.z))
        return false;
    outPosition[0]=local.x;
    outPosition[1]=local.y;
    outPosition[2]=local.z;

    // relative = conj(center) * eye
    const XrQuaternionf& e = g_views[eye].pose.orientation;
    outQuat[0] = cw * e.x - cx * e.w - cy * e.z + cz * e.y;
    outQuat[1] = cw * e.y + cx * e.z - cy * e.w - cz * e.x;
    outQuat[2] = cw * e.z - cx * e.y + cy * e.x - cz * e.w;
    outQuat[3] = cw * e.w + cx * e.x + cy * e.y + cz * e.z;
    return std::isfinite(outQuat[0]) && std::isfinite(outQuat[1]) &&
           std::isfinite(outQuat[2]) && std::isfinite(outQuat[3]);
}

float VR_GetScopeZoom()
{
    return g_scopeRuntimeZoom.load(std::memory_order_acquire);
}

bool VR_BeginAuthoredReticleCapture()
{
    if (!g_context || !g_config.crosshair ||
        !g_stereoEnabled.load(std::memory_order_relaxed) ||
        !g_preparedFrame.begun || g_reticleCaptureState.active)
        return false;

    // Prove that the runtime can host the hand-ray composition layer before
    // diverting Halo's native crosshair away from the eye render. Previously
    // the native widget could be hidden first and the swapchain could fail
    // later, leaving no crosshair on any headset using that runtime path.
    if (!EnsureReticleChain() || !EnsureAuthoredReticleTexture())
        return false;

    const uint64_t serial = g_preparedFrame.serial;
    if (g_authoredReticleSerial != serial)
    {
        const float clear[4] = {0, 0, 0, 0};
        g_context->ClearRenderTargetView(g_authoredReticleRtv, clear);
        g_authoredReticleSerial = serial;
        g_authoredReticleReady = false;
    }

    auto& saved = g_reticleCaptureState;
    g_context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT,
                                  saved.rtvs, &saved.dsv);
    saved.viewportCount =
        D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    g_context->RSGetViewports(&saved.viewportCount, saved.viewports);
    saved.scissorCount =
        D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    g_context->RSGetScissorRects(&saved.scissorCount, saved.scissors);

    D3D11_VIEWPORT captureViewport{};
    if (saved.viewportCount)
        captureViewport = saved.viewports[0];
    else
    {
        captureViewport.Width =
            static_cast<float>(g_gameBackbufferDesc.Width);
        captureViewport.Height =
            static_cast<float>(g_gameBackbufferDesc.Height);
        captureViewport.MinDepth = 0.0f;
        captureViewport.MaxDepth = 1.0f;
    }
    // Preserve Halo's screen-pixel authored proportions while cropping around
    // screen center. A 2x viewport zoom makes the stock reticles legible in
    // the angular-size range previously used by the procedural VR reticle.
    constexpr float kCaptureZoom = 2.0f;
    captureViewport.Width *= kCaptureZoom;
    captureViewport.Height *= kCaptureZoom;
    captureViewport.TopLeftX =
        (static_cast<float>(kReticleSize) - captureViewport.Width) * 0.5f;
    captureViewport.TopLeftY =
        (static_cast<float>(kReticleSize) - captureViewport.Height) * 0.5f;
    const D3D11_RECT captureScissor{
        0, 0, static_cast<LONG>(kReticleSize),
        static_cast<LONG>(kReticleSize)};
    g_context->OMSetRenderTargets(1, &g_authoredReticleRtv, nullptr);
    g_context->RSSetViewports(1, &captureViewport);
    g_context->RSSetScissorRects(1, &captureScissor);
    saved.active = true;
    return true;
}

void VR_EndAuthoredReticleCapture()
{
    auto& saved = g_reticleCaptureState;
    if (!saved.active || !g_context)
        return;

    g_context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT,
                                  saved.rtvs, saved.dsv);
    if (saved.viewportCount)
        g_context->RSSetViewports(saved.viewportCount, saved.viewports);
    if (saved.scissorCount)
        g_context->RSSetScissorRects(saved.scissorCount, saved.scissors);

    for (auto*& rtv : saved.rtvs)
    {
        if (rtv) rtv->Release();
        rtv = nullptr;
    }
    if (saved.dsv)
    {
        saved.dsv->Release();
        saved.dsv = nullptr;
    }
    saved.viewportCount = 0;
    saved.scissorCount = 0;
    saved.active = false;
    g_authoredReticleReady = true;
    static bool logged = false;
    if (!logged)
    {
        LOG("M3: Halo authored per-weapon crosshair redirected to VR aim quad");
        logged = true;
    }
}

void VR_SetReticleEnemy(bool enemy)
{
    g_reticleEnemy.store(enemy, std::memory_order_relaxed);
}

bool VR_IsTwoHandAiming() { return g_twoHandActive.load(); }

// The weapon-hand aim pose used by ALL aim consumers (bullet steering, the
// reticle, and the visible-gun barrel). Position is always the right hand.
// Orientation is the right controller's — UNLESS two-handed aim is engaged, in
// which case -Z is swung onto the line from the right hand to the left (support)
// hand, with roll kept from the right controller. Two-hand engages smoothly by
// pose (support hand up near the barrel line) so there is no button to hold.
bool VR_GetAimPose(float outQuat[4], float outPos[3])
{
    if (!g_headCsInit) return false;
    EnterCriticalSection(&g_headCs);
    const bool okR = g_rightAimPoseValid;
    XrQuaternionf rq = g_rightAimPose.orientation;
    XrVector3f rp = g_rightAimPose.position;
    const bool okL = g_leftAimPoseValid;
    XrPosef lpose = g_leftAimPose;
    const float gripL = g_padState.gripL;
    LeaveCriticalSection(&g_headCs);
    if (!okR) return false;
    // Match the activation point: measure the two-hand line to the HAND, not the
    // wrist (same forward shift used by the latch).
    const XrVector3f lp = LeftHandPoint(lpose);

    outPos[0]=rp.x; outPos[1]=rp.y; outPos[2]=rp.z;
    outQuat[0]=rq.x; outQuat[1]=rq.y; outQuat[2]=rq.z; outQuat[3]=rq.w;

    // Apply the user's controller-local mount calibration to the shared aim
    // pose itself. Every right-hand consumer (visible weapon, muzzle, authored
    // reticle and bullet steering) receives this same corrected orientation.
    // The axis/order mapping is equivalent to game.cpp's former local
    // BasisFromAngles(yaw,pitch,roll): OpenXR +Y yaw, +X pitch, -Z roll.
    auto finishAimPose = [&]() {
        auto multiply = [](const XrQuaternionf& a, const XrQuaternionf& b) {
            return XrQuaternionf{
                a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
                a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,
                a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w,
                a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z};
        };
        constexpr float kDegToRad = 0.01745329252f;
        const float yaw = g_config.gun_yaw_deg * kDegToRad;
        const float pitch = g_config.gun_pitch_deg * kDegToRad;
        const float roll = g_config.gun_roll_deg * kDegToRad;
        const XrQuaternionf qYaw{0.0f, sinf(yaw*0.5f), 0.0f, cosf(yaw*0.5f)};
        const XrQuaternionf qPitch{sinf(pitch*0.5f), 0.0f, 0.0f, cosf(pitch*0.5f)};
        const XrQuaternionf qRoll{0.0f, 0.0f, sinf(-roll*0.5f), cosf(roll*0.5f)};
        const XrQuaternionf base{outQuat[0],outQuat[1],outQuat[2],outQuat[3]};
        XrQuaternionf corrected = multiply(base, multiply(multiply(qYaw,qPitch),qRoll));
        const float length = sqrtf(corrected.x*corrected.x + corrected.y*corrected.y +
                                   corrected.z*corrected.z + corrected.w*corrected.w);
        if (!std::isfinite(length) || length < 1e-5f)
            return false;
        outQuat[0]=corrected.x/length; outQuat[1]=corrected.y/length;
        outQuat[2]=corrected.z/length; outQuat[3]=corrected.w/length;
        return true;
    };

    (void)gripL;
    // Engagement is decided once per frame in UpdateTwoHandLatch (toggle/hold +
    // barrel-zone). Here we only APPLY it: when latched, aim along the current
    // right->left hand line.
    if (!g_config.two_handed_aim || !okL || !g_twoHandLatched.load())
    { g_twoHandActive.store(false); return finishAimPose(); }

    const XrVector3f rup  = Rotate(rq, {0,1,0});
    XrVector3f v{lp.x-rp.x, lp.y-rp.y, lp.z-rp.z};
    const float len = sqrtf(v.x*v.x+v.y*v.y+v.z*v.z);
    if (len < 1e-4f) { g_twoHandActive.store(false); return finishAimPose(); }

    XrVector3f af{v.x/len, v.y/len, v.z/len};

    // Toggle mode stays latched after the support hand leaves the barrel. A
    // hand moving above/below the weapon could therefore swing the shared aim
    // line close to vertical even though the weapon controller is horizontal.
    // Keep two-hand aim only inside a generous cone around the raw weapon ray;
    // outside it, fall back to the valid one-hand pose until the hands realign.
    const XrVector3f rawForward = Rotate(rq, {0,0,-1});
    const float agreement = af.x*rawForward.x + af.y*rawForward.y + af.z*rawForward.z;
    if (!std::isfinite(agreement) || agreement < 0.35f)
    {
        g_twoHandActive.store(false);
        static uint64_t lastRejectLogMs = 0;
        const uint64_t now = GetTickCount64();
        if (now - lastRejectLogMs >= 2000)
        {
            LOG("M3: rejected extreme two-hand aim (ray agreement %.2f); using right controller", agreement);
            lastRejectLogMs = now;
        }
        return finishAimPose();
    }

    // Orthonormal basis: X=right, Y=up, Z=-forward, roll from the right hand up.
    auto cross=[](const XrVector3f&a,const XrVector3f&b){
        return XrVector3f{a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x}; };
    XrVector3f xa=cross(af,rup);
    float xl=sqrtf(xa.x*xa.x+xa.y*xa.y+xa.z*xa.z);
    if (xl<1e-4f) { g_twoHandActive.store(false); return finishAimPose(); }
    xa={xa.x/xl,xa.y/xl,xa.z/xl};
    const XrVector3f ya=cross(xa,af);          // up
    const XrVector3f za{-af.x,-af.y,-af.z};     // -forward

    // Rotation matrix (columns xa,ya,za) -> quaternion.
    const float m00=xa.x,m10=xa.y,m20=xa.z;
    const float m01=ya.x,m11=ya.y,m21=ya.z;
    const float m02=za.x,m12=za.y,m22=za.z;
    const float tr=m00+m11+m22;
    float qx,qy,qz,qw;
    if (tr>0){ float s=sqrtf(tr+1.0f)*2; qw=0.25f*s; qx=(m21-m12)/s; qy=(m02-m20)/s; qz=(m10-m01)/s; }
    else if (m00>m11 && m00>m22){ float s=sqrtf(1.0f+m00-m11-m22)*2; qw=(m21-m12)/s; qx=0.25f*s; qy=(m01+m10)/s; qz=(m02+m20)/s; }
    else if (m11>m22){ float s=sqrtf(1.0f+m11-m00-m22)*2; qw=(m02-m20)/s; qx=(m01+m10)/s; qy=0.25f*s; qz=(m12+m21)/s; }
    else { float s=sqrtf(1.0f+m22-m00-m11)*2; qw=(m10-m01)/s; qx=(m02+m20)/s; qy=(m12+m21)/s; qz=0.25f*s; }
    const float ql=sqrtf(qx*qx+qy*qy+qz*qz+qw*qw);
    if (ql<1e-5f) { g_twoHandActive.store(false); return finishAimPose(); }
    outQuat[0]=qx/ql; outQuat[1]=qy/ql; outQuat[2]=qz/ql; outQuat[3]=qw/ql;
    const bool wasActive=g_twoHandActive.exchange(true);
    if (!wasActive) LOG("M3: two-handed aim engaged (left grip held, hand on barrel)");
    return finishAimPose();
}

bool VR_GetLeftControllerPose(float outQuat[4], float outPos[3])
{
    if (!g_headCsInit)
        return false;
    EnterCriticalSection(&g_headCs);
    const bool ok = g_leftAimPoseValid;
    if (ok)
    {
        outQuat[0] = g_leftAimPose.orientation.x;
        outQuat[1] = g_leftAimPose.orientation.y;
        outQuat[2] = g_leftAimPose.orientation.z;
        outQuat[3] = g_leftAimPose.orientation.w;
        outPos[0] = g_leftAimPose.position.x;
        outPos[1] = g_leftAimPose.position.y;
        outPos[2] = g_leftAimPose.position.z;
    }
    LeaveCriticalSection(&g_headCs);
    return ok;
}

bool VR_GetEyeFov(int eye, float outFov[4])
{
    if (eye < 0 || eye > 1 || !outFov)
        return false;
    // xrLocateViews and the game render hook execute on the render thread in
    // this integration, so the latest located view is stable here.
    outFov[0] = g_views[eye].fov.angleLeft;
    outFov[1] = g_views[eye].fov.angleRight;
    outFov[2] = g_views[eye].fov.angleUp;
    outFov[3] = g_views[eye].fov.angleDown;
    return outFov[0] < 0.0f && outFov[1] > 0.0f &&
           outFov[2] > 0.0f && outFov[3] < 0.0f;
}

bool VR_GetGameRenderAspect(float& outAspect)
{
    if (!g_gameBackbufferDescValid || !g_gameBackbufferDesc.Width ||
        !g_gameBackbufferDesc.Height)
        return false;
    outAspect = static_cast<float>(g_gameBackbufferDesc.Width) /
                static_cast<float>(g_gameBackbufferDesc.Height);
    return isfinite(outAspect) && outAspect > 0.1f;
}

void VR_GetStatus(VrStatus& out)
{
    out = g_status;
}
