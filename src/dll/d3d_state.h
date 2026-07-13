#pragma once
#include <d3d11.h>

// Saves and restores the D3D11 pipeline state we touch. We are drawing with
// the *game's* device context in the middle of its frame — if we left our
// shaders/render targets bound, the game's next draw calls would go haywire.

struct D3DStateBackup
{
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11DepthStencilView* dsv = nullptr;
    D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    UINT numViewports = 0;
    ID3D11RasterizerState* rasterizer = nullptr;
    ID3D11BlendState* blend = nullptr;
    FLOAT blendFactor[4]{};
    UINT sampleMask = 0;
    ID3D11DepthStencilState* depthStencil = nullptr;
    UINT stencilRef = 0;
    ID3D11InputLayout* inputLayout = nullptr;
    D3D11_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11GeometryShader* gs = nullptr;
    ID3D11ShaderResourceView* psSrv0 = nullptr;
    ID3D11SamplerState* psSampler0 = nullptr;

    void Capture(ID3D11DeviceContext* ctx)
    {
        ctx->OMGetRenderTargets(1, &rtv, &dsv);
        numViewports = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        ctx->RSGetViewports(&numViewports, viewports);
        ctx->RSGetState(&rasterizer);
        ctx->OMGetBlendState(&blend, blendFactor, &sampleMask);
        ctx->OMGetDepthStencilState(&depthStencil, &stencilRef);
        ctx->IAGetInputLayout(&inputLayout);
        ctx->IAGetPrimitiveTopology(&topology);
        ctx->VSGetShader(&vs, nullptr, nullptr);
        ctx->PSGetShader(&ps, nullptr, nullptr);
        ctx->GSGetShader(&gs, nullptr, nullptr);
        ctx->PSGetShaderResources(0, 1, &psSrv0);
        ctx->PSGetSamplers(0, 1, &psSampler0);
    }

    void Restore(ID3D11DeviceContext* ctx)
    {
        ctx->OMSetRenderTargets(1, &rtv, dsv);
        if (numViewports)
            ctx->RSSetViewports(numViewports, viewports);
        ctx->RSSetState(rasterizer);
        ctx->OMSetBlendState(blend, blendFactor, sampleMask);
        ctx->OMSetDepthStencilState(depthStencil, stencilRef);
        ctx->IASetInputLayout(inputLayout);
        ctx->IASetPrimitiveTopology(topology);
        ctx->VSSetShader(vs, nullptr, 0);
        ctx->PSSetShader(ps, nullptr, 0);
        ctx->GSSetShader(gs, nullptr, 0);
        ctx->PSSetShaderResources(0, 1, &psSrv0);
        ctx->PSSetSamplers(0, 1, &psSampler0);
        Release();
    }

    void Release()
    {
        auto rel = [](IUnknown* p) { if (p) p->Release(); };
        rel(rtv); rtv = nullptr;
        rel(dsv); dsv = nullptr;
        rel(rasterizer); rasterizer = nullptr;
        rel(blend); blend = nullptr;
        rel(depthStencil); depthStencil = nullptr;
        rel(inputLayout); inputLayout = nullptr;
        rel(vs); vs = nullptr;
        rel(ps); ps = nullptr;
        rel(gs); gs = nullptr;
        rel(psSrv0); psSrv0 = nullptr;
        rel(psSampler0); psSampler0 = nullptr;
    }
};
