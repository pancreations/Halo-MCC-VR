// Stage 3CE - the Halo 4 capture framing follows the live CUI layout.
//
// Root cause (2026-08-30 evening, proven from C-H4-48 telemetry): the whole
// capture-framing chain was calibrated to ONE session's layout - the 3BH
// constant W=4134.312 equals that session's live offscreen-hide displacement
// exactly (16:34 log: hide 4134.312, art 609, working crosshair).  Tonight's
// sessions run a different CUI layout: hide 5217.107, base -1304.277/675.936.
// The 3BP selector still un-hid by 4134.312 (826 units short) and the pinned
// viewport still framed the old position, so every capture came out blank
// (pieces/art 0, blankHeld ~200) - the "broken crosshair".  Codex's 3CD
// repointed only the selector shift and left the viewport stale, so art
// stayed 0.
//
// Fix - two edits, one behavior ("framing follows the live layout"):
//  1. The gate call at 0x2FB992 lands here instead of the 3BR thunk.  This
//     thunk builds the private viewport from the LIVE hide displacement
//     (0x2A8368, published as a float by the native-hide path at 0x53E04 on
//     every gameplay CUI pass; C-H4-48 logs it as "offscreen hide"):
//         W   = hideX_live
//         TLx = (512 - W) * 0.5
//         H   = W * bbH / bbW              (3BN raster-aspect, unchanged)
//         TLy = (512 - H) * 0.5 - H * (104/1346.196)   (3BN/3BK bias)
//     then jumps into the 3BR thunk's own scale tail (0x2F9B29), which
//     applies the accepted 2.5x zoom about (256,256), calls RSSetViewports
//     with the same private buffer, and returns.  The 3BR constants
//     (0x2F9BD0..) and buffer (0x2F9BE8) are reused so the zoom, bias and
//     call are byte-identical to the accepted behavior.  A zero, negative
//     or NaN live value (first pass of a level, before any native hide has
//     published) falls back to the complete old 3BR thunk (0x2F9A90).
//  2. The 3BP selector's un-hide amount load at 0x2F99D7 is repointed from
//     the fixed constant (0x2FB800) to the same live value 0x2A8368 (this is
//     exactly the byte change Codex's 3CD proved safe), so the un-hide shift
//     and the discrimination threshold (0.5 * W) track the layout too.
//
// Register contract at the gate call (verified in 3BK/3BR): rbx = the game's
// immediate context; volatile xmm/gpr free; rsp 8-mod-16 (call frame), and
// both the 3BR tail and the fallback re-balance their own sub/add rsp 0x28.
//
// defsyms: CAPTURE_VIEWPORT=0x2AE774, LIVE_HIDE_X=0x2A8368,
//          BBW=0x2AEB58, BBH=0x2AEB5C,
//          K512=0x2F9BD0, KHALF=0x2F9BD4, KBIAS=0x2F9BD8,
//          VPBUF=0x2F9BE8, S3BR_SCALE_TAIL=0x2F9B29, S3BR_THUNK=0x2F9A90

.intel_syntax noprefix
.text

.globl s3ce_live_viewport
s3ce_live_viewport:
    sub     rsp, 0x28                       // matches the 3BR tail's add
    movups  xmm0, xmmword ptr [rip + CAPTURE_VIEWPORT]
    movups  xmmword ptr [rip + VPBUF], xmm0 // seed: stored vp (depth range)
    mov     rax, qword ptr [rip + CAPTURE_VIEWPORT + 0x10]
    mov     qword ptr [rip + VPBUF + 0x10], rax
    movss   xmm5, dword ptr [rip + LIVE_HIDE_X]
    ucomiss xmm5, xmm5
    jp      .Lfallback                      // NaN
    xorps   xmm1, xmm1
    comiss  xmm5, xmm1
    jbe     .Lfallback                      // not yet published (<= 0)
    mov     eax, dword ptr [rip + BBW]
    test    eax, eax
    jz      .Lfallback
    mov     ecx, dword ptr [rip + BBH]
    test    ecx, ecx
    jz      .Lfallback
    movss   dword ptr [rip + VPBUF + 8], xmm5   // W = live hide
    movss   xmm1, dword ptr [rip + K512]
    subss   xmm1, xmm5
    mulss   xmm1, dword ptr [rip + KHALF]
    movss   dword ptr [rip + VPBUF], xmm1       // TLx = (512 - W)/2
    cvtsi2ss xmm2, eax                          // bbW
    cvtsi2ss xmm1, ecx                          // bbH
    movaps  xmm0, xmm5
    mulss   xmm0, xmm1
    divss   xmm0, xmm2                          // H = W * bbH / bbW
    movss   dword ptr [rip + VPBUF + 12], xmm0
    movss   xmm1, dword ptr [rip + K512]
    subss   xmm1, xmm0
    mulss   xmm1, dword ptr [rip + KHALF]       // (512 - H)/2
    movaps  xmm2, xmm0
    mulss   xmm2, dword ptr [rip + KBIAS]       // H * (104/1346.196)
    subss   xmm1, xmm2
    movss   dword ptr [rip + VPBUF + 4], xmm1   // TopLeftY
    jmp     S3BR_SCALE_TAIL                     // 2.5x zoom + RSSetViewports
.Lfallback:
    add     rsp, 0x28
    jmp     S3BR_THUNK                          // exact old behavior
