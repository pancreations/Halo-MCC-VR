#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <vector>
#include <array>
#include <string>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "vr.h"
#include "menu.h"
#include "game.h"
#include "d3d11_hook.h"
#include "d3d_state.h"
#include "../common/log.h"
#include "../common/config.h"

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
    XrPath g_leftHandPath = XR_NULL_PATH;
    XrPath g_rightHandPath = XR_NULL_PATH;
    XrEnvironmentBlendMode g_blendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    bool g_sessionRunning = false;
    XrSessionState g_sessionState = XR_SESSION_STATE_UNKNOWN;

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

    // M2 sun-shaft neutralization. The engine computes the sun's screen
    // position once per frame from a single camera, so its screen-space
    // radial blur (sun shafts / god rays) is only correct for one of the two
    // eye rasters — the other eye shows streaks radiating from a shifted sun
    // (the "ghost follows whichever eye renders first" result). The shaft
    // chain starts at a small square two-channel (R16G16) occlusion target;
    // while stereo_sun_shafts is off we clear the game's copy to black and
    // divert the pass's writes into these dummies, so the later composite
    // adds nothing. Mono rendering is never touched.
    struct SunShaftDummy
    {
        ID3D11Texture2D* tex = nullptr;
        ID3D11RenderTargetView* rtv = nullptr;
        UINT width = 0, height = 0;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    };
    SunShaftDummy g_sunShaftDummies[4];

    bool IsR16G16Family(DXGI_FORMAT f)
    {
        switch (f)
        {
        case DXGI_FORMAT_R16G16_TYPELESS:
        case DXGI_FORMAT_R16G16_FLOAT:
        case DXGI_FORMAT_R16G16_UNORM:
        case DXGI_FORMAT_R16G16_UINT:
        case DXGI_FORMAT_R16G16_SNORM:
        case DXGI_FORMAT_R16G16_SINT:
            return true;
        default:
            return false;
        }
    }

    ID3D11RenderTargetView* GetSunShaftDummyRtv(const D3D11_TEXTURE2D_DESC& desc,
                                                ID3D11RenderTargetView* gameRtv)
    {
        for (SunShaftDummy& d : g_sunShaftDummies)
            if (d.rtv && d.width == desc.Width && d.height == desc.Height &&
                d.format == desc.Format)
                return d.rtv;
        for (SunShaftDummy& d : g_sunShaftDummies)
        {
            if (d.rtv)
                continue;
            D3D11_TEXTURE2D_DESC dd = desc;
            dd.MipLevels = 1;
            dd.ArraySize = 1;
            dd.CPUAccessFlags = 0;
            dd.MiscFlags = 0;
            dd.Usage = D3D11_USAGE_DEFAULT;
            dd.BindFlags = D3D11_BIND_RENDER_TARGET;
            if (FAILED(g_device->CreateTexture2D(&dd, nullptr, &d.tex)))
                return nullptr; // fail open: the effect just renders normally
            // Same typed view format as the game's own view (handles typeless).
            D3D11_RENDER_TARGET_VIEW_DESC vd{};
            gameRtv->GetDesc(&vd);
            if (FAILED(g_device->CreateRenderTargetView(d.tex, &vd, &d.rtv)))
            {
                d.tex->Release();
                d.tex = nullptr;
                return nullptr;
            }
            d.width = desc.Width;
            d.height = desc.Height;
            d.format = desc.Format;
            return d.rtv;
        }
        return nullptr;
    }

    void ReleaseSunShaftDummies()
    {
        for (SunShaftDummy& d : g_sunShaftDummies)
        {
            if (d.rtv) d.rtv->Release();
            if (d.tex) d.tex->Release();
            d = {};
        }
    }

    // M2 post-process isolation, v2 (cross-pass discovery). Halo's bloom
    // persistence carries last frame's glow through a PING-PONGED buffer
    // pair: each frame one buffer is read while the other is written, and the
    // roles flip on the next frame. In stereo the first-rendered eye blends
    // against the OTHER eye's previous glow — offset by the eye separation —
    // and the feedback repeats that offset into the trailing after-images the
    // user sees on bright sparkles (present even with the head still; follows
    // whichever eye renders first). v1 discovery looked for read-then-write
    // within a single pass, which a ping-pong pair NEVER does (the buffer
    // read this frame is written next frame), so it found nothing. v2 keeps
    // one record per texture across passes and promotes anything that is (a)
    // read in some pass before being written in that same pass — i.e. it
    // carried data INTO a pass — and (b) also written inside some eye pass.
    // Targets fed by passes outside the eye renders never satisfy (b), which
    // is exactly what made the old format-based full-res isolation harmful.
    constexpr unsigned kHistoryMax = 32;
    ID3D11Texture2D* g_histShared[kHistoryMax] = {};   // game resources, AddRef'd
    D3D11_TEXTURE2D_DESC g_histDescs[kHistoryMax] = {};
    ID3D11Texture2D* g_histShadow[2][kHistoryMax] = {};
    bool g_histValid[2][kHistoryMax] = {};
    unsigned g_histCount = 0;

    struct HistoryCandidate
    {
        ID3D11Texture2D* texture = nullptr; // AddRef'd while discovery runs
        D3D11_TEXTURE2D_DESC desc{};
        bool readSeen = false;          // sampled inside an eye pass
        bool writeSeen = false;         // RTV-bound inside an eye pass
        bool readCarriedIn = false;     // read in a pass it had not yet written
        bool writtenOutOfPass = false;  // RTV-bound at frame level (outside passes)
        bool blanked = false;           // already promoted to the blank list
        ID3D11RenderTargetView* outRtv = nullptr; // AddRef'd view from the out-of-pass bind
        unsigned lastWritePass = ~0u;
    };
    constexpr unsigned kHistoryCandidateMax = 96;
    HistoryCandidate g_historyCandidates[kHistoryCandidateMax]{};
    unsigned g_historyCandidateCount = 0;
    bool g_historyDiscoveryActive = true;
    unsigned g_historyDiscoveryPasses = 0; // eye passes observed this window
    unsigned g_historyPassId = 0;          // increments every eye pass

    // Frame-level shared history: written once per frame OUTSIDE the eye
    // passes (from the last-rendered eye's data) and sampled inside both eye
    // passes. Per-eye copies cannot fix that — there is only one write stream
    // — so in stereo these targets are cleared to black before the eyes read
    // them. Costs the effect, removes the cross-eye trails.
    constexpr unsigned kBlankMax = 16;
    ID3D11RenderTargetView* g_blankRtvs[kBlankMax] = {};
    D3D11_TEXTURE2D_DESC g_blankDescs[kBlankMax] = {};
    unsigned g_blankCount = 0;

    // THE GHOST ROOT CAUSE (observed via the CopyResource probe): between the
    // eye passes the engine snapshots the full-res scene into a sampleable
    // texture — its "previous frame" for temporal effects. Both eyes sample
    // that one snapshot next frame, but it was made from whichever eye
    // rendered last, so the first eye blends against the other eye's image
    // (the trailing after-images on bright pixels). Fix: learn the (src,dst)
    // pair from the copy hook, keep a per-eye copy of the snapshot captured
    // after each eye's own render, and substitute it into the game's dst
    // right before that eye renders again. Each eye then always sees its own
    // previous frame. Three texture copies per frame — no third render.
    struct HistoryPair
    {
        ID3D11Texture2D* src = nullptr;   // game texture the engine snapshots
        ID3D11Texture2D* dst = nullptr;   // the engine's snapshot destination
        ID3D11Texture2D* shadow[2] = {};  // per-eye snapshot versions
        bool valid[2] = {};
        bool srcIsRedirectedScene = false; // src is the final scene we redirect
                                           // per eye; use the eye caches instead
    };
    HistoryPair g_histPairs[4];
    unsigned g_histPairCount = 0;

    void ReleaseHistoryPairs()
    {
        for (unsigned i = 0; i < g_histPairCount; ++i)
        {
            HistoryPair& p = g_histPairs[i];
            if (p.src) p.src->Release();
            if (p.dst) p.dst->Release();
            for (int e = 0; e < 2; ++e)
                if (p.shadow[e]) p.shadow[e]->Release();
            p = {};
        }
        g_histPairCount = 0;
    }

    bool IsTrackedHistory(ID3D11Texture2D* texture)
    {
        for (unsigned i = 0; i < g_histCount; ++i)
            if (g_histShared[i] == texture)
                return true;
        return false;
    }

    void ReleaseHistoryCandidates()
    {
        for (unsigned i = 0; i < g_historyCandidateCount; ++i)
        {
            if (g_historyCandidates[i].texture)
                g_historyCandidates[i].texture->Release();
            if (g_historyCandidates[i].outRtv)
                g_historyCandidates[i].outRtv->Release();
            g_historyCandidates[i] = {};
        }
        g_historyCandidateCount = 0;
    }

    void ReleaseBlankTargets()
    {
        for (unsigned i = 0; i < g_blankCount; ++i)
        {
            if (g_blankRtvs[i]) g_blankRtvs[i]->Release();
            g_blankRtvs[i] = nullptr;
        }
        g_blankCount = 0;
    }

    // Shared filter + find-or-add for both record paths. Width/height > 8
    // skips the endlessly re-allocated 1x1 exposure scratch textures that
    // flooded (and capped out) the v1 candidate list. The UAV exclusion skips
    // the final scene target, which is already redirected per eye.
    HistoryCandidate* CandidateFor(ID3D11Resource* resource)
    {
        ID3D11Texture2D* texture = nullptr;
        if (FAILED(resource->QueryInterface(__uuidof(ID3D11Texture2D),
                                            reinterpret_cast<void**>(&texture))) || !texture)
            return nullptr;
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        const UINT needed = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if ((desc.BindFlags & needed) != needed || desc.SampleDesc.Count != 1 ||
            desc.ArraySize != 1 || desc.Width <= 8 || desc.Height <= 8 ||
            (desc.BindFlags & D3D11_BIND_UNORDERED_ACCESS) ||
            texture == g_eyeCache[0] || texture == g_eyeCache[1] ||
            IsTrackedHistory(texture))
        {
            texture->Release();
            return nullptr;
        }
        for (unsigned i = 0; i < g_historyCandidateCount; ++i)
        {
            if (g_historyCandidates[i].texture == texture)
            {
                texture->Release();
                return &g_historyCandidates[i];
            }
        }
        if (g_historyCandidateCount >= kHistoryCandidateMax)
        {
            texture->Release();
            return nullptr;
        }
        HistoryCandidate& c = g_historyCandidates[g_historyCandidateCount++];
        c = {};
        c.texture = texture; // keep the QueryInterface reference
        c.desc = desc;
        c.lastWritePass = ~0u;
        return &c;
    }

    void RecordHistoryUse(ID3D11Resource* resource, bool isRead)
    {
        if (!g_historyDiscoveryActive || !resource || !g_config.per_eye_history)
            return;
        HistoryCandidate* c = CandidateFor(resource);
        if (!c)
            return;
        if (isRead)
        {
            c->readSeen = true;
            if (c->lastWritePass != g_historyPassId)
                c->readCarriedIn = true;
        }
        else
        {
            c->writeSeen = true;
            c->lastWritePass = g_historyPassId;
        }
    }

    unsigned PromoteCrossPassHistory()
    {
        unsigned promoted = 0;
        for (unsigned i = 0; i < g_historyCandidateCount && g_histCount < kHistoryMax; ++i)
        {
            HistoryCandidate& c = g_historyCandidates[i];
            if (!c.texture || !c.readSeen || !c.writeSeen || !c.readCarriedIn ||
                IsTrackedHistory(c.texture))
                continue;
            c.texture->AddRef();
            g_histShared[g_histCount] = c.texture;
            g_histDescs[g_histCount] = c.desc;
            ++g_histCount;
            ++promoted;
            LOG("M2: isolating cross-pass history target %ux%u format=%u per eye (%u tracked)",
                c.desc.Width, c.desc.Height, (unsigned)c.desc.Format, g_histCount);
        }
        return promoted;
    }

    void PromoteFrameLevelHistory()
    {
        for (unsigned i = 0; i < g_historyCandidateCount && g_blankCount < kBlankMax; ++i)
        {
            HistoryCandidate& c = g_historyCandidates[i];
            // Written only at frame level, sampled inside eye passes: shared
            // per-frame history. (Anything also written IN pass is handled by
            // the cross-pass rule instead.)
            if (!c.texture || c.blanked || !c.readSeen || c.writeSeen ||
                !c.writtenOutOfPass || !c.outRtv)
                continue;
            c.blanked = true;
            c.outRtv->AddRef();
            g_blankRtvs[g_blankCount] = c.outRtv;
            g_blankDescs[g_blankCount] = c.desc;
            ++g_blankCount;
            LOG("M2: stereo-blanking frame-level history target %ux%u format=%u "
                "(written outside eye passes, sampled inside; %u blanked)",
                c.desc.Width, c.desc.Height, (unsigned)c.desc.Format, g_blankCount);
        }
    }

    void ReleaseEyeHistory()
    {
        ReleaseHistoryCandidates();
        ReleaseBlankTargets();
        for (unsigned i = 0; i < g_histCount; ++i)
        {
            if (g_histShared[i]) g_histShared[i]->Release();
            g_histShared[i] = nullptr;
            for (int e = 0; e < 2; ++e)
            {
                if (g_histShadow[e][i]) g_histShadow[e][i]->Release();
                g_histShadow[e][i] = nullptr;
                g_histValid[e][i] = false;
            }
        }
        g_histCount = 0;
        g_historyDiscoveryActive = true;
        g_historyDiscoveryPasses = 0;
        D3D11_SetHistoryDiscovery(true);
    }
    std::atomic<bool> g_stereoEnabled{false};
    int g_renderEye = 0;
    bool g_eyeHasImage[2] = {false, false};
    bool g_stereoValidationDone = false;
    std::atomic<int> g_rasterEye{-1};
    bool g_rasterRedirected[2] = {false, false};
    struct DrawTraceEntry
    {
        unsigned sequence;
        unsigned kind;
        unsigned count;
        const void* vs;
        const void* ps;
    };
    constexpr unsigned kDrawTraceCapacity = 2048;
    DrawTraceEntry g_drawTrace[kDrawTraceCapacity]{};
    std::atomic<unsigned> g_drawTraceSequence{0};
    std::atomic<bool> g_drawTraceDumped{false};
    IDXGISwapChain* g_gameSwapchain = nullptr; // borrowed; owned by the game

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

    // Status shown in the menu (only touched on the render thread)
    VrStatus g_status{};
    LARGE_INTEGER g_fpsTimer{};
    int g_fpsFrames = 0;

    // ---------------------------------------------------------------- utils

    const char* XrStr(XrResult r)
    {
        static char buf[XR_MAX_RESULT_STRING_SIZE];
        if (g_instance != XR_NULL_HANDLE && XR_SUCCEEDED(xrResultToString(g_instance, r, buf)))
            return buf;
        snprintf(buf, sizeof(buf), "XrResult(%d)", (int)r);
        return buf;
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
                 "Halo 3 VR mod could not start VR:\n\n%s\n\n"
                 "The game will keep running flat on the monitor.\n"
                 "Details are in halo3xr.log next to the mod DLL.", msg);
        CreateThread(nullptr, 0,
                     [](LPVOID p) -> DWORD {
                         MessageBoxA(nullptr, (const char*)p, "Halo 3 VR mod", MB_OK | MB_ICONWARNING | MB_TOPMOST);
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
        if (g_blitVs)
            return true;

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
    void Blit(ID3D11Texture2D* src, const D3D11_TEXTURE2D_DESC& srcDesc,
              ID3D11Texture2D* dst, uint32_t dstW, uint32_t dstH, ID3D11RenderTargetView* dstRtv)
    {
        const bool sameSize = srcDesc.Width == dstW && srcDesc.Height == dstH;
        const bool sameFamily = FormatFamily(srcDesc.Format) == FormatFamily((DXGI_FORMAT)g_xrFormat);
        if (sameSize && sameFamily && srcDesc.SampleDesc.Count <= 1)
        {
            g_context->CopyResource(dst, src);
            return;
        }

        if (!EnsureBlitPipeline())
            return;

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
                    return;
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
        ID3D11PixelShader* ps =
            (!IsSrgb(srcDesc.Format) && IsSrgb((DXGI_FORMAT)g_xrFormat)) ? g_blitPsLinearize : g_blitPsPass;

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
        g_context->PSSetShader(ps, nullptr, 0);
        g_context->PSSetShaderResources(0, 1, &srv);
        g_context->PSSetSamplers(0, 1, &g_blitSampler);
        g_context->Draw(3, 0);

        backup.Restore(g_context);
    }

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
        xrEnumerateSwapchainImages(chain, 0, &count, nullptr);
        std::vector<XrSwapchainImageD3D11KHR> xrImages(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        r = xrEnumerateSwapchainImages(chain, count, &count,
                                       reinterpret_cast<XrSwapchainImageBaseHeader*>(xrImages.data()));
        if (XR_FAILED(r))
        {
            LOG("xrEnumerateSwapchainImages(%s) failed: %s", what, XrStr(r));
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
                    xrEndSession(g_session);
                    g_sessionRunning = false;
                }
                else if (sc.state == XR_SESSION_STATE_EXITING || sc.state == XR_SESSION_STATE_LOSS_PENDING)
                {
                    g_sessionRunning = false;
                    Fail("The VR runtime ended the session (headset off / SteamVR closed?)");
                }
                break;
            }
            case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
                g_sessionRunning = false;
                Fail("The OpenXR runtime is shutting down");
                break;
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

    // Store the current head pose for the game camera hook to read. Called
    // once per frame on the render thread with the frame's predicted time.
    void CaptureHeadPose(XrTime time)
    {
        XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
        if (XR_FAILED(xrLocateSpace(g_viewSpace, g_localSpace, time, &loc)))
            return;
        constexpr XrSpaceLocationFlags need =
            XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
        if ((loc.locationFlags & need) != need)
            return;
        EnterCriticalSection(&g_headCs);
        g_headPose = loc.pose;
        g_headPoseValid = true;
        LeaveCriticalSection(&g_headCs);
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

    // ---------------------------------------------------------------- init

    // Part 1 (background thread): create the OpenXR instance and find the
    // headset. No D3D device needed here, so it can run while the game loads.
    bool InitInstance()
    {
        const char* ext = XR_KHR_D3D11_ENABLE_EXTENSION_NAME;
        XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
        strcpy_s(ici.applicationInfo.applicationName, "halo3xr");
        ici.applicationInfo.applicationVersion = 1;
        strcpy_s(ici.applicationInfo.engineName, "halo3xr");
        ici.applicationInfo.apiVersion = XR_API_VERSION_1_0;
        ici.enabledExtensionCount = 1;
        ici.enabledExtensionNames = &ext;
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
    XrAction g_actMenu = XR_NULL_HANDLE;
    VrPadState g_padState{};

    bool CreateControllerActions()
    {
        XrActionSetCreateInfo setInfo{XR_TYPE_ACTION_SET_CREATE_INFO};
        strcpy_s(setInfo.actionSetName, "gameplay");
        strcpy_s(setInfo.localizedActionSetName, "Halo 3 VR Gameplay");
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
        };
        const Bind simple[] = {
            {g_rightAimAction, "/user/hand/right/input/grip/pose"},
            {g_leftAimAction, "/user/hand/left/input/grip/pose"},
            {g_actTrigR,  "/user/hand/right/input/select/click"},
            {g_actMenu,   "/user/hand/left/input/menu/click"},
        };
        unsigned accepted = 0;
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
            valid = (location.locationFlags & required) == required;
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
                leftValid = (leftLocation.locationFlags & required) == required;
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
        EnterCriticalSection(&g_headCs);
        g_padState = pad;
        LeaveCriticalSection(&g_headCs);
        static bool padLogged = false;
        if (pad.valid && !padLogged)
        {
            LOG("M3: controller inputs active (sticks/buttons feeding the virtual gamepad)");
            padLogged = true;
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

    void Frame(IDXGISwapChain* sc)
    {
        g_frameNo++;
        FLog("xrWaitFrame");
        XrFrameWaitInfo wi{XR_TYPE_FRAME_WAIT_INFO};
        XrFrameState fs{XR_TYPE_FRAME_STATE};
        if (XR_FAILED(xrWaitFrame(g_session, &wi, &fs)))
            return;
        FLog("xrBeginFrame");
        XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO};
        if (XR_FAILED(xrBeginFrame(g_session, &bi)))
            return;

        CaptureRightControllerPose(fs.predictedDisplayTime);

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

        XrCompositionLayerQuad screenQuad, menuQuad;
        XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        std::vector<XrCompositionLayerProjectionView> projectionViews;
        std::vector<XrCompositionLayerBaseHeader*> layers;

        // Build the descriptors every frame from the predicted eye poses/FOV.
        // A later M2 render hook only needs to fill/release both swapchain
        // images and replace the mono quad in `layers` with `projection`.
        if (viewsValid && g_stereoChain != XR_NULL_HANDLE && locatedViewCount == 2)
        {
            projection.space = g_localSpace;
            projectionViews.assign(locatedViewCount, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW});

            // RenderViewHook rotates each eye's raster camera by the true
            // per-eye cant (VR_GetEyeCantQuat), so the rendered images really
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
            Game_GetRenderHalfFov(haloHalfX, haloHalfY);
            for (uint32_t i = 0; i < locatedViewCount; ++i)
            {
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
                CaptureHeadPose(fs.predictedDisplayTime);
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
                const bool stereo = g_stereoEnabled.load() && viewsValid &&
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
                        layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&projection));
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
                        const bool headLock = g_screenFollow.load() && Game_IsHeadTracking();
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
                                                stereo || (g_screenFollow.load() && Game_IsHeadTracking()));
                            layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&menuQuad));
                        }
                    }
                }
                backbuffer->Release();
            }
        }

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

        FLog("xrEndFrame");
        XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
        ei.displayTime = fs.predictedDisplayTime;
        ei.environmentBlendMode = g_blendMode;
        ei.layerCount = (uint32_t)layers.size();
        ei.layers = layers.data();
        XrResult r = xrEndFrame(g_session, &ei);
        if (XR_FAILED(r))
        {
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

void VR_OnPresent(IDXGISwapChain* sc)
{
    g_gameSwapchain = sc;
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

    PollEvents();
    if (g_state != State::Ready || !g_sessionRunning)
        return;

    Frame(sc);
}

void VR_OnResizeBuffers(IDXGISwapChain*)
{
    // The game is about to destroy its backbuffer; anything of ours that
    // references it must go first or the resize fails. The tracked history
    // targets are resolution-dependent too — drop and re-learn them.
    ReleaseSourceViews();
    ReleaseEyeHistory();
    ReleaseHistoryPairs();    // snapshot textures are resolution-dependent
    ReleaseSunShaftDummies(); // sized to the old resolution; recreated on demand
}

void VR_RequestRecenter()
{
    g_haveCenter = false;
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

void VR_CaptureRenderedEye(int eye)
{
    if (eye < 0 || eye > 1)
        return;
    if (g_rasterRedirected[eye] && g_eyeCache[eye])
    {
        g_eyeHasImage[eye] = true;
        if (eye == 0 && !g_drawTraceDumped.exchange(true))
        {
            const unsigned total = g_drawTraceSequence.load();
            const unsigned begin = total > kDrawTraceCapacity ? total - kDrawTraceCapacity : 0;
            LOG("M3 DRAW RUNS begin total=%u retained=%u", total, total - begin);
            unsigned sequence = begin;
            while (sequence < total)
            {
                const DrawTraceEntry& e = g_drawTrace[sequence % kDrawTraceCapacity];
                if (e.sequence != sequence) { ++sequence; continue; }
                const unsigned runBegin = sequence;
                unsigned minCount = e.count, maxCount = e.count;
                unsigned long long totalCount = e.count;
                ++sequence;
                while (sequence < total)
                {
                    const DrawTraceEntry& n = g_drawTrace[sequence % kDrawTraceCapacity];
                    if (n.sequence != sequence || n.kind != e.kind || n.vs != e.vs || n.ps != e.ps)
                        break;
                    if (n.count < minCount) minCount = n.count;
                    if (n.count > maxCount) maxCount = n.count;
                    totalCount += n.count;
                    ++sequence;
                }
                LOG("M3 DRAW RUN seq=%u-%u draws=%u type=%s count=%u..%u total=%llu vs=%p ps=%p",
                    runBegin, sequence - 1, sequence - runBegin, e.kind ? "indexed" : "draw",
                    minCount, maxCount, totalCount, e.vs, e.ps);
            }
            LOG("M3 DRAW RUNS end");
        }
        return;
    }
    static bool loggedMissing = false;
    if (!loggedMissing)
    {
        LOG("M2 RASTER: no internal scene-color RTV redirect occurred; refusing fake eye copy");
        loggedMissing = true;
    }
}

void VR_BeginRasterEye(int eye)
{
    if (eye < 0 || eye > 1 || !g_gameSwapchain || !g_device)
        return;
    ID3D11Texture2D* backbuffer = nullptr;
    if (FAILED(g_gameSwapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backbuffer)) || !backbuffer)
        return;
    // Eye caches are created lazily when Halo binds its final scene-color RTV.
    // That RTV's typed view format (not the swapchain resource format) controls
    // the required sRGB conversion.
    g_rasterRedirected[eye] = false;
    g_drawTraceSequence = 0;
    g_rasterEye = eye;
    ++g_historyPassId; // candidates persist across passes; only the id moves
    // Ghost probe, log-only: textures ALREADY bound as pixel-shader inputs
    // when the pass starts. Bind-call censuses can never see these (bound
    // once, read forever) — a render-target-capable texture sitting here is a
    // prime suspect for the shared state the warm-up render exists to flush.
    static std::atomic<unsigned> snapshotPasses{0};
    if (g_context && snapshotPasses.fetch_add(1) < 600)
    {
        ID3D11ShaderResourceView* srvs[16] = {};
        g_context->PSGetShaderResources(0, 16, srvs);
        for (UINT i = 0; i < 16; ++i)
        {
            if (!srvs[i])
                continue;
            ID3D11Resource* resource = nullptr;
            srvs[i]->GetResource(&resource);
            if (resource)
            {
                ID3D11Texture2D* tex = nullptr;
                if (SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Texture2D),
                                                       reinterpret_cast<void**>(&tex))))
                {
                    D3D11_TEXTURE2D_DESC d{};
                    tex->GetDesc(&d);
                    if (d.BindFlags & D3D11_BIND_RENDER_TARGET)
                    {
                        static ID3D11Resource* seen[64]{};
                        static unsigned seenCount = 0;
                        bool known = false;
                        for (unsigned k = 0; k < seenCount; ++k)
                            if (seen[k] == resource) { known = true; break; }
                        if (!known && seenCount < 64)
                        {
                            seen[seenCount++] = resource;
                            LOG("M2 PASS-SRV eye=%d slot=%u tex=%p %ux%u fmt=%u bind=0x%X",
                                eye, i, (void*)resource, d.Width, d.Height,
                                (unsigned)d.Format, d.BindFlags);
                        }
                    }
                    tex->Release();
                }
                resource->Release();
            }
            srvs[i]->Release();
        }
    }
    if (g_config.per_eye_history)
    {
        // Swap in this eye's private copy of the temporal history targets so
        // the pass blends against its own previous frame, not the other eye's.
        for (unsigned i = 0; i < g_histCount; ++i)
            if (g_histValid[eye][i] && g_histShadow[eye][i] && g_histShared[i])
                g_context->CopyResource(g_histShared[i], g_histShadow[eye][i]);
        // Frame-level shared history has a single write stream (built from
        // the last-rendered eye); blank it so neither eye samples the other's
        // leftovers. The game refills it after the eye passes each frame.
        const float black[4] = {0, 0, 0, 0};
        for (unsigned i = 0; i < g_blankCount; ++i)
            if (g_blankRtvs[i])
                g_context->ClearRenderTargetView(g_blankRtvs[i], black);
    }
    // THE GHOST FIX: before this eye renders, replace the engine's frame
    // snapshot with THIS eye's own previous-frame version, so its temporal
    // effects blend against the same viewpoint instead of the other eye's.
    for (unsigned i = 0; i < g_histPairCount; ++i)
    {
        HistoryPair& p = g_histPairs[i];
        if (p.valid[eye] && p.shadow[eye] && p.dst)
            g_context->CopyResource(p.dst, p.shadow[eye]);
    }
    backbuffer->Release();
}

