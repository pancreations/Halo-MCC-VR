// Stage 3AW - Halo 4 pause stock-state + authored-reticle capture space.
//
// The reticle and pause transactions are deliberately independent.  A refusal
// in either path leaves the camera core and OpenXR session armed.
//
// Stage 3AV proved that the Halo 4 whole-CUI replay, exact scene-target
// reroutes, and framing reasserts all execute, yet every gameplay capture is
// byte-black.  The H4EK-proven type-0x28 handler pushes the one 0x34-byte
// reticle transform before its descendants consume it.  The normal eye pass
// already writes that exact top entry to hide the flat duplicate; the capture
// replay only read it for telemetry.  This helper derives the capture viewport
// from that selected runtime transform before descendant commands draw.
//
// Called at Halo4CuiRenderCommandBody+0x240 (DLL RVA 0x539C0), immediately
// after the capture branch has safely read the pushed top matrix into the
// caller's [rsp+0x48] local.  At entry:
//
//   rsi  = CUI renderer
//   r14d = validated transform-stack count (1..0x60)
//   r12w = command payload size, consumed by displaced telemetry code
//   edi  = type-0x28 transform ID, consumed by displaced telemetry code
//
// The splice is a CALL.  This helper must finish with the displaced
// `movzx r8d,r12w; cmp edi,0x80000000`; RET and the five splice NOPs preserve
// those flags for the original JE at 0x539CA.

.intel_syntax noprefix
.text

.global stage3aw_capture_begin_gate
.global stage3aw_capture_local_framing
.global stage3aw_pause_stock_gate

// Do not bind/clear the private target on the first unrelated CUI command.
// Wait for the exact H4EK-proven 0x28/0x0C begin marker, then retain the
// capture through the existing full-front-end boundary so delayed batches are
// still caught.  Entered and exited by JMP; all live registers are untouched.
stage3aw_capture_begin_gate:
    cmp BYTE PTR [r15 + r8 + 5], bl // displaced attempted-state check
    jne CAPTURE_BEGIN_CALL_RVA
    cmp BYTE PTR [rsp + 0x21], bl   // beginPayloadReadable
    je CAPTURE_BEGIN_CALL_RVA
    jmp CAPTURE_BEGIN_ATTEMPT_RVA

stage3aw_capture_local_framing:
    // The splice is a CALL.  Its return address makes the caller's original
    // rsp equal to rsp+8; the caller's BoneMatrix local is therefore rsp+0x50.
    lea rdx, [rsp + 0x50]

    // Preserve the normal hide path's finite-matrix contract.  A malformed
    // optional capture fails only this feature and leaves the existing
    // procedural quad eligible.
    xor ecx, ecx
.validate_float:
    mov eax, DWORD PTR [rdx + rcx*4]
    and eax, 0x7fffffff
    cmp eax, 0x7f800000
    jae .refuse
    inc ecx
    cmp ecx, 13
    jb .validate_float

    // The runtime-proven centre form is (-halfWidth,+halfHeight).  Build a
    // symmetric viewport from THIS selected runtime transform rather than a
    // resolution/class constant.  Four times each half-extent gives the
    // intended 2x authored-art magnification in both axes.  8192 is bounded so
    // the resulting viewport stays within D3D11's finite viewport range.
    mov eax, DWORD PTR [rdx + 0x28]
    and eax, 0x7fffffff
    cmp eax, 0x3f800000             // 1.0f
    jb .refuse
    cmp eax, 0x46000000             // 8192.0f
    jae .refuse
    movd xmm0, eax                  // abs(baseX)

    mov ecx, DWORD PTR [rdx + 0x2c]
    and ecx, 0x7fffffff
    cmp ecx, 0x3f800000
    jb .refuse
    cmp ecx, 0x46000000
    jae .refuse
    movd xmm1, ecx                  // abs(baseY)

    // width/height = 4*halfExtent.
    movaps xmm2, xmm0
    addss xmm2, xmm2
    addss xmm2, xmm2               // width
    movaps xmm3, xmm1
    addss xmm3, xmm3
    addss xmm3, xmm3               // height

    // topLeft = (512-extent)/2 = 256-2*halfExtent.
    addss xmm0, xmm0
    addss xmm1, xmm1
    mov eax, 0x43800000             // 256.0f
    movd xmm4, eax
    movaps xmm5, xmm4
    subss xmm4, xmm0               // topLeftX
    subss xmm5, xmm1               // topLeftY

    // This is mod-owned capture state, already active on this exact replay.
    // Later exact scene-target OM rebinds synchronously reassert this viewport
    // before the delayed CUI batches draw.  No COM call is added to the command
    // hot hook.
    cmp BYTE PTR [rip + CAPTURE_ACTIVE_RVA], 0
    je .done
    movss DWORD PTR [rip + CAPTURE_VIEWPORT_RVA], xmm4
    movss DWORD PTR [rip + CAPTURE_VIEWPORT_RVA + 4], xmm5
    movss DWORD PTR [rip + CAPTURE_VIEWPORT_RVA + 8], xmm2
    movss DWORD PTR [rip + CAPTURE_VIEWPORT_RVA + 12], xmm3
    xorps xmm0, xmm0
    movss DWORD PTR [rip + CAPTURE_VIEWPORT_RVA + 16], xmm0
    mov eax, 0x3f800000             // 1.0f
    mov DWORD PTR [rip + CAPTURE_VIEWPORT_RVA + 20], eax
    jmp .done

