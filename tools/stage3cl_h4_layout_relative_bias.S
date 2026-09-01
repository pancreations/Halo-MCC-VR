// Stage 3CL - the Halo 4 capture window follows the authored reticle offset.
//
// Why (derived from the accepted capture, not guessed). The captured reticle
// does not sit at the centre of the CUI canvas: Stage 3BK measured it 104 px
// low in a 1346.196 px window and every later stage carried that as the
// FROZEN fraction 104/1346.196 = 0.077255. That fraction was measured while
// the engine reported base = (-1033.578, +336.549), an offset ratio
// baseY/|baseX| = 0.325615.
//
// The reticle offset and the canvas half-width come from the same authored
// projection, so the offset is proportional to that ratio - it is NOT a
// constant. The failing sessions report ratios 0.5182 and 0.5624 (near
// double), so the frozen nudge no longer reaches the reticle and it falls
// outside the 2.5x-zoomed window: a byte-empty capture, on every weapon,
// stable within a session because the ratio is stable within a session.
// That matches every observed session, including weapon swaps working while
// the ratio was at its calibrated value.
//
// The accepted calibration fixes the proportionality constant:
//     0.077255 = k * 0.325615  ->  k = 0.237258
// so, with the live published values (hide = 4|baseX| at 0x2A8368):
//     biasFraction = k * baseY / |baseX| = (4k) * baseY / hide
// which reproduces 0.077255 EXACTLY at the calibrated layout and tracks any
// other. Width keeps the accepted rule W = 4|baseX| = hide, so the reticle
// art keeps its accepted on-quad size everywhere.
//
// Everything else is the accepted chain, untouched: TLx = (512-W)/2
// (Begin's centring), H = W*bbH/bbW (3BN raster aspect), then a jump into
// the 3BR scale tail for the accepted 2.5x zoom about (256,256) and the
// RSSetViewports call, reusing 3BR's own constants and private buffer. Any
// missing, zero or NaN live value falls back to the complete accepted 3BR
// thunk (frozen behavior, never worse than before).
//
// defsyms: CAPTURE_VIEWPORT=0x2AE774, BASE_Y=0x2A8364, LIVE_HIDE_X=0x2A8368,
//          BBW=0x2AEB58, BBH=0x2AEB5C, K512=0x2F9BD0, KHALF=0x2F9BD4,
//          VPBUF=0x2F9BE8, S3BR_SCALE_TAIL=0x2F9B29, S3BR_THUNK=0x2F9A90

.intel_syntax noprefix
.text

.globl s3cl_layout_viewport
s3cl_layout_viewport:
    sub     rsp, 0x28
    movups  xmm0, xmmword ptr [rip + CAPTURE_VIEWPORT]
    movups  xmmword ptr [rip + VPBUF], xmm0
    mov     rax, qword ptr [rip + CAPTURE_VIEWPORT + 0x10]
    mov     qword ptr [rip + VPBUF + 0x10], rax

    movss   xmm5, dword ptr [rip + LIVE_HIDE_X]     // hide = 4|baseX|
    ucomiss xmm5, xmm5
    jp      .Lfallback
    xorps   xmm1, xmm1
    comiss  xmm5, xmm1
    jbe     .Lfallback
    movss   xmm4, dword ptr [rip + BASE_Y]          // authored reticle offset
    ucomiss xmm4, xmm4
    jp      .Lfallback
    mov     eax, dword ptr [rip + BBW]
    test    eax, eax
    jz      .Lfallback
    mov     ecx, dword ptr [rip + BBH]
    test    ecx, ecx
    jz      .Lfallback

    movss   dword ptr [rip + VPBUF + 8], xmm5       // W = hide (accepted rule)
    movss   xmm1, dword ptr [rip + K512]
    subss   xmm1, xmm5
    mulss   xmm1, dword ptr [rip + KHALF]
    movss   dword ptr [rip + VPBUF], xmm1           // TLx = (512 - W)/2

    cvtsi2ss xmm2, eax                              // bbW
    cvtsi2ss xmm1, ecx                              // bbH
    movaps  xmm0, xmm5
    mulss   xmm0, xmm1
    divss   xmm0, xmm2                              // H = W * bbH / bbW
    movss   dword ptr [rip + VPBUF + 12], xmm0

    movss   xmm1, dword ptr [rip + K512]
    subss   xmm1, xmm0
    mulss   xmm1, dword ptr [rip + KHALF]           // (512 - H)/2
    mulss   xmm4, dword ptr [rip + s3cl_k4]         // baseY * 4k
    divss   xmm4, xmm5                              // / hide
    mulss   xmm4, xmm0                              // * H = live bias px
    subss   xmm1, xmm4
    movss   dword ptr [rip + VPBUF + 4], xmm1       // TopLeftY
    jmp     S3BR_SCALE_TAIL
.Lfallback:
    add     rsp, 0x28
    jmp     S3BR_THUNK

.p2align 4
s3cl_k4:
    .long 0x3F72F3A3        // 0.949030f = 4 * 0.237258 (accepted calibration)
