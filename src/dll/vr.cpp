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
#include <limits>
#include <type_traits>
#include "vr.h"
#include "menu.h"
#include "game.h"
#include "d3d11_hook.h"
#include "d3d_state.h"
#include "smaa_resource.h"
#include "AreaTex.h"
#include "SearchTex.h"
#include "title_adapter.h"
#ifndef HALOMCCVR_EXPERIMENTAL_REACH_RENDER_CANDIDATE
#define HALOMCCVR_EXPERIMENTAL_REACH_RENDER_CANDIDATE 0
#endif
#if HALOMCCVR_EXPERIMENTAL_REACH_RENDER_CANDIDATE
#include "reach_render_candidate.h"
#endif
#include "../common/reach_chud_logic.h"
#include "../common/log.h"
#include "../common/config.h"
#include "../common/frame_pacing_logic.h"
#include "../common/input_logic.h"
#include "../common/scope_logic.h"
#include "../common/pose_inertia_filter.h"

extern "C" IMAGE_DOS_HEADER __ImageBase;

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
    // The headset's real panel rate, read once from the runtime (0 = the runtime
    // does not expose it). Compared against the rate xrWaitFrame targets us at,
    // which is a FRACTION of this whenever the runtime reprojects. No rate is
    // ever assumed: 72, 80, 90, 120, 144 all come from these two numbers.
    bool g_refreshRateExtEnabled = false;
    std::atomic<float> g_panelRefreshHz{0.0f};
    XrEnvironmentBlendMode g_blendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    bool g_sessionRunning = false;
    XrSessionState g_sessionState = XR_SESSION_STATE_UNKNOWN;
    std::atomic<float> g_requestedHaptics{0.0f};
    // Peak-hold companion to g_requestedHaptics: the maximum amplitude requested
    // since the last applied VR frame, so a short gunfire pulse that arrives and
    // clears between two frame samples is not aliased to zero. See SampleHapticPeak.
    std::atomic<float> g_peakHaptics{0.0f};
    void StopControllerHaptics();
    void LogHeadsetPanelRate();
    bool StartFrameWaitThread();
    bool StopFrameWaitThread();

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
    std::atomic<bool> g_reticleChainFailed{false};
    std::atomic<bool> g_authoredReticlePreparationReady{false};
    // A strict Reach swapchain failure aborts the current layer transaction and
    // enters the existing fatal session drain. Some failures (for example a
    // wait timeout) can leave an image acquired; other non-XR_SUCCESS results
    // (for example XR_SESSION_LOSS_PENDING from release) can still mean the
    // operation completed. In either case, submit no layers and pair a begun
    // frame with one empty xrEndFrame before session recovery.
    bool g_abortFrameForReachSwapchainFailure = false;
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
#if HALOMCCVR_EXPERIMENTAL_REACH_RENDER_CANDIDATE
    ReachDirectCopyGate g_reachDirectCopyGate;
    ReachModuleEpoch g_reachDisplayEpoch{};
    uint64_t g_reachDisplayResourceRevision = 0;
    uint64_t g_reachDisplayNextAttemptMs = 0;
    uint64_t g_reachDisplayLastFailureLogMs = 0;
    bool g_reachDisplayReadyLogged = false;
    SRWLOCK g_reachDisplayResourceLock = SRWLOCK_INIT;
    std::atomic<bool> g_reachPresentSoleEligible{false};
    std::atomic<uint64_t> g_reachPresentAvailabilitySetEpochMs{0};
    std::atomic<uint32_t> g_reachPresentGeneration{0};
    std::atomic<uintptr_t> g_reachPresentModuleBase{0};
    std::atomic<uint64_t> g_reachDisplayLifecycleSerial{1};
    std::atomic<uint64_t> g_reachPresentNextSnapshotMs{0};
    std::atomic<bool> g_reachResizeActive{false};
    ID3D11Texture2D* g_reachEyeCache[2]{};
    ID3D11Texture2D* g_reachCaptureSource = nullptr;
    ID3D11Texture2D* g_reachCaptureEyes[2]{};
    ID3D11DeviceContext* g_reachCaptureContext = nullptr;
    ReachDisplaySurfaceProof g_reachCaptureProof{};
    D3D11_TEXTURE2D_DESC g_reachCaptureDesc{};
    std::atomic<bool> g_reachCaptureEnabled{false};
    std::atomic<uint32_t> g_reachCaptureUsers{0};
    std::atomic<uint64_t> g_reachEyeSerial[2]{};
