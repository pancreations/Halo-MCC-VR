// Stage 3CQ - per-axis live capture framing for Halo 4.
//
// The decisive session table (14 sessions, no exception): the reticle capture
// works only when the engine's gameplay canvas half-extents are exactly
// (1033.578, 336.549) - the pair every framing constant in the 3BE..3BR chain
// was calibrated on - and is byte-empty on every other canvas
// ((1304.277,675.936), (1344.409,697.226), (1466.886,654.646), ...). The
// canvas pair is session/segment-constant and published live per CUI pass
// (baseX 0x2A8360, baseY 0x2A8364, hide = 4|baseX| at 0x2A8368).
//
// The shared axis bug in every previous live attempt (3CE/3CK/3CL and
// Codex's identical 3CO): the viewport HEIGHT was derived from the WIDTH via
// the raster aspect (H = W*bbH/bbW). The two canvas extents are independent
// axes; on the calibrated canvas the downstream 2.5x zoom and bias constants
// absorbed the aspect error, and on any other canvas the height is ~40% off,
// centring the window far from the reticle: zero pixels, exactly as dumped.
//
// Fix - scale each axis by ITS OWN live extent, normalised so the calibrated
// canvas reproduces the accepted net framing EXACTLY on both axes:
//     W = 4 * halfW           (= live hide, 4134.312 on the calibrated canvas)
//     H = KH * halfH          KH = (4134.312 * bbH/bbW) / 336.549 = 8.858026
//                             (H = 2981.157 on the calibrated canvas - the
//                              exact height the accepted 2.5x/bias tuned on)
//     TLx = (512 - W)/2
//     TLy = (512 - H)/2 - H * kbias     (kbias 0.0772547, scale-free, reused)
// then jump into the untouched 3BR scale tail for the accepted 2.5x zoom
// about (256,256) and the RSSetViewports call, reusing 3BR's constants and
// buffer. Any non-positive or NaN live value falls back to the complete
// accepted 3BR thunk.
//
// defsyms: CAPTURE_VIEWPORT=0x2AE774, BASE_Y=0x2A8364, LIVE_HIDE_X=0x2A8368,
//          K512=0x2F9BD0, KHALF=0x2F9BD4, KBIAS=0x2F9BD8, VPBUF=0x2F9BE8,
//          S3BR_SCALE_TAIL=0x2F9B29, S3BR_THUNK=0x2F9A90

.intel_syntax noprefix
.text

.globl s3cq_per_axis_viewport
s3cq_per_axis_viewport:
    sub     rsp, 0x28
    movups  xmm0, xmmword ptr [rip + CAPTURE_VIEWPORT]
    movups  xmmword ptr [rip + VPBUF], xmm0
    mov     rax, qword ptr [rip + CAPTURE_VIEWPORT + 0x10]
    mov     qword ptr [rip + VPBUF + 0x10], rax

    movss   xmm5, dword ptr [rip + LIVE_HIDE_X]     // W = 4*halfW, live
    ucomiss xmm5, xmm5
    jp      .Lfallback
    xorps   xmm1, xmm1
    comiss  xmm5, xmm1
    jbe     .Lfallback
    movss   xmm4, dword ptr [rip + BASE_Y]          // halfH, live
    ucomiss xmm4, xmm4
    jp      .Lfallback
    comiss  xmm4, xmm1
    jbe     .Lfallback

    movss   dword ptr [rip + VPBUF + 8], xmm5       // W
    movss   xmm1, dword ptr [rip + K512]
    subss   xmm1, xmm5
    mulss   xmm1, dword ptr [rip + KHALF]
    movss   dword ptr [rip + VPBUF], xmm1           // TLx = (512 - W)/2

    mulss   xmm4, dword ptr [rip + s3cq_kh]         // H = KH * halfH
    movss   dword ptr [rip + VPBUF + 12], xmm4
    movss   xmm1, dword ptr [rip + K512]
    subss   xmm1, xmm4
    mulss   xmm1, dword ptr [rip + KHALF]           // (512 - H)/2
    movaps  xmm2, xmm4
    mulss   xmm2, dword ptr [rip + KBIAS]           // H * 0.0772547
    subss   xmm1, xmm2
    movss   dword ptr [rip + VPBUF + 4], xmm1       // TopLeftY
    jmp     S3BR_SCALE_TAIL
.Lfallback:
    add     rsp, 0x28
    jmp     S3BR_THUNK

.p2align 4
s3cq_kh:
    .long 0x410DBA7A        // 8.858027f = accepted 2981.160 / 336.549
