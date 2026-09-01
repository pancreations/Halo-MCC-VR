// Stage 3AS payload 1 - safe Halo 4 capture admission for
// VR_RedirectRenderTargets (replaces the Stage 3AQ page at 0x2F9000).
//
// The 2026-08-28 07:38 headset session froze at the pause-resume "gameplay ->
// loading" transition (STALL, game stopped presenting). Cause: the Stage 3AQ
// admit path re-enters the function body with the raster eye unvalidated.
// The C++ re-checks g_reticleCaptureState.active inside the capture block; if
// a teardown clears it between our gate and that check (exactly what a mode
// transition does), execution falls through to the stock path, which reloads
// the eye from its spill slot [rsp+0x44], sign-extends it (cdqe at 0x3007A)
// and indexes g_eyeCacheRtvs with it (0x3007C). eye == -1 reads the qword
// BEFORE the array and binds it as a render target.
//
// Fix: on the admitted path, clamp an out-of-range eye to 0 in BOTH the
// register and the spill slot the body actually reloads from. In the normal
// admitted flow the capture block returns before the eye is ever used; the
// clamp only matters in the teardown race, where it turns a garbage RTV bind
// into one benign eye-0 cache bind.
//
// Entry: jmp from 0x2FF0E (unchanged). r9d = eye, spill at [rsp+0x44]
// (stored at 0x2FEE4, rsp unchanged since). Flags free: the continue edge at
// 0x2FF18 starts with its own test.

.intel_syntax noprefix
.text

.global stage3as_eye_gate

stage3as_eye_gate:
    cmp BYTE PTR [rip + TITLE_BYTE_RVA], 4        // GameTitle::Halo4
    jne .Lstock
    cmp BYTE PTR [rip + CAPTURE_ACTIVE_RVA], 0    // g_reticleCaptureState.active
    je .Lstock
    cmp r9d, 1
    jbe .Ladmit
    xor r9d, r9d
    mov DWORD PTR [rsp + 0x44], r9d               // clamp the spilled eye too
.Ladmit:
    jmp REDIRECT_CONTINUE_RVA

.Lstock:
    // Exact displaced original pair from 0x2FF0E.
    cmp r9d, 1
    ja .Lrefuse
    jmp REDIRECT_CONTINUE_RVA
.Lrefuse:
    jmp REDIRECT_REFUSE_RVA