#endif
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
    // Raw poses remain available for controller UI gestures. The canonical
    // aim/hand poses above are the only values gameplay and visible weapon
    // consumers receive, so inertia can never split gun art from its ray.
    XrPosef g_rawRightAimPose{{0, 0, 0, 1}, {0, 0, 0}};
    bool g_rawRightAimPoseValid = false;
    XrPosef g_rawLeftAimPose{{0, 0, 0, 1}, {0, 0, 0}};
    bool g_rawLeftAimPoseValid = false;
    PoseInertiaFilter g_rightWeaponInertia;
    PoseInertiaFilter g_leftWeaponInertia;
    XrTime g_lastControllerPoseTime = 0;
    GameTitle g_weaponInertiaTitle = GameTitle::Unknown;
    // Render-thread-only filtered copy for the compositor crosshair. Keeping it
    // separate preserves the existing optional visual-only reticle smoothing.
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

    // Image-quality pipeline: sharp bicubic resolve + selectable FXAA/SMAA 1x +
    // RCAS-based sharpen, applied when each eye is expanded into the headset.
    // SMAA is lazy/opt-in and adds its third eye-sized target only while selected.
    ID3D11PixelShader* g_iqResolveSharp = nullptr;   // sharp Keys bicubic resolve/upscale
    ID3D11PixelShader* g_iqResolveLinear = nullptr;  // linear resolve (matches old blit)
    ID3D11PixelShader* g_iqFxaa = nullptr;
    ID3D11PixelShader* g_iqRcas = nullptr;
    ID3D11Buffer* g_iqCb = nullptr;
    ID3D11Texture2D* g_iqChain[3] = {nullptr, nullptr, nullptr};
    ID3D11RenderTargetView* g_iqChainRtv[3] = {nullptr, nullptr, nullptr};
    ID3D11ShaderResourceView* g_iqChainSrv[3] = {nullptr, nullptr, nullptr};
    D3D11_TEXTURE2D_DESC g_iqChainDesc{};

    // Official SMAA 1x shaders and immutable lookup tables. They are created on
    // first SMAA selection and remain tiny; the third eye-sized target is the
    // mode-scoped allocation released when SMAA is deselected.
    ID3D11VertexShader* g_smaaEdgesVs = nullptr;
    ID3D11VertexShader* g_smaaWeightsVs = nullptr;
    ID3D11VertexShader* g_smaaNeighborhoodVs = nullptr;
    ID3D11PixelShader* g_smaaEdgesPs = nullptr;
    ID3D11PixelShader* g_smaaWeightsPs = nullptr;
    ID3D11PixelShader* g_smaaNeighborhoodPs = nullptr;
    ID3D11SamplerState* g_smaaPointSampler = nullptr;
    ID3D11Texture2D* g_smaaAreaTex = nullptr;
    ID3D11ShaderResourceView* g_smaaAreaSrv = nullptr;
    ID3D11Texture2D* g_smaaSearchTex = nullptr;
    ID3D11ShaderResourceView* g_smaaSearchSrv = nullptr;
    bool g_smaaInitFailed = false;
    struct IqParams // matches cbuffer IqParams (b0); 32 bytes
    {
        float srcW, srcH, dstW, dstH;
        float sharpness, srcIsSrgb, outPerceptual, aaStrength;
    };

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
    std::atomic<uint64_t> g_preparedViewSerialPublished{0};

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
    TimingRing<512> g_endFrameDurationsMs;
    TimingRing<512> g_renderWindowMs;
    TimingRing<512> g_predictionErrorMs;
    LARGE_INTEGER g_qpcFrequency{};
    LARGE_INTEGER g_lastBeforePresentQpc{};
    LARGE_INTEGER g_beginFrameQpc{};
    uint64_t g_timingLogStartMs = 0;
    XrTime g_lastPredictedDisplayTime = 0;
    // OpenXR's application-frame period, straight from xrWaitFrame. It is the
    // cadence the runtime is currently asking this app to submit at, and can be
    // a fraction of the separately queried physical panel refresh. No rate is
    // assumed: 72/80/90/120/144 and fractional cadences remain raw runtime data.
    std::atomic<uint64_t> g_displayPeriodNs{0};

    // Exact transition capture. The render thread publishes one immutable POD
    // record only after the corresponding DXGI Present returns. The existing
    // 50 ms title worker drains this SPSC queue and is solely responsible for
    // transition detection, formatting, allocation, and LOG/file I/O.
    struct FramePacingRecord
    {
        uint64_t serial = 0;
        uint64_t waitSequence = 0;
        uint64_t overlappingWorkerWaitSequence = 0;
        uint64_t nextWaitDispatchSequence = 0;
        uint32_t sessionEpoch = 0;
        uint32_t layerCount = 0;
        int64_t predictedDisplayTimeNs = 0;
        int64_t predictedDisplayPeriodNs = 0;
        int64_t waitCallStartQpc = 0;
        int64_t waitCallEndQpc = 0;
        int64_t waitReadyQpc = 0;
        int64_t renderWaitStartQpc = 0;
        int64_t renderWaitEndQpc = 0;
        int64_t nextWaitDispatchGateStartQpc = 0;
        int64_t nextWaitDispatchGateEndQpc = 0;
        int64_t beginStartQpc = 0;
        int64_t beginEndQpc = 0;
        int64_t consumedSignalQpc = 0;
        int64_t prepareEndQpc = 0;
        int64_t beforePresentQpc = 0;
        int64_t endStartQpc = 0;
        int64_t endEndQpc = 0;
        int64_t presentStartQpc = 0;
        int64_t presentEndQpc = 0;
        int64_t afterPresentQpc = 0;
        int32_t waitResult = XR_SUCCESS;
        int32_t beginResult = XR_SUCCESS;
        int32_t endResult = XR_SUCCESS;
        int32_t presentResult = S_OK;
        uint32_t eventWaitResult = WAIT_OBJECT_0;
        uint32_t nextWaitEventResult = WAIT_OBJECT_0;
        uint8_t title = static_cast<uint8_t>(GameTitle::None);
        bool waitPacketCoherent = true;
        bool nextWaitDispatchObservedBeforeBegin = false;
        bool shouldRender = false;
        bool focused = false;
        bool stereo = false;
        bool headTracking = false;
        bool scopeActive = false;
        bool submitted = false;
        GameFramePerfCounters perf{};
    };
    static_assert(std::is_trivially_copyable_v<FramePacingRecord>);

    constexpr uint32_t kFramePacingQueueSize = 1024;
    std::array<FramePacingRecord, kFramePacingQueueSize> g_framePacingQueue{};
    std::atomic<uint32_t> g_framePacingQueueHead{0};
    std::atomic<uint32_t> g_framePacingQueueTail{0};
    std::atomic<uint64_t> g_framePacingQueueDrops{0};
    FramePacingRecord g_framePacingPending{}; // render thread only
    GameFramePerfCounters g_framePacingPerfStart{}; // render thread only
    std::atomic<uint32_t> g_framePacingSessionEpoch{0};

    void SubtractPerfCounters(const GameFramePerfCounters& after,
                              const GameFramePerfCounters& before,
                              GameFramePerfCounters& delta)
    {
        auto subtract = [](uint64_t end, uint64_t start) {
            return end >= start ? end - start : 0;
        };
        delta = {};
        delta.viewRenders = subtract(after.viewRenders, before.viewRenders);
        for (int eye = 0; eye < 3; ++eye)
        {
            delta.fpPaletteRequests[eye] = subtract(
                after.fpPaletteRequests[eye], before.fpPaletteRequests[eye]);
            delta.fpPaletteFullSolves[eye] = subtract(
                after.fpPaletteFullSolves[eye],
                before.fpPaletteFullSolves[eye]);
            delta.fpPaletteCacheHits[eye] = subtract(
                after.fpPaletteCacheHits[eye], before.fpPaletteCacheHits[eye]);
            delta.fpPaletteCacheStores[eye] = subtract(
                after.fpPaletteCacheStores[eye],
                before.fpPaletteCacheStores[eye]);
            delta.fpPaletteCacheFull[eye] = subtract(
                after.fpPaletteCacheFull[eye], before.fpPaletteCacheFull[eye]);
        }
        delta.zoomLogWrites = subtract(
            after.zoomLogWrites, before.zoomLogWrites);
        delta.viewRateLogWrites = subtract(
            after.viewRateLogWrites, before.viewRateLogWrites);
        delta.paletteRateLogWrites = subtract(
            after.paletteRateLogWrites, before.paletteRateLogWrites);
        delta.cameraRateLogWrites = subtract(
            after.cameraRateLogWrites, before.cameraRateLogWrites);
        delta.fpDriverRateLogWrites = subtract(
            after.fpDriverRateLogWrites, before.fpDriverRateLogWrites);
    }

    void PublishFramePacingRecord(const FramePacingRecord& record)
    {
        const uint32_t head =
            g_framePacingQueueHead.load(std::memory_order_relaxed);
        const uint32_t next = (head + 1) % kFramePacingQueueSize;
        if (next == g_framePacingQueueTail.load(std::memory_order_acquire))
        {
            g_framePacingQueueDrops.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        g_framePacingQueue[head] = record;
        g_framePacingQueueHead.store(next, std::memory_order_release);
    }

    bool ConsumeFramePacingRecord(FramePacingRecord& record)
    {
        const uint32_t tail =
            g_framePacingQueueTail.load(std::memory_order_relaxed);
        if (tail == g_framePacingQueueHead.load(std::memory_order_acquire))
            return false;
        record = g_framePacingQueue[tail];
        g_framePacingQueueTail.store(
            (tail + 1) % kFramePacingQueueSize, std::memory_order_release);
        return true;
    }

    constexpr size_t kFramePacingPreFrames = 64;
    constexpr size_t kFramePacingPostFrames = 128;
    constexpr size_t kFramePacingCaptureFrames =
        kFramePacingPreFrames + 1 + kFramePacingPostFrames;
    struct FramePacingWorkerState
    {
        std::array<FramePacingRecord, kFramePacingPreFrames> history{};
        size_t historyNext = 0;
        size_t historyCount = 0;
        std::array<FramePacingRecord, kFramePacingCaptureFrames> capture{};
        size_t captureCount = 0;
        size_t triggerIndex = 0;
        size_t postRemaining = 0;
        int64_t oldPeriodNs = 0;
        int64_t newPeriodNs = 0;
        uint64_t dropsAtStart = 0;
        uint64_t lastDataMs = 0;
        bool capturing = false;
        bool exact = true;
    };
    FramePacingWorkerState g_framePacingWorker{}; // title worker only

    bool IsEligiblePacingRecord(const FramePacingRecord& record)
    {
        return record.predictedDisplayPeriodNs > 0 && record.shouldRender &&
            record.focused && record.stereo && record.headTracking;
    }

    bool IsExactWaitRecord(const FramePacingRecord& record)
    {
        return record.waitPacketCoherent && record.waitSequence != 0 &&
            record.nextWaitDispatchObservedBeforeBegin &&
            record.nextWaitEventResult == WAIT_OBJECT_0;
    }

    bool IsExactWaitPair(const FramePacingRecord& previous,
                         const FramePacingRecord& current)
    {
        return IsExactWaitRecord(previous) && IsExactWaitRecord(current) &&
            current.waitSequence == previous.waitSequence + 1;
    }

    const FramePacingRecord* LastPacingHistory(
        const FramePacingWorkerState& worker)
    {
        if (!worker.historyCount)
            return nullptr;
        const size_t index =
            (worker.historyNext + kFramePacingPreFrames - 1) %
            kFramePacingPreFrames;
        return &worker.history[index];
    }

    void AddPacingHistory(FramePacingWorkerState& worker,
                          const FramePacingRecord& record)
    {
        worker.history[worker.historyNext] = record;
        worker.historyNext =
            (worker.historyNext + 1) % kFramePacingPreFrames;
        if (worker.historyCount < kFramePacingPreFrames)
            ++worker.historyCount;
    }

    void ClearPacingHistory(FramePacingWorkerState& worker)
    {
        worker.historyNext = 0;
        worker.historyCount = 0;
    }

    void BeginPacingCapture(FramePacingWorkerState& worker,
                            const FramePacingRecord& trigger,
                            int64_t oldPeriodNs)
    {
        worker.captureCount = 0;
        worker.exact = true;
        const size_t first =
            (worker.historyNext + kFramePacingPreFrames -
             worker.historyCount) % kFramePacingPreFrames;
        for (size_t i = 0; i < worker.historyCount; ++i)
        {
            const FramePacingRecord& prior =
                worker.history[(first + i) % kFramePacingPreFrames];
            if (worker.captureCount &&
                !IsExactWaitPair(
                    worker.capture[worker.captureCount - 1], prior))
            {
                worker.exact = false;
            }
            worker.capture[worker.captureCount++] = prior;
            if (!IsExactWaitRecord(prior))
                worker.exact = false;
        }
        worker.triggerIndex = worker.captureCount;
        worker.capture[worker.captureCount++] = trigger;
        worker.postRemaining = kFramePacingPostFrames;
        worker.oldPeriodNs = oldPeriodNs;
        worker.newPeriodNs = trigger.predictedDisplayPeriodNs;
        worker.dropsAtStart =
            g_framePacingQueueDrops.load(std::memory_order_relaxed);
        worker.lastDataMs = GetTickCount64();
        worker.capturing = true;
        if (!IsExactWaitRecord(trigger) ||
            (worker.triggerIndex && !IsExactWaitPair(
                worker.capture[worker.triggerIndex - 1], trigger)))
        {
            worker.exact = false;
        }
    }

    double PacingQpcMs(int64_t end, int64_t start, int64_t frequency)
    {
        if (!end || !start || !frequency)
            return -1.0;
        return static_cast<double>(end - start) * 1000.0 /
            static_cast<double>(frequency);
    }

    void LogPacingCapture(const FramePacingWorkerState& worker,
                          const char* completion)
    {
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        const uint64_t dropsNow =
            g_framePacingQueueDrops.load(std::memory_order_relaxed);
        const uint64_t dropsDuring = dropsNow - worker.dropsAtStart;
        const bool exact = worker.exact && dropsDuring == 0;
        const double oldHz = worker.oldPeriodNs > 0
            ? 1000000000.0 / static_cast<double>(worker.oldPeriodNs) : 0.0;
        const double newHz = worker.newPeriodNs > 0
            ? 1000000000.0 / static_cast<double>(worker.newPeriodNs) : 0.0;
        const size_t postCount = worker.captureCount > worker.triggerIndex
            ? worker.captureCount - worker.triggerIndex - 1 : 0;

        std::string text;
        text.reserve(worker.captureCount * 640 + 2048);
        char line[1536]{};
        snprintf(line, sizeof(line),
            "PACING TRANSITION CAPTURE: %lldns (%.3fHz) -> %lldns "
            "(%.3fHz), triggerSerial=%llu, pre=%zu post=%zu, "
            "completion=%s exact=%d queueDrops=%llu panel=%.3fHz\n",
            static_cast<long long>(worker.oldPeriodNs), oldHz,
            static_cast<long long>(worker.newPeriodNs), newHz,
            static_cast<unsigned long long>(
                worker.capture[worker.triggerIndex].serial),
            worker.triggerIndex, postCount, completion, exact ? 1 : 0,
            static_cast<unsigned long long>(dropsDuring),
            static_cast<double>(
                g_panelRefreshHz.load(std::memory_order_relaxed)));
        text.append(line);
        text.append(
            "PACING F fields: rel serial epoch edge periodNs appHz predDeltaMs "
            "beginStepMs presentStepMs waitSeq source conflictSeq dispatchSeq "
            "dispatchSeen coherent workerWaitMs readyAgeMs eventWaitMs "
            "dispatchAckMs "
            "beginMs pairExact resumeMs "
            "waitStartVsPrevBeginMs waitEndVsPrevBeginMs prepareMs "
            "gameMs submitBuildMs renderWindowMs endMs preDxgiMs dxgiMs "
            "afterDxgiMs flags(S/F/T/H/C) layers title xr(wait/begin/end) "
            "eventWait/nextWait dxgiHr\n");
        text.append(
            "PACING C fields: serial views requests[e0/e1/out] "
            "fullSolves[e0/e1/out] hits[e0/e1/out] stores[e0/e1/out] "
            "cacheFull[e0/e1/out] hotLogs[zoom/view/palette/camera/fpDriver]\n");
        text.append(
            "PACING C semantics: views=Halo3 outer stereo transactions; "
            "fullSolves=successful non-explicit arm-IK misses; "
            "out=eye -1 (includes scope and other outside-eye work)\n");

        for (size_t i = 0; i < worker.captureCount; ++i)
        {
            const FramePacingRecord& record = worker.capture[i];
            const FramePacingRecord* previous = i ? &worker.capture[i - 1] : nullptr;
            const bool contiguous = previous &&
                previous->sessionEpoch == record.sessionEpoch &&
                previous->serial + 1 == record.serial;
            const bool edge = contiguous && IsMaterialFramePeriodTransition(
                static_cast<uint64_t>(previous->predictedDisplayPeriodNs),
                static_cast<uint64_t>(record.predictedDisplayPeriodNs));
            const double predictedDeltaMs = contiguous
                ? static_cast<double>(record.predictedDisplayTimeNs -
                    previous->predictedDisplayTimeNs) / 1000000.0 : -1.0;
            const double beginStepMs = contiguous
                ? PacingQpcMs(record.beginEndQpc, previous->beginEndQpc,
                              frequency.QuadPart) : -1.0;
            const double presentStepMs = contiguous
                ? PacingQpcMs(record.presentStartQpc,
                              previous->presentStartQpc,
                              frequency.QuadPart) : -1.0;
            const bool exactPair = contiguous &&
                IsExactWaitPair(*previous, record);
            const double resumeMs = exactPair
                ? PacingQpcMs(record.waitCallStartQpc,
                              previous->consumedSignalQpc,
                              frequency.QuadPart) : -1.0;
            const double waitStartVsPreviousBeginMs = exactPair
                ? PacingQpcMs(record.waitCallStartQpc,
                              previous->beginStartQpc,
                              frequency.QuadPart) : -1.0;
            const double waitEndVsPreviousBeginMs = exactPair
                ? PacingQpcMs(record.waitCallEndQpc,
                              previous->beginEndQpc,
                              frequency.QuadPart) : -1.0;
            const long long relative = static_cast<long long>(i) -
                static_cast<long long>(worker.triggerIndex);
            const double appHz = record.predictedDisplayPeriodNs > 0
                ? 1000000000.0 /
                    static_cast<double>(record.predictedDisplayPeriodNs) : 0.0;

            snprintf(line, sizeof(line),
                "PACING F rel=%+lld serial=%llu epoch=%u edge=%d "
                "periodNs=%lld appHz=%.3f predDeltaMs=%.3f "
                "beginStepMs=%.3f presentStepMs=%.3f waitSeq=%llu "
                "source=W conflictSeq=%llu dispatchSeq=%llu dispatchSeen=%d "
                "coherent=%d workerWaitMs=%.3f readyAgeMs=%.3f "
                "eventWaitMs=%.3f dispatchAckMs=%.3f beginMs=%.3f "
                "pairExact=%d resumeMs=%.3f waitStartVsPrevBeginMs=%.3f "
                "waitEndVsPrevBeginMs=%.3f prepareMs=%.3f gameMs=%.3f "
                "submitBuildMs=%.3f renderWindowMs=%.3f endMs=%.3f "
                "preDxgiMs=%.3f dxgiMs=%.3f afterDxgiMs=%.3f "
                "flags=%d/%d/%d/%d/%d layers=%u title=%u "
                "xr=%d/%d/%d event=%lu/%lu dxgi=0x%08X\n",
                relative, static_cast<unsigned long long>(record.serial),
                record.sessionEpoch, edge ? 1 : 0,
                static_cast<long long>(record.predictedDisplayPeriodNs), appHz,
                predictedDeltaMs, beginStepMs, presentStepMs,
                static_cast<unsigned long long>(record.waitSequence),
                static_cast<unsigned long long>(
                    record.overlappingWorkerWaitSequence),
                static_cast<unsigned long long>(
                    record.nextWaitDispatchSequence),
                record.nextWaitDispatchObservedBeforeBegin ? 1 : 0,
                record.waitPacketCoherent ? 1 : 0,
                PacingQpcMs(record.waitCallEndQpc,
                            record.waitCallStartQpc, frequency.QuadPart),
                PacingQpcMs(record.renderWaitStartQpc,
                            record.waitReadyQpc, frequency.QuadPart),
                PacingQpcMs(record.renderWaitEndQpc,
                            record.renderWaitStartQpc, frequency.QuadPart),
                PacingQpcMs(record.nextWaitDispatchGateEndQpc,
                            record.nextWaitDispatchGateStartQpc,
                            frequency.QuadPart),
                PacingQpcMs(record.beginEndQpc,
                            record.beginStartQpc, frequency.QuadPart),
                exactPair ? 1 : 0,
                resumeMs,
                waitStartVsPreviousBeginMs,
                waitEndVsPreviousBeginMs,
                PacingQpcMs(record.prepareEndQpc,
                            record.beginEndQpc, frequency.QuadPart),
                PacingQpcMs(record.beforePresentQpc,
                            record.prepareEndQpc, frequency.QuadPart),
                PacingQpcMs(record.endStartQpc,
                            record.beforePresentQpc, frequency.QuadPart),
                PacingQpcMs(record.endStartQpc,
                            record.beginEndQpc, frequency.QuadPart),
                PacingQpcMs(record.endEndQpc,
                            record.endStartQpc, frequency.QuadPart),
                PacingQpcMs(record.presentStartQpc,
                            record.endEndQpc, frequency.QuadPart),
                PacingQpcMs(record.presentEndQpc,
                            record.presentStartQpc, frequency.QuadPart),
                PacingQpcMs(record.afterPresentQpc,
                            record.presentEndQpc, frequency.QuadPart),
                record.shouldRender ? 1 : 0, record.focused ? 1 : 0,
                record.stereo ? 1 : 0, record.headTracking ? 1 : 0,
                record.scopeActive ? 1 : 0, record.layerCount,
                static_cast<unsigned>(record.title), record.waitResult,
                record.beginResult, record.endResult,
                static_cast<unsigned long>(record.eventWaitResult),
                static_cast<unsigned long>(record.nextWaitEventResult),
                static_cast<unsigned>(record.presentResult));
            text.append(line);

            const auto& perf = record.perf;
            snprintf(line, sizeof(line),
                "PACING C serial=%llu views=%llu requests=%llu/%llu/%llu "
                "fullSolves=%llu/%llu/%llu hits=%llu/%llu/%llu "
                "stores=%llu/%llu/%llu cacheFull=%llu/%llu/%llu "
                "hotLogs=%llu/%llu/%llu/%llu/%llu\n",
                static_cast<unsigned long long>(record.serial),
                static_cast<unsigned long long>(perf.viewRenders),
                static_cast<unsigned long long>(perf.fpPaletteRequests[0]),
                static_cast<unsigned long long>(perf.fpPaletteRequests[1]),
                static_cast<unsigned long long>(perf.fpPaletteRequests[2]),
                static_cast<unsigned long long>(perf.fpPaletteFullSolves[0]),
                static_cast<unsigned long long>(perf.fpPaletteFullSolves[1]),
                static_cast<unsigned long long>(perf.fpPaletteFullSolves[2]),
                static_cast<unsigned long long>(perf.fpPaletteCacheHits[0]),
                static_cast<unsigned long long>(perf.fpPaletteCacheHits[1]),
                static_cast<unsigned long long>(perf.fpPaletteCacheHits[2]),
                static_cast<unsigned long long>(perf.fpPaletteCacheStores[0]),
                static_cast<unsigned long long>(perf.fpPaletteCacheStores[1]),
                static_cast<unsigned long long>(perf.fpPaletteCacheStores[2]),
                static_cast<unsigned long long>(perf.fpPaletteCacheFull[0]),
                static_cast<unsigned long long>(perf.fpPaletteCacheFull[1]),
                static_cast<unsigned long long>(perf.fpPaletteCacheFull[2]),
                static_cast<unsigned long long>(perf.zoomLogWrites),
                static_cast<unsigned long long>(perf.viewRateLogWrites),
                static_cast<unsigned long long>(perf.paletteRateLogWrites),
                static_cast<unsigned long long>(perf.cameraRateLogWrites),
                static_cast<unsigned long long>(perf.fpDriverRateLogWrites));
            text.append(line);
        }
        LOG("%s", text.c_str());
    }

    void SeedHistoryFromCapture(FramePacingWorkerState& worker)
    {
        const size_t count = worker.captureCount;
        const size_t first = count > kFramePacingPreFrames
            ? count - kFramePacingPreFrames : 0;
        ClearPacingHistory(worker);
        for (size_t i = first; i < count; ++i)
        {
            const FramePacingRecord& record = worker.capture[i];
            const FramePacingRecord* previous = LastPacingHistory(worker);
            if (!IsEligiblePacingRecord(record) ||
                (previous && (previous->sessionEpoch != record.sessionEpoch ||
                              previous->serial + 1 != record.serial)))
            {
                ClearPacingHistory(worker);
                if (!IsEligiblePacingRecord(record))
                    continue;
            }
            AddPacingHistory(worker, record);
        }
    }

    void FinishPacingCapture(FramePacingWorkerState& worker,
                             const char* completion)
    {
        LogPacingCapture(worker, completion);
        SeedHistoryFromCapture(worker);
        worker.capturing = false;
        worker.captureCount = 0;
        worker.postRemaining = 0;
        worker.exact = true;
    }

    // --- Frame-wait pipelining ------------------------------------------
    // xrWaitFrame blocks until the runtime's next display time. Calling it
    // inline on the game's render thread meant the game sat IDLE inside it:
    // measured 11.4ms of a 17.6ms frame while the real work (render + present
    // + blit) was only ~6ms, with prediction error collapsing to 0.03ms -- the
    // loop was locked onto every other display interval instead of merely being
    // slow. This thread absorbs that block so the wait overlaps the game's
    // rendering: by the time the render thread asks for a frame state, the wait
    // has already happened and it proceeds immediately.
    //
    // Pairing stays legal: after the render thread claims Wait(N), it releases
    // the worker immediately BEFORE Begin(N). OpenXR requires the subsequent
    // Wait(N+1) to block until Begin(N), so pacing remains on the worker and the
    // overlap covers exactly the game's own render window.
    //
    // NO refresh rate is referenced here. Whatever period the runtime reports
    // is what we use, so 72/80/90/120/144 and anything future behave the same.
    HANDLE g_waitThread = nullptr;
    HANDLE g_waitReadyEvent = nullptr;    // wait thread -> render thread
    HANDLE g_waitConsumedEvent = nullptr; // render thread -> wait thread
    HANDLE g_waitStartedEvent = nullptr;  // wait thread -> render thread
    std::atomic<bool> g_waitThreadStop{false};
    XrFrameState g_waitedFrameState{XR_TYPE_FRAME_STATE};
    std::atomic<bool> g_waitedStateValid{false};
    std::atomic<bool> g_waitedStateFailed{false};
    std::atomic<XrResult> g_waitedStateResult{XR_SUCCESS};
    std::atomic<uint64_t> g_waitPacketMisses{0};
    std::atomic<uint64_t> g_waitFailuresObserved{0};
    std::atomic<uint64_t> g_waitEventSignalFailures{0};
    std::atomic<uint64_t> g_waitCallSequence{0};
    std::atomic<uint64_t> g_waitCallInFlight{0};
    std::atomic<uint64_t> g_waitConsumedSequence{0};
    std::atomic<bool> g_waitPipelineFaulted{false};
    std::atomic<uint64_t> g_waitedPacketSequence{0};
    std::atomic<uint64_t> g_waitedPacketVersion{0};
    std::atomic<uint32_t> g_waitedSessionEpochStart{0};
    std::atomic<uint32_t> g_waitedSessionEpochEnd{0};
    std::atomic<int64_t> g_waitedCallStartQpc{0};
    std::atomic<int64_t> g_waitedCallEndQpc{0};
    std::atomic<int64_t> g_waitedReadyQpc{0};
    // Render-thread owned. A fatal frame-loop error requests STOPPING first so
    // PollEvents can end the running session before Fail switches VR off.
    const char* g_frameWaitFatalExitReason = nullptr;
    bool g_frameWaitExitRequestAccepted = false;
    uint64_t g_frameWaitExitLastRequestMs = 0;
    uint64_t g_frameWaitExitLastLogMs = 0;

    uint64_t g_missedPredictions = 0;
    uint64_t g_duplicatePredictions = 0;
    uint64_t g_frameOrderFailures = 0;
    std::atomic<uint64_t> g_preparedSerialPublished{0};
    std::atomic<uint64_t> g_prepareQpcPublished{0};
    std::atomic<uint64_t> g_cameraSerialObserved{0};
    std::atomic<uint64_t> g_firstCameraDelayUs{0};
#if HALOMCCVR_EXPERIMENTAL_REACH_RENDER_CANDIDATE
    // Two fixed publication slots carry a coherent head/pad/eye sample from
    // PrepareNextFrame into Reach's later render transaction. The high state
    // bit is an exclusive writer claim; the remaining bits count pinned
    // readers. This closes the stale-reader-before-pin race that a bare
    // two-slot reader counter would leave.
    constexpr uint32_t kReachRenderSnapshotWriter = 0x80000000u;
    constexpr uint32_t kReachRenderSnapshotInvalid = 0xFFFFFFFFu;
    ReachVrRenderSnapshot g_reachRenderSnapshots[2]{};
    std::atomic<uint32_t> g_reachRenderSnapshotStates[2]{};
    std::atomic<uint32_t> g_reachRenderSnapshotIndex{
        kReachRenderSnapshotInvalid};
    static_assert(std::atomic<uint32_t>::is_always_lock_free);
    static_assert(std::is_trivially_copyable_v<ReachVrRenderSnapshot>);
#endif

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
    void ResetWeaponInertia(bool invalidateOutput);

    void Fail(const char* what, XrResult r = XR_SUCCESS)
    {
        char msg[512];
        if (r != XR_SUCCESS)
            snprintf(msg, sizeof(msg), "%s (%s)", what, XrStr(r));
        else
            snprintf(msg, sizeof(msg), "%s", what);
        LOG("VR FAILED: %s", msg);
        ResetWeaponInertia(true);
        g_authoredReticlePreparationReady.store(
            false, std::memory_order_release);
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

    // Acquire an SRV we can sample `src` from. Backbuffers usually can't be used
    // as shader input directly, so we may need an SRV-capable intermediate copy.
    // Returns a borrowed SRV (owned by g_srcSrv / g_intermediateSrv), or nullptr.
    ID3D11ShaderResourceView* AcquireSrcSrv(ID3D11Texture2D* src,
                                            const D3D11_TEXTURE2D_DESC& srcDesc)
    {
        if ((srcDesc.BindFlags & D3D11_BIND_SHADER_RESOURCE) && srcDesc.SampleDesc.Count <= 1)
        {
            if (g_srcSrvKey != src)
            {
                if (g_srcSrv) { g_srcSrv->Release(); g_srcSrv = nullptr; }
                if (FAILED(g_device->CreateShaderResourceView(src, nullptr, &g_srcSrv)))
                    g_srcSrv = nullptr;
                g_srcSrvKey = g_srcSrv ? src : nullptr;
            }
            if (g_srcSrv)
                return g_srcSrv;
        }
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
                return nullptr;
            }
            g_intermediateDesc = d;
        }
        if (srcDesc.SampleDesc.Count > 1)
            g_context->ResolveSubresource(g_intermediate, 0, src, 0, srcDesc.Format);
        else
            g_context->CopyResource(g_intermediate, src);
        return g_intermediateSrv;
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
        // Log every TRANSITION, not just the first blit. Logging once meant the
        // menu's cheap backbuffer blit reported FAST and the log then went silent
        // -- so a switch to the slow path on level load (the in-game scene target
        // is multisampled, the menu backbuffer is not) was invisible. The slow
        // path builds an intermediate and runs a full-screen draw PER EYE PER
        // FRAME at full render size, which is exactly the kind of cost that
        // halves the frame rate the moment a level loads.
        static int loggedPath = -1;
        if (loggedPath != (fastPath ? 1 : 0))
        {
            loggedPath = fastPath ? 1 : 0;
            LOG("PERF: eye blit uses %s path (src %ux%u fmt %d samples %u -> "
                "dst %ux%u xrfmt %d)",
                fastPath?"FAST CopyResource":"SLOW shader",
                srcDesc.Width,srcDesc.Height,(int)srcDesc.Format,
                srcDesc.SampleDesc.Count,
                dstW,dstH,(int)g_xrFormat);
        }
        if (fastPath)
        {
            g_context->CopyResource(dst, src);
            return true;
        }

        if (!EnsureBlitPipeline())
            return false;

        ID3D11ShaderResourceView* srv = AcquireSrcSrv(src, srcDesc);
        if (!srv)
            return false;

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

    // Image-quality pipeline. Compiles the resolve/AA/sharpen pixel shaders and
    // the params constant buffer once; reuses the blit VS/sampler/rasterizer/
    // depth. All passes operate in the display's perceptual (sRGB-encoded) space
    // via UNORM intermediates, decoding to linear only at the final write to the
    // sRGB XR target. The sharp bicubic resolve deliberately uses the LINEAR
    // sampler (its 9-tap form combines bilinear fetches).
    bool EnsureIqPipeline()
    {
        if (g_iqResolveSharp && g_iqResolveLinear && g_iqFxaa && g_iqRcas &&
            g_iqCb)
            return true;
        if (!EnsureBlitPipeline())
            return false;
        auto release=[&](auto*& o){ if (o) o->Release(); o=nullptr; };
        release(g_iqResolveSharp);
        release(g_iqResolveLinear);
        release(g_iqFxaa);
        release(g_iqRcas);
        release(g_iqCb);

        static const char* src = R"(
Texture2D srcTex : register(t0);
SamplerState smp : register(s0);
cbuffer IqParams : register(b0)
{
    float2 srcSize;      // dims of the texture being sampled this pass
    float2 dstSize;      // dims of the render target this pass
    float  sharpness;    // 0..1 UI range; RCAS correction is 2x overdriven
    float  srcIsSrgb;    // 1 = sampling srcTex already returns linear
    float  outPerceptual;// 1 = write perceptual (to UNORM); 0 = write linear (to sRGB RTV)
    float  aaStrength;   // 0 = FXAA, 1 = FXAA Strong
};
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
float3 toLin(float3 c){ return float3(
    c.r <= 0.04045 ? c.r/12.92 : pow((c.r+0.055)/1.055, 2.4),
    c.g <= 0.04045 ? c.g/12.92 : pow((c.g+0.055)/1.055, 2.4),
    c.b <= 0.04045 ? c.b/12.92 : pow((c.b+0.055)/1.055, 2.4)); }
float3 toSrgb(float3 c){ c=saturate(c); return float3(
    c.r <= 0.0031308 ? c.r*12.92 : 1.055*pow(c.r,1.0/2.4)-0.055,
    c.g <= 0.0031308 ? c.g*12.92 : 1.055*pow(c.g,1.0/2.4)-0.055,
    c.b <= 0.0031308 ? c.b*12.92 : 1.055*pow(c.b,1.0/2.4)-0.055); }
float3 srcLinear(float4 s){ return srcIsSrgb > 0.5 ? s.rgb : toLin(s.rgb); }
float3 encodeOut(float3 lin){ return outPerceptual > 0.5 ? toSrgb(lin) : lin; }
float3 finishPerceptual(float3 c){ return outPerceptual > 0.5 ? c : toLin(c); }
float lumaP(float3 c){ return dot(c, float3(0.299, 0.587, 0.114)); }

// Keys bicubic with a=-0.75. Catmull-Rom (a=-0.5) was mathematically valid but
// too close to linear at the modest horizontal scale used by this headset.
// The stronger negative lobe is still bounded, while making the F1 A/B visible.
float4 sampleSharpBicubic(float2 uv)
{
    float2 texSize = srcSize;
    float2 samplePos = uv * texSize;
    float2 texPos1 = floor(samplePos - 0.5) + 0.5;
    float2 f = samplePos - texPos1;
    const float a = -0.75;
    float2 w0 = a * f * (1.0 - f) * (1.0 - f);
    float2 w1 = 1.0 - (a + 3.0) * f * f + (a + 2.0) * f * f * f;
    float2 g = 1.0 - f;
    float2 w2 = 1.0 - (a + 3.0) * g * g + (a + 2.0) * g * g * g;
    float2 w3 = a * f * f * (1.0 - f);
    float2 w12 = w1 + w2;
    float2 offset12 = w2 / w12;
    float2 p0 = (texPos1 - 1.0) / texSize;
    float2 p3 = (texPos1 + 2.0) / texSize;
    float2 p12 = (texPos1 + offset12) / texSize;
    float4 r = float4(0,0,0,0);
    r += srcTex.SampleLevel(smp, float2(p0.x,  p0.y ), 0) * (w0.x  * w0.y );
    r += srcTex.SampleLevel(smp, float2(p12.x, p0.y ), 0) * (w12.x * w0.y );
    r += srcTex.SampleLevel(smp, float2(p3.x,  p0.y ), 0) * (w3.x  * w0.y );
    r += srcTex.SampleLevel(smp, float2(p0.x,  p12.y), 0) * (w0.x  * w12.y);
    r += srcTex.SampleLevel(smp, float2(p12.x, p12.y), 0) * (w12.x * w12.y);
    r += srcTex.SampleLevel(smp, float2(p3.x,  p12.y), 0) * (w3.x  * w12.y);
    r += srcTex.SampleLevel(smp, float2(p0.x,  p3.y ), 0) * (w0.x  * w3.y );
    r += srcTex.SampleLevel(smp, float2(p12.x, p3.y ), 0) * (w12.x * w3.y );
    r += srcTex.SampleLevel(smp, float2(p3.x,  p3.y ), 0) * (w3.x  * w3.y );
    return r;
}

float4 ps_resolve_sharp(VSOut i) : SV_Target
{
    float4 s = sampleSharpBicubic(i.uv);
    return float4(encodeOut(srcLinear(s)), s.a);
}
float4 ps_resolve_linear(VSOut i) : SV_Target
{
    float4 s = srcTex.SampleLevel(smp, i.uv, 0);
    return float4(encodeOut(srcLinear(s)), s.a);
}

// FXAA 3.11 console-quality edge smoothing, in perceptual space.
float4 ps_fxaa(VSOut i) : SV_Target
{
    float2 rcp = 1.0 / srcSize;
    float2 uv = i.uv;
    float3 m  = srcTex.SampleLevel(smp, uv, 0).rgb;
    float3 nw = srcTex.SampleLevel(smp, uv + float2(-rcp.x,-rcp.y), 0).rgb;
    float3 ne = srcTex.SampleLevel(smp, uv + float2( rcp.x,-rcp.y), 0).rgb;
    float3 sw = srcTex.SampleLevel(smp, uv + float2(-rcp.x, rcp.y), 0).rgb;
    float3 se = srcTex.SampleLevel(smp, uv + float2( rcp.x, rcp.y), 0).rgb;
    float lM=lumaP(m), lNW=lumaP(nw), lNE=lumaP(ne), lSW=lumaP(sw), lSE=lumaP(se);
    float lMin = min(lM, min(min(lNW,lNE), min(lSW,lSE)));
    float lMax = max(lM, max(max(lNW,lNE), max(lSW,lSE)));
    float edgeThresholdMin = lerp(0.0312, 0.0156, saturate(aaStrength));
    float edgeThreshold = lerp(0.125, 0.063, saturate(aaStrength));
    if ((lMax - lMin) < max(edgeThresholdMin, lMax * edgeThreshold))
        return float4(finishPerceptual(m), 1);
    float2 dir;
    dir.x = -((lNW + lNE) - (lSW + lSE));
    dir.y =  ((lNW + lSW) - (lNE + lSE));
    float dirReduce = max((lNW + lNE + lSW + lSE) * (0.25 * 0.03125), 0.0078125);
    float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);
    float span = lerp(8.0, 12.0, saturate(aaStrength));
    dir = clamp(dir * rcpDirMin, -span, span) * rcp;
    float3 rgbA = 0.5 * (
        srcTex.SampleLevel(smp, uv + dir * (1.0/3.0 - 0.5), 0).rgb +
        srcTex.SampleLevel(smp, uv + dir * (2.0/3.0 - 0.5), 0).rgb);
    float3 rgbB = rgbA * 0.5 + 0.25 * (
        srcTex.SampleLevel(smp, uv + dir * -0.5, 0).rgb +
        srcTex.SampleLevel(smp, uv + dir *  0.5, 0).rgb);
    float lB = lumaP(rgbB);
    float3 result = (lB < lMin || lB > lMax) ? rgbA : rgbB;
    return float4(finishPerceptual(result), 1);
}

float3 loadClamped(int2 p)
{
    p = clamp(p, int2(0, 0), int2(srcSize) - 1);
    return srcTex.Load(int3(p, 0)).rgb;
}

// AMD FidelityFX FSR1 RCAS 32-bit limiter/resolve, followed by a 2x correction
// overdrive. This retains RCAS's safe lobe/denominator and the same five loads;
// the user can pull the intentionally aggressive top end back with the slider.
float4 ps_rcas(VSOut i) : SV_Target
{
    int2 p = int2(i.pos.xy);
    float3 b = loadClamped(p + int2( 0,-1));
    float3 d = loadClamped(p + int2(-1, 0));
    float3 e = loadClamped(p);
    float3 f = loadClamped(p + int2( 1, 0));
    float3 h = loadClamped(p + int2( 0, 1));
    float3 mn4 = min(min(b, d), min(f, h));
    float3 mx4 = max(max(b, d), max(f, h));

    // Solve the negative lobe that cannot clip either side of [0,1], then
    // apply RCAS's documented -0.1875 natural-result limit.
    float3 hitMin = min(mn4, e) / max(4.0 * mx4, 1e-5);
    float3 hitMax = (1.0 - max(mx4, e)) / min(4.0 * mn4 - 4.0, -1e-5);
    float3 lobes = max(-hitMin, hitMax);
    float lobe = max(-0.1875, min(max(lobes.r, max(lobes.g, lobes.b)), 0.0));
    float scaledLobe = lobe * saturate(sharpness);
    float3 rcas = (scaledLobe * (b + d + f + h) + e) /
                  (4.0 * scaledLobe + 1.0);
    float3 res = saturate(e + 2.0 * (rcas - e));
    return float4(finishPerceptual(res), 1);
}
)";
        ID3DBlob* err = nullptr;
        auto compile = [&](const char* entry) -> ID3DBlob* {
            ID3DBlob* out = nullptr;
            const HRESULT hr = D3DCompile(
                src, strlen(src), nullptr, nullptr, nullptr, entry, "ps_5_0",
                D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &out, &err);
            if (FAILED(hr))
            {
                LOG("IQ shader '%s' failed to compile: %s", entry,
                    err ? (const char*)err->GetBufferPointer() : "?");
                if (err) { err->Release(); err = nullptr; }
                return nullptr;
            }
            if (err) { err->Release(); err = nullptr; }
            return out;
        };
        struct Target { const char* entry; ID3D11PixelShader** out; };
        const Target targets[] = {
            {"ps_resolve_sharp",   &g_iqResolveSharp},
            {"ps_resolve_linear",  &g_iqResolveLinear},
            {"ps_fxaa",            &g_iqFxaa},
            {"ps_rcas",            &g_iqRcas},
        };
        for (const auto& t : targets)
        {
            ID3DBlob* blob = compile(t.entry);
            if (!blob) return false;
            HRESULT hr = g_device->CreatePixelShader(blob->GetBufferPointer(),
                                                     blob->GetBufferSize(), nullptr, t.out);
            blob->Release();
            if (FAILED(hr)) return false;
        }
        D3D11_BUFFER_DESC cb{};
        cb.ByteWidth = sizeof(IqParams);
        cb.Usage = D3D11_USAGE_DEFAULT;
        cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(g_device->CreateBuffer(&cb, nullptr, &g_iqCb)))
            return false;
        return true;
    }

    void ReleaseSmaaPipeline()
    {
        auto release = [](auto*& o) { if (o) o->Release(); o = nullptr; };
        release(g_smaaEdgesVs);
        release(g_smaaWeightsVs);
        release(g_smaaNeighborhoodVs);
        release(g_smaaEdgesPs);
        release(g_smaaWeightsPs);
        release(g_smaaNeighborhoodPs);
        release(g_smaaPointSampler);
        release(g_smaaAreaSrv);
        release(g_smaaAreaTex);
        release(g_smaaSearchSrv);
        release(g_smaaSearchTex);
    }

    // Genuine SMAA 1x: official three-stage edge -> blend-weight -> neighborhood
    // pipeline with the highest-coverage 1x preset and official lookup tables.
    // Lazy creation keeps initial Off/FXAA startup unchanged; deselected modes
    // run no SMAA passes and retain no SMAA-sized eye target.
    bool EnsureSmaaPipeline()
    {
        if (g_smaaEdgesVs && g_smaaWeightsVs && g_smaaNeighborhoodVs &&
            g_smaaEdgesPs && g_smaaWeightsPs && g_smaaNeighborhoodPs &&
            g_smaaPointSampler && g_smaaAreaSrv && g_smaaSearchSrv)
            return true;
        if (g_smaaInitFailed)
            return false;

        ReleaseSmaaPipeline();
        auto fail = [&]() {
            ReleaseSmaaPipeline();
            g_smaaInitFailed = true;
            return false;
        };

        HMODULE self = reinterpret_cast<HMODULE>(&__ImageBase);
        auto resourceBytes = [&](int id, const char* name,
                                 const void*& bytes, DWORD& byteCount) {
            HRSRC resource = FindResourceW(
                self, MAKEINTRESOURCEW(id), RT_RCDATA);
            HGLOBAL handle = resource ? LoadResource(self, resource) : nullptr;
            bytes = handle ? LockResource(handle) : nullptr;
            byteCount = resource ? SizeofResource(self, resource) : 0;
            if (!bytes || !byteCount)
                LOG("IQ ERROR: embedded SMAA shader '%s' unavailable (winerr=%lu)",
                    name, GetLastError());
            return bytes && byteCount;
        };

        auto createVs = [&](int id, const char* name, ID3D11VertexShader** out) {
            const void* bytes = nullptr;
            DWORD byteCount = 0;
            if (!resourceBytes(id, name, bytes, byteCount))
                return false;
            const HRESULT hr = g_device->CreateVertexShader(
                bytes, byteCount, nullptr, out);
            if (FAILED(hr))
                LOG("IQ ERROR: SMAA vertex shader '%s' creation failed (0x%08X)",
                    name, (unsigned)hr);
            return SUCCEEDED(hr);
        };
        auto createPs = [&](int id, const char* name, ID3D11PixelShader** out) {
            const void* bytes = nullptr;
            DWORD byteCount = 0;
            if (!resourceBytes(id, name, bytes, byteCount))
                return false;
            const HRESULT hr = g_device->CreatePixelShader(
                bytes, byteCount, nullptr, out);
            if (FAILED(hr))
                LOG("IQ ERROR: SMAA pixel shader '%s' creation failed (0x%08X)",
                    name, (unsigned)hr);
            return SUCCEEDED(hr);
        };

        if (!createVs(IDR_SMAA_EDGES_VS, "edges-vs", &g_smaaEdgesVs) ||
            !createVs(IDR_SMAA_WEIGHTS_VS, "weights-vs", &g_smaaWeightsVs) ||
            !createVs(IDR_SMAA_NEIGHBORHOOD_VS, "neighborhood-vs", &g_smaaNeighborhoodVs) ||
            !createPs(IDR_SMAA_EDGES_PS, "edges-ps", &g_smaaEdgesPs) ||
            !createPs(IDR_SMAA_WEIGHTS_PS, "weights-ps", &g_smaaWeightsPs) ||
            !createPs(IDR_SMAA_NEIGHBORHOOD_PS, "neighborhood-ps", &g_smaaNeighborhoodPs))
            return fail();

        D3D11_SAMPLER_DESC point{};
        point.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        point.AddressU = point.AddressV = point.AddressW =
            D3D11_TEXTURE_ADDRESS_CLAMP;
        HRESULT hr = g_device->CreateSamplerState(&point, &g_smaaPointSampler);
        if (FAILED(hr))
        {
            LOG("IQ ERROR: SMAA point sampler creation failed (0x%08X)",
                (unsigned)hr);
            return fail();
        }

        auto createLut = [&](const unsigned char* bytes, UINT width, UINT height,
                             UINT pitch, DXGI_FORMAT format, const char* name,
                             ID3D11Texture2D** texture,
                             ID3D11ShaderResourceView** srv) {
            D3D11_TEXTURE2D_DESC d{};
            d.Width = width;
            d.Height = height;
            d.MipLevels = 1;
            d.ArraySize = 1;
            d.Format = format;
            d.SampleDesc.Count = 1;
            d.Usage = D3D11_USAGE_IMMUTABLE;
            d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA init{};
            init.pSysMem = bytes;
            init.SysMemPitch = pitch;
            HRESULT lutHr = g_device->CreateTexture2D(&d, &init, texture);
            if (SUCCEEDED(lutHr))
                lutHr = g_device->CreateShaderResourceView(*texture, nullptr, srv);
            if (FAILED(lutHr))
                LOG("IQ ERROR: SMAA %s lookup texture creation failed (0x%08X)",
                    name, (unsigned)lutHr);
            return SUCCEEDED(lutHr);
        };

        static_assert(sizeof(areaTexBytes) == AREATEX_SIZE);
        static_assert(sizeof(searchTexBytes) == SEARCHTEX_SIZE);
        if (!createLut(areaTexBytes, AREATEX_WIDTH, AREATEX_HEIGHT,
                       AREATEX_PITCH, DXGI_FORMAT_R8G8_UNORM, "area",
                       &g_smaaAreaTex, &g_smaaAreaSrv) ||
            !createLut(searchTexBytes, SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT,
                       SEARCHTEX_PITCH, DXGI_FORMAT_R8_UNORM, "search",
                       &g_smaaSearchTex, &g_smaaSearchSrv))
            return fail();

        LOG("IQ: SMAA 1x pipeline ready -- threshold=0.05 search=32 diag=16 "
            "color-edge upstream=71c806a");
        return true;
    }

    void ReleaseIqChain()
    {
        for (auto*& s : g_iqChainSrv) { if (s) s->Release(); s = nullptr; }
        for (auto*& r : g_iqChainRtv) { if (r) r->Release(); r = nullptr; }
        for (auto*& t : g_iqChain)    { if (t) t->Release(); t = nullptr; }
        g_iqChainDesc = {};
    }

    void ReleaseIqChainSlot(int i)
    {
        if (g_iqChainSrv[i]) { g_iqChainSrv[i]->Release(); g_iqChainSrv[i] = nullptr; }
        if (g_iqChainRtv[i]) { g_iqChainRtv[i]->Release(); g_iqChainRtv[i] = nullptr; }
        if (g_iqChain[i]) { g_iqChain[i]->Release(); g_iqChain[i] = nullptr; }
    }

    // Two normal ping-pong intermediates at eye size. Genuine SMAA temporarily
    // needs a third target for blend weights while preserving resolved color.
    // Slot 2 is released as soon as SMAA is no longer selected.
    bool EnsureIqChain(uint32_t w, uint32_t h, bool needSmaa)
    {
        const DXGI_FORMAT format = UnormSibling((DXGI_FORMAT)g_xrFormat);
        if (g_iqChainDesc.Width != w || g_iqChainDesc.Height != h ||
            g_iqChainDesc.Format != format)
            ReleaseIqChain();
        if (!needSmaa)
            ReleaseIqChainSlot(2);

        D3D11_TEXTURE2D_DESC d{};
        d.Width = w;
        d.Height = h;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = format;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        const int targetCount = needSmaa ? 3 : 2;
        for (int i = 0; i < targetCount; ++i)
        {
            if (g_iqChain[i] && g_iqChainRtv[i] && g_iqChainSrv[i])
                continue;
            ReleaseIqChainSlot(i);
            if (FAILED(g_device->CreateTexture2D(&d, nullptr, &g_iqChain[i])) ||
                FAILED(g_device->CreateRenderTargetView(g_iqChain[i], nullptr, &g_iqChainRtv[i])) ||
                FAILED(g_device->CreateShaderResourceView(g_iqChain[i], nullptr, &g_iqChainSrv[i])))
            {
                LOG("IQ ERROR: intermediate chain slot %d creation failed (%ux%u)",
                    i, w, h);
                ReleaseIqChain();
                return false;
            }
        }
        g_iqChainDesc = d;
        return true;
    }

    // Expand one captured eye (src, render resolution) into the XR eye image
    // (dst, headset resolution) with the user's chosen resolve filter, optional
    // anti-aliasing, and optional sharpening. Universal to every title. This
    // function always owns the eye resolve; missing prerequisites fail loudly.
    bool BlitImageQuality(ID3D11Texture2D* src, const D3D11_TEXTURE2D_DESC& srcDesc,
                          ID3D11Texture2D* dst, uint32_t dstW, uint32_t dstH,
                          ID3D11RenderTargetView* dstRtv)
    {
        const bool xrSrgb = IsSrgb((DXGI_FORMAT)g_xrFormat);
        const bool finalPerceptual = !xrSrgb;
        const bool wantSharp = g_config.upscale_filter == 1;     // sharp bicubic resolve vs linear
        const bool wantSmaa = g_config.aa_mode == 3 || g_config.aa_mode == 4;
        const bool wantFxaa = g_config.aa_mode == 1 || g_config.aa_mode == 2 ||
                              g_config.aa_mode == 4;
        const bool fxaaStrong = g_config.aa_mode == 2 || g_config.aa_mode == 4;
        const bool wantAa = wantSmaa || wantFxaa;
        const bool wantRcas = g_config.sharpness > 0.001f;
        const bool post = wantAa || wantRcas;

        // NO FALLBACKS (project policy). The IQ path always renders the eye --
        // "everything off" still runs the linear resolve pass, it never calls
        // back into the stock blit. A genuinely missing prerequisite is a BUG and
        // is surfaced LOUDLY, not hidden by silently reverting to the old look.
        if (!EnsureIqPipeline())
        {
            static bool logged = false;
            if (!logged) { logged = true; LOG("IQ ERROR: shader pipeline unavailable; eye NOT processed"); }
            return false;
        }
        if (wantSmaa && !EnsureSmaaPipeline())
        {
            static bool logged = false;
            if (!logged) { logged = true; LOG("IQ ERROR: SMAA 1x pipeline unavailable; eye NOT processed"); }
            return false;
        }
        ID3D11ShaderResourceView* srcSrv = AcquireSrcSrv(src, srcDesc);
        if (!srcSrv)
        {
            static bool logged = false;
            if (!logged) { logged = true; LOG("IQ ERROR: source SRV unavailable; eye NOT processed"); }
            return false;
        }
        if (!post)
            ReleaseIqChain();
        else if (!EnsureIqChain(dstW, dstH, wantSmaa))
        {
            static bool logged = false;
            if (!logged) { logged = true; LOG("IQ ERROR: intermediate chain unavailable; eye NOT processed"); }
            return false;
        }

        // One-time and on-change: prove in the log exactly what the eye pass does.
        {
            const char* aaName = "off";
            if (g_config.aa_mode == 1) aaName = "fxaa";
            else if (g_config.aa_mode == 2) aaName = "fxaa-strong";
            else if (g_config.aa_mode == 3) aaName = "smaa1x";
            else if (g_config.aa_mode == 4) aaName = "smaa1x+fxaa-strong";
            static int lf = -99, la = -99; static float ls = -1.0f;
            static uint32_t lsw = 0, ldw = 0;
            if (lf != g_config.upscale_filter || la != g_config.aa_mode ||
                ls != g_config.sharpness || lsw != srcDesc.Width || ldw != dstW)
            {
                lf = g_config.upscale_filter; la = g_config.aa_mode;
                ls = g_config.sharpness; lsw = srcDesc.Width; ldw = dstW;
                LOG("IQ: eye pass active -- resolve=%s aa=%s(%d) sharpen=%.2f "
                    "rcasGain=2.00 src=%ux%u dst=%ux%u post=%d xrSrgb=%d",
                    wantSharp ? "bicubic-a075" : "linear", aaName,
                    g_config.aa_mode, g_config.sharpness, srcDesc.Width,
                    srcDesc.Height, dstW, dstH, post ? 1 : 0, xrSrgb ? 1 : 0);
            }
        }

        const float srcIsSrgb = IsSrgb(srcDesc.Format) ? 1.0f : 0.0f;

        D3DStateBackup backup;
        backup.Capture(g_context);
        ID3D11Buffer* savedPsCb0 = nullptr;
        ID3D11Buffer* savedVsCb0 = nullptr;
        ID3D11ShaderResourceView* savedExtraSrvs[2]{};
        ID3D11SamplerState* savedPointSampler = nullptr;
        g_context->PSGetConstantBuffers(0, 1, &savedPsCb0);
        if (wantSmaa)
        {
            g_context->VSGetConstantBuffers(0, 1, &savedVsCb0);
            g_context->PSGetShaderResources(1, 2, savedExtraSrvs);
            g_context->PSGetSamplers(1, 1, &savedPointSampler);
        }

        // Shared pipeline state for every pass.
        g_context->RSSetState(g_blitRasterizer);
        g_context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
        g_context->OMSetDepthStencilState(g_blitDepthOff, 0);
        g_context->IASetInputLayout(nullptr);
        g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g_context->GSSetShader(nullptr, nullptr, 0);
        g_context->PSSetSamplers(0, 1, &g_blitSampler);
        if (wantSmaa)
        {
            g_context->PSSetSamplers(1, 1, &g_smaaPointSampler);
            g_context->VSSetConstantBuffers(0, 1, &g_iqCb);
        }
        g_context->PSSetConstantBuffers(0, 1, &g_iqCb);

        auto pass = [&](ID3D11VertexShader* vs, ID3D11PixelShader* ps,
                         ID3D11ShaderResourceView* in0,
                         ID3D11ShaderResourceView* in1,
                         ID3D11ShaderResourceView* in2,
                         ID3D11RenderTargetView* out, uint32_t inW, uint32_t inH,
                         uint32_t outW, uint32_t outH, bool outPerceptual, float inIsSrgb) {
            IqParams p{};
            p.srcW = (float)inW; p.srcH = (float)inH;
            p.dstW = (float)outW; p.dstH = (float)outH;
            p.sharpness = g_config.sharpness;
            p.srcIsSrgb = inIsSrgb;
            p.outPerceptual = outPerceptual ? 1.0f : 0.0f;
            p.aaStrength = fxaaStrong ? 1.0f : 0.0f;
            g_context->UpdateSubresource(g_iqCb, 0, nullptr, &p, 0, 0);
            g_context->OMSetRenderTargets(1, &out, nullptr);
            D3D11_VIEWPORT vp{0, 0, (float)outW, (float)outH, 0, 1};
            g_context->RSSetViewports(1, &vp);
            g_context->VSSetShader(vs, nullptr, 0);
            g_context->PSSetShader(ps, nullptr, 0);
            ID3D11ShaderResourceView* inputs[3] = {in0, in1, in2};
            const UINT inputCount = (in1 || in2) ? 3u : 1u;
            g_context->PSSetShaderResources(0, inputCount, inputs);
            g_context->Draw(3, 0);
            ID3D11ShaderResourceView* nullSrvs[3]{}; // unbind before an input becomes an RTV
            g_context->PSSetShaderResources(0, inputCount, nullSrvs);
        };

        ID3D11PixelShader* resolve = wantSharp ? g_iqResolveSharp : g_iqResolveLinear;
        if (!post)
        {
            // One pass straight to XR: linear for an sRGB RTV, perceptual for
            // the runtime's non-sRGB fallback formats.
            pass(g_blitVs, resolve, srcSrv, nullptr, nullptr, dstRtv,
                 srcDesc.Width, srcDesc.Height, dstW, dstH,
                 finalPerceptual, srcIsSrgb);
        }
        else
        {
            int cur = 0;
            pass(g_blitVs, resolve, srcSrv, nullptr, nullptr, g_iqChainRtv[cur],
                 srcDesc.Width, srcDesc.Height, dstW, dstH, true, srcIsSrgb);

            if (wantSmaa)
            {
                // Preserve resolved color in slot 0; edge data goes to slot 1,
                // official blend weights to slot 2, then slot 1 is reused for
                // neighborhood output if another pass follows.
                pass(g_smaaEdgesVs, g_smaaEdgesPs, g_iqChainSrv[0], nullptr,
                     nullptr, g_iqChainRtv[1], dstW, dstH, dstW, dstH, true, 1.0f);
                pass(g_smaaWeightsVs, g_smaaWeightsPs, g_iqChainSrv[1],
                     g_smaaAreaSrv, g_smaaSearchSrv, g_iqChainRtv[2],
                     dstW, dstH, dstW, dstH, true, 1.0f);

                const bool smaaFinal = !wantFxaa && !wantRcas;
                ID3D11RenderTargetView* smaaOut =
                    smaaFinal ? dstRtv : g_iqChainRtv[1];
                pass(g_smaaNeighborhoodVs, g_smaaNeighborhoodPs,
                     g_iqChainSrv[0], g_iqChainSrv[2], nullptr, smaaOut,
                     dstW, dstH, dstW, dstH,
                     smaaFinal ? finalPerceptual : true, 1.0f);
                cur = 1;
            }

            if (wantFxaa)
            {
                const bool fxaaFinal = !wantRcas;
                const int next = 1 - cur;
                ID3D11RenderTargetView* fxaaOut =
                    fxaaFinal ? dstRtv : g_iqChainRtv[next];
                pass(g_blitVs, g_iqFxaa, g_iqChainSrv[cur], nullptr, nullptr,
                     fxaaOut, dstW, dstH, dstW, dstH,
                     fxaaFinal ? finalPerceptual : true, 1.0f);
                cur = next;
            }
            if (wantRcas)
            {
                pass(g_blitVs, g_iqRcas, g_iqChainSrv[cur], nullptr, nullptr,
                     dstRtv, dstW, dstH, dstW, dstH,
                     finalPerceptual, 1.0f);
            }
        }

        backup.Restore(g_context);
        g_context->PSSetConstantBuffers(0, 1, &savedPsCb0);
        if (savedPsCb0) savedPsCb0->Release();
        if (wantSmaa)
        {
            g_context->VSSetConstantBuffers(0, 1, &savedVsCb0);
            g_context->PSSetShaderResources(1, 2, savedExtraSrvs);
            g_context->PSSetSamplers(1, 1, &savedPointSampler);
            if (savedVsCb0) savedVsCb0->Release();
            for (auto*& srv : savedExtraSrvs) if (srv) srv->Release();
            if (savedPointSampler) savedPointSampler->Release();
        }
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

#if HALOMCCVR_EXPERIMENTAL_REACH_RENDER_CANDIDATE
    template <typename T>
    class ReachComRef
    {
    public:
        ReachComRef() = default;
        ~ReachComRef() { Reset(); }
        ReachComRef(const ReachComRef&) = delete;
        ReachComRef& operator=(const ReachComRef&) = delete;

        T* Get() const noexcept { return m_value; }
        T* Detach() noexcept
        {
            T* value = m_value;
            m_value = nullptr;
            return value;
        }
        T** Put() noexcept
        {
            Reset();
            return &m_value;
        }
        void Reset() noexcept
        {
            if (m_value)
            {
                m_value->Release();
                m_value = nullptr;
            }
        }

    private:
        T* m_value = nullptr;
    };

    enum class ReachDisplayFailure : uint8_t
    {
        None = 0,
        StaticPreflight,
        ModuleReference,
        EngineFields,
        SwapchainContract,
        EyeAllocation,
        DeviceContext,
        Publication,
    };

    const char* ReachDisplayFailureName(ReachDisplayFailure failure)
    {
        switch (failure)
        {
        case ReachDisplayFailure::None: return "none";
        case ReachDisplayFailure::StaticPreflight: return "static-preflight";
        case ReachDisplayFailure::ModuleReference: return "module-reference";
        case ReachDisplayFailure::EngineFields: return "engine-fields";
        case ReachDisplayFailure::SwapchainContract: return "swapchain-contract";
        case ReachDisplayFailure::EyeAllocation: return "eye-allocation";
        case ReachDisplayFailure::DeviceContext: return "device-context";
        case ReachDisplayFailure::Publication: return "resource-publication";
        default: return "unknown";
        }
    }

    class ReachModuleReference
    {
    public:
        ~ReachModuleReference()
        {
            if (m_module)
                FreeLibrary(m_module);
        }

        bool Acquire(uintptr_t moduleBase) noexcept
        {
            if (m_module || !moduleBase)
                return false;
            HMODULE module = nullptr;
            if (!GetModuleHandleExW(
                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                    reinterpret_cast<LPCWSTR>(moduleBase), &module) ||
                reinterpret_cast<uintptr_t>(module) != moduleBase)
            {
                if (module)
                    FreeLibrary(module);
                return false;
            }
            m_module = module;
            return true;
        }

    private:
        HMODULE m_module = nullptr;
    };

    class ReachExclusiveResourceLock
    {
    public:
        explicit ReachExclusiveResourceLock(SRWLOCK& lock) noexcept
            : m_lock(&lock)
        {
            AcquireSRWLockExclusive(m_lock);
        }
        ~ReachExclusiveResourceLock()
        {
            ReleaseSRWLockExclusive(m_lock);
        }
        ReachExclusiveResourceLock(const ReachExclusiveResourceLock&) = delete;
        ReachExclusiveResourceLock& operator=(
            const ReachExclusiveResourceLock&) = delete;

    private:
        PSRWLOCK m_lock;
    };

    struct ReachDisplayAdmission
    {
        TitleRuntimeAvailabilitySnapshot availability{};
        ReachModuleEpoch epoch{};
        GameTitle activeTitle = GameTitle::None;
        bool coherent = false;
        bool resident = false;
        bool sole = false;
    };

    bool SameReachAvailability(
        const TitleRuntimeAvailabilitySnapshot& left,
        const TitleRuntimeAvailabilitySnapshot& right) noexcept
    {
        return left.stable && right.stable &&
            left.availabilityMask == right.availabilityMask &&
            left.availabilitySetEpochMs == right.availabilitySetEpochMs &&
            left.revision == right.revision &&
            left.moduleBases == right.moduleBases;
    }

    bool ReadReachDisplayAdmission(
        ReachDisplayAdmission& admission) noexcept
    {
        admission = {};
        constexpr GameTitle title = GameTitle::HaloReach;
        constexpr size_t slot = TitleRuntimeSlotIndex(title);
        constexpr uint32_t bit = TitleRuntimeAvailabilityBit(title);
        static_assert(slot < kTitleRuntimeSlotCount);
        static_assert(bit != 0);

        const TitleRuntimeAvailabilitySnapshot before =
            TitleAdapter_GetAvailability();
        const GameTitle activeBefore = TitleAdapter_GetActiveTitle();
        const uint32_t generationBefore =
            TitleAdapter_GetGeneration(title);
        const TitleRuntimeAvailabilitySnapshot after =
            TitleAdapter_GetAvailability();
        const GameTitle activeAfter = TitleAdapter_GetActiveTitle();
        const uint32_t generationAfter =
            TitleAdapter_GetGeneration(title);
        if (!SameReachAvailability(before, after) ||
            activeBefore != activeAfter ||
            generationBefore != generationAfter)
        {
            return false;
        }

        admission.availability = after;
        admission.activeTitle = activeAfter;
        admission.coherent = true;
        admission.resident =
            (after.availabilityMask & bit) != 0 &&
            after.moduleBases[slot] != 0 && generationAfter != 0;
        if (admission.resident)
        {
            admission.epoch = {
                after.moduleBases[slot], generationAfter};
        }
        admission.sole = admission.resident &&
            after.availabilityMask == bit &&
            activeAfter == title;
        return true;
    }

    bool ReachSameDisplayAdmission(
        const ReachDisplayAdmission& left,
        const ReachDisplayAdmission& right) noexcept
    {
        return left.coherent && right.coherent && left.resident && right.resident &&
            left.sole && right.sole &&
            left.activeTitle == right.activeTitle &&
            left.availability.availabilityMask ==
                right.availability.availabilityMask &&
            left.availability.availabilitySetEpochMs ==
                right.availability.availabilitySetEpochMs &&
            ReachSameModuleEpoch(left.epoch, right.epoch);
    }

    void BumpReachDisplayLifecycleSerial() noexcept
    {
        uint64_t current = g_reachDisplayLifecycleSerial.load(
            std::memory_order_relaxed);
        while (current != std::numeric_limits<uint64_t>::max() &&
               !g_reachDisplayLifecycleSerial.compare_exchange_weak(
                   current, current + 1,
                   std::memory_order_release,
                   std::memory_order_relaxed))
        {
        }
    }

    void InvalidateReachPresentAdmission() noexcept
    {
        g_reachPresentSoleEligible.store(false, std::memory_order_release);
        g_reachPresentAvailabilitySetEpochMs.store(0, std::memory_order_release);
        g_reachPresentGeneration.store(0, std::memory_order_release);
        g_reachPresentModuleBase.store(0, std::memory_order_release);
        BumpReachDisplayLifecycleSerial();
    }

    void CaptureReachDisplaySnapshot(
        IDXGISwapChain* presentSwapchain,
        const ReachDisplayAdmission& admission) noexcept;

    // Present contributes only a throttled safe-boundary snapshot with retained
    // swapchain-owned interfaces and descriptors. Hashing, allocation,
    // publication, and logging stay on WaitThread.
    void ObserveReachPresentSwapchain(IDXGISwapChain* presentSwapchain) noexcept
    {
        ReachDisplayAdmission admission{};
        const bool eligible =
            ReadReachDisplayAdmission(admission) && admission.sole;
        const uint64_t availabilitySetEpochMs = eligible
            ? admission.availability.availabilitySetEpochMs : 0;
        const uint32_t generation = eligible
            ? admission.epoch.generation : 0;
        const uintptr_t moduleBase = eligible
            ? admission.epoch.moduleBase : 0;
        const bool changed =
            g_reachPresentSoleEligible.load(
                std::memory_order_relaxed) != eligible ||
            g_reachPresentAvailabilitySetEpochMs.load(
                std::memory_order_relaxed) != availabilitySetEpochMs ||
            g_reachPresentGeneration.load(
                std::memory_order_relaxed) != generation ||
            g_reachPresentModuleBase.load(
                std::memory_order_relaxed) != moduleBase;
        if (changed)
        {
            g_reachPresentSoleEligible.store(
                eligible, std::memory_order_release);
            g_reachPresentAvailabilitySetEpochMs.store(
                availabilitySetEpochMs, std::memory_order_release);
            g_reachPresentGeneration.store(
                generation, std::memory_order_release);
            g_reachPresentModuleBase.store(
                moduleBase, std::memory_order_release);
            BumpReachDisplayLifecycleSerial();
        }
        if (!eligible || !presentSwapchain)
            return;
        CaptureReachDisplaySnapshot(presentSwapchain, admission);
    }

    void ReleaseReachEyeCaches() noexcept
    {
        for (auto*& cache : g_reachEyeCache)
        {
            if (cache)
                cache->Release();
            cache = nullptr;
        }
    }

    bool InvalidateReachCaptureResources() noexcept
    {
        g_reachCaptureEnabled.store(false, std::memory_order_seq_cst);
        for (unsigned spin = 0;
             g_reachCaptureUsers.load(std::memory_order_seq_cst) != 0 &&
             spin < 2000; ++spin)
        {
            Sleep(1);
        }
        if (g_reachCaptureUsers.load(std::memory_order_seq_cst) != 0)
            return false;
        if (g_reachCaptureSource)
            g_reachCaptureSource->Release();
        for (auto*& eye : g_reachCaptureEyes)
        {
            if (eye)
                eye->Release();
            eye = nullptr;
        }
        if (g_reachCaptureContext)
            g_reachCaptureContext->Release();
        g_reachCaptureSource = nullptr;
        g_reachCaptureContext = nullptr;
        g_reachCaptureProof = {};
        g_reachCaptureDesc = {};
        g_reachEyeSerial[0].store(0, std::memory_order_release);
        g_reachEyeSerial[1].store(0, std::memory_order_release);
        return true;
    }

    template <typename T>
    bool ReadReachLocal(uintptr_t address, T& value) noexcept
    {
        if (!address || sizeof(T) >
                std::numeric_limits<uintptr_t>::max() - address)
            return false;
        SIZE_T received = 0;
        return ReadProcessMemory(
                   GetCurrentProcess(),
                   reinterpret_cast<const void*>(address),
                   &value, sizeof(value), &received) &&
            received == sizeof(value);
    }

    uintptr_t ReachComIdentity(IUnknown* value) noexcept
    {
        if (!value)
            return 0;
        IUnknown* identity = nullptr;
        const HRESULT hr = value->QueryInterface(
            __uuidof(IUnknown), reinterpret_cast<void**>(&identity));
        const uintptr_t result = SUCCEEDED(hr) && identity
            ? reinterpret_cast<uintptr_t>(identity)
            : 0;
        if (identity)
            identity->Release();
        return result;
    }

    uintptr_t ReachResourceDeviceIdentity(
        ID3D11Resource* resource) noexcept
    {
        if (!resource)
            return 0;
        ReachComRef<ID3D11Device> device;
        resource->GetDevice(device.Put());
        return ReachComIdentity(device.Get());
    }

    ReachCopyShape ReachShape(
        const D3D11_TEXTURE2D_DESC& desc) noexcept
    {
        return {
            desc.Width,
            desc.Height,
            desc.MipLevels,
            desc.ArraySize,
            static_cast<uint32_t>(desc.Format),
            desc.SampleDesc.Count,
            desc.SampleDesc.Quality,
        };
    }

    bool EnsureReachEyeCaches(
        ID3D11Device* device,
        const D3D11_TEXTURE2D_DESC& sourceDesc,
        const ReachCopyShape& sourceShape) noexcept
    {
        const uintptr_t deviceIdentity = ReachComIdentity(device);
        bool reusable = deviceIdentity != 0;
        uintptr_t existingIdentities[2]{};
        for (size_t eye = 0; eye < 2 && reusable; ++eye)
        {
            if (!g_reachEyeCache[eye])
            {
                reusable = false;
                break;
            }
            D3D11_TEXTURE2D_DESC eyeDesc{};
            g_reachEyeCache[eye]->GetDesc(&eyeDesc);
            existingIdentities[eye] =
                ReachComIdentity(g_reachEyeCache[eye]);
            reusable = existingIdentities[eye] != 0 &&
                ReachSameCopyShape(sourceShape, ReachShape(eyeDesc)) &&
                ReachResourceDeviceIdentity(g_reachEyeCache[eye]) ==
                    deviceIdentity;
        }
        if (reusable &&
            existingIdentities[0] != existingIdentities[1])
        {
            return true;
        }

        D3D11_TEXTURE2D_DESC cacheDesc = sourceDesc;
        cacheDesc.Usage = D3D11_USAGE_DEFAULT;
        cacheDesc.BindFlags =
            D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        cacheDesc.CPUAccessFlags = 0;
        cacheDesc.MiscFlags = 0;
        cacheDesc.MipLevels = 1;
        cacheDesc.ArraySize = 1;
        ReachComRef<ID3D11Texture2D> replacement[2];
        if (FAILED(device->CreateTexture2D(
                &cacheDesc, nullptr, replacement[0].Put())) ||
            FAILED(device->CreateTexture2D(
                &cacheDesc, nullptr, replacement[1].Put())))
        {
            return false;
        }

        const uintptr_t replacementIdentities[2] = {
            ReachComIdentity(replacement[0].Get()),
            ReachComIdentity(replacement[1].Get())};
        D3D11_TEXTURE2D_DESC replacementDesc[2]{};
        replacement[0].Get()->GetDesc(&replacementDesc[0]);
        replacement[1].Get()->GetDesc(&replacementDesc[1]);
        if (!replacementIdentities[0] || !replacementIdentities[1] ||
            replacementIdentities[0] == replacementIdentities[1] ||
            !ReachSameCopyShape(
                sourceShape, ReachShape(replacementDesc[0])) ||
            !ReachSameCopyShape(
                sourceShape, ReachShape(replacementDesc[1])) ||
            ReachResourceDeviceIdentity(replacement[0].Get()) !=
                deviceIdentity ||
            ReachResourceDeviceIdentity(replacement[1].Get()) !=
                deviceIdentity)
        {
            return false;
        }

        // Commit only after both replacements pass every check. Reach never
        // releases or mutates Halo 3/ODST's shared g_eyeCache resources.
        ReleaseReachEyeCaches();
        g_reachEyeCache[0] = replacement[0].Detach();
        g_reachEyeCache[1] = replacement[1].Detach();
        return true;
    }

    struct ReachDisplayFields
    {
        IDXGISwapChain* engineSwapchain = nullptr;
        uint32_t specializationCount = 0;
        uintptr_t surfaceArray = 0;
        ID3D11RenderTargetView* record0Rtv = nullptr;
        ID3D11ShaderResourceView* record0Srv = nullptr;
        ID3D11RenderTargetView* selectedRtv = nullptr;
        uint32_t selectedSpecialization = 0;
    };

    bool ReadReachDisplayFields(
        const ReachModuleEpoch& epoch,
        ReachDisplayFields& fields) noexcept
    {
        if (!ReachModuleEpochValid(epoch))
            return false;
        const uintptr_t group = epoch.moduleBase + kReachDisplayGroupRva;
        if (!ReadReachLocal(
                epoch.moduleBase + kReachDisplaySwapchainRva,
                fields.engineSwapchain) ||
            !ReadReachLocal(
                group + kReachDisplaySurfaceCountOffset,
                fields.specializationCount) ||
            !ReadReachLocal(
                group + kReachDisplaySurfaceArrayOffset,
                fields.surfaceArray) ||
            !fields.surfaceArray ||
            fields.surfaceArray >
                std::numeric_limits<uintptr_t>::max() -
                    kReachDisplaySurfaceSrvOffset ||
            !ReadReachLocal(
                fields.surfaceArray + kReachDisplaySurfaceRtvOffset,
                fields.record0Rtv) ||
            !ReadReachLocal(
                fields.surfaceArray + kReachDisplaySurfaceSrvOffset,
                fields.record0Srv) ||
            !ReadReachLocal(
                epoch.moduleBase + kReachDisplaySelectedRtvRva,
                fields.selectedRtv) ||
            !ReadReachLocal(
                epoch.moduleBase + kReachSelectedSpecializationRva,
                fields.selectedSpecialization))
        {
            return false;
        }
        return fields.engineSwapchain && fields.record0Rtv &&
            fields.record0Srv && fields.selectedRtv;
    }

    bool ReachPresentMatchesEngineSwapchain(
        const ReachModuleEpoch& epoch,
        IDXGISwapChain* presentSwapchain) noexcept
    {
        if (!ReachModuleEpochValid(epoch) || !presentSwapchain)
            return false;
        IDXGISwapChain* engineSwapchain = nullptr;
        return ReadReachLocal(
                   epoch.moduleBase + kReachDisplaySwapchainRva,
                   engineSwapchain) &&
            engineSwapchain == presentSwapchain;
    }

    enum class ReachDisplaySnapshotState : uint8_t
    {
        Free = 0,
        Writing,
        Ready,
        Reading,
    };

    struct ReachDisplaySnapshot
    {
        std::atomic<uint8_t> state{
            static_cast<uint8_t>(ReachDisplaySnapshotState::Free)};
        ReachModuleEpoch epoch{};
        uint64_t availabilitySetEpochMs = 0;
        uint64_t lifecycleSerial = 0;
        ReachDisplayFields fields{};
        ID3D11Texture2D* buffer0 = nullptr;
        ID3D11Device* device = nullptr;
        D3D11_TEXTURE2D_DESC buffer0Desc{};
        DXGI_SWAP_CHAIN_DESC swapchainDesc{};
        uintptr_t buffer0Identity = 0;
        uintptr_t deviceIdentity = 0;
        uintptr_t immediateContextIdentity = 0;
        uint32_t deviceCreationFlags = 0;
    };

    constexpr size_t kReachDisplaySnapshotCapacity = 3;
    std::array<ReachDisplaySnapshot,
               kReachDisplaySnapshotCapacity> g_reachDisplaySnapshots{};

    bool ReachSameDisplayFields(
        const ReachDisplayFields& left,
        const ReachDisplayFields& right) noexcept
    {
        return left.engineSwapchain == right.engineSwapchain &&
            left.specializationCount == right.specializationCount &&
            left.surfaceArray == right.surfaceArray &&
            left.record0Rtv == right.record0Rtv &&
            left.record0Srv == right.record0Srv &&
            left.selectedRtv == right.selectedRtv &&
            left.selectedSpecialization == right.selectedSpecialization;
    }

    void ReleaseReachDisplaySnapshot(
        ReachDisplaySnapshot& snapshot) noexcept
    {
        if (snapshot.device)
            snapshot.device->Release();
        if (snapshot.buffer0)
            snapshot.buffer0->Release();
        if (snapshot.fields.engineSwapchain)
            snapshot.fields.engineSwapchain->Release();
        snapshot.epoch = {};
        snapshot.availabilitySetEpochMs = 0;
        snapshot.lifecycleSerial = 0;
        snapshot.fields = {};
        snapshot.buffer0 = nullptr;
        snapshot.device = nullptr;
        snapshot.buffer0Desc = {};
        snapshot.swapchainDesc = {};
        snapshot.buffer0Identity = 0;
        snapshot.deviceIdentity = 0;
        snapshot.immediateContextIdentity = 0;
        snapshot.deviceCreationFlags = 0;
        snapshot.state.store(
            static_cast<uint8_t>(ReachDisplaySnapshotState::Free),
            std::memory_order_release);
    }

    class ReachDisplaySnapshotLease
    {
    public:
        ~ReachDisplaySnapshotLease()
        {
            if (m_snapshot)
                ReleaseReachDisplaySnapshot(*m_snapshot);
        }

        bool Acquire() noexcept
        {
            if (m_snapshot)
                return false;
            for (auto& snapshot : g_reachDisplaySnapshots)
            {
                uint8_t expected =
                    static_cast<uint8_t>(ReachDisplaySnapshotState::Ready);
                if (snapshot.state.compare_exchange_strong(
                        expected,
                        static_cast<uint8_t>(
                            ReachDisplaySnapshotState::Reading),
                        std::memory_order_acquire,
                        std::memory_order_relaxed))
                {
                    m_snapshot = &snapshot;
                    return true;
                }
            }
            return false;
        }

        const ReachDisplaySnapshot* Get() const noexcept
        {
            return m_snapshot;
        }

    private:
        ReachDisplaySnapshot* m_snapshot = nullptr;
    };

    void DiscardReachDisplaySnapshots(bool waitForWriters) noexcept
    {
        for (auto& snapshot : g_reachDisplaySnapshots)
        {
            uint8_t state = snapshot.state.load(std::memory_order_acquire);
            if (waitForWriters)
            {
                unsigned spins = 0;
                while (state == static_cast<uint8_t>(
                                    ReachDisplaySnapshotState::Writing))
                {
                    if (++spins < 1024)
                        YieldProcessor();
                    else
                        SwitchToThread();
                    state = snapshot.state.load(std::memory_order_acquire);
                }
            }
            uint8_t expected =
                static_cast<uint8_t>(ReachDisplaySnapshotState::Ready);
            if (snapshot.state.compare_exchange_strong(
                    expected,
                    static_cast<uint8_t>(
                        ReachDisplaySnapshotState::Reading),
                    std::memory_order_acquire,
                    std::memory_order_relaxed))
            {
                ReleaseReachDisplaySnapshot(snapshot);
            }
        }
    }

    void CaptureReachDisplaySnapshot(
        IDXGISwapChain* presentSwapchain,
        const ReachDisplayAdmission& admission) noexcept
    {
        if (!presentSwapchain || !admission.sole ||
            g_reachResizeActive.load(std::memory_order_acquire))
        {
            return;
        }

        // An overlay/video Present must never consume the Reach snapshot
        // throttle. This is a raw identity read only; the full stable-boundary
        // snapshot and COM retention still happen after the exact match.
        if (!ReachPresentMatchesEngineSwapchain(
                admission.epoch, presentSwapchain))
        {
            return;
        }

        const uint64_t now = GetTickCount64();
        uint64_t next = g_reachPresentNextSnapshotMs.load(
            std::memory_order_relaxed);
        if (now < next ||
            !g_reachPresentNextSnapshotMs.compare_exchange_strong(
                next, now + 250,
                std::memory_order_relaxed,
                std::memory_order_relaxed))
        {
            return;
        }

        ReachDisplaySnapshot* destination = nullptr;
        for (auto& snapshot : g_reachDisplaySnapshots)
        {
            uint8_t expected =
                static_cast<uint8_t>(ReachDisplaySnapshotState::Free);
            if (snapshot.state.compare_exchange_strong(
                    expected,
                    static_cast<uint8_t>(
                        ReachDisplaySnapshotState::Writing),
                    std::memory_order_acquire,
                    std::memory_order_relaxed))
            {
                destination = &snapshot;
                break;
            }
        }
        if (!destination)
            return;
        if (g_reachResizeActive.load(std::memory_order_acquire))
        {
            destination->state.store(
                static_cast<uint8_t>(ReachDisplaySnapshotState::Free),
                std::memory_order_release);
            return;
        }

        const uint64_t lifecycleSerial =
            g_reachDisplayLifecycleSerial.load(
                std::memory_order_acquire);
        ReachDisplayFields initial{};
        const bool initialReady =
            lifecycleSerial != 0 &&
            lifecycleSerial != std::numeric_limits<uint64_t>::max() &&
            ReadReachDisplayFields(admission.epoch, initial) &&
            initial.engineSwapchain == presentSwapchain &&
            initial.specializationCount == kReachDisplaySurfaceCount &&
            initial.selectedSpecialization == 0 &&
            initial.selectedRtv == initial.record0Rtv;
        if (!initialReady)
        {
            destination->state.store(
                static_cast<uint8_t>(ReachDisplaySnapshotState::Free),
                std::memory_order_release);
            return;
        }

        // Capture only interfaces whose lifetime is owned by this live Present
        // receiver. Raw engine RTV/SRV values remain structural identities and
        // are never dereferenced across whole-rasterizer recovery.
        ReachComRef<ID3D11Texture2D> buffer0;
        ReachComRef<ID3D11Device> device;
        ReachComRef<ID3D11DeviceContext> immediateContext;
        DXGI_SWAP_CHAIN_DESC swapchainDesc{};
        D3D11_TEXTURE2D_DESC buffer0Desc{};
        const bool d3dReady =
            SUCCEEDED(presentSwapchain->GetBuffer(
                0, __uuidof(ID3D11Texture2D),
                reinterpret_cast<void**>(buffer0.Put()))) &&
            buffer0.Get() &&
            SUCCEEDED(presentSwapchain->GetDevice(
                __uuidof(ID3D11Device),
                reinterpret_cast<void**>(device.Put()))) &&
            device.Get() &&
            SUCCEEDED(presentSwapchain->GetDesc(&swapchainDesc));
        if (!d3dReady)
        {
            destination->state.store(
                static_cast<uint8_t>(ReachDisplaySnapshotState::Free),
                std::memory_order_release);
            return;
        }
        const uint32_t deviceCreationFlags = device.Get()->GetCreationFlags();
        if (deviceCreationFlags & D3D11_CREATE_DEVICE_SINGLETHREADED)
        {
            destination->state.store(
                static_cast<uint8_t>(ReachDisplaySnapshotState::Free),
                std::memory_order_release);
            return;
        }
        buffer0.Get()->GetDesc(&buffer0Desc);
        device.Get()->GetImmediateContext(immediateContext.Put());
        const uintptr_t buffer0Identity = ReachComIdentity(buffer0.Get());
        const uintptr_t deviceIdentity = ReachComIdentity(device.Get());
        const uintptr_t immediateContextIdentity =
            ReachComIdentity(immediateContext.Get());
        if (!buffer0Identity || !deviceIdentity ||
            !immediateContextIdentity)
        {
            destination->state.store(
                static_cast<uint8_t>(ReachDisplaySnapshotState::Free),
                std::memory_order_release);
            return;
        }

        ReachDisplayFields final{};
        const bool finalReady =
            !g_reachResizeActive.load(std::memory_order_acquire) &&
            ReadReachDisplayFields(admission.epoch, final) &&
            ReachSameDisplayFields(initial, final) &&
            g_reachDisplayLifecycleSerial.load(
                std::memory_order_acquire) == lifecycleSerial &&
            g_reachPresentSoleEligible.load(
                std::memory_order_acquire) &&
            g_reachPresentAvailabilitySetEpochMs.load(
                std::memory_order_acquire) ==
                admission.availability.availabilitySetEpochMs &&
            g_reachPresentGeneration.load(
                std::memory_order_acquire) == admission.epoch.generation &&
            g_reachPresentModuleBase.load(
                std::memory_order_acquire) == admission.epoch.moduleBase;
        if (!finalReady)
        {
            destination->state.store(
                static_cast<uint8_t>(ReachDisplaySnapshotState::Free),
                std::memory_order_release);
            return;
        }

        destination->epoch = admission.epoch;
        destination->availabilitySetEpochMs =
            admission.availability.availabilitySetEpochMs;
        destination->lifecycleSerial = lifecycleSerial;
        destination->fields = initial;
        presentSwapchain->AddRef();
        destination->buffer0 = buffer0.Detach();
        destination->device = device.Detach();
        destination->buffer0Desc = buffer0Desc;
        destination->swapchainDesc = swapchainDesc;
        destination->buffer0Identity = buffer0Identity;
        destination->deviceIdentity = deviceIdentity;
        destination->immediateContextIdentity = immediateContextIdentity;
        destination->deviceCreationFlags = deviceCreationFlags;
        destination->state.store(
            static_cast<uint8_t>(ReachDisplaySnapshotState::Ready),
            std::memory_order_release);
    }

    ReachDisplayContinuity MakeReachDisplayContinuity(
        const ReachModuleEpoch& epoch, uint64_t revision,
        uintptr_t buffer0Identity,
        const ReachDisplayFields& fields) noexcept
    {
        ReachDisplayContinuity continuity{};
        continuity.epoch = epoch;
        continuity.resourceRevision = revision;
        continuity.swapchainIdentity =
            reinterpret_cast<uintptr_t>(fields.engineSwapchain);
        continuity.buffer0Identity = buffer0Identity;
        continuity.surfaceArrayIdentity = fields.surfaceArray;
        continuity.record0RtvIdentity =
            reinterpret_cast<uintptr_t>(fields.record0Rtv);
        continuity.record0SrvIdentity =
            reinterpret_cast<uintptr_t>(fields.record0Srv);
        continuity.selectedRtvIdentity =
            reinterpret_cast<uintptr_t>(fields.selectedRtv);
        continuity.specializationCount = fields.specializationCount;
        continuity.selectedSpecialization =
            fields.selectedSpecialization;
        return continuity;
    }

    void ResetReachDisplayCandidateLocked(
        bool teardown, bool releaseResources) noexcept
    {
        if (ReachModuleEpochValid(g_reachDisplayEpoch))
        {
            if (teardown)
                g_reachDirectCopyGate.Teardown(g_reachDisplayEpoch);
            else
                g_reachDirectCopyGate.Invalidate(g_reachDisplayEpoch);
        }
        if (teardown)
        {
            g_reachDisplayEpoch = {};
            g_reachDisplayResourceRevision = 0;
        }
        if (releaseResources)
        {
            if (InvalidateReachCaptureResources())
            {
                ReleaseReachEyeCaches();
                DiscardReachDisplaySnapshots(false);
            }
        }
        g_reachDisplayNextAttemptMs = 0;
        g_reachDisplayReadyLogged = false;
    }

    bool BuildReachDisplayProof(
        const ReachDisplaySnapshot& snapshot,
        const ReachModuleEpoch& epoch,
        const ReachPreflightToken& preflight,
        ReachDisplaySurfaceProof& proof,
        ReachDisplayFailure& failure)
    {
        const ReachDisplayFields& initialFields = snapshot.fields;
        const uint64_t lifecycleSerial = snapshot.lifecycleSerial;
        failure = ReachDisplayFailure::EngineFields;
        if (!initialFields.engineSwapchain || !snapshot.buffer0 ||
            !snapshot.device || !snapshot.buffer0Identity ||
            !snapshot.deviceIdentity ||
            !snapshot.immediateContextIdentity ||
            (snapshot.deviceCreationFlags &
             D3D11_CREATE_DEVICE_SINGLETHREADED) != 0 ||
            initialFields.specializationCount !=
                kReachDisplaySurfaceCount ||
            initialFields.selectedSpecialization != 0 ||
            initialFields.selectedRtv != initialFields.record0Rtv)
            return false;

        const uintptr_t bufferIdentity = snapshot.buffer0Identity;
        const DXGI_SWAP_CHAIN_DESC& swapchainDesc = snapshot.swapchainDesc;
        const D3D11_TEXTURE2D_DESC& sourceDesc = snapshot.buffer0Desc;
        // The injected retail run from d0a5434 is authoritative over the
        // earlier static constructor reading: MCC's live Reach Present owner
        // is a two-buffer flip-discard chain. Keep this exact rather than
        // broadening admission to arbitrary flip-model swapchains.
        const bool swapchainContract =
            swapchainDesc.BufferCount == 2 &&
            swapchainDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD &&
            swapchainDesc.BufferDesc.Format ==
                DXGI_FORMAT_R8G8B8A8_UNORM &&
            swapchainDesc.SampleDesc.Count == 1 &&
            swapchainDesc.SampleDesc.Quality == 0 &&
            (swapchainDesc.BufferUsage &
             (DXGI_USAGE_SHADER_INPUT |
              DXGI_USAGE_RENDER_TARGET_OUTPUT)) ==
                (DXGI_USAGE_SHADER_INPUT |
                 DXGI_USAGE_RENDER_TARGET_OUTPUT);
        const ReachCopyShape sourceShape = ReachShape(sourceDesc);
        const bool sourceContract =
            ReachDisplayCopyShapeValid(sourceShape) &&
            sourceDesc.Usage == D3D11_USAGE_DEFAULT &&
            sourceDesc.CPUAccessFlags == 0 &&
            (sourceDesc.BindFlags &
             (D3D11_BIND_RENDER_TARGET |
              D3D11_BIND_SHADER_RESOURCE)) ==
                (D3D11_BIND_RENDER_TARGET |
                 D3D11_BIND_SHADER_RESOURCE);
        if (!swapchainContract || !sourceContract)
        {
            failure = ReachDisplayFailure::SwapchainContract;
            return false;
        }

        if (!EnsureReachEyeCaches(
                snapshot.device, sourceDesc, sourceShape) ||
            !g_reachEyeCache[0] || !g_reachEyeCache[1])
        {
            failure = ReachDisplayFailure::EyeAllocation;
            return false;
        }

        D3D11_TEXTURE2D_DESC eyeDesc[2]{};
        g_reachEyeCache[0]->GetDesc(&eyeDesc[0]);
        g_reachEyeCache[1]->GetDesc(&eyeDesc[1]);
        const ReachCopyShape eyeShape[2] = {
            ReachShape(eyeDesc[0]), ReachShape(eyeDesc[1])};
        const uintptr_t eyeIdentity[2] = {
            ReachComIdentity(g_reachEyeCache[0]),
            ReachComIdentity(g_reachEyeCache[1])};
        if (!eyeIdentity[0] || !eyeIdentity[1] ||
            eyeIdentity[0] == eyeIdentity[1] ||
            eyeIdentity[0] == bufferIdentity ||
            eyeIdentity[1] == bufferIdentity ||
            !ReachSameCopyShape(sourceShape, eyeShape[0]) ||
            !ReachSameCopyShape(sourceShape, eyeShape[1]))
        {
            failure = ReachDisplayFailure::EyeAllocation;
            return false;
        }

        const uintptr_t deviceIdentity = snapshot.deviceIdentity;
        const uintptr_t contextIdentity =
            snapshot.immediateContextIdentity;
        const bool sameDevice = deviceIdentity && contextIdentity &&
            ReachResourceDeviceIdentity(snapshot.buffer0) == deviceIdentity &&
            ReachResourceDeviceIdentity(g_reachEyeCache[0]) == deviceIdentity &&
            ReachResourceDeviceIdentity(g_reachEyeCache[1]) == deviceIdentity;
        const bool immediate = contextIdentity != 0;
        if (!sameDevice || !immediate)
        {
            failure = ReachDisplayFailure::DeviceContext;
            return false;
        }

        ReachDisplayFields finalFields{};
        if (!ReadReachDisplayFields(epoch, finalFields) ||
            finalFields.engineSwapchain != initialFields.engineSwapchain ||
            finalFields.specializationCount !=
                initialFields.specializationCount ||
            finalFields.surfaceArray != initialFields.surfaceArray ||
            finalFields.record0Rtv != initialFields.record0Rtv ||
            finalFields.record0Srv != initialFields.record0Srv ||
            finalFields.selectedRtv != initialFields.selectedRtv ||
            finalFields.selectedSpecialization !=
                initialFields.selectedSpecialization)
        {
            failure = ReachDisplayFailure::EngineFields;
            return false;
        }
        if (g_reachDisplayResourceRevision ==
            std::numeric_limits<uint64_t>::max())
        {
            failure = ReachDisplayFailure::Publication;
            return false;
        }
        ++g_reachDisplayResourceRevision;
        proof = {};
        proof.continuity = MakeReachDisplayContinuity(
            epoch, g_reachDisplayResourceRevision,
            bufferIdentity, finalFields);
        proof.continuity.lifecycleSerial = lifecycleSerial;
        proof.continuity.deviceIdentity = deviceIdentity;
        proof.continuity.immediateContextIdentity = contextIdentity;
        proof.continuity.eyeResourceIdentities[0] = eyeIdentity[0];
        proof.continuity.eyeResourceIdentities[1] = eyeIdentity[1];
        proof.preflight = preflight;
        proof.immediateContextIdentity = contextIdentity;
        proof.eyeResourceIdentities[0] = eyeIdentity[0];
        proof.eyeResourceIdentities[1] = eyeIdentity[1];
        proof.source = sourceShape;
        proof.eyes[0] = eyeShape[0];
        proof.eyes[1] = eyeShape[1];
        proof.readyEyeMask = 0x3u;
        proof.engineSwapchainMatchesPresent = true;
        proof.selectedRtvMatchesRecord0 = true;
        proof.swapchainContract = true;
        proof.sameDevice = true;
        proof.immediateContext = true;
        failure = ReachDisplayFailure::None;
        return ReachDisplaySurfaceProofComplete(proof);
    }

    bool PublishReachCaptureResources(
        const ReachDisplaySnapshot& snapshot,
        const ReachDisplaySurfaceProof& proof) noexcept
    {
        if (!snapshot.buffer0 || !snapshot.device ||
            !g_reachEyeCache[0] || !g_reachEyeCache[1] ||
            !ReachDisplaySurfaceProofComplete(proof))
            return false;
        if (g_reachCaptureEnabled.load(std::memory_order_acquire) &&
            g_reachCaptureSource == snapshot.buffer0 &&
            g_reachCaptureEyes[0] == g_reachEyeCache[0] &&
            g_reachCaptureEyes[1] == g_reachEyeCache[1] &&
            ReachSameModuleEpoch(g_reachCaptureProof.continuity.epoch,
                                 proof.continuity.epoch) &&
            g_reachCaptureProof.continuity.lifecycleSerial ==
                proof.continuity.lifecycleSerial)
            return true;

        if (!InvalidateReachCaptureResources())
            return false;
        ID3D11DeviceContext* context = nullptr;
        snapshot.device->GetImmediateContext(&context);
        if (!context || ReachComIdentity(context) !=
                proof.continuity.immediateContextIdentity)
        {
            if (context)
                context->Release();
            return false;
        }
        snapshot.buffer0->AddRef();
        g_reachEyeCache[0]->AddRef();
        g_reachEyeCache[1]->AddRef();
        g_reachCaptureSource = snapshot.buffer0;
        g_reachCaptureEyes[0] = g_reachEyeCache[0];
        g_reachCaptureEyes[1] = g_reachEyeCache[1];
        g_reachCaptureContext = context;
        g_reachCaptureProof = proof;
        g_reachCaptureDesc = snapshot.buffer0Desc;
        g_reachCaptureEnabled.store(true, std::memory_order_seq_cst);
        return true;
    }

    void PollReachDisplayCandidate()
    {
        ReachExclusiveResourceLock lock(g_reachDisplayResourceLock);
        ReachDisplayAdmission admission{};
        if (!ReadReachDisplayAdmission(admission))
        {
            if (ReachModuleEpochValid(g_reachDisplayEpoch))
                g_reachDirectCopyGate.Invalidate(g_reachDisplayEpoch);
            g_reachDisplayReadyLogged = false;
            return;
        }

        if (!admission.resident)
        {
            ResetReachDisplayCandidateLocked(true, true);
            return;
        }
        if (!admission.sole)
        {
            if (ReachSameModuleEpoch(
                    admission.epoch, g_reachDisplayEpoch))
            {
                // H3/Reach overlap is normal during MCC title transitions.
                // Preserve the resident Reach epoch and its revision high-water
                // so the same generation can safely re-arm after ambiguity.
                ResetReachDisplayCandidateLocked(false, true);
            }
            else
            {
                ResetReachDisplayCandidateLocked(true, true);
            }
            return;
        }

        if (!ReachSameModuleEpoch(
                admission.epoch, g_reachDisplayEpoch))
        {
            ResetReachDisplayCandidateLocked(true, true);
            if (!g_reachDirectCopyGate.AdvanceEpoch(admission.epoch))
                return;
            g_reachDisplayEpoch = admission.epoch;
        }

        const uint64_t lifecycleSerial =
            g_reachDisplayLifecycleSerial.load(
                std::memory_order_acquire);
        const bool presentAdmissionMatches =
            lifecycleSerial != std::numeric_limits<uint64_t>::max() &&
            g_reachPresentSoleEligible.load(
                std::memory_order_acquire) &&
            g_reachPresentAvailabilitySetEpochMs.load(
                std::memory_order_acquire) ==
                admission.availability.availabilitySetEpochMs &&
            g_reachPresentGeneration.load(
                std::memory_order_acquire) ==
                admission.epoch.generation &&
            g_reachPresentModuleBase.load(
                std::memory_order_acquire) ==
                admission.epoch.moduleBase;
        const ReachPreflightToken preflight =
            ReachRenderCandidate_GetPreflight(admission.epoch);
        if (!presentAdmissionMatches || !preflight.Complete() ||
            !ReachRenderCandidate_IsPreflightCurrent(preflight))
        {
            g_reachDirectCopyGate.Invalidate(admission.epoch);
            g_reachDisplayReadyLogged = false;
            return;
        }

        const uint64_t now = GetTickCount64();
        if (now < g_reachDisplayNextAttemptMs)
            return;
        g_reachDisplayNextAttemptMs = now + 250;
        // Every cold refresh revokes the old capability before touching any
        // engine or COM object. A failed replacement can never leave the prior
        // resource proof current.
        g_reachDirectCopyGate.Invalidate(admission.epoch);

        ReachModuleReference moduleReference;
        ReachDisplayFailure failure = ReachDisplayFailure::ModuleReference;
        if (!moduleReference.Acquire(admission.epoch.moduleBase))
        {
            g_reachDisplayReadyLogged = false;
        }
        else
        {
            ReachDisplaySnapshotLease snapshotLease;
            const bool haveSnapshot = snapshotLease.Acquire();
            const ReachDisplaySnapshot* snapshot = snapshotLease.Get();
            const bool snapshotMatches = haveSnapshot && snapshot &&
                ReachSameModuleEpoch(snapshot->epoch, admission.epoch) &&
                snapshot->availabilitySetEpochMs ==
                    admission.availability.availabilitySetEpochMs &&
                snapshot->lifecycleSerial == lifecycleSerial;
            failure = snapshotMatches
                ? ReachDisplayFailure::SwapchainContract
                : ReachDisplayFailure::EngineFields;

            ReachDisplaySurfaceProof proof{};
            const bool built = snapshotMatches &&
                BuildReachDisplayProof(
                    *snapshot, admission.epoch, preflight,
                    proof, failure);
            ReachDisplayAdmission finalAdmission{};
            const bool admissionStillCurrent =
                ReadReachDisplayAdmission(finalAdmission) &&
                ReachSameDisplayAdmission(admission, finalAdmission) &&
                g_reachDisplayLifecycleSerial.load(
                    std::memory_order_acquire) == lifecycleSerial &&
                g_reachPresentSoleEligible.load(
                    std::memory_order_acquire) &&
                g_reachPresentAvailabilitySetEpochMs.load(
                    std::memory_order_acquire) ==
                    admission.availability.availabilitySetEpochMs &&
                g_reachPresentGeneration.load(
                    std::memory_order_acquire) ==
                    admission.epoch.generation &&
                g_reachPresentModuleBase.load(
                    std::memory_order_acquire) ==
                    admission.epoch.moduleBase &&
                ReachRenderCandidate_IsPreflightCurrent(preflight);
            const bool published = built && admissionStillCurrent &&
                g_reachDirectCopyGate.Publish(proof) &&
                PublishReachCaptureResources(*snapshot, proof);
            if (published)
            {
                if (!g_reachDisplayReadyLogged)
                {
                    g_reachDisplayReadyLogged = true;
                    LOG("Reach display worker proof PASS: exact Present "
                        "buffer0, specialization0 structural continuity, "
                        "same device/context identity, and two private exact "
                        "eye caches; camera core remains gated by armed state "
                        "and exact current-frame render access");
                }
                return;
            }
            if (built && !admissionStillCurrent)
                failure = ReachDisplayFailure::Publication;
            else if (built)
                failure = ReachDisplayFailure::Publication;
            g_reachDisplayReadyLogged = false;
        }

        if (!g_reachDisplayLastFailureLogMs ||
            now - g_reachDisplayLastFailureLogMs >= 2000)
        {
            g_reachDisplayLastFailureLogMs = now;
            LOG("Reach display worker proof waiting (%s); stock Reach remains active",
                ReachDisplayFailureName(failure));
        }
    }
#endif

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
        g_preparedViewSerialPublished.store(0, std::memory_order_release);
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

    void EnterFrameWaitFatalDrain(const char* reason)
    {
        ResetWeaponInertia(true);
        g_waitPipelineFaulted.store(true, std::memory_order_release);
        g_waitThreadStop.store(true, std::memory_order_release);
        if (g_waitConsumedEvent)
            SetEvent(g_waitConsumedEvent);
        if (g_waitReadyEvent)
            SetEvent(g_waitReadyEvent);
        if (g_waitStartedEvent)
            SetEvent(g_waitStartedEvent);
        g_authoredReticlePreparationReady.store(
            false, std::memory_order_release);
        g_sessionRunning = false;
        if (!g_frameWaitFatalExitReason)
            g_frameWaitFatalExitReason = reason;
        g_frameWaitExitRequestAccepted = false;
        g_frameWaitExitLastRequestMs = 0;
        g_frameWaitExitLastLogMs = 0;
        Game_DetachForVrRuntimeFailure();
    }

    bool RequireReachSwapchainCompletion(
        XrResult result, const char* failureReason)
    {
        // Reach accepts only exact XR_SUCCESS. XR_TIMEOUT_EXPIRED does not
        // complete a wait; XR_SESSION_LOSS_PENDING is a successful operation
        // result but signals an unhealthy session. Both abort this candidate's
        // complete layer transaction and enter terminal session recovery. Do
        // not attempt another acquire/release from this frame.
        if (result == XR_SUCCESS)
            return true;
        g_abortFrameForReachSwapchainFailure = true;
        EnterFrameWaitFatalDrain(failureReason);
        return false;
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
                    ResetWeaponInertia(true);
                    XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
                    bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    XrResult r = xrBeginSession(g_session, &bi);
                    if (XR_SUCCEEDED(r))
                    {
                        g_framePacingSessionEpoch.fetch_add(
                            1, std::memory_order_release);
                        if (StartFrameWaitThread())
                        {
                            g_sessionRunning = true;
                            // Publish only after xrBeginSession and the exclusive
                            // frame-wait worker are both live. SteamVR can accept
                            // earlier swapchains yet present them black.
                            g_authoredReticlePreparationReady.store(
                                true, std::memory_order_release);
                        }
                        else
                        {
                            EnterFrameWaitFatalDrain(
                                "The exclusive OpenXR frame-wait worker could "
                                "not start");
                        }
                    }
                    else
                        LOG("xrBeginSession failed: %s", XrStr(r));
                }
                else if (sc.state == XR_SESSION_STATE_STOPPING)
                {
                    ResetWeaponInertia(true);
                    const char* fatalExitReason =
                        g_frameWaitFatalExitReason;
                    StopControllerHaptics();
                    EndPreparedFrameWithoutLayers("session stopping");
                    g_authoredReticlePreparationReady.store(
                        false, std::memory_order_release);
                    g_sessionRunning = false;
                    if (!StopFrameWaitThread())
                    {
                        Game_DetachForVrRuntimeFailure();
                        Fail("The OpenXR frame-wait worker did not stop; refusing "
                             "to end the session while a frame call may be active");
                        break;
                    }
                    const XrResult endResult = xrEndSession(g_session);
                    if (XR_FAILED(endResult))
                        LOG("xrEndSession failed: %s", XrStr(endResult));
                    if (fatalExitReason)
                    {
                        g_frameWaitFatalExitReason = nullptr;
                        g_frameWaitExitRequestAccepted = false;
                        g_frameWaitExitLastRequestMs = 0;
                        g_frameWaitExitLastLogMs = 0;
                        Game_DetachForVrRuntimeFailure();
                        Fail(fatalExitReason);
                    }
                }
                else if (sc.state == XR_SESSION_STATE_EXITING || sc.state == XR_SESSION_STATE_LOSS_PENDING)
                {
                    ResetWeaponInertia(true);
                    StopControllerHaptics();
                    ResetPreparedFrame();
                    g_authoredReticlePreparationReady.store(
                        false, std::memory_order_release);
                    g_sessionRunning = false;
                    StopFrameWaitThread();
                    Fail("The VR runtime ended the session (headset off / SteamVR closed?)");
                }
                break;
            }
            case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
                StopControllerHaptics();
                ResetWeaponInertia(true);
                ResetPreparedFrame();
                g_authoredReticlePreparationReady.store(
                    false, std::memory_order_release);
                g_sessionRunning = false;
                StopFrameWaitThread();
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

        // A structural frame-loop failure disables new frame calls but keeps
        // event polling alive until the runtime accepts the exit request and
        // reports STOPPING. Retry a rejected request without flooding the log.
        if (g_state == State::Ready && g_frameWaitFatalExitReason &&
            !g_frameWaitExitRequestAccepted &&
            g_sessionState != XR_SESSION_STATE_STOPPING &&
            g_sessionState != XR_SESSION_STATE_EXITING &&
            g_sessionState != XR_SESSION_STATE_LOSS_PENDING)
        {
            const uint64_t nowMs = GetTickCount64();
            if (!g_frameWaitExitLastRequestMs ||
                nowMs - g_frameWaitExitLastRequestMs >= 250)
            {
                g_frameWaitExitLastRequestMs = nowMs;
                const XrResult exitResult = xrRequestExitSession(g_session);
                if (XR_SUCCEEDED(exitResult))
                {
                    g_frameWaitExitRequestAccepted = true;
                }
                else if (!g_frameWaitExitLastLogMs ||
                         nowMs - g_frameWaitExitLastLogMs >= 2000)
                {
                    g_frameWaitExitLastLogMs = nowMs;
                    LOG("pacing: fatal frame-loop drain is retrying "
                        "xrRequestExitSession: %s", XrStr(exitResult));
                }
            }
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
    bool CaptureHeadPose(XrTime time)
    {
        XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
        if (XR_FAILED(xrLocateSpace(g_viewSpace, g_localSpace, time, &loc)))
            return false;
        constexpr XrSpaceLocationFlags need =
            XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
        if ((loc.locationFlags & need) != need)
            return false;
        if (!NormalizeTrackedPose(loc.pose))
            return false;
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
        return true;
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
        if (g_reticleChainFailed.load(std::memory_order_acquire))
            return false;
        if (g_reticleChain == XR_NULL_HANDLE)
        {
            if (!CreateChain(kReticleSize, kReticleSize, g_reticleChain, g_reticleImages,
                             g_reticleRtvs, "crosshair"))
            {
                g_reticleChainFailed.store(true, std::memory_order_release);
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
        // This must comfortably exceed the authored upload's frame cap. At 2,
        // the grace expired between capped uploads, the procedural reticle was
        // repainted over the authored art, and the next upload put it back -
        // the two crosshairs visibly alternating at the beat frequency of the
        // two numbers. Holding the last authored image instead is also what
        // removes the per-frame swapchain repaint this comment warns about,
        // which is the same cost Halo 3 and ODST pay.
        constexpr uint64_t kAuthoredReticleGraceFrames = 24;
        const bool authoredCaptureRecent = g_authoredReticleSerial != 0 &&
            g_preparedFrame.serial >= g_authoredReticleSerial &&
            g_preparedFrame.serial - g_authoredReticleSerial <=
                kAuthoredReticleGraceFrames;
        if (g_reticleContainsAuthored && authoredCaptureRecent)
            return true;

        // Once the swapchain holds real authored art, LEAVE IT ALONE. For a
        // title that captures its crosshair, repainting can only ever destroy
        // that art - it paints the procedural reticle, which for these titles
        // is fully transparent, so the crosshair simply vanishes until the next
        // upload restores it. That is the flashing, and the repaint is also
        // pure per-frame cost for something that does not change.
        //
        // Reach legitimately stops drawing its crosshair during reloads, melee
        // and similar, which made the old grace window expire and wipe the art
        // mid-fight. Holding the last art is both correct and free; a genuine
        // change (weapon swap, zoom, colour) re-uploads through the key path.
        const bool titleCapturesAuthoredArt =
            Game_TitleCapturesAuthoredCrosshair();
        if (g_reticleContainsAuthored && titleCapturesAuthoredArt)
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
        // Reach DOES capture authored art now - its crosshair widgets are
        // found through the owning collection's scripting class rather than a
        // class-2 gate - so it is in the same case as Halo 3: the authored
        // widget is the crosshair and the procedural one must stay invisible.
        // While Reach was still listed here as having no capture, it painted
        // the procedural reticle fully opaque into the SAME swapchain the
        // authored art lives in, wiping that art out on every repaint. That is
        // what showed the old crosshair, made the two alternate, and cost a
        // swapchain repaint per frame.
        const bool titleHasAuthoredCapture =
            Game_TitleCapturesAuthoredCrosshair();
        const float kProceduralOpacity =
            titleHasAuthoredCapture ? 0.0f : 1.0f;
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
            g_reticleChainFailed.store(true, std::memory_order_release);
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

    bool UploadAuthoredReticle(bool requireSuccessfulRelease)
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
        const XrResult acquireResult =
            xrAcquireSwapchainImage(g_reticleChain, &acquire, &index);
        if (requireSuccessfulRelease && acquireResult != XR_SUCCESS)
        {
            if (XR_SUCCEEDED(acquireResult))
            {
                (void)RequireReachSwapchainCompletion(
                    acquireResult,
                    "Reach authored-reticle swapchain acquire did not complete");
            }
            return false;
        }
        if (XR_FAILED(acquireResult))
            return false;
        const XrResult waitResult =
            xrWaitSwapchainImage(g_reticleChain, &wait);
        if (requireSuccessfulRelease)
        {
            if (!RequireReachSwapchainCompletion(
                    waitResult,
                    "Reach authored-reticle swapchain wait did not complete"))
            {
                return false;
            }
        }
        else if (XR_FAILED(waitResult))
            return false;

        D3D11_TEXTURE2D_DESC sourceDesc{};
        g_authoredReticleTexture->GetDesc(&sourceDesc);
        const bool copied = Blit(g_authoredReticleTexture, sourceDesc,
                                 g_reticleImages[index], kReticleSize,
                                 kReticleSize,
                                 GetRtv(g_reticleImages, g_reticleRtvs, index));
        const XrResult releaseResult =
            xrReleaseSwapchainImage(g_reticleChain, &release);
        const bool released = requireSuccessfulRelease
            ? RequireReachSwapchainCompletion(
                  releaseResult,
                  "Reach authored-reticle swapchain release did not complete")
            : XR_SUCCEEDED(releaseResult);
        if (!copied || (requireSuccessfulRelease && !released))
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
            g_peakHaptics = 0.0f;
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
        // Read-only: lets us log the headset's REAL panel rate. xrWaitFrame's
        // predictedDisplayPeriod reports the rate the runtime is TARGETING the
        // app at, which is a fraction of the panel rate whenever SteamVR engages
        // reprojection (120 panel -> 60 or 40 targeted). Logging both side by
        // side is the only way to tell "the headset is slow" apart from "the
        // runtime halved us", which look identical from the period alone.
        g_refreshRateExtEnabled =
            hasExtension(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
        if (g_refreshRateExtEnabled)
            enabledExtensions.push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);

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
        // The launcher injects the mod at process start, before the game window
        // exists, so the runtime and headset are often still coming online the
        // first time we ask. XR_ERROR_FORM_FACTOR_UNAVAILABLE means "no HMD yet",
        // which is transient, so poll for up to ~60s instead of giving up on the
        // first race. This runs on the background init thread and never blocks
        // the game; a headset connected shortly after launch still brings up VR.
        for (int waited = 0;
             r == XR_ERROR_FORM_FACTOR_UNAVAILABLE && waited < 60;
             ++waited)
        {
            if (waited % 10 == 0)
                LOG("waiting for a headset on OpenXR runtime '%s' "
                    "(connect it now if it is off)...", g_status.runtime);
            Sleep(1000);
            r = xrGetSystem(g_instance, &sgi, &g_systemId);
        }
        if (XR_FAILED(r))
        {
            char msg[320];
            snprintf(msg, sizeof(msg),
                "No headset found on the active OpenXR runtime '%s' after 60s.\n\n"
                "The mod uses whichever runtime owns OpenXR. If your headset runs\n"
                "through SteamVR (PSVR2, Index, Vive, etc.), set SteamVR -> Settings\n"
                "-> OpenXR -> Set SteamVR as OpenXR Runtime. If you stream with\n"
                "Virtual Desktop, connect it before launching.",
                g_status.runtime);
            Fail(msg, r);
            return false;
        }
        LOG("OpenXR instance ready; headset found on runtime %s", g_status.runtime);
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
    std::atomic<float> g_twoHandBlend{0.0f};

    // Called once per frame from the pose capture. Toggle mode (default): a
    // left-grip press while the left hand is inside the thin/long barrel zone
    // flips two-hand ON; the next left-grip press flips it OFF (anywhere). Hold
    // mode: the zone acquires the hold, which stays engaged until grip release.
    // The OpenXR aim pose sits back at the wrist. Shift the left-hand sample to
    // the palm so activation and the two-hand line are measured at the rendered
    // support hand. The same configured correction is used by game.cpp's left
    // arm target, keeping the visible wrist and the aiming point together.
    XrVector3f LeftHandPointWithOffsets(
        const XrPosef& lpose, float handForwardM, float gripForwardM)
    {
        const XrVector3f lfwd = Rotate(lpose.orientation, {0,0,-1});
        // Hand-target correction PLUS the rendered wrist-to-palm depth: the
        // two-hand line and grab zone meet the visible PALM, not the wrist
        // bone the hand target anchors (23:26 headset result).
        const float k = std::clamp(handForwardM, -0.15f, 0.30f)
                      + std::clamp(gripForwardM, -0.05f, 0.25f);
        return {lpose.position.x + lfwd.x*k,
                lpose.position.y + lfwd.y*k,
                lpose.position.z + lfwd.z*k};
    }

    XrVector3f LeftHandPoint(const XrPosef& lpose)
    {
        return LeftHandPointWithOffsets(
            lpose, g_config.left_hand_forward_m,
            g_config.left_grip_forward_m);
    }

    struct AimPoseInputs
    {
        bool rightValid = false;
        XrPosef right{{0, 0, 0, 1}, {0, 0, 0}};
        bool leftValid = false;
        XrPosef left{{0, 0, 0, 1}, {0, 0, 0}};
        bool twoHandEnabled = false;
        bool twoHandLatched = false;
        float leftHandForwardM = 0.0f;
        float twoHandBlend = 0.0f;
        float leftGripForwardM = 0.0f;
        float gunYawDeg = 0.0f;
        float gunPitchDeg = 0.0f;
        float gunRollDeg = 0.0f;
    };

    struct AimPoseResult
    {
        bool valid = false;
        // False when the right pose itself is invalid; callers then preserve
        // the existing two-hand activity indicator exactly as before.
        bool updateTwoHandActivity = false;
        bool twoHandActive = false;
        bool rejectedExtreme = false;
        float rejectedAgreement = 0.0f;
        XrPosef pose{{0, 0, 0, 1}, {0, 0, 0}};
    };

    AimPoseInputs CurrentAimPoseInputs(
        bool rightValid, const XrPosef& right,
        bool leftValid, const XrPosef& left) noexcept
    {
        AimPoseInputs inputs{};
        inputs.rightValid = rightValid;
        inputs.right = right;
        inputs.leftValid = leftValid;
        inputs.left = left;
        inputs.twoHandEnabled = g_config.two_handed_aim;
        inputs.twoHandLatched = g_twoHandLatched.load();
        inputs.twoHandBlend = g_twoHandBlend.load();
        inputs.leftHandForwardM = g_config.left_hand_forward_m;
        inputs.leftGripForwardM = g_config.left_grip_forward_m;
        inputs.gunYawDeg = g_config.gun_yaw_deg;
        inputs.gunPitchDeg = g_config.gun_pitch_deg;
        inputs.gunRollDeg = g_config.gun_roll_deg;
        return inputs;
    }

    // Pure aim calculation shared by the lock-taking public getter and Reach's
    // exact-serial snapshot publisher. It reads no globals, takes no locks, and
    // performs no logging or state publication.
    AimPoseResult ComputeAimPose(const AimPoseInputs& inputs) noexcept
    {
        AimPoseResult result{};
        if (!inputs.rightValid)
            return result;

        result.updateTwoHandActivity = true;
        result.pose = inputs.right;

        auto finishAimPose = [&]() {
            auto multiply = [](const XrQuaternionf& a,
                               const XrQuaternionf& b) {
                return XrQuaternionf{
                    a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
                    a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,
                    a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w,
                    a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z};
            };
            constexpr float kDegToRad = 0.01745329252f;
            const float yaw = inputs.gunYawDeg * kDegToRad;
            const float pitch = inputs.gunPitchDeg * kDegToRad;
            const float roll = inputs.gunRollDeg * kDegToRad;
            const XrQuaternionf qYaw{
                0.0f, sinf(yaw*0.5f), 0.0f, cosf(yaw*0.5f)};
            const XrQuaternionf qPitch{
                sinf(pitch*0.5f), 0.0f, 0.0f, cosf(pitch*0.5f)};
            const XrQuaternionf qRoll{
                0.0f, 0.0f, sinf(-roll*0.5f), cosf(roll*0.5f)};
            const XrQuaternionf corrected = multiply(
                result.pose.orientation,
                multiply(multiply(qYaw, qPitch), qRoll));
            const float length = sqrtf(
                corrected.x*corrected.x + corrected.y*corrected.y +
                corrected.z*corrected.z + corrected.w*corrected.w);
            if (!std::isfinite(length) || length < 1e-5f)
                return;
            result.pose.orientation = {
                corrected.x/length, corrected.y/length,
                corrected.z/length, corrected.w/length};
            result.valid = true;
        };

        if (!inputs.twoHandEnabled || !inputs.leftValid ||
            inputs.twoHandBlend <= 0.0001f)
        {
            finishAimPose();
            return result;
        }

        // Match the activation point: measure the two-hand line to the HAND,
        // not the wrist (same forward shift used by the latch).
        const XrVector3f lp = LeftHandPointWithOffsets(
            inputs.left, inputs.leftHandForwardM,
            inputs.leftGripForwardM);
        const XrQuaternionf rq = inputs.right.orientation;
        const XrVector3f rp = inputs.right.position;
        const XrVector3f rup = Rotate(rq, {0,1,0});
        XrVector3f v{lp.x-rp.x, lp.y-rp.y, lp.z-rp.z};
        const float len = sqrtf(v.x*v.x+v.y*v.y+v.z*v.z);
        if (len < 1e-4f)
        {
            finishAimPose();
            return result;
        }

        XrVector3f af{v.x/len, v.y/len, v.z/len};
        const XrVector3f rawForward = Rotate(rq, {0,0,-1});
        const float agreement =
            af.x*rawForward.x + af.y*rawForward.y + af.z*rawForward.z;
        if (!std::isfinite(agreement) || agreement < 0.35f)
        {
            result.rejectedExtreme = true;
            result.rejectedAgreement = agreement;
            finishAimPose();
            return result;
        }

        auto cross=[](const XrVector3f& a, const XrVector3f& b) {
            return XrVector3f{
                a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z,
                a.x*b.y-a.y*b.x};
        };
        XrVector3f xa = cross(af, rup);
        const float xl = sqrtf(xa.x*xa.x+xa.y*xa.y+xa.z*xa.z);
        if (xl < 1e-4f)
        {
            finishAimPose();
            return result;
        }
        xa = {xa.x/xl, xa.y/xl, xa.z/xl};
        const XrVector3f ya = cross(xa, af);
        const XrVector3f za{-af.x, -af.y, -af.z};

        const float m00=xa.x,m10=xa.y,m20=xa.z;
        const float m01=ya.x,m11=ya.y,m21=ya.z;
        const float m02=za.x,m12=za.y,m22=za.z;
        const float tr=m00+m11+m22;
        float qx,qy,qz,qw;
        if (tr>0)
        {
            const float s=sqrtf(tr+1.0f)*2;
            qw=0.25f*s; qx=(m21-m12)/s;
            qy=(m02-m20)/s; qz=(m10-m01)/s;
        }
        else if (m00>m11 && m00>m22)
        {
            const float s=sqrtf(1.0f+m00-m11-m22)*2;
            qw=(m21-m12)/s; qx=0.25f*s;
            qy=(m01+m10)/s; qz=(m02+m20)/s;
        }
        else if (m11>m22)
        {
            const float s=sqrtf(1.0f+m11-m00-m22)*2;
            qw=(m02-m20)/s; qx=(m01+m10)/s;
            qy=0.25f*s; qz=(m12+m21)/s;
        }
        else
        {
            const float s=sqrtf(1.0f+m22-m00-m11)*2;
            qw=(m10-m01)/s; qx=(m02+m20)/s;
            qy=(m12+m21)/s; qz=0.25f*s;
        }
        const float ql=sqrtf(qx*qx+qy*qy+qz*qz+qw*qw);
        if (ql < 1e-5f)
        {
            finishAimPose();
            return result;
        }
        XrQuaternionf twoHand{qx/ql, qy/ql, qz/ql, qw/ql};
        float dot = rq.x*twoHand.x + rq.y*twoHand.y +
                    rq.z*twoHand.z + rq.w*twoHand.w;
        if (dot < 0.0f)
        {
            twoHand = {-twoHand.x, -twoHand.y, -twoHand.z, -twoHand.w};
        }
        const float blend = std::clamp(inputs.twoHandBlend, 0.0f, 1.0f);
        XrQuaternionf blended{
            rq.x + (twoHand.x-rq.x)*blend,
            rq.y + (twoHand.y-rq.y)*blend,
            rq.z + (twoHand.z-rq.z)*blend,
            rq.w + (twoHand.w-rq.w)*blend};
        const float blendedLength = sqrtf(
            blended.x*blended.x + blended.y*blended.y +
            blended.z*blended.z + blended.w*blended.w);
        if (blendedLength >= 1e-5f && std::isfinite(blendedLength))
            result.pose.orientation = {
                blended.x/blendedLength, blended.y/blendedLength,
                blended.z/blendedLength, blended.w/blendedLength};
        result.twoHandActive = inputs.twoHandLatched;
        finishAimPose();
        return result;
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

    PoseInertiaPose ToInertiaPose(const XrPosef& pose) noexcept
    {
        PoseInertiaPose converted{};
        converted.orientation[0] = pose.orientation.x;
        converted.orientation[1] = pose.orientation.y;
        converted.orientation[2] = pose.orientation.z;
        converted.orientation[3] = pose.orientation.w;
        converted.position[0] = pose.position.x;
        converted.position[1] = pose.position.y;
        converted.position[2] = pose.position.z;
        return converted;
    }

    XrPosef FromInertiaPose(const PoseInertiaPose& pose) noexcept
    {
        return {
            {pose.orientation[0], pose.orientation[1],
             pose.orientation[2], pose.orientation[3]},
            {pose.position[0], pose.position[1], pose.position[2]}};
    }

    PoseInertiaSettings CurrentWeaponInertiaSettings() noexcept
    {
        return {
            g_config.weapon_position_follow,
            g_config.weapon_rotation_follow,
            g_config.weapon_catchup_speed};
    }

    void ResetWeaponInertia(bool invalidateOutput)
    {
        if (!g_headCsInit)
            return;
        EnterCriticalSection(&g_headCs);
        g_rightWeaponInertia.Reset();
        g_leftWeaponInertia.Reset();
        g_lastControllerPoseTime = 0;
        g_weaponInertiaTitle = GameTitle::Unknown;
        if (invalidateOutput)
        {
            g_rightAimPoseValid = false;
            g_leftAimPoseValid = false;
            g_rawRightAimPoseValid = false;
            g_rawLeftAimPoseValid = false;
        }
        LeaveCriticalSection(&g_headCs);
        g_twoHandLatched.store(false);
        g_twoHandBlend.store(0.0f);
        g_twoHandActive.store(false);
    }

    void UpdateTwoHandBlend(float deltaSeconds, bool posesValid) noexcept
    {
        const bool target = posesValid && g_config.two_handed_aim &&
            g_twoHandLatched.load();
        if (!posesValid || !std::isfinite(deltaSeconds) ||
            deltaSeconds <= 0.0f || deltaSeconds > 0.25f)
        {
            g_twoHandBlend.store(target ? 1.0f : 0.0f);
            return;
        }
        const float previous = g_twoHandBlend.load();
        const float follow = 1.0f - std::exp(-18.0f * deltaSeconds);
        g_twoHandBlend.store(previous + ((target ? 1.0f : 0.0f) - previous) * follow);
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
        const bool capabilityAllows =
            Game_HasTitleCapability(TitleCapability_Haptics);
        if (!capabilityAllows)
        {
            // Stop is not enough: the requested amplitude is persistent. Drop
            // it while ownership/arming is absent so it cannot be replayed
            // after a title transition without a fresh XInput request. The
            // accumulated peak is dropped for the same reason.
            g_requestedHaptics.store(0.0f, std::memory_order_release);
            g_peakHaptics.store(0.0f, std::memory_order_release);
        }
        const float intensity =
            std::clamp(g_config.haptic_intensity, 0.0f, 1.0f);
        // Peak-hold: peek (without consuming) the max amplitude requested since
        // the last applied frame so a short gunfire pulse (SetState(high) then
        // SetState(0) between two frame samples) cannot be aliased to zero.
        const float latest = capabilityAllows
            ? std::clamp(
                g_requestedHaptics.load(std::memory_order_acquire),
                0.0f, 1.0f)
            : 0.0f;
        const float peekPeak = capabilityAllows
            ? g_peakHaptics.load(std::memory_order_acquire)
            : 0.0f;
        float amplitude = SampleHapticPeak(peekPeak, latest).apply * intensity;
        const bool mustStop = amplitude <= 0.0f || !trackingValid || !modeAllows ||
            !capabilityAllows ||
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
            return; // keep accumulating the peak; apply on the next unthrottled frame
        // Applying now: consume the peak and carry the latest sustained value
        // forward, so a one-shot pulse fires exactly once and a held rumble
        // persists across the 40 ms re-apply throttle.
        const float appliedPeak =
            g_peakHaptics.exchange(latest, std::memory_order_acq_rel);
        amplitude = SampleHapticPeak(appliedPeak, latest).apply * intensity;
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

    bool CaptureRightControllerPose(XrTime time)
    {
        if (g_gameplayActions == XR_NULL_HANDLE ||
            g_rightAimAction == XR_NULL_HANDLE ||
            g_rightAimSpace == XR_NULL_HANDLE)
        {
            if (g_config.weapon_inertia)
                ResetWeaponInertia(true);
            return false;
        }
        XrActiveActionSet active{g_gameplayActions, XR_NULL_PATH};
        XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
        sync.countActiveActionSets = 1;
        sync.activeActionSets = &active;
        if (XR_FAILED(xrSyncActions(g_session, &sync)))
        {
            if (g_config.weapon_inertia)
                ResetWeaponInertia(true);
            return false;
        }
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
        // Left hand uses the same locate path for support-hand placement and
        // the independently weighted two-hand aim line.
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

        const GameTitle activeTitle = TitleAdapter_GetActiveTitle();
        float poseDeltaSeconds = 0.0f;
        bool titleChanged = false;
        EnterCriticalSection(&g_headCs);
        titleChanged = activeTitle != g_weaponInertiaTitle;
        if (titleChanged)
        {
            g_rightWeaponInertia.Reset();
            g_leftWeaponInertia.Reset();
            g_lastControllerPoseTime = 0;
            g_weaponInertiaTitle = activeTitle;
        }
        if (g_lastControllerPoseTime && time > g_lastControllerPoseTime)
            poseDeltaSeconds = static_cast<float>(
                static_cast<double>(time - g_lastControllerPoseTime) /
                1000000000.0);
        g_lastControllerPoseTime = time;

        g_rawRightAimPoseValid = valid;
        if (valid)
            g_rawRightAimPose = location.pose;
        g_rawLeftAimPoseValid = leftValid;
        if (leftValid)
            g_rawLeftAimPose = leftLocation.pose;

        if (!g_config.weapon_inertia)
        {
            // Preserve the accepted disabled path exactly: already-normalized
            // OpenXR poses are copied directly, with no spring math.
            g_rightWeaponInertia.Reset();
            g_leftWeaponInertia.Reset();
            g_rightAimPoseValid = valid;
            if (valid)
                g_rightAimPose = location.pose;
            g_leftAimPoseValid = leftValid;
            if (leftValid)
                g_leftAimPose = leftLocation.pose;
        }
        else
        {
            const PoseInertiaSettings inertiaSettings =
                CurrentWeaponInertiaSettings();
            PoseInertiaPose filtered{};
            g_rightAimPoseValid = g_rightWeaponInertia.Update(
                true, valid, ToInertiaPose(location.pose), poseDeltaSeconds,
                inertiaSettings, filtered);
            if (g_rightAimPoseValid)
                g_rightAimPose = FromInertiaPose(filtered);
            g_leftAimPoseValid = g_leftWeaponInertia.Update(
                true, leftValid, ToInertiaPose(leftLocation.pose),
                poseDeltaSeconds, inertiaSettings, filtered);
            if (g_leftAimPoseValid)
                g_leftAimPose = FromInertiaPose(filtered);
        }
        LeaveCriticalSection(&g_headCs);
        if (titleChanged)
        {
            g_twoHandLatched.store(false);
            g_twoHandBlend.store(0.0f);
            g_twoHandActive.store(false);
        }
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
        UpdateTwoHandBlend(poseDeltaSeconds, valid && leftValid);
        ApplyControllerHaptics(valid && leftValid);
        static bool padLogged = false;
        if (pad.valid && !padLogged)
        {
            LOG("M3: controller inputs active (sticks/buttons feeding the virtual gamepad)");
            padLogged = true;
        }
        return true;
    }

    void UpdateMenuPointer(bool headLocked)
    {
        static bool triggerPressed = false;
        static bool hadHit = false;
        static float smoothU = 0.5f, smoothV = 0.5f;
        static uint64_t lastDiagMs = 0;
        if (!g_rawRightAimPoseValid || (headLocked && !g_headPoseValid) ||
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
            g_rawRightAimPose.position.x - anchor.position.x,
            g_rawRightAimPose.position.y - anchor.position.y,
            g_rawRightAimPose.position.z - anchor.position.z};
        const XrVector3f localOrigin = Rotate(inverseAnchor, relative);
        const XrVector3f worldDirection =
            Rotate(g_rawRightAimPose.orientation, {0.0f, 0.0f, -1.0f});
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
        LogHeadsetPanelRate();
        LOG("OpenXR session created");
        return true;
    }

    // Read the headset's REAL panel rate once, so the log can separate two very
    // different things that look identical from the frame period alone:
    //   - the headset is genuinely running slow, versus
    //   - the runtime is TARGETING us at a fraction of the panel (reprojection),
    //     which pins us to panel/2 or panel/3 no matter what the GPU could do.
    // Works for any rate (72, 80, 90, 120, 144); nothing here assumes a number.
    void LogHeadsetPanelRate()
    {
        if (!g_refreshRateExtEnabled)
        {
            LOG("headset: this runtime does not expose %s, so the panel rate is "
                "unknown; only the rate it targets us at can be read",
                XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
            return;
        }
        PFN_xrGetDisplayRefreshRateFB getRate = nullptr;
        if (XR_FAILED(xrGetInstanceProcAddr(
                g_instance, "xrGetDisplayRefreshRateFB",
                (PFN_xrVoidFunction*)&getRate)) || !getRate)
        {
            LOG("headset: xrGetDisplayRefreshRateFB missing though the extension "
                "is enabled; panel rate unknown");
            return;
        }
        float hz = 0.0f;
        const XrResult r = getRate(g_session, &hz);
        if (XR_FAILED(r) || hz <= 0.0f)
        {
            LOG("headset: panel rate query failed (%s); panel rate unknown",
                XrStr(r));
            return;
        }
        g_panelRefreshHz.store(hz, std::memory_order_relaxed);
        LOG("headset: panel is running at %.1fHz", hz);
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

    bool LocateViewsForUpcomingRender(XrTime displayTime);

#if HALOMCCVR_EXPERIMENTAL_REACH_RENDER_CANDIDATE
    bool PublishReachRenderSnapshot(
        uint64_t preparedSerial, bool padFresh) noexcept
    {
        if (!preparedSerial || !g_headPoseValid || g_views.size() != 2)
            return false;

        ReachVrRenderSnapshot next{};
        next.preparedSerial = preparedSerial;
        next.headOrientation[0] = g_headPose.orientation.x;
        next.headOrientation[1] = g_headPose.orientation.y;
        next.headOrientation[2] = g_headPose.orientation.z;
        next.headOrientation[3] = g_headPose.orientation.w;
        next.headPosition[0] = g_headPose.position.x;
        next.headPosition[1] = g_headPose.position.y;
        next.headPosition[2] = g_headPose.position.z;
        // This publisher runs on the same OpenXR frame thread that just wrote
        // the controller poses in CaptureRightControllerPose. Compute from
        // those owned values directly: Reach's render reader remains lock-free,
        // and the aim/head/eyes all belong to this exact prepared serial.
        // A failed action sync leaves the last successful globals intact for
        // the legacy getters.  Never admit those stale values into a newly
        // prepared Reach serial: this snapshot is an exact-frame contract.
        const bool rightPoseFresh = padFresh && g_rightAimPoseValid;
        const bool leftPoseFresh = padFresh && g_leftAimPoseValid;
        const AimPoseResult aim = ComputeAimPose(CurrentAimPoseInputs(
            rightPoseFresh, g_rightAimPose,
            leftPoseFresh, g_leftAimPose));
        next.rightAimValid = aim.valid;
        if (aim.valid)
        {
            next.rightAimOrientation[0] = aim.pose.orientation.x;
            next.rightAimOrientation[1] = aim.pose.orientation.y;
            next.rightAimOrientation[2] = aim.pose.orientation.z;
            next.rightAimOrientation[3] = aim.pose.orientation.w;
            next.rightAimPosition[0] = aim.pose.position.x;
            next.rightAimPosition[1] = aim.pose.position.y;
            next.rightAimPosition[2] = aim.pose.position.z;
        }
        next.leftControllerValid = leftPoseFresh;
        if (leftPoseFresh)
        {
            next.leftControllerOrientation[0] =
                g_leftAimPose.orientation.x;
            next.leftControllerOrientation[1] =
                g_leftAimPose.orientation.y;
            next.leftControllerOrientation[2] =
                g_leftAimPose.orientation.z;
            next.leftControllerOrientation[3] =
                g_leftAimPose.orientation.w;
            next.leftControllerPosition[0] = g_leftAimPose.position.x;
            next.leftControllerPosition[1] = g_leftAimPose.position.y;
            next.leftControllerPosition[2] = g_leftAimPose.position.z;
        }
        if (padFresh)
            next.pad = g_padState;

        for (int eye = 0; eye < 2; ++eye)
        {
            if (!VR_GetEyeViewOffset(
                    eye, next.eyes[eye].position,
                    next.eyes[eye].orientation) ||
                !VR_GetEyeFov(eye, next.eyes[eye].fov))
            {
                return false;
            }
        }

        const uint32_t current =
            g_reachRenderSnapshotIndex.load(std::memory_order_seq_cst);
        const uint32_t target = current < 2 ? 1u - current : 0u;
        uint32_t expectedState = 0;
        if (!g_reachRenderSnapshotStates[target].compare_exchange_strong(
                expectedState, kReachRenderSnapshotWriter,
                std::memory_order_seq_cst,
                std::memory_order_seq_cst))
        {
            // A reader still owns the old non-current slot. Skipping this
            // publication is safer than waiting in the OpenXR frame path; the
            // exact-serial Reach reader will fall open for this frame.
            return false;
        }

        g_reachRenderSnapshots[target] = next;
        g_reachRenderSnapshotIndex.store(target, std::memory_order_seq_cst);
        g_reachRenderSnapshotStates[target].store(
            0, std::memory_order_seq_cst);
        return true;
    }
#endif

    // The wait thread: block in xrWaitFrame here instead of on the game's render
    // thread, then hand the state over. Once the render thread claims Wait(N),
    // it releases this worker immediately before Begin(N). The next Wait(N+1)
    // is dispatched at that boundary and then overlaps the game's render work.
    DWORD WINAPI FrameWaitThread(LPVOID)
    {
        while (!g_waitThreadStop.load(std::memory_order_acquire))
        {
            XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
            XrFrameState state{XR_TYPE_FRAME_STATE};
            const uint64_t waitSequence =
                g_waitCallSequence.fetch_add(1, std::memory_order_relaxed) + 1;
            const uint32_t waitSessionEpoch =
                g_framePacingSessionEpoch.load(std::memory_order_acquire);
            LARGE_INTEGER waitStart{}, waitEnd{};
            QueryPerformanceCounter(&waitStart);
            g_waitCallInFlight.store(waitSequence, std::memory_order_release);
            if (!SetEvent(g_waitStartedEvent))
                g_waitEventSignalFailures.fetch_add(1, std::memory_order_relaxed);
            const XrResult r = xrWaitFrame(g_session, &waitInfo, &state);
            QueryPerformanceCounter(&waitEnd);
            const uint32_t returnSessionEpoch =
                g_framePacingSessionEpoch.load(std::memory_order_acquire);
            g_waitCallInFlight.store(0, std::memory_order_release);
            if (g_waitThreadStop.load(std::memory_order_acquire))
                break;
            if (XR_FAILED(r))
            {
                // Don't spin on a dead session. Report the failed worker wait;
                // the render thread never substitutes its own xrWaitFrame.
                g_waitedStateValid.store(false, std::memory_order_release);
                g_waitedStateResult.store(r, std::memory_order_relaxed);
                g_waitedStateFailed.store(true, std::memory_order_release);
                SetEvent(g_waitReadyEvent);
                Sleep(4);
                continue;
            }
            LARGE_INTEGER ready{};
            QueryPerformanceCounter(&ready);
            // Version the existing packet handoff so a consumed-event timeout
            // and overwrite cannot be mislabeled as an exact diagnostic row.
            g_waitedPacketVersion.fetch_add(1, std::memory_order_acq_rel);
            g_waitedFrameState = state;
            g_waitedCallStartQpc.store(
                waitStart.QuadPart, std::memory_order_relaxed);
            g_waitedCallEndQpc.store(
                waitEnd.QuadPart, std::memory_order_relaxed);
            g_waitedReadyQpc.store(
                ready.QuadPart, std::memory_order_relaxed);
            g_waitedSessionEpochStart.store(
                waitSessionEpoch, std::memory_order_relaxed);
            g_waitedSessionEpochEnd.store(
                returnSessionEpoch, std::memory_order_relaxed);
            g_waitedPacketSequence.store(
                waitSequence, std::memory_order_release);
            g_waitedPacketVersion.fetch_add(1, std::memory_order_release);
            g_waitedStateFailed.store(false, std::memory_order_release);
            g_waitedStateValid.store(true, std::memory_order_release);
            SetEvent(g_waitReadyEvent);
            // The event is only a wakeup; the exact sequence acknowledgement is
            // the permit. A timeout or stale event must never authorize another
            // xrWaitFrame, because concurrent waits are undefined by OpenXR.
            for (;;)
            {
                if (g_waitThreadStop.load(std::memory_order_acquire))
                    return 0;
                const FrameWaitPermit permit = ClassifyFrameWaitPermit(
                    waitSequence,
                    g_waitConsumedSequence.load(std::memory_order_acquire));
                if (permit == FrameWaitPermit::StartNextWait)
                    break;
                if (permit == FrameWaitPermit::Fault)
                {
                    g_waitPipelineFaulted.store(true, std::memory_order_release);
                    SetEvent(g_waitReadyEvent);
                    return 0;
                }
                const DWORD wake = WaitForSingleObject(g_waitConsumedEvent, 50);
                if (wake == WAIT_FAILED)
                {
                    g_waitPipelineFaulted.store(true, std::memory_order_release);
                    SetEvent(g_waitReadyEvent);
                    return 0;
                }
            }
        }
        return 0;
    }

    bool StartFrameWaitThread()
    {
        if (g_waitThread)
            return true;
        g_waitThreadStop.store(false, std::memory_order_release);
        g_waitedStateValid.store(false, std::memory_order_release);
        g_waitedStateFailed.store(false, std::memory_order_release);
        g_waitedStateResult.store(XR_SUCCESS, std::memory_order_release);
        g_waitPacketMisses.store(0, std::memory_order_release);
        g_waitFailuresObserved.store(0, std::memory_order_release);
        g_waitEventSignalFailures.store(0, std::memory_order_release);
        g_waitCallSequence.store(0, std::memory_order_release);
        g_waitCallInFlight.store(0, std::memory_order_release);
        g_waitConsumedSequence.store(0, std::memory_order_release);
        g_waitPipelineFaulted.store(false, std::memory_order_release);
        g_abortFrameForReachSwapchainFailure = false;
        g_waitedPacketSequence.store(0, std::memory_order_release);
        g_waitedPacketVersion.store(0, std::memory_order_release);
        g_waitReadyEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        g_waitConsumedEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        g_waitStartedEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!g_waitReadyEvent || !g_waitConsumedEvent || !g_waitStartedEvent)
        {
            LOG("pacing: could not create frame-wait events (%lu); XR frame "
                "preparation will remain disabled (no inline wait)",
                static_cast<unsigned long>(GetLastError()));
            if (g_waitReadyEvent)
                CloseHandle(g_waitReadyEvent);
            if (g_waitConsumedEvent)
                CloseHandle(g_waitConsumedEvent);
            if (g_waitStartedEvent)
                CloseHandle(g_waitStartedEvent);
            g_waitReadyEvent = nullptr;
            g_waitConsumedEvent = nullptr;
            g_waitStartedEvent = nullptr;
            return false;
        }
        g_waitThread = CreateThread(nullptr, 0, &FrameWaitThread, nullptr, 0, nullptr);
        if (!g_waitThread)
        {
            LOG("pacing: could not start the frame-wait thread (%lu); XR frame "
                "preparation will remain disabled (no inline wait)",
                static_cast<unsigned long>(GetLastError()));
            CloseHandle(g_waitReadyEvent);
            CloseHandle(g_waitConsumedEvent);
            CloseHandle(g_waitStartedEvent);
            g_waitReadyEvent = nullptr;
            g_waitConsumedEvent = nullptr;
            g_waitStartedEvent = nullptr;
            return false;
        }
        LOG("pacing: frame-wait thread started; the game's render thread no "
            "longer blocks in xrWaitFrame (whatever rate the runtime reports)");
        return true;
    }

    bool StopFrameWaitThread()
    {
        if (!g_waitThread)
            return true;

        g_waitThreadStop.store(true, std::memory_order_release);
        if (g_waitConsumedEvent)
            SetEvent(g_waitConsumedEvent);
        if (g_waitReadyEvent)
            SetEvent(g_waitReadyEvent);
        if (g_waitStartedEvent)
            SetEvent(g_waitStartedEvent);
        const DWORD joined = WaitForSingleObject(g_waitThread, 2000);
        if (joined != WAIT_OBJECT_0)
        {
            g_waitPipelineFaulted.store(true, std::memory_order_release);
            LOG("pacing: frame-wait worker did not stop cleanly (wait=%lu)",
                static_cast<unsigned long>(joined));
            return false;
        }

        CloseHandle(g_waitThread);
        g_waitThread = nullptr;
        if (g_waitReadyEvent)
            CloseHandle(g_waitReadyEvent);
        if (g_waitConsumedEvent)
            CloseHandle(g_waitConsumedEvent);
        if (g_waitStartedEvent)
            CloseHandle(g_waitStartedEvent);
        g_waitReadyEvent = nullptr;
        g_waitConsumedEvent = nullptr;
        g_waitStartedEvent = nullptr;
        g_waitedStateValid.store(false, std::memory_order_release);
        g_waitedStateFailed.store(false, std::memory_order_release);
        g_waitCallInFlight.store(0, std::memory_order_release);
        return true;
    }

    void PrepareNextFrame()
    {
        if (g_preparedFrame.begun)
            return;

        g_framePacingPending = {};
        ++g_frameNo;
        FLog("xrWaitFrame after DXGI Present");
        XrFrameState frameState{XR_TYPE_FRAME_STATE};
        bool haveState = false;
        bool workerPacketClaimed = false;
        bool waitPacketCoherent = true;
        uint64_t waitSequence = 0;
        uint64_t overlappingWorkerWaitSequence = 0;
        uint32_t waitSessionEpoch = 0;
        int64_t waitCallStartQpc = 0;
        int64_t waitCallEndQpc = 0;
        int64_t waitReadyQpc = 0;
        LARGE_INTEGER consumedSignal{};
        LARGE_INTEGER nextWaitDispatchGateStart{}, nextWaitDispatchGateEnd{};
        DWORD eventWaitResult = WAIT_OBJECT_0;
        DWORD nextWaitEventResult = WAIT_OBJECT_0;
        uint64_t nextWaitDispatchSequence = 0;
        bool nextWaitDispatchObservedBeforeBegin = false;
        bool nextWaitDispatchGateFailed = false;
        LARGE_INTEGER waitStart{}, waitEnd{};
        QueryPerformanceCounter(&waitStart);
        if (g_waitThread &&
            !g_waitPipelineFaulted.load(std::memory_order_acquire))
        {
            // Events are wakeup hints only. Always inspect packet status before
            // and after waiting so a coalesced or just-missed signal cannot
            // strand a valid packet while the exact-sequence worker is parked.
            if (!g_waitedStateValid.load(std::memory_order_acquire) &&
                !g_waitedStateFailed.load(std::memory_order_acquire))
            {
                // Normally already signalled. The timeout is only a bounded
                // observation point; it never transfers Wait ownership.
                eventWaitResult = WaitForSingleObject(g_waitReadyEvent, 1000);
            }
            // A successful packet supersedes any stale failure notification.
            if (g_waitedStateValid.exchange(false, std::memory_order_acq_rel))
            {
                g_waitedStateFailed.store(false, std::memory_order_release);
                const uint64_t versionBefore =
                    g_waitedPacketVersion.load(std::memory_order_acquire);
                const uint64_t sequenceBefore =
                    g_waitedPacketSequence.load(std::memory_order_acquire);
                frameState = g_waitedFrameState;
                waitCallStartQpc = g_waitedCallStartQpc.load(
                    std::memory_order_relaxed);
                waitCallEndQpc = g_waitedCallEndQpc.load(
                    std::memory_order_relaxed);
                waitReadyQpc = g_waitedReadyQpc.load(
                    std::memory_order_relaxed);
                const uint32_t epochAtStart =
                    g_waitedSessionEpochStart.load(std::memory_order_relaxed);
                const uint32_t epochAtEnd =
                    g_waitedSessionEpochEnd.load(std::memory_order_relaxed);
                const uint64_t sequenceAfter =
                    g_waitedPacketSequence.load(std::memory_order_acquire);
                const uint64_t versionAfter =
                    g_waitedPacketVersion.load(std::memory_order_acquire);
                overlappingWorkerWaitSequence =
                    g_waitCallInFlight.load(std::memory_order_acquire);
                const uint32_t currentEpoch =
                    g_framePacingSessionEpoch.load(std::memory_order_acquire);
                waitSequence = sequenceAfter;
                waitSessionEpoch = epochAtStart;
                waitPacketCoherent = sequenceBefore != 0 &&
                    sequenceBefore == sequenceAfter &&
                    versionBefore == versionAfter &&
                    (versionBefore & 1u) == 0 && epochAtStart != 0 &&
                    epochAtStart == epochAtEnd &&
                    epochAtStart == currentEpoch &&
                    overlappingWorkerWaitSequence == 0;
                haveState = true;
                workerPacketClaimed = waitPacketCoherent;
            }
            else if (g_waitedStateFailed.exchange(
                        false, std::memory_order_acq_rel))
            {
                QueryPerformanceCounter(&waitEnd);
                g_waitDurationsMs.Add(QpcMs(waitEnd.QuadPart - waitStart.QuadPart));
                ++g_frameOrderFailures;
                g_waitFailuresObserved.fetch_add(
                    1, std::memory_order_relaxed);
                return;
            }
        }
        if (haveState && !waitPacketCoherent)
        {
            // Do not acknowledge an incoherent or cross-session packet. The
            // worker remains parked, so drain and reset the running session.
            ++g_frameOrderFailures;
            EnterFrameWaitFatalDrain(
                "The OpenXR wait worker published an incoherent or stale "
                "frame packet");
            return;
        }
        if (!haveState)
        {
            if (g_waitPipelineFaulted.load(std::memory_order_acquire) ||
                (g_waitThread &&
                 WaitForSingleObject(g_waitThread, 0) == WAIT_OBJECT_0))
            {
                ++g_frameOrderFailures;
                EnterFrameWaitFatalDrain(
                    "The exclusive OpenXR wait-worker pipeline stopped");
                return;
            }
            // The worker is the sole xrWaitFrame owner. Never substitute an
            // inline wait: it can race an in-flight worker wait, and it restores
            // the exact render-thread pacing stall this pipeline exists to fix.
            QueryPerformanceCounter(&waitEnd);
            g_waitDurationsMs.Add(QpcMs(waitEnd.QuadPart - waitStart.QuadPart));
            g_waitPacketMisses.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        QueryPerformanceCounter(&waitEnd);
        g_waitDurationsMs.Add(QpcMs(waitEnd.QuadPart - waitStart.QuadPart));

        // A subsequent xrWaitFrame is explicitly legal before this Begin and
        // must block until this frame has begun. Dispatch it now so the
        // runtime applies cadence delay on the worker, not inside xrBeginFrame
        // on Halo's render thread. Publish the exact claimed sequence before
        // waking; stale auto-reset event credits cannot advance the worker. Do
        // not rely on scheduler luck alone: observe the exact Wait(N+1)
        // dispatch boundary before entering Begin(N), then use cross-frame
        // timing to verify the runtime-call relationship in the headset.
        if (ShouldReleaseFrameWaitWorkerBeforeBegin(
                g_waitThread != nullptr, workerPacketClaimed))
        {
            QueryPerformanceCounter(&consumedSignal);
            g_waitConsumedSequence.store(waitSequence, std::memory_order_release);
            if (!SetEvent(g_waitConsumedEvent))
            {
                ++g_frameOrderFailures;
                g_waitEventSignalFailures.fetch_add(
                    1, std::memory_order_relaxed);
            }

            QueryPerformanceCounter(&nextWaitDispatchGateStart);
            const uint64_t dispatchDeadlineMs = GetTickCount64() + 1000;
            for (;;)
            {
                nextWaitDispatchSequence =
                    g_waitCallInFlight.load(std::memory_order_acquire);
                if (IsExpectedNextFrameWaitDispatch(
                        waitSequence, nextWaitDispatchSequence))
                {
                    nextWaitDispatchObservedBeforeBegin = true;
                    break;
                }
                if (nextWaitDispatchSequence != 0 ||
                    g_waitPipelineFaulted.load(std::memory_order_acquire) ||
                    g_waitThreadStop.load(std::memory_order_acquire) ||
                    g_waitedStateFailed.load(std::memory_order_acquire))
                {
                    break;
                }
                const uint64_t dispatchNowMs = GetTickCount64();
                if (dispatchNowMs >= dispatchDeadlineMs)
                {
                    nextWaitEventResult = WAIT_TIMEOUT;
                    break;
                }
                nextWaitEventResult =
                    WaitForSingleObject(
                        g_waitStartedEvent,
                        static_cast<DWORD>(dispatchDeadlineMs - dispatchNowMs));
                if (nextWaitEventResult == WAIT_FAILED)
                    break;
            }
            nextWaitDispatchSequence =
                g_waitCallInFlight.load(std::memory_order_acquire);
            nextWaitDispatchObservedBeforeBegin =
                IsExpectedNextFrameWaitDispatch(
                    waitSequence, nextWaitDispatchSequence);
            QueryPerformanceCounter(&nextWaitDispatchGateEnd);
            if (!nextWaitDispatchObservedBeforeBegin)
            {
                ++g_frameOrderFailures;
                nextWaitDispatchGateFailed = true;
            }
        }

        FLog("xrBeginFrame before Halo render");
        XrFrameBeginInfo beginInfo{XR_TYPE_FRAME_BEGIN_INFO};
        LARGE_INTEGER beginStart{}, beginEnd{};
        QueryPerformanceCounter(&beginStart);
        const XrResult beginResult = xrBeginFrame(g_session, &beginInfo);
        QueryPerformanceCounter(&beginEnd);
        if (XR_FAILED(beginResult))
        {
            ++g_frameOrderFailures;
            LOG("timing: xrBeginFrame failed: %s", XrStr(beginResult));
            EnterFrameWaitFatalDrain(
                "xrBeginFrame failed after the exclusive wait worker was "
                "released");
            return;
        }

        g_beginFrameQpc = beginEnd;

        g_preparedFrame.state = frameState;
        g_preparedFrame.begun = true;
        g_preparedFrame.serial = ++g_nextPreparedSerial;
        g_preparedShouldRender.store(
            frameState.shouldRender == XR_TRUE, std::memory_order_release);

        FramePacingRecord& pacing = g_framePacingPending;
        pacing.serial = g_preparedFrame.serial;
        pacing.waitSequence = waitSequence;
        pacing.overlappingWorkerWaitSequence =
            overlappingWorkerWaitSequence;
        pacing.nextWaitDispatchSequence = nextWaitDispatchSequence;
        pacing.sessionEpoch = waitSessionEpoch;
        pacing.predictedDisplayTimeNs = frameState.predictedDisplayTime;
        pacing.predictedDisplayPeriodNs = frameState.predictedDisplayPeriod;
        pacing.waitCallStartQpc = waitCallStartQpc;
        pacing.waitCallEndQpc = waitCallEndQpc;
        pacing.waitReadyQpc = waitReadyQpc;
        pacing.renderWaitStartQpc = waitStart.QuadPart;
        pacing.renderWaitEndQpc = waitEnd.QuadPart;
        pacing.nextWaitDispatchGateStartQpc =
            nextWaitDispatchGateStart.QuadPart;
        pacing.nextWaitDispatchGateEndQpc =
            nextWaitDispatchGateEnd.QuadPart;
        pacing.beginStartQpc = beginStart.QuadPart;
        pacing.beginEndQpc = beginEnd.QuadPart;
        pacing.consumedSignalQpc = consumedSignal.QuadPart;
        pacing.waitResult = XR_SUCCESS;
        pacing.beginResult = beginResult;
        pacing.eventWaitResult = eventWaitResult;
        pacing.nextWaitEventResult = nextWaitEventResult;
        pacing.title = static_cast<uint8_t>(TitleAdapter_GetActiveTitle());
        pacing.waitPacketCoherent = waitPacketCoherent;
        pacing.nextWaitDispatchObservedBeforeBegin =
            nextWaitDispatchObservedBeforeBegin;
        pacing.shouldRender = frameState.shouldRender == XR_TRUE;
        pacing.focused = g_sessionState == XR_SESSION_STATE_FOCUSED;
        pacing.stereo = g_stereoEnabled.load(std::memory_order_relaxed);
        pacing.headTracking = Game_IsHeadTracking();
        pacing.scopeActive = g_scopeActive.load(std::memory_order_relaxed);
        Game_ReadFramePerfCounters(g_framePacingPerfStart);

        if (nextWaitDispatchGateFailed)
        {
            EndPreparedFrameWithoutLayers("next wait dispatch gate failed");
            EnterFrameWaitFatalDrain(
                "The exclusive OpenXR wait worker did not dispatch the exact "
                "next wait before xrBeginFrame");
            return;
        }

        if (g_lastPredictedDisplayTime)
        {
            const XrDuration delta =
                frameState.predictedDisplayTime - g_lastPredictedDisplayTime;
            const XrDuration period = frameState.predictedDisplayPeriod;
            if (period > 0)
                g_displayPeriodNs.store(static_cast<uint64_t>(period),
                                        std::memory_order_relaxed);
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
        const bool upcomingPadFresh =
            CaptureRightControllerPose(frameState.predictedDisplayTime);
        const bool upcomingViewsValid =
            LocateViewsForUpcomingRender(frameState.predictedDisplayTime);
        g_preparedFrame.viewsValid = upcomingViewsValid;
        g_preparedFrame.viewCount = upcomingViewsValid
            ? static_cast<uint32_t>(g_views.size()) : 0;
        g_preparedViewSerialPublished.store(
            upcomingViewsValid ? g_preparedFrame.serial : 0,
            std::memory_order_release);
        const bool upcomingHeadValid =
            CaptureHeadPose(frameState.predictedDisplayTime);
#if HALOMCCVR_EXPERIMENTAL_REACH_RENDER_CANDIDATE
        if (upcomingViewsValid && upcomingHeadValid)
            PublishReachRenderSnapshot(
                g_preparedFrame.serial, upcomingPadFresh);
#endif

        LARGE_INTEGER preparedAt{};
        QueryPerformanceCounter(&preparedAt);
        g_prepareQpcPublished.store(static_cast<uint64_t>(preparedAt.QuadPart),
                                    std::memory_order_release);
        g_preparedSerialPublished.store(g_preparedFrame.serial,
                                        std::memory_order_release);
        if (g_frameNo == 1)
            LOG("timing: exact OpenXR pipeline active; headset smoothing %.1f%%",
                std::clamp(g_config.headset_smoothing, 0.0f, 0.10f) * 100.0f);
        LARGE_INTEGER prepareEnd{};
        QueryPerformanceCounter(&prepareEnd);
        pacing.prepareEndQpc = prepareEnd.QuadPart;
    }

    bool LocateViewsForUpcomingRender(XrTime displayTime)
    {
        if (g_views.empty())
            return false;
        XrViewLocateInfo info{XR_TYPE_VIEW_LOCATE_INFO};
        info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        info.displayTime = displayTime;
        info.space = g_localSpace;
        XrViewState state{XR_TYPE_VIEW_STATE};
        uint32_t count = 0;
        const XrResult result = xrLocateViews(
            g_session, &info, &state, static_cast<uint32_t>(g_views.size()),
            &count, g_views.data());
        return XR_SUCCEEDED(result) && count == g_views.size() &&
            (state.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) &&
            (state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT);
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
            float haloHalfX[2] = {atanf(1.091595f), atanf(1.091595f)};
            float haloHalfY[2] = {atanf(1.114286f), atanf(1.114286f)};
            const bool renderFovsValid = Game_GetRenderHalfFovs(
                g_preparedFrame.serial, haloHalfX, haloHalfY);
            for (uint32_t i = 0; renderFovsValid && i < locatedViewCount; ++i)
            {
                projectionViews[i].pose = g_views[i].pose;
                projectionViews[i].fov = {
                    -haloHalfX[i], haloHalfX[i], haloHalfY[i], -haloHalfY[i]};
                projectionViews[i].subImage.swapchain = g_stereoChain;
                projectionViews[i].subImage.imageRect = {
                    {0, 0}, {(int32_t)g_stereoW, (int32_t)g_stereoH}};
                projectionViews[i].subImage.imageArrayIndex = i;
            }
            if (renderFovsValid)
            {
                projection.viewCount = locatedViewCount;
                projection.views = projectionViews.data();
            }
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
#if HALOMCCVR_EXPERIMENTAL_REACH_RENDER_CANDIDATE
                    ReachVrRenderAccess reachAccess{};
                    bool reachImages = false;
                    uint32_t reachGeneration = 0;
                    const bool reachTitle =
                        TitleAdapter_GetActiveTitle() == GameTitle::HaloReach;
                    if (reachTitle)
                    {
                        constexpr size_t reachSlot =
                            TitleRuntimeSlotIndex(GameTitle::HaloReach);
                        const TitleRuntimeAvailabilitySnapshot availability =
                            TitleAdapter_GetAvailability();
                        const ReachModuleEpoch epoch{
                            availability.moduleBases[reachSlot],
                            TitleAdapter_GetGeneration(GameTitle::HaloReach)};
                        reachGeneration = epoch.generation;
                        const ReachPreparedFrameToken prepared =
                            ReachPreparedFrameToken::Create(
                                epoch, g_preparedFrame.serial, true);
                        reachImages = VR_ReachBeginRenderAccess(
                                epoch, prepared, reachAccess) &&
                            g_reachEyeSerial[0].load(
                                std::memory_order_acquire) ==
                                g_preparedFrame.serial &&
                            g_reachEyeSerial[1].load(
                                std::memory_order_acquire) ==
                                g_preparedFrame.serial;
                        if (!reachImages && reachAccess.active)
                            VR_ReachEndRenderAccess(reachAccess);
                    }
#else
                    const bool reachTitle = false;
                    const bool reachImages = false;
                    const uint32_t reachGeneration = 0;
#endif
                    if (!reachTitle)
                        ValidateStereoImagesOnce();
                    uint32_t idx = 0;
                    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
                    XrSwapchainImageWaitInfo swi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                    swi.timeout = 1000000000;
                    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                    bool reachStereoUploadComplete = !reachTitle;
                    const XrResult stereoAcquire =
                        xrAcquireSwapchainImage(g_stereoChain, &ai, &idx);
                    const bool stereoAcquired = reachTitle
                        ? stereoAcquire == XR_SUCCESS
                        : XR_SUCCEEDED(stereoAcquire);
                    if (reachTitle && stereoAcquire != XR_SUCCESS &&
                        XR_SUCCEEDED(stereoAcquire))
                    {
                        (void)RequireReachSwapchainCompletion(
                            stereoAcquire,
                            "Reach world swapchain acquire did not complete");
                    }
                    bool stereoWaitCompleted = false;
                    if (stereoAcquired)
                    {
                        const XrResult stereoWait =
                            xrWaitSwapchainImage(g_stereoChain, &swi);
                        stereoWaitCompleted = reachTitle
                            ? RequireReachSwapchainCompletion(
                                  stereoWait,
                                  "Reach world swapchain wait did not complete")
                            : XR_SUCCEEDED(stereoWait);
                    }
                    if (stereoAcquired && stereoWaitCompleted)
                    {
                        bool everyReachEyeUploaded = reachImages;
                        for (uint32_t eye = 0; eye < 2; ++eye)
                        {
                            const bool haveImage = reachImages ||
                                (!reachTitle && g_eyeHasImage[eye]);
                            ID3D11Texture2D* source = reachImages
                                ? reachAccess.eyes[eye]
                                : (reachTitle ? nullptr : g_eyeCache[eye]);
                            const D3D11_TEXTURE2D_DESC& sourceDesc = reachImages
                                ? g_reachCaptureDesc : g_eyeCacheDesc;
                            bool eyeUploaded = false;
                            if (haveImage && source)
                                if (ID3D11RenderTargetView* rtv = GetStereoRtv(idx, eye))
                                    eyeUploaded = BlitImageQuality(
                                        source, sourceDesc, g_stereoImages[idx],
                                        g_stereoW, g_stereoH, rtv);
                            if (reachTitle)
                                everyReachEyeUploaded =
                                    everyReachEyeUploaded && eyeUploaded;
                        }
                        const XrResult stereoRelease =
                            xrReleaseSwapchainImage(g_stereoChain, &ri);
                        const bool stereoReleased = reachTitle
                            ? RequireReachSwapchainCompletion(
                                  stereoRelease,
                                  "Reach world swapchain release did not complete")
                            : XR_SUCCEEDED(stereoRelease);
                        if (reachTitle)
                            reachStereoUploadComplete =
                                everyReachEyeUploaded && stereoReleased;
                    }

                    // A Reach frame that cannot expose a complete stereo pair
                    // is SKIPPED, not fatal. Revoke this frame's copied eye
                    // serials so nothing stale or partial is queued, then let
                    // the next frame try again.
                    //
                    // This used to call Game_RejectReachAuthoredReticle, which
                    // disarms the core and unhooks the mod. projection.viewCount
                    // is only set to 2 once Game_GetRenderHalfFovs matches the
                    // prepared frame serial, and Reach publishes those FOVs
                    // while its eyes render -- so an ordinary ordering gap on a
                    // single frame permanently destroyed Reach VR. That is the
                    // teardown behind every unexplained "stereo OFF" on
                    // 2026-07-26. Halo 3 and ODST skip such a frame and recover.
#if HALOMCCVR_EXPERIMENTAL_REACH_RENDER_CANDIDATE
                    if (reachTitle && reachImages &&
                        (!reachStereoUploadComplete || projection.viewCount != 2))
                    {
                        g_reachEyeSerial[0].store(
                            0, std::memory_order_release);
                        g_reachEyeSerial[1].store(
                            0, std::memory_order_release);
                        g_authoredReticleReady = false;
                        g_authoredReticleSerial = 0;
                        // Loud but rate-limited: never a silent degrade.
                        static std::atomic<uint64_t> lastSkipLogMs{0};
                        const uint64_t nowMs = GetTickCount64();
                        uint64_t previousMs =
                            lastSkipLogMs.load(std::memory_order_relaxed);
                        if (nowMs - previousMs >= 2000 &&
                            lastSkipLogMs.compare_exchange_strong(
                                previousMs, nowMs, std::memory_order_relaxed))
                        {
                            LOG("Reach frame skipped (%s); the camera core "
                                "stays armed and the next frame retries",
                                !reachStereoUploadComplete
                                    ? "eye pair did not finish uploading to the "
                                      "XR swapchain"
                                    : "projection did not expose two views");
                        }
                    }
#endif
                    const bool projectionImagesReady = reachTitle
                        ? reachImages && reachStereoUploadComplete
                        : g_eyeHasImage[0] && g_eyeHasImage[1];
                    if (projectionImagesReady &&
                        projection.viewCount == 2)
                    {
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
                        const bool authoredReticleThisFrame =
                            g_authoredReticleReady &&
                            g_authoredReticleSerial == g_preparedFrame.serial;
#if HALOMCCVR_EXPERIMENTAL_REACH_RENDER_CANDIDATE
                        // Reachability proof for the whole block below. If the
                        // quad heartbeat is silent while THIS is alive, the
                        // difference is the `reachTitle` test; if both are
                        // silent, the compositor is not reaching Reach's
                        // authored path at all and every conclusion drawn from
                        // that silence is void.
                        {
                            static uint64_t lastAliveMs = 0;
                            const uint64_t aliveNow = GetTickCount64();
                            if (aliveNow - lastAliveMs >= 2000)
                            {
                                lastAliveMs = aliveNow;
                                LOG("REACHHUD: compositor alive "
                                    "(reachTitle=%d reachImages=%d "
                                    "ownsReach=%d authoredThisFrame=%d "
                                    "heldArt=%d key=%016llX)",
                                    reachTitle ? 1 : 0,
                                    reachImages ? 1 : 0,
                                    Game_OwnsReachAuthoredReticle() ? 1 : 0,
                                    authoredReticleThisFrame ? 1 : 0,
                                    g_reticleContainsAuthored ? 1 : 0,
                                    static_cast<unsigned long long>(
                                        Game_GetAuthoredCrosshairKey()));
                            }
                        }
#endif
                        const bool reachAuthoredReticleThisFrame =
                            reachTitle && reachImages &&
                            authoredReticleThisFrame &&
                            Game_OwnsReachAuthoredReticle();
                        // Reach reaches the shared aim-ray quad through the
                        // PROCEDURAL path only. The authored branch below --
                        // and the transaction rejection inside its failure
                        // handler -- stays Halo 3 / ODST only, so a crosshair
                        // problem can never disarm Reach's camera core.
                        // ODST does not consult the capability system for its
                        // reticle either -- it uses a direct ownership check
                        // (Game_IsCameraOnlyBringup), which is why it worked
                        // immediately. Reach gets the same treatment: ask the
                        // Reach core directly whether it owns this frame.
                        // Game_HasTitleCapability keeps returning false for
                        // Reach because the shared runtime snapshot spends much
                        // of its time "pending", which reports zero
                        // capabilities by design.
                        const bool reticleTitleAdmitted =
                            Game_HasTitleCapability(
                                TitleCapability_ControllerAim) ||
                            Game_OwnsReachAuthoredReticle();
                        const bool reticleUploadAdmitted =
                            reticleTitleAdmitted &&
                            g_config.crosshair && haveAim &&
                            EnsureReticleChain();
                        // Reach uploads its captured widget art exactly like
                        // Halo 3 and ODST. The previous "!reachTitle" excluded
                        // Reach from ever publishing the art it captures, which
                        // is why its crosshair stayed procedural even once the
                        // capture worked. The exclusion dated from when the
                        // Reach CHUD hook could not run at all.
                        // Re-uploading the authored reticle costs an OpenXR
                        // swapchain acquire, a BLOCKING wait, a copy and a
                        // release - every frame, on a second swapchain, on top
                        // of Reach's own stereo swapchain. Reach's crosshair art
                        // is static between weapon and state changes, so that
                        // work is almost always spent re-publishing an image
                        // identical to the one already in the swapchain. Skip it
                        // while the captured art's identity is unchanged; the
                        // released image stays valid and the quad keeps showing
                        // it. Halo 3 and ODST are untouched.
                        static uint64_t s_lastUploadedReachKey = 0;
                        static uint64_t s_lastUploadFrame = 0;
                        static uint64_t s_uploadsDone = 0;
                        static uint64_t s_uploadsSkipped = 0;
                        static uint64_t s_lastUploadLogMs = 0;
                        // Halo 3 and ODST pay this same per-frame blocking
                        // swapchain upload for art that almost never changes.
                        const bool titleCapturesArt =
                            Game_TitleCapturesAuthoredCrosshair();
                        const uint64_t reachCrosshairKey =
                            titleCapturesArt ? Game_GetAuthoredCrosshairKey() : 0;
                        // Measured: publishing the art costs ~4-5ms of the
                        // render window, which is the difference between
                        // fitting a 120Hz budget (8.33ms) and missing it and
                        // halving to 60. The art is static between weapon and
                        // state changes, so an unchanged key means the image
                        // already in the swapchain is still correct.
                        //
                        // The frame cap is a floor on that saving: even if the
                        // key were to churn, the blocking swapchain wait can
                        // then happen at most once every few frames instead of
                        // every single one.
                        constexpr uint64_t kReachUploadMinFrameGap = 6;
                        const bool reachKeyUnchanged =
                            titleCapturesArt && reachCrosshairKey != 0 &&
                            reachCrosshairKey == s_lastUploadedReachKey;
                        const bool reachUploadTooSoon =
                            titleCapturesArt && s_lastUploadFrame != 0 &&
                            g_preparedFrame.serial >= s_lastUploadFrame &&
                            g_preparedFrame.serial - s_lastUploadFrame <
                                kReachUploadMinFrameGap;
                        // key 0 means this frame's capture contained no
                        // crosshair widgets at all. Uploading then publishes a
                        // blank image over good art, which is what made the
                        // crosshair flash: the capture texture is cleared every
                        // frame, so any frame whose capture was empty would
                        // blank the swapchain until the next upload.
                        const bool reachCaptureEmpty =
                            titleCapturesArt && reachCrosshairKey == 0;
                        // Deliberately NOT gated on the key being unchanged.
                        // Doing that uploaded once and never again, and if that
                        // single upload caught the capture texture before its
                        // widgets had been drawn it published nothing and there
                        // was no second chance - perfect frame rate, no art.
                        // The key's job here is to prove the capture HAS
                        // content; the frame gap keeps the cost bounded. A
                        // steady low-rate refresh also keeps state changes
                        // (enemy/friendly colour, zoom) live.
                        // Steady state does no upload at all: the art only
                        // changes on a weapon swap, zoom or reticle-state
                        // change, and the key changes with it. The frame gap
                        // remains only as a floor so a churning key can never
                        // reintroduce a per-frame blocking swapchain wait.
                        // The key describes WHICH widgets drew, not how they
                        // look. Reach animates its crosshair, tints it red on
                        // an enemy and green on a hit, and fades it in and out
                        // - none of which change the key. Skipping the upload
                        // on an unchanged key therefore froze one snapshot
                        // forever: no animation, no colour states, and if the
                        // snapshot happened to be taken while the crosshair
                        // was faded out, a permanently blank crosshair. That
                        // is the headset blackout, proven 2026-07-27: at the
                        // moment the crosshair was gone, every gate was open
                        // (authoredThisFrame=1, quad SUBMITTED, heldArt=1) and
                        // the key sat frozen at DFFEE7EAB8A8F81F for the rest
                        // of the session. The art was being captured and shown
                        // the whole time - it was blank.
                        //
                        // Refresh on the frame gap instead. That keeps the
                        // measured cost bounded (the blocking swapchain
                        // acquire/wait/copy is ~4-5ms, so it must never run
                        // every frame at 120Hz) while letting animation,
                        // colour state and fade recovery through. (void) on
                        // reachKeyUnchanged: kept computed so the reasoning
                        // above stays legible next to it.
                        (void)reachKeyUnchanged;
                        const bool reachArtAlreadyPublished =
                            reachCaptureEmpty || reachUploadTooSoon;
                        const bool shouldUploadAuthoredReticle =
                            authoredReticleThisFrame && reticleUploadAdmitted &&
                            !reachArtAlreadyPublished;
                        bool authoredUploadFailed = false;
                        if (shouldUploadAuthoredReticle &&
                            UploadAuthoredReticle(false))
                        {
                            s_lastUploadedReachKey = reachCrosshairKey;
                            s_lastUploadFrame = g_preparedFrame.serial;
                            ++s_uploadsDone;
                        }
                        else if (shouldUploadAuthoredReticle)
                        {
                            // Never expose stale or undefined swapchain
                            // contents: repaint the chain, exactly as the
                            // accepted Halo 3 / ODST path does. A crosshair
                            // upload failure must not disarm Reach's camera
                            // core or drop its world projection - failure
                            // isolation, per AGENTS.md. It is reported loudly
                            // rather than passing silently.
                            g_authoredReticleReady = false;
                            EnsureReticleChain();
                            static std::atomic<bool> loggedUploadLoss{false};
                            if (!loggedUploadLoss.exchange(
                                    true, std::memory_order_relaxed))
                                LOG("authored reticle upload FAILED; the VR "
                                    "reticle is showing its procedural art "
                                    "this frame instead of the game's widget");
                        }
                        if (reachTitle)
                        {
                            if (reachArtAlreadyPublished)
                                ++s_uploadsSkipped;
                            const uint64_t nowMs = GetTickCount64();
                            if (nowMs - s_lastUploadLogMs >= 2000)
                            {
                                s_lastUploadLogMs = nowMs;
                                LOG("Reach reticle upload: %llu uploaded, %llu "
                                    "skipped in the last window (key %llX)",
                                    static_cast<unsigned long long>(
                                        s_uploadsDone),
                                    static_cast<unsigned long long>(
                                        s_uploadsSkipped),
                                    static_cast<unsigned long long>(
                                        reachCrosshairKey));
                                s_uploadsDone = 0;
                                s_uploadsSkipped = 0;
                            }
                        }
                        const bool liveReachOwnerAfterUpload =
                            !reachTitle || Game_OwnsReachAuthoredReticle();
                        if (reachTitle && !liveReachOwnerAfterUpload)
                        {
                            // A title/lifecycle transition can complete while
                            // OpenXR waits for the authored upload. Revoke the
                            // pair again after that wait and age out any uploaded
                            // Reach art so the next title clears it before use.
#if HALOMCCVR_EXPERIMENTAL_REACH_RENDER_CANDIDATE
                            g_reachEyeSerial[0].store(
                                0, std::memory_order_release);
                            g_reachEyeSerial[1].store(
                                0, std::memory_order_release);
#endif
                            if (reachAuthoredReticleThisFrame)
                            {
                                g_authoredReticleReady = false;
                                g_authoredReticleSerial = 0;
                            }
                        }
                        const bool reachProjectionAdmitted =
                            ReachCanSubmitCompleteProjection(
                                reachTitle, reachStereoUploadComplete,
                                authoredUploadFailed,
                                liveReachOwnerAfterUpload);
                        const bool reticleOwnerAdmitted =
                            reticleTitleAdmitted &&
                            // Reach shows its crosshair only alongside its own
                            // admitted world projection.
                            (!reachTitle || reachProjectionAdmitted);
                        if (reachProjectionAdmitted)
                        {
                            layers.push_back(
                                reinterpret_cast<XrCompositionLayerBaseHeader*>(
                                    &projection));
                        }
                        const bool reticleChainAdmitted = reticleUploadAdmitted;
                        const bool reticleQuadSubmitted =
                            reticleOwnerAdmitted && g_config.crosshair &&
                            haveAim && reticleChainAdmitted;
#if HALOMCCVR_EXPERIMENTAL_REACH_RENDER_CANDIDATE
                        // The 2026-07-27 objective blackout is proven NOT to be
                        // the capture, the class resolution, or the redirect:
                        // during it Reach draws every other HUD widget at its
                        // normal rate and simply emits no crosshair, while the
                        // authored art stays held in the swapchain and the
                        // repaint guard leaves it alone. So the art exists and
                        // the quad still stops reaching the eye - which means
                        // one of these gates is refusing. Name it rather than
                        // guess a third time. Throttled state-change only, on
                        // the compositor thread that already logs status lines.
                        if (reachTitle)
                        {
                            // Heartbeat as well as state change. Twice this
                            // point produced ZERO lines, which was ambiguous
                            // between "the decision never changed" and "this
                            // code never runs" - and a conclusion was drawn
                            // from it anyway. A periodic line removes the
                            // ambiguity: silence now means only that this
                            // point is unreachable.
                            static bool loggedSubmitted = true;
                            static uint64_t lastHeartbeatMs = 0;
                            const uint64_t nowMs = GetTickCount64();
                            const bool heartbeat =
                                nowMs - lastHeartbeatMs >= 2000;
                            if (heartbeat)
                                lastHeartbeatMs = nowMs;
                            if (reticleQuadSubmitted != loggedSubmitted ||
                                heartbeat)
                            {
                                loggedSubmitted = reticleQuadSubmitted;
                                LOG("REACHHUD: crosshair quad %s "
                                    "(ownerAdmitted=%d projectionAdmitted=%d "
                                    "cfgCrosshair=%d haveAim=%d chain=%d "
                                    "authoredThisFrame=%d heldArt=%d)",
                                    reticleQuadSubmitted ? "SUBMITTED"
                                                         : "NOT submitted",
                                    reticleOwnerAdmitted ? 1 : 0,
                                    reachProjectionAdmitted ? 1 : 0,
                                    g_config.crosshair ? 1 : 0,
                                    haveAim ? 1 : 0,
                                    reticleChainAdmitted ? 1 : 0,
                                    authoredReticleThisFrame ? 1 : 0,
                                    g_reticleContainsAuthored ? 1 : 0);
                            }
                        }
#endif
                        if (reticleQuadSubmitted)
                        {
                            XrPosef rawAim{{aimQ[0],aimQ[1],aimQ[2],aimQ[3]},
                                           {aimP[0],aimP[1],aimP[2]}};
                            // Weapon weight already owns the shared gun/bullet/
                            // reticle ray. Do not add a second visual-only lag.
                            const float smoothing = g_config.weapon_inertia ? 0.0f :
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

                        if (reachProjectionAdmitted &&
                            Game_AllowsSharedGameplayFeatures() &&
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
#if HALOMCCVR_EXPERIMENTAL_REACH_RENDER_CANDIDATE
                    if (reachAccess.active)
                        VR_ReachEndRenderAccess(reachAccess);
#endif
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

                if (g_abortFrameForReachSwapchainFailure)
                {
                    // Do not acquire menu/fade images or submit any partial
                    // Reach layer set. xrBeginFrame still requires a matching
                    // xrEndFrame; an empty layer list references no potentially
                    // outstanding swapchain image.
                    Menu_ClearVrPointer();
                    backbuffer->Release();
                    EndPreparedFrameWithoutLayers(
                        "Reach swapchain transaction failed");
                    return;
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
        // Preserve the legacy rolling submit timer. Transition diagnosis uses
        // the correlated raw-QPC record above; this aggregate cannot identify
        // which logical frame changed cadence.
        LARGE_INTEGER endStart{}, endEnd{};
        QueryPerformanceCounter(&endStart);
        if (g_beginFrameQpc.QuadPart)
            g_renderWindowMs.Add(QpcMs(endStart.QuadPart - g_beginFrameQpc.QuadPart));
        XrResult r = xrEndFrame(g_session, &ei);
        QueryPerformanceCounter(&endEnd);
        g_endFrameDurationsMs.Add(QpcMs(endEnd.QuadPart - endStart.QuadPart));
        if (g_framePacingPending.serial == g_preparedFrame.serial)
        {
            g_framePacingPending.endStartQpc = endStart.QuadPart;
            g_framePacingPending.endEndQpc = endEnd.QuadPart;
            g_framePacingPending.endResult = r;
            g_framePacingPending.layerCount =
                static_cast<uint32_t>(layers.size());
            g_framePacingPending.submitted = true;
        }
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

#if HALOMCCVR_EXPERIMENTAL_REACH_RENDER_CANDIDATE
void VR_ReachRenderCandidate_ColdPoll()
{
    PollReachDisplayCandidate();
}

ReachPreparedFrameToken VR_ReachPreparedFrame(
    const ReachModuleEpoch& epoch)
{
    const uint64_t serial =
        g_preparedSerialPublished.load(std::memory_order_acquire);
    const uint64_t viewSerial =
        g_preparedViewSerialPublished.load(std::memory_order_acquire);
    return ReachPreparedFrameToken::Create(
        epoch, serial,
        serial != 0 &&
        viewSerial == serial &&
        g_preparedShouldRender.load(std::memory_order_acquire));
}

bool VR_ReachGetRenderSnapshot(
    const ReachPreparedFrameToken& prepared,
    ReachVrRenderSnapshot& snapshot)
{
    snapshot = {};
    if (!prepared.Ready() || !prepared.Serial())
        return false;

    const uint32_t index =
        g_reachRenderSnapshotIndex.load(std::memory_order_seq_cst);
    if (index >= 2)
        return false;

    uint32_t state =
        g_reachRenderSnapshotStates[index].load(std::memory_order_seq_cst);
    if ((state & kReachRenderSnapshotWriter) != 0 ||
        state >= kReachRenderSnapshotWriter - 1)
    {
        return false;
    }
    if (!g_reachRenderSnapshotStates[index].compare_exchange_strong(
            state, state + 1,
            std::memory_order_seq_cst,
            std::memory_order_seq_cst))
    {
        return false;
    }

    // A publication can switch slots between our index load and reader pin.
    // Revalidate before touching plain snapshot storage. A writer cannot claim
    // this slot while the reader count is nonzero.
    if (g_reachRenderSnapshotIndex.load(std::memory_order_seq_cst) != index)
    {
        g_reachRenderSnapshotStates[index].fetch_sub(
            1, std::memory_order_seq_cst);
        return false;
    }

    const ReachVrRenderSnapshot local = g_reachRenderSnapshots[index];
    g_reachRenderSnapshotStates[index].fetch_sub(
        1, std::memory_order_seq_cst);
    if (local.preparedSerial != prepared.Serial())
        return false;
    snapshot = local;
    return true;
}

bool VR_ReachDisplayReady(const ReachModuleEpoch& epoch)
{
    // Pin the plain proof fields exactly like BeginRenderAccess. The worker
    // disables publication and waits for this reader count before releasing or
    // zeroing them, so readiness cannot race resource invalidation.
    g_reachCaptureUsers.fetch_add(1, std::memory_order_seq_cst);
    const bool ready =
        g_reachCaptureEnabled.load(std::memory_order_seq_cst) &&
        ReachSameModuleEpoch(g_reachCaptureProof.continuity.epoch, epoch) &&
        ReachRenderCandidate_IsPreflightCurrent(
            g_reachCaptureProof.preflight);
    g_reachCaptureUsers.fetch_sub(1, std::memory_order_seq_cst);
    return ready;
}

bool VR_ReachBeginRenderAccess(
    const ReachModuleEpoch& epoch,
    const ReachPreparedFrameToken& prepared,
    ReachVrRenderAccess& access)
{
    access = {};
    g_reachCaptureUsers.fetch_add(1, std::memory_order_seq_cst);
    if (!g_reachCaptureEnabled.load(std::memory_order_seq_cst) ||
        !prepared.Ready() ||
        !ReachSameModuleEpoch(prepared.Epoch(), epoch) ||
        !g_reachCaptureSource || !g_reachCaptureContext ||
        !g_reachCaptureEyes[0] || !g_reachCaptureEyes[1] ||
        !ReachSameModuleEpoch(g_reachCaptureProof.continuity.epoch, epoch) ||
        !ReachRenderCandidate_IsPreflightCurrent(
            g_reachCaptureProof.preflight))
    {
        g_reachCaptureUsers.fetch_sub(1, std::memory_order_seq_cst);
        return false;
    }
    access.proof = g_reachCaptureProof;
    access.source = g_reachCaptureSource;
    access.eyes[0] = g_reachCaptureEyes[0];
    access.eyes[1] = g_reachCaptureEyes[1];
    access.context = g_reachCaptureContext;
    access.preparedSerial = prepared.Serial();
    access.active = true;
    return true;
}

bool VR_ReachCopyEye(ReachVrRenderAccess& access, int eye)
{
    if (!access.active || eye < 0 || eye > 1 ||
        !access.context || !access.source || !access.eyes[eye] ||
        !g_reachCaptureEnabled.load(std::memory_order_seq_cst))
        return false;
    access.context->CopyResource(access.eyes[eye], access.source);
    g_reachEyeSerial[eye].store(
        access.preparedSerial, std::memory_order_release);
    return true;
}

void VR_ReachEndRenderAccess(ReachVrRenderAccess& access)
{
    if (!access.active)
        return;
    access = {};
    g_reachCaptureUsers.fetch_sub(1, std::memory_order_seq_cst);
}
#endif

void VR_BeforePresent(IDXGISwapChain* sc)
{
    LARGE_INTEGER pacingBeforePresent{};
    QueryPerformanceCounter(&pacingBeforePresent);
    if (g_framePacingPending.serial)
    {
        g_framePacingPending.beforePresentQpc =
            pacingBeforePresent.QuadPart;
        g_framePacingPending.scopeActive =
            g_scopeActive.load(std::memory_order_relaxed);
        GameFramePerfCounters perfEnd{};
        Game_ReadFramePerfCounters(perfEnd);
        SubtractPerfCounters(
            perfEnd, g_framePacingPerfStart, g_framePacingPending.perf);
    }
    g_gameSwapchain = sc;
#if HALOMCCVR_EXPERIMENTAL_REACH_RENDER_CANDIDATE
    // This observation is intentionally before every OpenXR/session early
    // return. It rejects unrelated Presents with one raw engine-swapchain
    // identity read, then at most every 250 ms captures a fixed-storage retained
    // buffer/device snapshot at the exact Reach Present boundary. It performs
    // no file/signature scan, resource allocation, lock, or logging.
    ObserveReachPresentSwapchain(sc);
#endif
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
            LOG("fps %.0f (stereo %s); app cadence %.1fHz "
                "(xrWaitFrame period), panel %.1fHz",
                g_status.fps, g_stereoEnabled.load() ? "on" : "off",
                VR_HeadsetRefreshHz(),
                g_panelRefreshHz.load(std::memory_order_relaxed));
        }
    }

    const uint64_t timingNowMs = GetTickCount64();
    if (!g_timingLogStartMs)
        g_timingLogStartMs = timingNowMs;
    else if (timingNowMs - g_timingLogStartMs >= 10000)
    {
        // Backward-compatible rolling summaries. These are the latest 512
        // samples (not a fixed 10-second window) and are intentionally not used
        // to attribute a transition; the PACING capture correlates exact rows.
        LOG("timing: frame interval p95 %.2fms p99 %.2fms; "
            "renderWindow p95 %.2fms; xrEndFrame p95 %.2fms; "
            "DXGI Present p95 %.2fms; wait handoff p95 %.2fms; "
            "prediction error p95 %.3fms; missed=%llu duplicate=%llu "
            "orderFailures=%llu firstCamera=%.3fms",
            TimingPercentile(g_presentIntervalsMs, 0.95),
            TimingPercentile(g_presentIntervalsMs, 0.99),
            TimingPercentile(g_renderWindowMs, 0.95),
            TimingPercentile(g_endFrameDurationsMs, 0.95),
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
}

void VR_AfterPresent(IDXGISwapChain* sc, int64_t presentStartQpc,
                     int64_t presentEndQpc, HRESULT presentResult)
{
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    if (presentEndQpc >= presentStartQpc && presentStartQpc)
        g_presentDurationsMs.Add(
            QpcMs(presentEndQpc - presentStartQpc));

    if (g_framePacingPending.serial && g_framePacingPending.submitted)
    {
        g_framePacingPending.presentStartQpc = presentStartQpc;
        g_framePacingPending.presentEndQpc = presentEndQpc;
        g_framePacingPending.afterPresentQpc = now.QuadPart;
        g_framePacingPending.presentResult = presentResult;
        PublishFramePacingRecord(g_framePacingPending);
    }
    g_framePacingPending = {};

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

void VR_FramePacingWorkerPoll()
{
    // Keep exceptional pipeline reporting off the game's render path. Packet
    // misses are counted there with one atomic increment and summarized here.
    static uint64_t reportedPacketMisses = 0;
    static uint64_t reportedWaitFailures = 0;
    static uint64_t reportedSignalFailures = 0;
    static uint64_t lastPacketMissReportMs = 0;
    static uint64_t lastWaitFailureReportMs = 0;
    static uint64_t lastSignalFailureReportMs = 0;
    static bool pipelineFaultReported = false;
    const uint64_t workerNowMs = GetTickCount64();
    const uint64_t packetMisses =
        g_waitPacketMisses.load(std::memory_order_relaxed);
    if (!packetMisses)
    {
        reportedPacketMisses = 0;
        lastPacketMissReportMs = 0;
    }
    else if (packetMisses != reportedPacketMisses &&
             (!lastPacketMissReportMs ||
              workerNowMs - lastPacketMissReportMs >= 1000))
    {
        LOG("pacing: wait-worker packet unavailable; skipped XR preparation "
            "without an inline wait (%llu misses total)",
            static_cast<unsigned long long>(packetMisses));
        reportedPacketMisses = packetMisses;
        lastPacketMissReportMs = workerNowMs;
    }

    const uint64_t waitFailures =
        g_waitFailuresObserved.load(std::memory_order_relaxed);
    if (!waitFailures)
    {
        reportedWaitFailures = 0;
        lastWaitFailureReportMs = 0;
    }
    else if (waitFailures != reportedWaitFailures &&
             (!lastWaitFailureReportMs ||
              workerNowMs - lastWaitFailureReportMs >= 1000))
    {
        LOG("timing: worker xrWaitFrame failure observed (%llu total): %s",
            static_cast<unsigned long long>(waitFailures),
            XrStr(g_waitedStateResult.load(std::memory_order_acquire)));
        reportedWaitFailures = waitFailures;
        lastWaitFailureReportMs = workerNowMs;
    }

    const uint64_t signalFailures =
        g_waitEventSignalFailures.load(std::memory_order_relaxed);
    if (!signalFailures)
    {
        reportedSignalFailures = 0;
        lastSignalFailureReportMs = 0;
    }
    else if (signalFailures != reportedSignalFailures &&
             (!lastSignalFailureReportMs ||
              workerNowMs - lastSignalFailureReportMs >= 1000))
    {
        LOG("pacing: frame-wait event signalling failed (%llu failures total); "
            "exact sequence predicates remain authoritative",
            static_cast<unsigned long long>(signalFailures));
        reportedSignalFailures = signalFailures;
        lastSignalFailureReportMs = workerNowMs;
    }

    const bool pipelineFaulted =
        g_waitPipelineFaulted.load(std::memory_order_acquire);
    if (pipelineFaulted && !pipelineFaultReported)
    {
        LOG("pacing: exact wait-worker pipeline faulted; no render-thread "
            "xrWaitFrame will be substituted");
        pipelineFaultReported = true;
    }
    else if (!pipelineFaulted)
    {
        pipelineFaultReported = false;
    }

    FramePacingWorkerState& worker = g_framePacingWorker;
    FramePacingRecord record{};
    while (ConsumeFramePacingRecord(record))
    {
        if (worker.capturing)
        {
            const FramePacingRecord& previous =
                worker.capture[worker.captureCount - 1];
            if (record.sessionEpoch != previous.sessionEpoch ||
                !IsEligiblePacingRecord(record))
            {
                FinishPacingCapture(worker, "eligibility-ended");
            }
            else
            {
                if (record.serial != previous.serial + 1 ||
                    !IsExactWaitPair(previous, record))
                {
                    worker.exact = false;
                }
                if (worker.captureCount < worker.capture.size())
                    worker.capture[worker.captureCount++] = record;
                else
                    worker.exact = false;
                worker.lastDataMs = GetTickCount64();
                if (worker.postRemaining)
                    --worker.postRemaining;
                if (!worker.postRemaining ||
                    worker.captureCount == worker.capture.size())
                {
                    FinishPacingCapture(worker, "post-roll-complete");
                }
                continue;
            }
        }

        if (!IsEligiblePacingRecord(record))
        {
            ClearPacingHistory(worker);
            continue;
        }

        const FramePacingRecord* previous = LastPacingHistory(worker);
        if (previous &&
            (previous->sessionEpoch != record.sessionEpoch ||
             previous->serial + 1 != record.serial))
        {
            ClearPacingHistory(worker);
            previous = nullptr;
        }
        if (previous && IsMaterialFramePeriodTransition(
                static_cast<uint64_t>(previous->predictedDisplayPeriodNs),
                static_cast<uint64_t>(record.predictedDisplayPeriodNs)))
        {
            BeginPacingCapture(
                worker, record, previous->predictedDisplayPeriodNs);
        }
        else
        {
            AddPacingHistory(worker, record);
        }
    }

    if (worker.capturing && worker.lastDataMs &&
        GetTickCount64() - worker.lastDataMs >= 3000)
    {
        worker.exact = false;
        FinishPacingCapture(worker, "post-roll-timeout");
    }
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
#if HALOMCCVR_EXPERIMENTAL_REACH_RENDER_CANDIDATE
    // Resize is rare and must exclude the worker while DXGI owns the buffer
    // transition. VR_AfterResizeBuffers releases this lock after the real call.
    AcquireSRWLockExclusive(&g_reachDisplayResourceLock);
    g_reachResizeActive.store(true, std::memory_order_release);
    InvalidateReachPresentAdmission();
    DiscardReachDisplaySnapshots(true);
#endif
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
#if HALOMCCVR_EXPERIMENTAL_REACH_RENDER_CANDIDATE
    // Buffer 0 and the engine's record-0 views are invalid at this edge. A
    // copied readiness token must fail immediately even if COM reuses every
    // pointer value after ResizeBuffers.
    ResetReachDisplayCandidateLocked(false, false);
#endif
    if (ID3D11Texture2D* retained =
            g_nextGameBackbuffer.exchange(nullptr, std::memory_order_acq_rel))
        retained->Release();
}

void VR_AfterResizeBuffers(IDXGISwapChain*)
{
#if HALOMCCVR_EXPERIMENTAL_REACH_RENDER_CANDIDATE
    g_reachPresentNextSnapshotMs.store(0, std::memory_order_relaxed);
    g_reachResizeActive.store(false, std::memory_order_release);
    ReleaseSRWLockExclusive(&g_reachDisplayResourceLock);
#endif
}

void VR_RequestRecenter()
{
    g_haveCenter = false;
    ResetWeaponInertia(false);
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

float VR_HeadsetRefreshHz()
{
    const uint64_t period = g_displayPeriodNs.load(std::memory_order_relaxed);
    return period ? (float)(1000000000.0 / (double)period) : 0.0f;
}

bool VR_IsFramePacingOwned()
{
    // True once the runtime is driving our cadence through xrWaitFrame. Until
    // then the desktop present must keep the game's own timing untouched.
    return g_state == State::Ready && g_sessionRunning &&
           g_displayPeriodNs.load(std::memory_order_relaxed) != 0;
}

void VR_DetachGamePresentation()
{
    ResetWeaponInertia(true);
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
#if HALOMCCVR_EXPERIMENTAL_REACH_RENDER_CANDIDATE
    // The worker owns gate/resource mutation. Present only revokes the
    // admission stamp so a proof racing this detach cannot publish.
    InvalidateReachPresentAdmission();
#endif
    ReleaseSourceViews();
    ReleaseIqChain();
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


// FSR diagnostic (fsr_probe, off by default). Log-only: observe slot 0 of every
// OMSetRenderTargets bind and record each DISTINCT (width, height, format,
// bind-flags) SCENE-SCALE render target, plus the viewport and current raster
// eye at that bind. When MCC's built-in FSR is toggled, a newly logged tuple
// shows whether FSR renders the scene into a smaller target (proven: it does)
// and, via the raster-eye column, whether that target is bound inside the
// per-eye render loop. Runs on the render thread only, allocation-free and
// lock-free: the table is a fixed static seen only from that thread.
//
// Two guards keep the log bounded (the first version dumped 1.16M lines / 145MB
// because MCC binds hundreds of tiny transient post-fx/shadow targets and the
// 12-slot table saturated instantly): skip anything narrower than ~40% of the
// backbuffer (below any real FSR render scale, which bottoms out at 50%), and
// dedup into a 64-slot table.
static void ProbeFsrTargets(ID3D11DeviceContext* context, UINT count,
                            ID3D11RenderTargetView* const* input)
{
    if (!g_config.fsr_probe || !context || !count || !input || !input[0])
        return;

    ID3D11Resource* resource = nullptr;
    input[0]->GetResource(&resource);
    if (!resource)
        return;
    ID3D11Texture2D* tex = nullptr;
    D3D11_TEXTURE2D_DESC desc{};
    const bool isTexture = SUCCEEDED(resource->QueryInterface(
        __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex)));
    if (isTexture)
        tex->GetDesc(&desc);
    if (tex)
        tex->Release();
    resource->Release();
    if (!isTexture)
        return;

    // Only scene-scale targets are interesting for the FSR question. Anything
    // below ~40% of the backbuffer width is a shadow/post-fx buffer, not a
    // candidate scene render (FSR's lowest tier is 50%). Fail-open if the
    // backbuffer size is not known yet: keep a fixed 800px floor.
    const UINT bbW = g_gameBackbufferDescValid ? g_gameBackbufferDesc.Width : 0;
    const UINT bbH = g_gameBackbufferDescValid ? g_gameBackbufferDesc.Height : 0;
    const UINT minWidth = bbW ? (bbW * 2) / 5 : 800u;
    if (desc.Width < minWidth)
        return;

    // Dedup on (size, format, bind, rasterEye) so each distinct large target is
    // logged once PER eye-context. The raster eye is the key new signal: it tells
    // us whether a given FSR target is bound inside the per-eye redirect scope
    // (0/1) or outside it (-1), which the old flat log could not show.
    const int rasterEye = g_rasterEye.load(std::memory_order_relaxed);
    struct SeenTarget { UINT w, h; UINT format, bind; int eye; };
    static SeenTarget seen[96]{};
    static unsigned seenCount = 0;
    const UINT descFormat = static_cast<UINT>(desc.Format);
    for (unsigned i = 0; i < seenCount; ++i)
        if (seen[i].w == desc.Width && seen[i].h == desc.Height &&
            seen[i].format == descFormat && seen[i].bind == desc.BindFlags &&
            seen[i].eye == rasterEye)
            return; // already logged this exact target shape in this eye-context

    D3D11_VIEWPORT vp{};
    UINT vpCount = 1;
    context->RSGetViewports(&vpCount, &vp);

    LOG("FSRPROBE: slot0 RT %ux%u fmt=%u bind=0x%X | viewport %.0fx%.0f at (%.0f,%.0f) "
        "| backbuffer %ux%u | rasterEye=%d | rtCount=%u",
        desc.Width, desc.Height, descFormat, desc.BindFlags,
        vpCount ? vp.Width : 0.0f, vpCount ? vp.Height : 0.0f,
        vpCount ? vp.TopLeftX : 0.0f, vpCount ? vp.TopLeftY : 0.0f,
        bbW, bbH, rasterEye, count);

    if (seenCount < 96)
        seen[seenCount++] = {desc.Width, desc.Height, descFormat, desc.BindFlags, rasterEye};
}

bool VR_RedirectRenderTargets(ID3D11DeviceContext* context, UINT count,
                              ID3D11RenderTargetView* const* input,
                              ID3D11RenderTargetView** output)
{
    ProbeFsrTargets(context, count, input);
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
    const float v = std::clamp(amplitude, 0.0f, 1.0f);
    g_requestedHaptics.store(v, std::memory_order_release);
    // Peak-hold: raise the running peak so a pulse that arrives and clears
    // between two VR-frame samples still registers. Lock-free CAS max keeps
    // this XInput SetState hook thread hot-path-safe (no lock/alloc/logging).
    float cur = g_peakHaptics.load(std::memory_order_relaxed);
    while (v > cur && !g_peakHaptics.compare_exchange_weak(cur, v,
        std::memory_order_release, std::memory_order_relaxed))
    {
    }
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

bool VR_CanPrepareAuthoredReticleResources()
{
    return g_authoredReticlePreparationReady.load(
        std::memory_order_acquire);
}

AuthoredReticlePreparationResult VR_PrepareAuthoredReticleResources()
{
#if HALOMCCVR_EXPERIMENTAL_REACH_RENDER_CANDIDATE
    ReachExclusiveResourceLock lock(g_reachDisplayResourceLock);
#endif
    if (!VR_CanPrepareAuthoredReticleResources() ||
        !g_device || !g_context || g_session == XR_NULL_HANDLE)
    {
        return AuthoredReticlePreparationResult::NotReady;
    }
    if (g_reticleChainFailed.load(std::memory_order_acquire))
        return AuthoredReticlePreparationResult::Failed;
    if (g_reticleChain == XR_NULL_HANDLE &&
        !CreateChain(kReticleSize, kReticleSize, g_reticleChain,
                     g_reticleImages, g_reticleRtvs, "crosshair"))
    {
        g_reticleChainFailed.store(true, std::memory_order_release);
        return AuthoredReticlePreparationResult::Failed;
    }
    if (g_reticleImages.empty() ||
        g_reticleRtvs.size() != g_reticleImages.size())
    {
        return AuthoredReticlePreparationResult::Failed;
    }
    for (uint32_t index = 0;
         index < static_cast<uint32_t>(g_reticleImages.size()); ++index)
    {
        if (!GetRtv(g_reticleImages, g_reticleRtvs, index))
            return AuthoredReticlePreparationResult::Failed;
    }
    return EnsureAuthoredReticleTexture()
        ? AuthoredReticlePreparationResult::Ready
        : AuthoredReticlePreparationResult::Failed;
}

void VR_InvalidatePreparedReachAuthoredReticleCapture()
{
    // Reach calls this only on the render thread before opening a new admitted
    // eye transaction. Halo 3/ODST retain their accepted serial lifecycle.
    g_authoredReticleReady = false;
    g_authoredReticleSerial = 0;
}

static bool BeginAuthoredReticleCaptureInternal(
    bool requirePreparedResources, bool requireCrosshairEnabled)
{
    // `crosshair=0` means "do not show a VR crosshair". For Halo 3/ODST,
    // refusing here is the whole implementation: they hide their native
    // reticle through a separate visibility predicate, so declining the
    // redirect correctly leaves stock drawing alone.
    //
    // Reach has no such predicate, and its CHUD alpha field is inert when
    // written (proven: 1.000 across 860 live samples while the mod wrote 0).
    // This redirect is therefore the ONLY thing that keeps Reach's native
    // crosshair off the eye. Refusing it for `crosshair=0` meant the setting
    // that asks for no crosshair produced the stock flat one instead - and,
    // before the failure-isolation fix, tore VR down as well. Reach passes
    // requireCrosshairEnabled=false so the redirect always runs; whether the
    // captured art is ever SHOWN is decided separately by the compositor,
    // which still requires g_config.crosshair before uploading or submitting
    // the quad. So `crosshair=0` in Reach now means exactly what it says:
    // no crosshair anywhere, flat or VR.
    if (!g_context ||
        (requireCrosshairEnabled && !g_config.crosshair) ||
        !g_stereoEnabled.load(std::memory_order_relaxed) ||
        !g_preparedFrame.begun || g_reticleCaptureState.active)
        return false;

    if (requirePreparedResources)
    {
        if (g_reticleChain == XR_NULL_HANDLE || g_reticleImages.empty() ||
            g_reticleRtvs.size() != g_reticleImages.size() ||
            !g_authoredReticleTexture || !g_authoredReticleRtv)
        {
            return false;
        }
        for (ID3D11RenderTargetView* rtv : g_reticleRtvs)
            if (!rtv)
                return false;
    }
    else
    {
        // Preserve the accepted Halo 3/ODST lazy preparation path. Reach calls
        // only the prepared entry above, so allocation and logging never occur
        // from its HREK widget hook.
        if (!EnsureReticleChain() || !EnsureAuthoredReticleTexture())
            return false;
    }

    const uint64_t serial = g_preparedFrame.serial;
    if (g_authoredReticleSerial != serial)
    {
        const float clear[4] = {0, 0, 0, 0};
        g_context->ClearRenderTargetView(g_authoredReticleRtv, clear);
        g_authoredReticleSerial = serial;
        g_authoredReticleReady = false;
        // New displayed frame: start the art-identity accumulation clean so a
        // static crosshair produces the same key every frame.
        Game_ResetAuthoredCrosshairKey();
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

bool VR_BeginAuthoredReticleCapture()
{
    return BeginAuthoredReticleCaptureInternal(false, true);
}

// Reach's hide-the-widget entry: lazily creates whatever is missing (so a
// transient resource state can never fail it - that regression cost a build)
// and never refuses because the crosshair is switched off, because refusing
// is what lets Reach's flat crosshair through.
bool VR_BeginAuthoredReticleRedirect()
{
    return BeginAuthoredReticleCaptureInternal(false, false);
}

bool VR_BeginPreparedAuthoredReticleCapture()
{
    return BeginAuthoredReticleCaptureInternal(true, false);
}

static bool EndAuthoredReticleCaptureInternal(bool allowFirstCaptureLog)
{
    auto& saved = g_reticleCaptureState;
    if (!saved.active || !g_context)
        return false;

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
    if (allowFirstCaptureLog && !logged)
    {
        LOG("M3: Halo authored per-weapon crosshair redirected to VR aim quad");
        logged = true;
    }
    return true;
}

void VR_EndAuthoredReticleCapture()
{
    (void)EndAuthoredReticleCaptureInternal(true);
}

bool VR_EndPreparedAuthoredReticleCapture()
{
    return EndAuthoredReticleCaptureInternal(false);
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
    if (!g_headCsInit)
        return false;
    EnterCriticalSection(&g_headCs);
    const bool okR = g_rightAimPoseValid;
    const XrPosef right = g_rightAimPose;
    const bool okL = g_leftAimPoseValid;
    const XrPosef left = g_leftAimPose;
    LeaveCriticalSection(&g_headCs);

    const AimPoseResult aim = ComputeAimPose(
        CurrentAimPoseInputs(okR, right, okL, left));
    if (!aim.updateTwoHandActivity)
        return false;

    // Preserve the getter's existing output contract: once the right pose is
    // valid, publish the best pose even if final quaternion validation fails.
    outQuat[0] = aim.pose.orientation.x;
    outQuat[1] = aim.pose.orientation.y;
    outQuat[2] = aim.pose.orientation.z;
    outQuat[3] = aim.pose.orientation.w;
    outPos[0] = aim.pose.position.x;
    outPos[1] = aim.pose.position.y;
    outPos[2] = aim.pose.position.z;

    if (aim.twoHandActive)
    {
        const bool wasActive = g_twoHandActive.exchange(true);
        if (!wasActive)
            LOG("M3: two-handed aim engaged (left grip held, hand on barrel)");
    }
    else
    {
        g_twoHandActive.store(false);
    }
    if (aim.rejectedExtreme)
    {
        static uint64_t lastRejectLogMs = 0;
        const uint64_t now = GetTickCount64();
        if (now - lastRejectLogMs >= 2000)
        {
            LOG("M3: rejected extreme two-hand aim (ray agreement %.2f); "
                "using right controller", aim.rejectedAgreement);
            lastRejectLogMs = now;
        }
    }
    if (!aim.valid)
        return false;
    return true;
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
bool VR_GetRawRightControllerPose(float outQuat[4], float outPos[3])
{
    if (!g_headCsInit)
        return false;
    EnterCriticalSection(&g_headCs);
    const bool ok = g_rawRightAimPoseValid;
    if (ok)
    {
        outQuat[0] = g_rawRightAimPose.orientation.x;
        outQuat[1] = g_rawRightAimPose.orientation.y;
        outQuat[2] = g_rawRightAimPose.orientation.z;
        outQuat[3] = g_rawRightAimPose.orientation.w;
        outPos[0] = g_rawRightAimPose.position.x;
        outPos[1] = g_rawRightAimPose.position.y;
        outPos[2] = g_rawRightAimPose.position.z;
    }
    LeaveCriticalSection(&g_headCs);
    return ok;
}

bool VR_GetRawLeftControllerPose(float outQuat[4], float outPos[3])
{
    if (!g_headCsInit)
        return false;
    EnterCriticalSection(&g_headCs);
    const bool ok = g_rawLeftAimPoseValid;
    if (ok)
    {
        outQuat[0] = g_rawLeftAimPose.orientation.x;
        outQuat[1] = g_rawLeftAimPose.orientation.y;
        outQuat[2] = g_rawLeftAimPose.orientation.z;
        outQuat[3] = g_rawLeftAimPose.orientation.w;
        outPos[0] = g_rawLeftAimPose.position.x;
        outPos[1] = g_rawLeftAimPose.position.y;
        outPos[2] = g_rawLeftAimPose.position.z;
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
