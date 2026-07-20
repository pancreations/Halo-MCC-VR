#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
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
    XrPath g_leftHandPath = XR_NULL_PATH;
    XrPath g_rightHandPath = XR_NULL_PATH;
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
    constexpr uint32_t kReticleSize = 64;
    // Color the reticle was last painted with, so we repaint only when the
    // user changes it (not every frame). Sentinel forces the first paint.
    float g_reticlePaintedColor[3] = {-1.0f, -1.0f, -1.0f};
    bool g_reticleEnemyPainted = false; // which color is currently on the image
    // Set by the game layer when the crosshair is over an enemy (the engine's
    // target-lock state). While set, the reticle repaints red like the OG HUD.
    std::atomic<bool> g_reticleEnemy{false};

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
    D3D11_TEXTURE2D_DESC g_gameBackbufferDesc{};
    bool g_gameBackbufferDescValid = false;

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
                    xrEndSession(g_session);
                    g_sessionRunning = false;
                }
                else if (sc.state == XR_SESSION_STATE_EXITING || sc.state == XR_SESSION_STATE_LOSS_PENDING)
                {
                    StopControllerHaptics();
                    g_sessionRunning = false;
                    Fail("The VR runtime ended the session (headset off / SteamVR closed?)");
                }
                break;
            }
            case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
                StopControllerHaptics();
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

    // Create the crosshair swapchain on first use (lazily, once the session is
    // running — SteamVR presented pre-session eye chains black) and paint the
    // reticle into it a single time.  Four cyan arc segments and short inner
    // ticks echo Halo 3's original blue CHUD reticle, but at a VR-friendly
    // angular size. A dark-blue outline keeps it legible without a black disc.
    // Paint the reticle image in the given color (0-1 per channel). The bright
    // arcs/ticks take the color; the outline is a darkened version of the same
    // hue so it reads at any color. Called on first use and whenever the color
    // changes (user edit, or the enemy-red switch below) — not per frame.
    bool PaintReticle(float cr, float cg, float cb)
    {
        std::vector<uint32_t> px(kReticleSize * kReticleSize);
        const float c = (kReticleSize - 1) * 0.5f;
        // coverage: 1 inside the shape, 0 outside, ~1px linear edge for AA
        auto cov = [](float d, float halfWidth) {
            const float v = halfWidth - d + 0.5f;
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
                const float dx = x - c, dy = y - c;
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
                const uint32_t r8=(uint32_t)(fminf(255.0f,olR*outline+brR*bright)+0.5f);
                const uint32_t g8=(uint32_t)(fminf(255.0f,olG*outline+brG*bright)+0.5f);
                const uint32_t b8=(uint32_t)(fminf(255.0f,olB*outline+brB*bright)+0.5f);
                const uint32_t a8=(uint32_t)(outline*255.0f+0.5f);
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
        // Enemy target-lock -> OG-style red; otherwise the user's base color.
        const bool enemy = g_reticleEnemy.load(std::memory_order_relaxed);
        const float wantR = enemy ? 1.0f : g_config.reticle_r;
        const float wantG = enemy ? 0.18f : g_config.reticle_g;
        const float wantB = enemy ? 0.14f : g_config.reticle_b;
        // Repaint only when the desired color changed (compositor keeps showing
        // the last released image, so a static color costs nothing per frame).
        const bool colorChanged =
            g_reticleEnemyPainted != enemy ||
            (!enemy && (g_reticlePaintedColor[0] != g_config.reticle_r ||
                        g_reticlePaintedColor[1] != g_config.reticle_g ||
                        g_reticlePaintedColor[2] != g_config.reticle_b));
        if (!colorChanged)
            return true;
        if (!PaintReticle(wantR, wantG, wantB))
        {
            failed = true;
            return false;
        }
        g_reticlePaintedColor[0]=wantR;
        g_reticlePaintedColor[1]=wantG;
        g_reticlePaintedColor[2]=wantB;
        g_reticleEnemyPainted=enemy;
        LOG("M3: crosshair reticle painted (%ux%u) rgb=(%.2f,%.2f,%.2f)%s",
            kReticleSize, kReticleSize, wantR, wantG, wantB,
            enemy?" [enemy red]":"");
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
        const char* ext = XR_KHR_D3D11_ENABLE_EXTENSION_NAME;
        XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
        strcpy_s(ici.applicationInfo.applicationName, "HaloMCCVR");
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
    // mode: engaged only while the grip is held with the hand in the zone.
    // The OpenXR aim pose sits back at the wrist. Shift the left-hand sample to
    // the palm so activation and the two-hand line are measured at the rendered
    // support hand. The same configured correction is used by game.cpp's left
    // arm target, keeping the visible wrist and the aiming point together.
    XrVector3f LeftHandPoint(const XrPosef& lpose)
    {
        const XrVector3f lfwd = Rotate(lpose.orientation, {0,0,-1});
        const float k = std::clamp(g_config.left_hand_forward_m, -0.15f, 0.30f);
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
        const XrVector3f lh = LeftHandPoint(lpose);
        const XrVector3f v{lh.x-rpose.position.x,
                           lh.y-rpose.position.y,
                           lh.z-rpose.position.z};
        const float along = v.x*rfwd.x + v.y*rfwd.y + v.z*rfwd.z;
        const XrVector3f perp{v.x-along*rfwd.x, v.y-along*rfwd.y, v.z-along*rfwd.z};
        const float lateral = sqrtf(perp.x*perp.x+perp.y*perp.y+perp.z*perp.z);
        const bool inZone = along>0.08f && along<0.80f && lateral<0.09f;
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
            g_twoHandLatched.store(gripHeld && inZone);
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
        static VrPadState previousPad{};
        if (pad.menu && !previousPad.menu) LOG("controller edge: Menu/Start");
        if (pad.a && !previousPad.a) LOG("controller edge: A");
        if (pad.b && !previousPad.b) LOG("controller edge: B");
        if (pad.x && !previousPad.x) LOG("controller edge: X");
        if (pad.y && !previousPad.y) LOG("controller edge: Y");
        previousPad = pad;
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

        XrCompositionLayerQuad screenQuad, menuQuad, reticleQuad, fadeQuad;
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
                        // Raw aim -Z, matching Game_ComputeAimStick: the cursor
                        // is a fixed laser from the controller; the mount trim
                        // rotates only the gun mesh toward it.
                        // Shared aim pose so the reticle rides the SAME ray as
                        // the bullets + barrel (two-hand line when engaged).
                        float aimQ[4], aimP[3];
                        const bool haveAim = VR_GetAimPose(aimQ, aimP);
                        const XrQuaternionf aimOri{aimQ[0],aimQ[1],aimQ[2],aimQ[3]};
                        const XrVector3f aimRay=Rotate(aimOri,{0.0f,0.0f,-1.0f});
                        const float aimDir[3]={aimRay.x,aimRay.y,aimRay.z};
                        if (g_config.crosshair && haveAim && EnsureReticleChain())
                        {
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
                            reticleQuad.pose.position = {aimP[0] + aimDir[0] * dist,
                                                         aimP[1] + aimDir[1] * dist,
                                                         aimP[2] + aimDir[2] * dist};
                            const float w = 2.0f * dist *
                                tanf(g_config.crosshair_size_deg * 0.5f * 0.0174533f);
                            reticleQuad.size = {w, w};
                            layers.push_back(
                                reinterpret_cast<XrCompositionLayerBaseHeader*>(&reticleQuad));
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

    // Auto-enter/exit VR when a level loads/unloads (no F2/F11 needed).
    Game_AutoVrTick();

    Frame(sc);
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
    g_gameBackbufferDesc = {};
    g_gameBackbufferDescValid = false;
}

void VR_RequestRecenter()
{
    g_haveCenter = false;
}

void VR_RequestPausePresentation(bool paused)
{
    g_pauseRequest = paused ? 1 : 0;
}

bool VR_IsPausePresentation()
{
    return g_pausePresentation.load();
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
        return;
    }
    static bool loggedMissing = false;
    if (!loggedMissing)
    {
        LOG("M2 RASTER: no internal scene-color RTV redirect occurred; refusing fake eye copy");
        loggedMissing = true;
    }
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


bool VR_RedirectRenderTargets(ID3D11DeviceContext* context, UINT count,
                              ID3D11RenderTargetView* const* input,
                              ID3D11RenderTargetView** output)
{
    const int eye = g_rasterEye.load();
    if (eye < 0 || eye > 1 || !input || !output || !g_gameSwapchain)
        return false;
    bool changed = false;      // any slot rewritten (scene color or sun shaft)
    bool sceneChanged = false; // scene-color redirect only: marks the eye image valid
    for (UINT i = 0; i < count; ++i)
    {
        output[i] = input[i];
        if (!input[i]) continue;

        // Normal steady-state path: two pointer comparisons and no COM calls.
        if (g_sceneColorRtv)
        {
            if (input[i] == g_sceneColorRtv && g_eyeCacheRtvs[eye])
            {
                output[i] = g_eyeCacheRtvs[eye];
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
        g_rasterRedirected[eye] = true;
        VR_TraceEvent("rtv-redirect", eye, 0);
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

    (void)gripL;
    // Engagement is decided once per frame in UpdateTwoHandLatch (toggle/hold +
    // barrel-zone). Here we only APPLY it: when latched, aim along the current
    // right->left hand line.
    if (!g_config.two_handed_aim || !okL || !g_twoHandLatched.load())
    { g_twoHandActive.store(false); return true; }

    const XrVector3f rup  = Rotate(rq, {0,1,0});
    XrVector3f v{lp.x-rp.x, lp.y-rp.y, lp.z-rp.z};
    const float len = sqrtf(v.x*v.x+v.y*v.y+v.z*v.z);
    if (len < 1e-4f) { g_twoHandActive.store(false); return true; }

    XrVector3f af{v.x/len, v.y/len, v.z/len};

    // Orthonormal basis: X=right, Y=up, Z=-forward, roll from the right hand up.
    auto cross=[](const XrVector3f&a,const XrVector3f&b){
        return XrVector3f{a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x}; };
    XrVector3f xa=cross(af,rup);
    float xl=sqrtf(xa.x*xa.x+xa.y*xa.y+xa.z*xa.z);
    if (xl<1e-4f) { g_twoHandActive.store(false); return true; }
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
    if (ql<1e-5f) { g_twoHandActive.store(false); return true; }
    outQuat[0]=qx/ql; outQuat[1]=qy/ql; outQuat[2]=qz/ql; outQuat[3]=qw/ql;
    const bool wasActive=g_twoHandActive.exchange(true);
    if (!wasActive) LOG("M3: two-handed aim engaged (left grip held, hand on barrel)");
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
