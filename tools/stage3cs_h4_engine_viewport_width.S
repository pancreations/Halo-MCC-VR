// Stage 3CS - pin the H4 capture width to the engine's OWN CUI viewport.
//
// Decoded from the accepted 3BR thunk at 0x2F9A90 (this image, byte-exact):
//     sub    rsp,0x28
//     movups xmm0,[rip+0x2AE774]      // seed VPBUF from the capture-viewport SLOT
//     movups [rip+0x2F9BE8],xmm0
//     mov    rax,[rip+0x2AE784]
//     mov    [rip+0x2F9BF8],rax
// 0x2F9AB0:
//     mov    eax,[rip+0x2AEB58]       // bbW
//     test   eax,eax / jz  ...
//     mov    ecx,[rip+0x2AEB5C]       // bbH
//     cvtsi2ss xmm2,eax / cvtsi2ss xmm1,ecx
//     movss  xmm0,[rip+0x2F9BF0]      // WIDTH, read back out of VPBUF+8
//     ... H = W*bbH/bbW, vertical bias, 2.5x zoom, RSSetViewports
//
// There is NO width constant in the framing path: the width is whatever the
// slot at 0x2AE774 holds. The headset logs show the slot tracks the live CUI
// canvas (4*|baseX|), while the engine's own viewport at the captured draw is
// INVARIANT at 4134.312 x 1346.196 @ (-1811,-417) in every session recorded -
// working (canvas 1033.578) and blank (1304.277 / 1344.409 / 1635.814) alike.
//
// Those two numbers are equal ONLY on the calibrated canvas
// (4 x 1033.578 = 4134.312), which is exactly why the capture worked there and
// nowhere else: today the window is pinned 1.58x too wide (16358x11801 instead
// of the accepted 10335x7452), so it frames the wrong HUD art - the black
// square the headset reported.
//
// Fix: seed VPBUF exactly as 3BR does, then overwrite the width with the
// engine's own measured viewport width before rejoining 3BR at 0x2F9AB0, so
// every later step (aspect height, bias, 2.5x zoom, RSSetViewports) is the
// accepted arithmetic on the accepted width, on ANY canvas. The reticle's
// position inside that window stays correct because the selector un-hide is
// live (Stage 3CR, 0x2F99D7 -> 0x2A8368) and cancels the live hide exactly.
//
// defsyms: CAPTURE_VIEWPORT=0x2AE774, VPBUF=0x2F9BE8, S3BR_ASPECT=0x2F9AB0

.intel_syntax noprefix
.text

.globl s3cs_engine_viewport_width
s3cs_engine_viewport_width:
    sub     rsp, 0x28
    movups  xmm0, xmmword ptr [rip + CAPTURE_VIEWPORT]
    movups  xmmword ptr [rip + VPBUF], xmm0
    mov     rax, qword ptr [rip + CAPTURE_VIEWPORT + 0x10]
    mov     qword ptr [rip + VPBUF + 0x10], rax
    movss   xmm0, dword ptr [rip + s3cs_engine_width]
    movss   dword ptr [rip + VPBUF + 8], xmm0
    jmp     S3BR_ASPECT

.p2align 4
s3cs_engine_width:
    .long 0x4581327F        // 4134.312f - the engine's own CUI viewport width