void VR_RecordShaderResourceReads(UINT count, ID3D11ShaderResourceView* const* srvs)
{
    if (!g_historyDiscoveryActive || g_rasterEye.load() < 0 || !srvs)
        return;
    for (UINT i = 0; i < count; ++i)
    {
        if (!srvs[i])
            continue;
        ID3D11Resource* resource = nullptr;
        srvs[i]->GetResource(&resource);
        if (resource)
        {
            RecordHistoryUse(resource, true);
            resource->Release();
        }
    }
}

void VR_RecordCopy(ID3D11Resource* dst, ID3D11Resource* src, const char* what)
{
    // Ghost probe + fix. Every earlier census watched BIND calls, so a
    // resource moved by a GPU copy was invisible. Log each unique (dst, src)
    // texture pair seen while stereo runs, tagged with the eye (-1 = between
    // passes), and LEARN the frame-level full-res scene snapshot pairs the
    // per-eye substitution needs (see HistoryPair).
    if (!g_stereoEnabled.load() || !dst || !src)
        return;
    // Learn snapshot pairs: frame-level (between passes), both full-res
    // textures, destination sampleable. Our own copies never qualify (they
    // run inside passes or into bind-0 staging).
    if (g_rasterEye.load() < 0 && g_histPairCount < 4 && g_eyeCacheDesc.Width != 0)
    {
        ID3D11Texture2D* dt = nullptr;
        ID3D11Texture2D* st = nullptr;
        if (SUCCEEDED(dst->QueryInterface(__uuidof(ID3D11Texture2D),
                                          reinterpret_cast<void**>(&dt))) &&
            SUCCEEDED(src->QueryInterface(__uuidof(ID3D11Texture2D),
                                          reinterpret_cast<void**>(&st))))
        {
            D3D11_TEXTURE2D_DESC dd{}, sd{};
            dt->GetDesc(&dd);
            st->GetDesc(&sd);
            bool known = false;
            for (unsigned i = 0; i < g_histPairCount; ++i)
                if (g_histPairs[i].dst == dt)
                    known = true;
            const bool ours = dt == g_eyeCache[0] || dt == g_eyeCache[1] ||
                              st == g_eyeCache[0] || st == g_eyeCache[1];
            if (!known && !ours &&
                dd.Width == g_eyeCacheDesc.Width && dd.Height == g_eyeCacheDesc.Height &&
                sd.Width == dd.Width && sd.Height == dd.Height &&
                (dd.BindFlags & D3D11_BIND_SHADER_RESOURCE) &&
                dd.SampleDesc.Count == 1 && dd.ArraySize == 1)
            {
                HistoryPair& p = g_histPairs[g_histPairCount++];
                dt->AddRef();
                st->AddRef();
                p.dst = dt;
                p.src = st;
                p.srcIsRedirectedScene =
                    sd.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS &&
                    (sd.BindFlags & D3D11_BIND_UNORDERED_ACCESS);
                LOG("M2: per-eye history snapshot armed: src=%p fmt=%u%s -> dst=%p fmt=%u (%u pairs)",
                    (void*)st, (unsigned)sd.Format,
                    p.srcIsRedirectedScene ? " (redirected scene; using eye caches)" : "",
                    (void*)dt, (unsigned)dd.Format, g_histPairCount);
            }
        }
        if (dt) dt->Release();
        if (st) st->Release();
    }
    struct Pair { ID3D11Resource* dst; ID3D11Resource* src; };
    static Pair seen[96];
    static std::atomic<unsigned> seenCount{0};
    const unsigned n = seenCount.load();
    for (unsigned i = 0; i < n && i < 96; ++i)
        if (seen[i].dst == dst && seen[i].src == src)
            return;
    const unsigned slot = seenCount.fetch_add(1);
    if (slot >= 96)
        return;
    seen[slot] = {dst, src};
    D3D11_TEXTURE2D_DESC dd{}, sd{};
    ID3D11Texture2D* dt = nullptr;
    ID3D11Texture2D* st = nullptr;
    if (SUCCEEDED(dst->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&dt))))
    {
        dt->GetDesc(&dd);
        dt->Release();
    }
    if (SUCCEEDED(src->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&st))))
    {
        st->GetDesc(&sd);
        st->Release();
    }
    LOG("M2 COPY eye=%d %s: src=%p %ux%u fmt=%u bind=0x%X -> dst=%p %ux%u fmt=%u bind=0x%X",
        g_rasterEye.load(), what,
        (void*)src, sd.Width, sd.Height, (unsigned)sd.Format, sd.BindFlags,
        (void*)dst, dd.Width, dd.Height, (unsigned)dd.Format, dd.BindFlags);
}

