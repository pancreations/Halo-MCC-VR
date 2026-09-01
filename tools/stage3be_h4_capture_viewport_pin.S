// Stage 3BE - pin the Halo 4 capture framing at every captured draw.
//
// Evidence chain (2026-08-28):
//
//   * Stage 3BD implemented the intended 4x centre framing ({-768,-768,
//     2048,2048}, private constant at 0x2FB800) and the headset result was
//     "some other random element ... IT DIDNT SHRINK".  A 4x viewport change
//     that produces no size change on the captured art means the framing this
//     mod sets is NOT the framing the draws execute with.
//   * The shipped binary already re-asserts viewport+scissor after every
//     mid-capture scene-target rebind (VR_RedirectRenderTargets, 0x2FFDE..
//     0x30010, counters "9 exact capture OM reroutes / 3 captures").  Still
//     wrong on the headset - so the engine (re)sets its own viewport AFTER
//     the rebind, between our re-assert and the batched draw flush.  SCENEPROBE
//     measured exactly that: the same learned RTV bound with a 947x683
//     viewport at one moment and a full-raster viewport at another.
//   * The only place that cannot be raced is the draw itself.  This DLL
//     already owns production MinHook detours on ID3D11DeviceContext::Draw
//     (Halo2DrawCensusHook, 0xD0A0) and ::DrawIndexed
//     (Halo2DrawIndexedCensusHook, 0xD200); both begin with
//     `call TitleAdapter_GetActiveTitle` (0x879C0) after stashing the context
//     in rbx.  Those two 5-byte call sites are retargeted here.
//
// stage3be_draw_gate runs in place of that title call and tail-jumps into it,
// so the hooks' own `cmp al,6` Halo 2 logic is untouched.  While a Halo 4
// authored capture is active AND the currently bound render target IS the
// capture target, the saved capture viewport+scissor (the exact ones
// BeginAuthoredReticleCaptureInternal stored at 0x2AE774/0x2AE78C) are set
// back immediately before the draw.  Nothing can intervene between this and
// the draw call, so every captured draw executes with the centred 4x framing
// regardless of what the engine set in the meantime.
//
// Cost when idle (every title, every draw): one byte compare and a branch.
// Deterministic, allocation-free.  The OMGetRenderTargets/Release pair runs
// only while a Halo 4 capture is live (bounded sample frames), and only adds
// ref-count traffic, never allocation.
//
// Fail-open: if the capture target is never bound, or the title is not
// Halo 4, or the context is not the game's immediate context, behaviour is
// byte-identical to Stage 3BD.

.intel_syntax noprefix
.text

.global stage3be_data
.global stage3be_draw_gate

// --------------------------------------------------------------------------
// State.  All bytes are zero at build time; the builder proves the region
// was free beforehand.
// --------------------------------------------------------------------------
stage3be_data:
s3be_reasserts:
    .long 0                       // diagnostic: framings pinned at a draw
s3be_logged:
    .byte 0                       // one-time proof line emitted
    .byte 0, 0, 0                 // pad to 8

// --------------------------------------------------------------------------
// Called in place of `call TitleAdapter_GetActiveTitle` at 0xD0C5 (Draw) and
// 0xD229 (DrawIndexed).  Register contract at both sites, from the compiled
// prologues: rbx = the hook's saved ID3D11DeviceContext*, edi/esi(/ebp) hold
// the saved draw arguments, every volatile register is dead.  Must end by
// tail-jumping the real title adapter so `al` still returns the active title
// to the hook's own `cmp al, 6`.
// --------------------------------------------------------------------------
stage3be_draw_gate:
    cmp BYTE PTR [rip + CAPTURE_ACTIVE_RVA], 0
    je .out
    cmp BYTE PTR [rip + FRAMING_CAPTURED_RVA], 0
    je .out
    cmp BYTE PTR [rip + ACTIVE_TITLE_RVA], 4      // Halo 4 only
    jne .out
    cmp rbx, QWORD PTR [rip + G_CONTEXT_RVA]      // immediate context only
    jne .out
    // The live capture target, selected exactly the way the reroute selects
    // it (publishesAuthored ? authored : discard).
    cmp BYTE PTR [rip + PUBLISHES_AUTHORED_RVA], 0
    mov rax, QWORD PTR [rip + DISCARD_RTV_RVA]
    cmovne rax, QWORD PTR [rip + AUTHORED_RTV_RVA]
    test rax, rax
    jz .out

    sub rsp, 0x38
    mov QWORD PTR [rsp + 0x28], rax               // expected capture target
    mov QWORD PTR [rsp + 0x20], 0                 // RTV out slot
    mov rcx, rbx
    mov edx, 1
    lea r8, [rsp + 0x20]
    xor r9d, r9d                                  // no DSV out
    mov rax, QWORD PTR [rbx]
    call QWORD PTR [rax + 0x2C8]                  // OMGetRenderTargets
    mov rcx, QWORD PTR [rsp + 0x20]               // bound RTV, AddRef'd
    test rcx, rcx
    jz .done                                      // nothing bound: leave alone
    cmp rcx, QWORD PTR [rsp + 0x28]
    sete BYTE PTR [rsp + 0x30]
    mov rax, QWORD PTR [rcx]
    call QWORD PTR [rax + 0x10]                   // IUnknown::Release
    cmp BYTE PTR [rsp + 0x30], 0
    je .done                                      // engine's own sub-target:
                                                  // its viewport is its business
    // The capture target IS what this draw renders to.  Force the framing
    // BeginAuthoredReticleCaptureInternal saved for this exact capture.
    mov rcx, rbx
    mov edx, 1
    lea r8, [rip + CAPTURE_VIEWPORT_RVA]
    mov rax, QWORD PTR [rbx]
    call QWORD PTR [rax + 0x160]                  // RSSetViewports
    mov rcx, rbx
    mov edx, 1
    lea r8, [rip + CAPTURE_SCISSOR_RVA]
    mov rax, QWORD PTR [rbx]
    call QWORD PTR [rax + 0x168]                  // RSSetScissorRects
    inc DWORD PTR [rip + s3be_reasserts]
    cmp BYTE PTR [rip + s3be_logged], 0
    jne .done
    mov BYTE PTR [rip + s3be_logged], 1
    lea rcx, [rip + s3be_msg]
    call LOG_RVA
.done:
    add rsp, 0x38
.out:
    jmp TITLE_ADAPTER_RVA

// --------------------------------------------------------------------------
s3be_msg:
    .asciz "S3BE: H4 captured draw reached with the capture target bound; pinning the 4x centre viewport at every such draw"
