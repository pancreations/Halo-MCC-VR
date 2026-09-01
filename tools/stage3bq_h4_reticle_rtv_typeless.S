// Stage 3BQ - create the reticle swapchain RTV with an explicit format when
// the null-desc attempt fails (SteamVR hands out TYPELESS swapchain textures).
//
// Evidence (Steam log 15:52, Stage 3BP run): every authored upload fails with
//   blit: missing shader-path resource (... rtv=0000000000000000)
// while the capture itself is perfect (all four raw dumps hold the stable
// 1485-texel AR reticle).  GetRtv (vr.cpp 3263) creates the view lazily with
// CreateRenderTargetView(image, NULL, &rtv): a null desc is invalid on a
// TYPELESS resource, which is what SteamVR's OpenXR runtime returns for
// swapchain images.  Nothing noticed before 3BO because every other consumer
// avoids the view: eye and reticle blits took the FAST CopyResource path and
// the procedural crosshair paints via UpdateSubresource.  The failed upload
// falls back to the procedural repaint, the next frame retries, and the two
// alternate - the reported "placeholder crosshair flashing like crazy".
//
// The inlined GetRtv at the reticle-upload Blit call (UploadAuthoredReticle)
// is at 0x2A9DA: `mov rax,[rcx]` + `call [rax+0x48]` with rcx=g_device,
// rdx=images[idx], r8d=0 (the null desc), r9=&rtvs[idx].  Those 6 bytes
// become a call here.  This thunk first performs the exact original call; if
// it fails, it retries once with an explicit D3D11_RENDER_TARGET_VIEW_DESC
// { Format = (DXGI_FORMAT)g_xrFormat, ViewDimension = TEXTURE2D(4),
//   Texture2D.MipSlice = 0 } - valid both on a TYPELESS resource and on one
// already created with the concrete format.  rdx/r9 are saved because the
// first COM call may clobber them.  The call site ignores the HRESULT and
// re-reads rtvs[idx], so the second attempt's result flows naturally.

.intel_syntax noprefix
.text

.globl s3bq_create_rtv
s3bq_create_rtv:
    sub     rsp, 0x58                  // entry rsp==8 mod16 -> 0 mod16
    mov     qword ptr [rsp + 0x40], rdx    // images[idx]
    mov     qword ptr [rsp + 0x48], r9     // &rtvs[idx]
    mov     rax, qword ptr [rcx]           // displaced instruction
    call    qword ptr [rax + 0x48]         // CreateRenderTargetView(dev, tex, NULL, &rtv)
    test    eax, eax
    jns     .Ldone                     // SUCCEEDED: original behaviour
    mov     rcx, qword ptr [rip + G_DEVICE]
    test    rcx, rcx
    jz      .Ldone
    mov     eax, dword ptr [rip + XR_FORMAT]   // (DXGI_FORMAT)g_xrFormat
    mov     dword ptr [rsp + 0x28], eax        // desc.Format
    mov     dword ptr [rsp + 0x2C], 4          // D3D11_RTV_DIMENSION_TEXTURE2D
    xor     eax, eax
    mov     qword ptr [rsp + 0x30], rax        // Texture2D.MipSlice = 0 (+pad)
    mov     dword ptr [rsp + 0x38], eax        // rest of the union
    mov     rdx, qword ptr [rsp + 0x40]
    lea     r8, [rsp + 0x28]
    mov     r9, qword ptr [rsp + 0x48]
    mov     rax, qword ptr [rcx]
    call    qword ptr [rax + 0x48]         // retry with the explicit desc
.Ldone:
    add     rsp, 0x58
    ret