void VR_RecordFrameRtv(UINT count, ID3D11RenderTargetView* const* rtvs)
{
    // Only interesting while discovery runs, stereo is on, and we are OUTSIDE
    // the eye passes: these are the frame-level writes the eye-pass hooks are
    // blind to. In-pass binds go through VR_RedirectRenderTargets instead.
    if (!g_historyDiscoveryActive || !g_config.per_eye_history || !rtvs ||
        g_rasterEye.load() >= 0 || !g_stereoEnabled.load())
        return;
    for (UINT i = 0; i < count && i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
    {
        if (!rtvs[i])
            continue;
        ID3D11Resource* resource = nullptr;
        rtvs[i]->GetResource(&resource);
        if (!resource)
            continue;
        if (HistoryCandidate* c = CandidateFor(resource))
        {
            c->writtenOutOfPass = true;
            if (!c->outRtv)
            {
                c->outRtv = rtvs[i];
                c->outRtv->AddRef();
            }
        }
        resource->Release();
    }
}

void VR_RecordDraw(unsigned kind, unsigned count, const void* vertexShader,
                   const void* pixelShader)
{
    if (g_rasterEye.load() < 0 || g_drawTraceDumped.load())
        return;
    const unsigned sequence = g_drawTraceSequence.fetch_add(1);
    g_drawTrace[sequence % kDrawTraceCapacity] =
        {sequence, kind, count, vertexShader, pixelShader};
}

void VR_EndRasterEye()
{
    // Promote any newly identified history, then save this eye's copies of
    // every tracked target before the other eye (or next frame) overwrites
    // them. A ping-pong pair only reveals its read side a frame after its
    // write side, so discovery stays open for a fixed window (~2 s of
    // stereo) instead of closing at the first find.
    const int eye = g_rasterEye.load();
    if (g_historyDiscoveryActive && g_config.per_eye_history)
    {
        PromoteCrossPassHistory();
        PromoteFrameLevelHistory();
        if (++g_historyDiscoveryPasses >= 480)
        {
            g_historyDiscoveryActive = false;
            D3D11_SetHistoryDiscovery(false);
            ReleaseHistoryCandidates();
            LOG("M2: history discovery window closed; %u per-eye cop%s, %u frame-level blank(s)",
                g_histCount, g_histCount == 1 ? "y" : "ies", g_blankCount);
        }
    }
    if (eye >= 0 && eye < 2 && g_device && g_context && g_config.per_eye_history)
    {
        for (unsigned i = 0; i < g_histCount; ++i)
        {
            if (!g_histShared[i])
                continue;
            if (!g_histShadow[eye][i])
            {
                D3D11_TEXTURE2D_DESC d = g_histDescs[i];
                d.BindFlags = 0;
                d.MiscFlags = 0;
                d.CPUAccessFlags = 0;
                d.Usage = D3D11_USAGE_DEFAULT;
                if (FAILED(g_device->CreateTexture2D(&d, nullptr, &g_histShadow[eye][i])))
                    g_histShadow[eye][i] = nullptr;
            }
            if (g_histShadow[eye][i])
            {
                g_context->CopyResource(g_histShadow[eye][i], g_histShared[i]);
                g_histValid[eye][i] = true;
            }
        }
    }
    // THE GHOST FIX, capture side: save this eye's fresh scene as its own
    // snapshot for next frame. For the redirected final scene the fresh
    // content lives in our eye cache, not the game's (stale) texture.
    if (eye >= 0 && eye < 2 && g_device && g_context)
    {
        for (unsigned i = 0; i < g_histPairCount; ++i)
        {
            HistoryPair& p = g_histPairs[i];
            ID3D11Texture2D* source =
                p.srcIsRedirectedScene ? g_eyeCache[eye] : p.src;
            if (!source || !p.src)
                continue;
            if (!p.shadow[eye])
            {
                D3D11_TEXTURE2D_DESC d{};
                p.src->GetDesc(&d);
                d.BindFlags = 0;
                d.MiscFlags = 0;
                d.CPUAccessFlags = 0;
                d.Usage = D3D11_USAGE_DEFAULT;
                d.MipLevels = 1;
                d.ArraySize = 1;
                if (FAILED(g_device->CreateTexture2D(&d, nullptr, &p.shadow[eye])))
                    p.shadow[eye] = nullptr;
            }
            if (p.shadow[eye])
            {
                g_context->CopyResource(p.shadow[eye], source);
                p.valid[eye] = true;
            }
        }
    }
    g_rasterEye = -1;
}

void VR_RecordUavCensus(UINT count, ID3D11UnorderedAccessView* const* uavs)
{
    // The eye-order test proved the left-eye ghosting comes from shared
    // temporal history, and it is not in any RTV we can see — so it is being
    // written through a UAV (compute or pixel-shader). Log every unique UAV
    // resource bound during an eye pass; the persistent full-res candidates
    // in this census are the isolation targets for the next attempt.
    const int eye = g_rasterEye.load();
    if (eye < 0 || !uavs)
        return;
    for (UINT i = 0; i < count; ++i)
    {
        if (!uavs[i])
            continue;
        ID3D11Resource* resource = nullptr;
        uavs[i]->GetResource(&resource);
        if (!resource)
            continue;
        static ID3D11Resource* seen[128]{};
        static unsigned seenCount = 0;
        bool known = false;
        for (unsigned n = 0; n < seenCount; ++n)
            if (seen[n] == resource) { known = true; break; }
        if (!known && seenCount < 128)
        {
            seen[seenCount++] = resource;
            ID3D11Texture2D* tex = nullptr;
            ID3D11Buffer* buf = nullptr;
            if (SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Texture2D),
                                                   reinterpret_cast<void**>(&tex))))
            {
                D3D11_TEXTURE2D_DESC d{};
                tex->GetDesc(&d);
                LOG("M2 UAV CENSUS eye=%d slot=%u tex=%p %ux%u format=%u bind=0x%X",
                    eye, i, resource, d.Width, d.Height, (unsigned)d.Format, d.BindFlags);
                tex->Release();
            }
            else if (SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Buffer),
                                                        reinterpret_cast<void**>(&buf))))
            {
                D3D11_BUFFER_DESC bd{};
                buf->GetDesc(&bd);
                LOG("M2 UAV CENSUS eye=%d slot=%u buffer=%p bytes=%u bind=0x%X misc=0x%X",
                    eye, i, resource, bd.ByteWidth, bd.BindFlags, bd.MiscFlags);
                buf->Release();
            }
        }
        resource->Release();
    }
}