.refuse:
    // Existing C-H4-48 heartbeat reports this feature-local counter as
    // `write failures`; no logging or allocation is introduced in the hook.
    lock inc QWORD PTR [rip + RETICLE_FAILURES_RVA]

.done:
    movzx r8d, r12w                // displaced 0x539C0
    cmp edi, 0x80000000            // displaced 0x539C4
    ret

// --------------------------------------------------------------------------
// Halo 4 pause stock-screen ownership.
//
// Halo4WrapperBody used to keep consuming the one stock wrapper into private
// eye targets after the compositor switched to the head-locked stock-
// backbuffer quad.  The Stage 3AV headset log consequently reported zero
// stock windows throughout the black pause screen.  This helper runs before
// the armed/stereo admission gate.  While the pause TARGET is active it routes
// only this H4 wrapper invocation through the existing stock tail; the H4 core
// stays armed and the compositor receives the native menu/backbuffer.
//
// H4EK proves that c_start_menu_pause_component owns native pause reason 3:
// its lifetime method sets that bit and its destructor clears it.  Both pinned
// retail editions have the same unique getter at halo4+0xA0AE4 with ABI
// bool __fastcall(int32_t reason).  Latching reason 3 true and observing its
// later false edge is therefore the semantic Resume event.  At that edge this
// helper requests stereo and stamps Stage 3X's existing resume grace.  It does
// not infer menu selection from raw controller input.
//
// Outside pause it clears the feature-local latch and replays the displaced
// armed load exactly.
//
// This is entered by JMP, so the parent function's stack remains live.  Its
// RSP is already 16-byte aligned here; reserve only the Windows x64 shadow
// space for the pause-target query.
// --------------------------------------------------------------------------
stage3aw_pause_stock_gate:
    sub rsp, 0x20
    call PAUSE_TARGET_RVA
    test al, al
    jnz .pause_target

    mov BYTE PTR [rip + s3aw_native_pause_seen], 0
    add rsp, 0x20

    movzx eax, BYTE PTR [rip + H4_ARMED_RVA] // displaced 0x58BED
    jmp H4_ARMED_CONTINUE_RVA

.pause_target:
    // The H4 camera install owns a loader pin while this module reference is
    // nonzero.  A missing pin fails this optional semantic-exit feature to the
    // stock pause presentation; it never disarms the camera core.
    mov rax, QWORD PTR [rip + H4_MODULE_REF_RVA]
    test rax, rax
    je .pause_stock
    add rax, 0x000A0AE4

    // Feature-local fixed-site guard for the unique 51-byte retail getter.
    // Bytes 2..5 are its RIP displacement and are the signature wildcard.
    // A mismatch retains the visible stock pause path and never calls or
    // disables anything.  This is a bounded check, not a hot-hook scan.
    cmp WORD PTR [rax], 0x158b
    jne .pause_stock
    mov rdx, 0x00005825048b4865
    cmp QWORD PTR [rax + 6], rdx
    jne .pause_stock
    mov rdx, 0x4800000090b84100
    cmp QWORD PTR [rax + 14], rdx
    jne .pause_stock
    mov rdx, 0x3200148b49d0048b
    cmp QWORD PTR [rax + 22], rdx
    jne .pause_stock
    mov rdx, 0x0001b80f740238c0
    cmp QWORD PTR [rax + 30], rdx
    jne .pause_stock
    mov rdx, 0x428566e0d3660000
    cmp QWORD PTR [rax + 38], rdx
    jne .pause_stock
    cmp DWORD PTR [rax + 46], 0xc0950f02
    jne .pause_stock
    cmp BYTE PTR [rax + 50], 0xc3
    jne .pause_stock

    mov ecx, 3
    call rax
    test al, al
    jz .pause_maybe_resumed

    mov BYTE PTR [rip + s3aw_native_pause_seen], 1
    jmp .pause_stock

.pause_maybe_resumed:
    cmp BYTE PTR [rip + s3aw_native_pause_seen], 0
    je .pause_stock

    // Native component destruction has proved Resume.  Restore the stereo
    // target and protect its first camera callbacks with the same grace the
    // established Stage 3X B-edge path uses.
    mov BYTE PTR [rip + s3aw_native_pause_seen], 0
    xor ecx, ecx
    call REQUEST_PAUSE_RVA
    call QWORD PTR [rip + IAT_GET_TICK_COUNT64_RVA]
    mov QWORD PTR [rip + PAUSE_GRACE_RVA], rax
    add rsp, 0x20
    movzx eax, BYTE PTR [rip + H4_ARMED_RVA]
    jmp H4_ARMED_CONTINUE_RVA

.pause_stock:
    add rsp, 0x20
    jmp H4_STOCK_TAIL_RVA

.p2align 3, 0
s3aw_native_pause_seen:
    .byte 0
