#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <MinHook.h>
#include "d3d11_hook.h"
#include "game.h"
#include "vr.h"
#include "../common/log.h"

// We can't hook "the game's swapchain" directly because it doesn't exist yet
// when we're injected. Instead we create a throwaway D3D11 device + swapchain
// of our own, read the addresses of Present/ResizeBuffers out of its vtable
// (all swapchains in the process share the same implementation), hook those,
// and throw the dummy away.

typedef HRESULT(STDMETHODCALLTYPE* PresentFn)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* Present1Fn)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
typedef HRESULT(STDMETHODCALLTYPE* ResizeBuffersFn)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
typedef void(STDMETHODCALLTYPE* OMSetRenderTargetsFn)(ID3D11DeviceContext*, UINT,
    ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
typedef void(STDMETHODCALLTYPE* OMSetRenderTargetsAndUavsFn)(ID3D11DeviceContext*, UINT,
    ID3D11RenderTargetView* const*, ID3D11DepthStencilView*, UINT, UINT,
    ID3D11UnorderedAccessView* const*);
typedef void(STDMETHODCALLTYPE* PSSetShaderResourcesFn)(ID3D11DeviceContext*, UINT, UINT,
    ID3D11ShaderResourceView* const*);
typedef void(STDMETHODCALLTYPE* CopyResourceFn)(ID3D11DeviceContext*, ID3D11Resource*,
    ID3D11Resource*);
typedef void(STDMETHODCALLTYPE* CopySubresourceRegionFn)(ID3D11DeviceContext*, ID3D11Resource*,
    UINT, UINT, UINT, UINT, ID3D11Resource*, UINT, const D3D11_BOX*);

static PresentFn g_origPresent = nullptr;
static Present1Fn g_origPresent1 = nullptr;
static ResizeBuffersFn g_origResizeBuffers = nullptr;
static OMSetRenderTargetsFn g_origOMSetRenderTargets = nullptr;
static OMSetRenderTargetsAndUavsFn g_origOMSetRenderTargetsAndUavs = nullptr;
static PSSetShaderResourcesFn g_origPSSetShaderResources = nullptr;
static CopyResourceFn g_origCopyResource = nullptr;
static CopySubresourceRegionFn g_origCopySubresourceRegion = nullptr;
static void* g_psSetShaderResourcesTarget = nullptr;

// NOTE: UpdateSubresource/Map/Unmap were hooked twice tonight (constant
// census, then exact-match matrix substitution). Both are REMOVED: the
// census sagged fps by ~25%, and the matrix matcher scored zero hits in a
// full session — the engine does not reuse our built matrices verbatim, so
// the per-frame effect params must be computed from the engine's own camera
// state on the game thread. Do not re-add hooks on those three entry points;
// they are the hottest calls in the frame.

// Ghost hunt, the two probes every previous census missed: GPU-side copies
// (never hooked before) and resources moved without any bind call. Log-only.
static void STDMETHODCALLTYPE CopyResourceHook(ID3D11DeviceContext* context,
    ID3D11Resource* dst, ID3D11Resource* src)
{
    VR_RecordCopy(dst, src, "CopyResource");
    g_origCopyResource(context, dst, src);
}

static void STDMETHODCALLTYPE CopySubresourceRegionHook(ID3D11DeviceContext* context,
    ID3D11Resource* dst, UINT dstSub, UINT dstX, UINT dstY, UINT dstZ,
    ID3D11Resource* src, UINT srcSub, const D3D11_BOX* box)
{
    VR_RecordCopy(dst, src, "CopySubresource");
    g_origCopySubresourceRegion(context, dst, dstSub, dstX, dstY, dstZ, src, srcSub, box);
}

static void STDMETHODCALLTYPE PSSetShaderResourcesHook(ID3D11DeviceContext* context, UINT startSlot,
    UINT count, ID3D11ShaderResourceView* const* srvs)
{
    VR_RecordShaderResourceReads(count, srvs);
    g_origPSSetShaderResources(context, startSlot, count, srvs);
}

void D3D11_SetHistoryDiscovery(bool enabled)
{
    if (!g_psSetShaderResourcesTarget)
        return;
    const MH_STATUS status = enabled ? MH_EnableHook(g_psSetShaderResourcesTarget)
                                     : MH_DisableHook(g_psSetShaderResourcesTarget);
    if (status != MH_OK && status != MH_ERROR_ENABLED && status != MH_ERROR_DISABLED)
        LOG("M2: could not %s post-process history discovery hook (%s)",
            enabled ? "enable" : "disable", MH_StatusToString(status));
}

static void STDMETHODCALLTYPE OMSetRenderTargetsHook(ID3D11DeviceContext* context, UINT count,
    ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv)
{
    ID3D11RenderTargetView* redirected[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    if (count <= D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT &&
        VR_RedirectRenderTargets(context, count, rtvs, redirected))
    {
        g_origOMSetRenderTargets(context, count, redirected, dsv);
    }
    else
    {
        VR_RecordFrameRtv(count, rtvs); // no-op unless discovery runs outside an eye pass
        g_origOMSetRenderTargets(context, count, rtvs, dsv);
    }
}

// Frame-level post can bind through the RTV+UAV variant, which the plain
// OMSetRenderTargets hook never sees. Record-only; nothing is redirected here.
static void STDMETHODCALLTYPE OMSetRenderTargetsAndUavsHook(ID3D11DeviceContext* context,
    UINT count, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv,
    UINT uavStart, UINT uavCount, ID3D11UnorderedAccessView* const* uavs)
{
    if (count != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL && rtvs)
        VR_RecordFrameRtv(count, rtvs);
    g_origOMSetRenderTargetsAndUavs(context, count, rtvs, dsv, uavStart, uavCount, uavs);
}

// Present1 can forward to Present internally; this depth counter makes sure
// we only run the VR frame once per game frame.
static thread_local int g_presentDepth = 0;

static HRESULT STDMETHODCALLTYPE PresentHook(IDXGISwapChain* sc, UINT syncInterval, UINT flags)
{
    const bool topLevel = (g_presentDepth++ == 0);
    if (topLevel && !(flags & DXGI_PRESENT_TEST))
        VR_OnPresent(sc);
    HRESULT hr = g_origPresent(sc, syncInterval, flags);
    g_presentDepth--;
    return hr;
}

static HRESULT STDMETHODCALLTYPE Present1Hook(IDXGISwapChain1* sc, UINT syncInterval, UINT flags,
                                              const DXGI_PRESENT_PARAMETERS* params)
{
    const bool topLevel = (g_presentDepth++ == 0);
    if (topLevel && !(flags & DXGI_PRESENT_TEST))
        VR_OnPresent(sc);
    HRESULT hr = g_origPresent1(sc, syncInterval, flags, params);
    g_presentDepth--;
    return hr;
}

static HRESULT STDMETHODCALLTYPE ResizeBuffersHook(IDXGISwapChain* sc, UINT bufferCount, UINT width,
                                                   UINT height, DXGI_FORMAT format, UINT flags)
{
    LOG("game resized its swapchain to %ux%u", width, height);
    VR_OnResizeBuffers(sc); // we must drop any references to the old backbuffer first
    return g_origResizeBuffers(sc, bufferCount, width, height, format, flags);
}

bool InstallD3D11Hooks()
{
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"halo3xr_dummy_window";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPED,
                                0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd)
    {
        LOG("dummy window creation failed (%lu)", GetLastError());
        return false;
    }

    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 1;
    sd.BufferDesc.Width = 64;
    sd.BufferDesc.Height = 64;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    IDXGISwapChain* sc = nullptr;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                               nullptr, 0, D3D11_SDK_VERSION, &sd, &sc, &dev, &fl, &ctx);
    if (FAILED(hr))
    {
        LOG("dummy D3D11 device creation failed (hr=0x%08X)", (unsigned)hr);
        DestroyWindow(hwnd);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return false;
    }

    void** vtbl = *(void***)sc;
    void** contextVtbl = *(void***)ctx;
    g_psSetShaderResourcesTarget = contextVtbl[8];
    bool ok = MH_CreateHook(vtbl[8], (void*)&PresentHook, (void**)&g_origPresent) == MH_OK &&
              MH_CreateHook(vtbl[13], (void*)&ResizeBuffersHook, (void**)&g_origResizeBuffers) == MH_OK &&
              MH_CreateHook(g_psSetShaderResourcesTarget, (void*)&PSSetShaderResourcesHook,
                            (void**)&g_origPSSetShaderResources) == MH_OK &&
              MH_CreateHook(contextVtbl[33], (void*)&OMSetRenderTargetsHook,
                            (void**)&g_origOMSetRenderTargets) == MH_OK &&
              MH_CreateHook(contextVtbl[34], (void*)&OMSetRenderTargetsAndUavsHook,
                            (void**)&g_origOMSetRenderTargetsAndUavs) == MH_OK &&
              MH_CreateHook(contextVtbl[46], (void*)&CopySubresourceRegionHook,
                            (void**)&g_origCopySubresourceRegion) == MH_OK &&
              MH_CreateHook(contextVtbl[47], (void*)&CopyResourceHook,
                            (void**)&g_origCopyResource) == MH_OK;

    IDXGISwapChain1* sc1 = nullptr;
    if (SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain1), (void**)&sc1)))
    {
        void** vtbl1 = *(void***)sc1;
        if (MH_CreateHook(vtbl1[22], (void*)&Present1Hook, (void**)&g_origPresent1) != MH_OK)
            LOG("warning: Present1 hook failed; flip-model swapchains may not be captured");
        sc1->Release();
    }

    sc->Release();
    ctx->Release();
    dev->Release();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);

    if (!ok)
    {
        LOG("MinHook could not hook Present/ResizeBuffers");
        return false;
    }
    return MH_EnableHook(MH_ALL_HOOKS) == MH_OK;
}