bool VR_RedirectRenderTargets(ID3D11DeviceContext* context, UINT count,
                              ID3D11RenderTargetView* const* input,
                              ID3D11RenderTargetView** output)
{
    const int eye = g_rasterEye.load();
    if (eye < 0 || eye > 1 || !input || !output || !g_gameSwapchain)
        return false;
    ID3D11Texture2D* backbuffer = nullptr;
    if (FAILED(g_gameSwapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backbuffer)) || !backbuffer)
        return false;
    bool changed = false;      // any slot rewritten (scene color or sun shaft)
    bool sceneChanged = false; // scene-color redirect only: marks the eye image valid
    for (UINT i = 0; i < count; ++i)
    {
        output[i] = input[i];
        if (!input[i]) continue;
        ID3D11Resource* resource = nullptr;
        input[i]->GetResource(&resource);
        if (resource)
            RecordHistoryUse(resource, false);
        if (resource)
        {
            static ID3D11Resource* seenResources[128]{};
            static unsigned seenResourceCount = 0;
            bool seen = false;
            for (unsigned n = 0; n < seenResourceCount; ++n)
                if (seenResources[n] == resource) { seen = true; break; }
            if (!seen && seenResourceCount < 128)
            {
                seenResources[seenResourceCount++] = resource;
                ID3D11Texture2D* texture = nullptr;
                if (SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Texture2D),
                                                       reinterpret_cast<void**>(&texture))))
                {
                    D3D11_TEXTURE2D_DESC desc{};
                    texture->GetDesc(&desc);
                    LOG("M2 RTV CENSUS eye=%d slot=%u resource=%p size=%ux%u format=%u samples=%u bind=0x%X",
                        eye, i, resource, desc.Width, desc.Height, (unsigned)desc.Format,
                        desc.SampleDesc.Count, desc.BindFlags);
                    texture->Release();
                }
            }
        }
        ID3D11Texture2D* candidate = nullptr;
        D3D11_TEXTURE2D_DESC candidateDesc{};
        const bool isTexture = resource &&
            SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Texture2D),
                                               reinterpret_cast<void**>(&candidate)));
        if (isTexture)
            candidate->GetDesc(&candidateDesc);

        // Sun-shaft neutralization (see the SunShaftDummy comment). The only
        // R16G16 render target bound inside the eye passes is the shaft
        // chain's square occlusion buffer (800x800 R16G16_FLOAT in the RTV
        // census). Clear the game's copy to black so the composite that
        // samples it adds nothing, and send the pass's own writes to a dummy
        // so it stays black. Toggling "Sun shafts in stereo" in the F1 menu
        // stops this instantly for a live A/B comparison.
        if (isTexture && context && !g_config.stereo_sun_shafts &&
            IsR16G16Family(candidateDesc.Format) &&
            candidateDesc.Width <= 2048 && candidateDesc.Height <= 2048 &&
            candidateDesc.SampleDesc.Count == 1 &&
            (candidateDesc.BindFlags & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE)) ==
                (D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE))
        {
            if (ID3D11RenderTargetView* dummy = GetSunShaftDummyRtv(candidateDesc, input[i]))
            {
                const float black[4] = {0, 0, 0, 0};
                context->ClearRenderTargetView(input[i], black);
                output[i] = dummy;
                changed = true;
                static std::atomic<unsigned> loggedShaft{0};
                if (loggedShaft.fetch_add(1) < 4)
                    LOG("M2: sun-shaft occlusion target %ux%u fmt=%u neutralized (eye %d, stereo_sun_shafts=0)",
                        candidateDesc.Width, candidateDesc.Height,
                        (unsigned)candidateDesc.Format, eye);
            }
        }

        D3D11_TEXTURE2D_DESC backbufferDesc{};
        backbuffer->GetDesc(&backbufferDesc);

        // Halo 3's completed frame is the unique full-resolution typeless RGBA
        // resource with RTV+SRV+UAV bindings at the end of the inner render.  The
        // preceding typed RGBA RT is an intermediate and can remain black.  Keep
        // our eye caches typed (from the game backbuffer) so they can be sampled
        // directly by the OpenXR blit.
        const bool isInternalSceneColor = i == 0 && candidate &&
            candidateDesc.Width == backbufferDesc.Width &&
            candidateDesc.Height == backbufferDesc.Height &&
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
            output[i] = g_eyeCacheRtvs[eye];
            changed = true;
            sceneChanged = true;
        }
        if (candidate) candidate->Release();
        if (resource) resource->Release();
    }
    backbuffer->Release();
    if (sceneChanged)
    {
        g_rasterRedirected[eye] = true;
        static std::atomic<unsigned> logged{0};
        if (logged.fetch_add(1) < 4)
            LOG("M2 RASTER: redirected internal scene-color RTV to eye %d target", eye);
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

bool VR_GetEyeCantQuat(int eye, float outQuat[4])
{
    if (eye < 0 || eye > 1 || !outQuat || g_views.size() < 2)
        return false;
    const XrQuaternionf& a = g_views[0].pose.orientation;
    const XrQuaternionf& b = g_views[1].pose.orientation;
    // Midpoint orientation: normalized component average. Before the first
    // successful xrLocateViews the orientations are all-zero and the length
    // check below rejects them.
    float cx = a.x + b.x, cy = a.y + b.y, cz = a.z + b.z, cw = a.w + b.w;
    const float len = sqrtf(cx * cx + cy * cy + cz * cz + cw * cw);
    if (len < 1e-5f)
        return false;
    cx /= len; cy /= len; cz /= len; cw /= len;
    // relative = conj(center) * eye
    const XrQuaternionf& e = g_views[eye].pose.orientation;
    outQuat[0] = cw * e.x - cx * e.w - cy * e.z + cz * e.y;
    outQuat[1] = cw * e.y + cx * e.z - cy * e.w - cz * e.x;
    outQuat[2] = cw * e.z - cx * e.y + cy * e.x - cz * e.w;
    outQuat[3] = cw * e.w + cx * e.x + cy * e.y + cz * e.z;
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

void VR_GetStatus(VrStatus& out)
{
    out = g_status;
}
