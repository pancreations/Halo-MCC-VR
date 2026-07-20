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

static PresentFn g_origPresent = nullptr;
static Present1Fn g_origPresent1 = nullptr;
static ResizeBuffersFn g_origResizeBuffers = nullptr;
static OMSetRenderTargetsFn g_origOMSetRenderTargets = nullptr;

// Hooks deliberately NOT installed here, each retired after disproving a
// theory. Re-adding any of them costs frame time for information we already
// have:
//   UpdateSubresource/Map/Unmap - constant census sagged fps ~25%; the
//     exact-match matrix matcher scored zero hits in a full session.
//   CopyResource/CopySubresourceRegion - learned the frame-level scene
//     snapshot pairs; per-eye substitution of both sides left the ghost
//     unchanged. The learning also allocated a full-resolution shadow texture
//     per eye per pair (~25 MB each) and re-copied them every eye pass.
//   PSSetShaderResources - cross-pass history discovery promoted 0 targets in
//     two sessions.
//   OMSetRenderTargetsAndUnorderedAccessViews - frame-level RTV discovery
//     promoted 0 targets.
//   PSSetShader/VSSetShader/Draw* - the CHUD steal-and-requad classifier
//     (2026-07-18): removed the native HUD from both eyes, never displayed
//     its hand quad, and its calibration retry loop cost ~30 fps.
// OMSetRenderTargets makes the two eye renders land in separate textures.

static void STDMETHODCALLTYPE OMSetRenderTargetsHook(ID3D11DeviceContext* context, UINT count,
    ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv)
{
    ID3D11RenderTargetView* redirected[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    if (count <= D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT &&
        VR_RedirectRenderTargets(context, count, rtvs, redirected))
    {
        g_origOMSetRenderTargets(context, count, redirected, dsv);
        return;
    }
    g_origOMSetRenderTargets(context, count, rtvs, dsv);
}

// Present1 can forward to Present internally; this depth counter makes sure
// we only run the VR frame once per game frame.
static thread_local int g_presentDepth = 0;

static HRESULT STDMETHODCALLTYPE PresentHook(IDXGISwapChain* sc, UINT syncInterval, UINT flags)
{
    const bool topLevel = (g_presentDepth++ == 0);
    const bool runVrFrame = topLevel && !(flags & DXGI_PRESENT_TEST);
    if (runVrFrame)
        VR_BeforePresent(sc);
    HRESULT hr = g_origPresent(sc, syncInterval, flags);
    if (runVrFrame)
        VR_AfterPresent(sc);
    g_presentDepth--;
    return hr;
}

static HRESULT STDMETHODCALLTYPE Present1Hook(IDXGISwapChain1* sc, UINT syncInterval, UINT flags,
                                              const DXGI_PRESENT_PARAMETERS* params)
{
    const bool topLevel = (g_presentDepth++ == 0);
    const bool runVrFrame = topLevel && !(flags & DXGI_PRESENT_TEST);
    if (runVrFrame)
        VR_BeforePresent(sc);
    HRESULT hr = g_origPresent1(sc, syncInterval, flags, params);
    if (runVrFrame)
        VR_AfterPresent(sc);
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
    bool ok = MH_CreateHook(vtbl[8], (void*)&PresentHook, (void**)&g_origPresent) == MH_OK &&
              MH_CreateHook(vtbl[13], (void*)&ResizeBuffersHook, (void**)&g_origResizeBuffers) == MH_OK &&
              MH_CreateHook(contextVtbl[33], (void*)&OMSetRenderTargetsHook,
                            (void**)&g_origOMSetRenderTargets) == MH_OK;

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
        LOG("MinHook could not hook Present/ResizeBuffers/HUD draw path");
        return false;
    }
    return MH_EnableHook(MH_ALL_HOOKS) == MH_OK;
}
