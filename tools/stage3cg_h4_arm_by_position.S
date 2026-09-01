// Stage 3CG - the capture recognizes the reticle by its hidden position.
//
// The 3BP selector armed the capture rescue on a type-0x28 command whose id
// word equals 0xC - a value measured in one session, the last frozen
// calibration in the capture chain (framing and un-hide went live in 3CE).
// Tonight's dumps are byte-empty while captures/hides run, consistent with
// that id no longer matching. This stage replaces the id check with the
// definition itself: the reticle container is THE container sitting at the
// mod's own hidden position. Arm exactly when the container transform X is
// at least half the live hide displacement - only mod-hidden content can be
// there (threshold = 2x the canvas half-width). No measured constant
// remains; a not-yet-published hide (<= 0 or NaN) simply skips arming for
// that pass, which also makes the whole selector a no-op until the first
// native hide publishes (un-hide amount would be 0 anyway).
//
// Entry: the selector's post-render type dispatch, via the re-pointed jmp at
// 0x2F995D (rsp unchanged; rsi = the CUI container, same pointer the rescue
// helper 0x2F99AA indexes; the container transform record is
// [rsi + (idx-1)*0x34 + 0x878] with idx = [rsi+0x870] in 1..0x60, X at +0x28
// - identical addressing to the shipped helper). comiss with a NaN operand
// clears the arm branch (unordered => jb), so no explicit NaN checks.
//
// defsyms: LIVE_HIDE_X=0x2A8368, KHALF=0x2F9BD4 (3BR 0.5f const),
//          ARM_PATH=0x2F9965, SKIP_PATH=0x2F9994

.intel_syntax noprefix
.text

.globl s3cg_arm_by_position
s3cg_arm_by_position:
    test    rsi, rsi
    jz      .Lskip
    mov     ecx, dword ptr [rsi + 0x870]
    test    ecx, ecx
    jz      .Lskip
    cmp     ecx, 0x60
    ja      .Lskip
    lea     eax, [rcx - 1]
    imul    rax, rax, 0x34
    movss   xmm0, dword ptr [rsi + rax + 0x8A0]   // container transform X
    movss   xmm1, dword ptr [rip + LIVE_HIDE_X]
    mulss   xmm1, dword ptr [rip + KHALF]         // threshold = hide/2
    xorps   xmm2, xmm2
    comiss  xmm1, xmm2
    jbe     .Lskip                                // hide not yet live
    comiss  xmm0, xmm1
    jb      .Lskip                                // not at the hidden position
    jmp     ARM_PATH
.Lskip:
    jmp     SKIP_PATH
