// Stage 3BU - Halo 4: copy each finished eye back into the engine's REAL
// scene texture.
//
// Why. Halo 4 stereo redirects every OM bind of the learned scene-colour RTV
// (g_sceneColorRtv, 3786x2730 fmt28 = the engine's final composited target)
// into our per-eye caches (VR_RedirectRenderTargets, pointer-only steady
// state). The engine's REAL scene texture therefore never receives a pixel
// while a frame is claimed. Two user-visible bugs share that one starvation:
//   1. The desktop mirror is black: the engine's final present pass samples
//      the real scene texture into the backbuffer, and it is black.
//   2. Taking damage blacks out the headset: the damage screen-effect chain
//      READS the real scene texture as its input and composites the result
//      back into the (redirected) scene target - vignette over black, i.e. a
//      black frame, for as long as the effect runs. (Pause frames are
//      unclaimed, which is why the pause menu still renders - the starvation
//      only exists on claimed eyes.)
//
// Fix. At the end of every claimed Halo 4 eye, CopyResource the eye cache
// into the real scene texture. The engine's present blit then mirrors the
// last eye to the desktop, and any screen-effect pass sampling the scene
// texture sees real (at worst one-eye-stale) content instead of black.
// GPU-only work: one full-size CopyResource per eye (~41 MB, ~10 GB/s at
// 120 fps) - far below the copies the H3 ghosting fix already ships.
//
// Splice. VR_EndRasterEye (0x2C280) inlines Halo4ResolveSceneTargetAtEyeEnd.
// Its steady-state branch at 0x2C2BC - `jne 0x2C3E5`, taken exactly when
// eye >= 0 AND title == Halo 4 AND g_sceneColorRtv is already latched - is
// re-pointed here. The learning path (RTV not yet latched) falls through
// unchanged. At the splice rsp = 16-aligned; rdi = 0, rbp/rsi/rbx/r12-r15
// must be preserved (we only touch volatiles); the resume block at 0x2C3E5
// overwrites rax/rcx/rdx/rbx/r8-r11/xmm0-2 before reading them.
//
// COM slots (verified against the mod's own hook indices: contextVtbl[33] =
// OMSetRenderTargets, contextVtbl[47] = CopyResource):
//   ID3D11View::GetResource   = slot  7 -> vtbl+0x38 (returns AddRef'd)
//   IUnknown::Release         = slot  2 -> vtbl+0x10
//   ID3D11DeviceContext::CopyResource = slot 47 -> vtbl+0x178
//
// Globals (disassembly-verified in Stage 3BT):
//   RASTER_EYE = 0x2420A8   g_rasterEye (int; 0/1 on this path, -1 later)
//   EYE_CACHE  = 0x2AE808   g_eyeCache[2] (ID3D11Texture2D*)
//   SCENE_RTV  = 0x2AEAF0   g_sceneColorRtv (ID3D11RenderTargetView*)
//   G_CONTEXT  = 0x2AE298   g_context (18 refs in Blit)
//   RESUME     = 0x2C3E5    original jne destination

.intel_syntax noprefix
.text

.globl s3bu_writeback
s3bu_writeback:
    sub     rsp, 0x40                        // 16-aligned; 0x20 shadow + spills
    mov     eax, dword ptr [rip + RASTER_EYE]
    cmp     eax, 1
    ja      .Ldone                           // defensive: eye must be 0 or 1
    lea     rcx, [rip + EYE_CACHE]
    mov     rdx, qword ptr [rcx + rax*8]     // src = g_eyeCache[eye]
    test    rdx, rdx
    jz      .Ldone
    mov     rcx, qword ptr [rip + SCENE_RTV] // the latched view (nonzero here)
    test    rcx, rcx
    jz      .Ldone
    mov     qword ptr [rsp + 0x30], rdx      // spill src
    mov     qword ptr [rsp + 0x28], 0        // out param
    lea     rdx, [rsp + 0x28]
    mov     rax, qword ptr [rcx]
    call    qword ptr [rax + 0x38]           // rtv->GetResource(&out)
    mov     rcx, qword ptr [rip + G_CONTEXT]
    test    rcx, rcx
    jz      .Lrelease
    mov     rdx, qword ptr [rsp + 0x28]      // dst = real scene texture
    test    rdx, rdx
    jz      .Lrelease
    mov     r8,  qword ptr [rsp + 0x30]      // src = eye cache
    mov     rax, qword ptr [rcx]
    call    qword ptr [rax + 0x178]          // context->CopyResource(dst, src)
.Lrelease:
    mov     rcx, qword ptr [rsp + 0x28]
    test    rcx, rcx
    jz      .Ldone
    mov     rax, qword ptr [rcx]
    call    qword ptr [rax + 0x10]           // dst->Release() (GetResource AddRef'd)
.Ldone:
    add     rsp, 0x40
    jmp     RESUME                           // continue VR_EndRasterEye at 0x2C3E5
