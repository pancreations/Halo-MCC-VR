// Stage 3CK - the Halo 4 capture window follows the authored layout.
//
// Proven 2026-08-30 by log forensics over 11 sessions: the reticle capture
// produced art ONLY while the engine reported `native base -1033.578`
// (4|baseX| = 4134.312 = the frozen Stage 3BH framing constant). Every
// session reporting -1304.277 or -1893.000 captured nothing at all.
//
// Geometry. The capture viewport is W pixels wide for the NDC span [-1,1],
// so the 512-texel target shows the central 512/W of the canvas: a LARGER W
// zooms IN and covers LESS canvas. The reticle sits at the authored layout
// position, which is further from canvas centre on the layouts that failed,
// so it fell outside the frozen window. Coverage must therefore scale
// INVERSELY with the layout - Stage 3CE scaled it directly (W = hide), which
// is why it stayed blank.
//
// The accepted calibration fixes the invariant exactly:
//     W * |baseX| = 4134.312 * 1033.578 = 4272942.9
// and the engine publishes hide = 4|baseX| live (0x2A8368, stored by the
// native-hide path at 0x53E04), so
//     W = 4 * 4272942.9 / hide = 17091771.6 / hide
// which reproduces the accepted 4134.312 exactly at the calibrated layout
// and widens coverage proportionally on any other one.
//
// Everything downstream is the accepted chain, untouched: TLx = (512-W)/2
// (Begin's own centring), H = W*bbH/bbW (3BN raster aspect), TLy = (512-H)/2
// - H*(104/1346.196) (3BN/3BK reticle bias), then a jump into the 3BR scale
// tail for the accepted 2.5x zoom about (256,256) and the RSSetViewports
// call, reusing 3BR's own constants and private buffer. A missing, zero or
// NaN layout value falls back to the complete accepted 3BR thunk.
//
// defsyms: CAPTURE_VIEWPORT=0x2AE774, LIVE_HIDE_X=0x2A8368, BBW=0x2AEB58,
//          BBH=0x2AEB5C, K512=0x2F9BD0, KHALF=0x2F9BD4, KBIAS=0x2F9BD8,
//          VPBUF=0x2F9BE8, S3BR_SCALE_TAIL=0x2F9B29, S3BR_THUNK=0x2F9A90

.intel_syntax noprefix
.text

.globl s3ck_layout_viewport
s3ck_layout_viewport:
    sub     rsp, 0x28
    movups  xmm0, xmmword ptr [rip + CAPTURE_VIEWPORT]
    movups  xmmword ptr [rip + VPBUF], xmm0        // seed (keeps depth range)
    mov     rax, qword ptr [rip + CAPTURE_VIEWPORT + 0x10]
    mov     qword ptr [rip + VPBUF + 0x10], rax
    movss   xmm5, dword ptr [rip + LIVE_HIDE_X]    // hide = 4|baseX|, live
    ucomiss xmm5, xmm5
    jp      .Lfallback                             // NaN
    xorps   xmm1, xmm1
    comiss  xmm5, xmm1
    jbe     .Lfallback                             // not published yet
    mov     eax, dword ptr [rip + BBW]
    test    eax, eax
    jz      .Lfallback
    mov     ecx, dword ptr [rip + BBH]
    test    ecx, ecx
    jz      .Lfallback

    movss   xmm0, dword ptr [rip + s3ck_kinvariant]
    divss   xmm0, xmm5                             // W = 4*K / hide
    movss   dword ptr [rip + VPBUF + 8], xmm0
    movss   xmm1, dword ptr [rip + K512]
    subss   xmm1, xmm0
    mulss   xmm1, dword ptr [rip + KHALF]
    movss   dword ptr [rip + VPBUF], xmm1          // TLx = (512 - W)/2

    cvtsi2ss xmm2, eax                             // bbW
    cvtsi2ss xmm1, ecx                             // bbH
    mulss   xmm0, xmm1
    divss   xmm0, xmm2                             // H = W * bbH / bbW
    movss   dword ptr [rip + VPBUF + 12], xmm0
    movss   xmm1, dword ptr [rip + K512]
    subss   xmm1, xmm0
    mulss   xmm1, dword ptr [rip + KHALF]          // (512 - H)/2
    movaps  xmm2, xmm0
    mulss   xmm2, dword ptr [rip + KBIAS]          // H * (104/1346.196)
    subss   xmm1, xmm2
    movss   dword ptr [rip + VPBUF + 4], xmm1      // TopLeftY
    jmp     S3BR_SCALE_TAIL                        // accepted 2.5x + set
.Lfallback:
    add     rsp, 0x28
    jmp     S3BR_THUNK                             // accepted frozen path

.p2align 4
s3ck_kinvariant:
    .long 0x4B8267DC        // 17092536.0f = 4 * 4134.312 * 1033.578
