// Stage 3BF - Stage 3BE's draw-time framing pin + engine-viewport telemetry.
//
// Stage 3BE headset result (2026-08-28): the captured art is now CONSISTENT
// (health bar + objective) instead of a per-run random element.  The pin
// works - the capture window is deterministic - but it frames the health/
// objective region of the HUD instead of the screen centre.  That means the
// CUI draws' NDC-to-screen mapping is not the centred full-frame mapping the
// {-768,-768,2048,2048} constant assumes.
//
// The missing calibration is the ENGINE'S OWN viewport for these exact draws:
// it defines how NDC maps onto the screen, and therefore where the screen
// centre (the reticle) sits in NDC.  This stage does not guess a new framing.
// The gate now records, for the first 8 captured draws of the session, the
// live viewport and scissor the engine had set at that draw (read BEFORE the
// override), one log line each.  From those numbers the exact final viewport
// is arithmetic, not a fourth guess.
//
// Behaviour is otherwise byte-for-byte Stage 3BE: same guard chain, same
// re-assert of the saved capture viewport/scissor, same tail-jump into
// TitleAdapter_GetActiveTitle.  Idle cost unchanged (one compare + branch).
// The telemetry runs at most 8 times per session, only on captured draws.

.intel_syntax noprefix
.text

.global stage3bf_data
.global stage3bf_draw_gate

// --------------------------------------------------------------------------
// State.  All bytes are zero at build time; the builder proves the whole
// region was free beforehand.
// --------------------------------------------------------------------------
stage3bf_data:
s3bf_reasserts:
    .long 0                       // diagnostic: framings pinned at a draw
s3bf_logged:
    .byte 0                       // one-time proof line emitted
    .byte 0, 0, 0                 // pad
s3bf_vp_budget:
    .long 0                       // engine-viewport samples logged (max 8)
    .long 0                       // pad to 16

// --------------------------------------------------------------------------
// Called in place of `call TitleAdapter_GetActiveTitle` at 0xD0C5 (Draw) and
// 0xD229 (DrawIndexed).  rbx = the hook's saved ID3D11DeviceContext*; every
// volatile register is dead; must end by tail-jumping the real title adapter
// so `al` still carries the active title back to the hook's `cmp al, 6`.
//
// Frame layout after sub rsp, 0x78 (entry rsp % 16 == 8, so the frame is
// 16-aligned at every inner call):
//   [rsp+0x00..0x1F]  callee shadow space
//   [rsp+0x20]        5th vararg slot for LOG
//   [rsp+0x28]        UINT numViewports (in/out for RSGetViewports)
//   [rsp+0x30]        RTV out slot (OMGetRenderTargets)
//   [rsp+0x38]        expected capture target
//   [rsp+0x40]        bound-target match flag
//   [rsp+0x48..0x5F]  D3D11_VIEWPORT out (24 bytes)
//   [rsp+0x60..0x6F]  D3D11_RECT scissor out (16 bytes)
// --------------------------------------------------------------------------
stage3bf_draw_gate:
    cmp BYTE PTR [rip + CAPTURE_ACTIVE_RVA], 0
    je .out
    cmp BYTE PTR [rip + FRAMING_CAPTURED_RVA], 0
    je .out
    cmp BYTE PTR [rip + ACTIVE_TITLE_RVA], 4      // Halo 4 only
    jne .out
    cmp rbx, QWORD PTR [rip + G_CONTEXT_RVA]      // immediate context only
    jne .out
    cmp BYTE PTR [rip + PUBLISHES_AUTHORED_RVA], 0
    mov rax, QWORD PTR [rip + DISCARD_RTV_RVA]
    cmovne rax, QWORD PTR [rip + AUTHORED_RTV_RVA]
    test rax, rax
    jz .out

    sub rsp, 0x78
    mov QWORD PTR [rsp + 0x38], rax               // expected capture target
    mov QWORD PTR [rsp + 0x30], 0                 // RTV out slot
    mov rcx, rbx
    mov edx, 1
    lea r8, [rsp + 0x30]
    xor r9d, r9d                                  // no DSV out
    mov rax, QWORD PTR [rbx]
    call QWORD PTR [rax + 0x2C8]                  // OMGetRenderTargets
    mov rcx, QWORD PTR [rsp + 0x30]               // bound RTV, AddRef'd
    test rcx, rcx
    jz .done
    cmp rcx, QWORD PTR [rsp + 0x38]
    sete BYTE PTR [rsp + 0x40]
    mov rax, QWORD PTR [rcx]
    call QWORD PTR [rax + 0x10]                   // IUnknown::Release
    cmp BYTE PTR [rsp + 0x40], 0
    je .done                                      // engine's own sub-target

    // ---- telemetry: the engine's live framing at this captured draw ------
    cmp DWORD PTR [rip + s3bf_vp_budget], 8
    jae .pin
    inc DWORD PTR [rip + s3bf_vp_budget]
    mov DWORD PTR [rsp + 0x28], 1                 // read one viewport
    mov rcx, rbx
    lea rdx, [rsp + 0x28]
    lea r8, [rsp + 0x48]
    mov rax, QWORD PTR [rbx]
    call QWORD PTR [rax + 0x2F8]                  // RSGetViewports
    mov DWORD PTR [rsp + 0x28], 1                 // read one scissor rect
    mov rcx, rbx
    lea rdx, [rsp + 0x28]
    lea r8, [rsp + 0x60]
    mov rax, QWORD PTR [rbx]
    call QWORD PTR [rax + 0x300]                  // RSGetScissorRects
    // LOG("S3BF vp: tl=%d,%d wh=%dx%d", (int)TLx, (int)TLy, (int)W, (int)H)
    cvttss2si edx, DWORD PTR [rsp + 0x48]         // TopLeftX
    cvttss2si r8d, DWORD PTR [rsp + 0x4C]         // TopLeftY
    cvttss2si r9d, DWORD PTR [rsp + 0x50]         // Width
    cvttss2si eax, DWORD PTR [rsp + 0x54]         // Height
    mov DWORD PTR [rsp + 0x20], eax               // 5th vararg on stack
    lea rcx, [rip + s3bf_msg_vp]
    call LOG_RVA
    // LOG("S3BF sc: %d,%d..%d,%d", left, top, right, bottom)
    mov edx, DWORD PTR [rsp + 0x60]
    mov r8d, DWORD PTR [rsp + 0x64]
    mov r9d, DWORD PTR [rsp + 0x68]
    mov eax, DWORD PTR [rsp + 0x6C]
    mov DWORD PTR [rsp + 0x20], eax
    lea rcx, [rip + s3bf_msg_sc]
    call LOG_RVA

.pin:
    // ---- Stage 3BE behaviour: force the saved capture framing back -------
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
    inc DWORD PTR [rip + s3bf_reasserts]
    cmp BYTE PTR [rip + s3bf_logged], 0
    jne .done
    mov BYTE PTR [rip + s3bf_logged], 1
    lea rcx, [rip + s3bf_msg]
    call LOG_RVA
.done:
    add rsp, 0x78
.out:
    jmp TITLE_ADAPTER_RVA

// --------------------------------------------------------------------------
s3bf_msg:
    .asciz "S3BF: H4 captured draw reached with the capture target bound; pinning the centre viewport at every such draw"
s3bf_msg_vp:
    .asciz "S3BF vp: engine viewport at captured draw tl=%d,%d wh=%dx%d"
s3bf_msg_sc:
    .asciz "S3BF sc: engine scissor at captured draw %d,%d..%d,%d"
