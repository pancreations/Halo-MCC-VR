// Stage 3BH - adopt the engine's OWN measured CUI framing, and inspect every
// texel of the capture instead of one in 256.
//
// Measured 2026-08-28 18:44 (Stage 3BG telemetry, Steam):
//
//   S3BG vp: engine viewport at captured draw tl=-1811,-417 wh=4134x1346
//
// on seven of eight captured draws (the eighth is Begin's own, read before
// the first override).  Two things follow.
//
// 1. That rect's centre is (-1811+2067, -417+673) = (256,256) - EXACTLY the
//    centre of the 512x512 capture texture.  The engine already centres its
//    CUI layout on whatever target is bound.  Its width/height are also
//    exactly 4x the live CUI half-extents the log reports
//    (4 x 1033.578 = 4134.312, 4 x 336.549 = 1346.196), i.e. precisely what
//    BeginAuthoredReticleCaptureInternal's own (512-W)/2 centring formula
//    produces when fed those numbers.
// 2. It frames +/-128 authored units square.  The H4EK reticle is +/-37, so
//    it would occupy 28.9% of the texture in BOTH axes - the same occupancy
//    Halo 3 and ODST ship.  The Stage 3BD/3BE/3BF/3BG constant
//    ({2048,2048}) instead frames +/-258 x +/-84: wrong aspect, and it was
//    overriding the engine's correct framing at every draw.
//
// So this stage keeps the draw-time pin but points it at the engine's own
// numbers: the builder rewrites the private constant at 0x2FB800 from
// {2048,2048,0,1} to {4134.312,1346.196,0,1}, which makes Begin, the
// existing reroute re-assert, and this gate all agree with the engine.
//
// Second change, to the instrument: Stage 3BG's map sampled ONE texel per
// 16x16 cell - 0.4% of the texture.  The reticle's ticks are 2.2 authored
// units (~4 px) and its arcs are thin outlines, so a blank centre was not
// evidence of absence.  The dump now takes the MAX over all 256 texels of
// each cell, so any ink anywhere in a cell shows up.
//
// Stage 3BE/3BF proved the capture window is now deterministic (consistent
// "health bar + objective" content) but aimed away from the reticle, and the
// 17:25 3BE log proves counters alone cannot say where the window sits.  The
// decisive instrument prescribed after Stage 3BC is a picture of the capture.
// This stage logs one: after the third Halo 4 "reticle upload" stats window
// (game settled, art published), the published capture texture is copied to a
// CPU staging texture and sampled on a 32x32 grid (one texel per 16x16 cell),
// each cell logged as '.' (no ink) or '0'-'9' (intensity).  32 log lines,
// once per session, on the same render thread and cold path the stats line
// already uses.  From that picture plus the H4EK authored layout, the correct
// window offset is measurement, not a fourth guess.
//
// Splices (all on the Stage 3BD base):
//   0xD0C5 / 0xD229 - the census hooks' TitleAdapter calls -> s3bh_draw_gate
//     (identical behaviour to Stage 3BF: pin + first-8 viewport telemetry).
//   0x2763C - the `call LOG` that prints "%s reticle upload: ..." ->
//     s3bh_upload_tap, which forwards the call (copying its five stack
//     varargs into a fresh frame) and afterwards runs the one-time dump.
//
// Disassembly-proven addresses used by the dump (this session):
//   g_device                 0x2AE290 (every CreateTexture2D call in Ensure)
//   g_context                0x2AE298
//   g_authoredReticleTexture 0x2AE440 (CreateTexture2D out at 0x18641-66,
//                            RTV source operand at 0x18451/0x1867B)
//   vtable: CreateTexture2D dev+0x28, GetDesc tex+0x50, CopyResource
//   ctx+0x178, Map ctx+0x70, Unmap ctx+0x78, Release +0x10.
//
// The dump fails open at every step: a null texture/context/device, a failed
// CreateTexture2D or Map, each logs one line and leaves everything else
// exactly as Stage 3BF.

.intel_syntax noprefix
.text

.global stage3bh_data
.global s3bh_draw_gate
.global s3bh_upload_tap

// --------------------------------------------------------------------------
stage3bh_data:
s3bh_reasserts:
    .long 0                       // framings pinned at a draw
s3bh_logged:
    .byte 0                       // one-time pin proof line
    .byte 0, 0, 0
s3bh_vp_budget:
    .long 0                       // engine-viewport samples logged (max 8)
s3bh_windows:
    .long 0                       // H4 upload stats windows seen
s3bh_dumped:
    .byte 0                       // map emitted this session
    .byte 0, 0, 0
    .long 0                       // pad to 24
s3bh_rowbuf:
    .zero 40                      // 32 chars + NUL + pad

// --------------------------------------------------------------------------
// Identical to the Stage 3BF gate: called in place of
// `call TitleAdapter_GetActiveTitle` in both census draw hooks; rbx = the
// hook's saved context; every volatile register is dead; ends by tail-
// jumping the real title adapter so `al` carries the title back.
// Frame after sub rsp,0x78: [0x20] 5th vararg, [0x28] UINT count in/out,
// [0x30] RTV slot, [0x38] expected target, [0x40] match flag,
// [0x48..0x5F] viewport out, [0x60..0x6F] scissor out.
// --------------------------------------------------------------------------
s3bh_draw_gate:
    cmp BYTE PTR [rip + CAPTURE_ACTIVE_RVA], 0
    je .out
    cmp BYTE PTR [rip + FRAMING_CAPTURED_RVA], 0
    je .out
    cmp BYTE PTR [rip + ACTIVE_TITLE_RVA], 4
    jne .out
    cmp rbx, QWORD PTR [rip + G_CONTEXT_RVA]
    jne .out
    cmp BYTE PTR [rip + PUBLISHES_AUTHORED_RVA], 0
    mov rax, QWORD PTR [rip + DISCARD_RTV_RVA]
    cmovne rax, QWORD PTR [rip + AUTHORED_RTV_RVA]
    test rax, rax
    jz .out

    sub rsp, 0x78
    mov QWORD PTR [rsp + 0x38], rax
    mov QWORD PTR [rsp + 0x30], 0
    mov rcx, rbx
    mov edx, 1
    lea r8, [rsp + 0x30]
    xor r9d, r9d
    mov rax, QWORD PTR [rbx]
    call QWORD PTR [rax + 0x2C8]                  // OMGetRenderTargets
    mov rcx, QWORD PTR [rsp + 0x30]
    test rcx, rcx
    jz .done
    cmp rcx, QWORD PTR [rsp + 0x38]
    sete BYTE PTR [rsp + 0x40]
    mov rax, QWORD PTR [rcx]
    call QWORD PTR [rax + 0x10]                   // Release
    cmp BYTE PTR [rsp + 0x40], 0
    je .done

    cmp DWORD PTR [rip + s3bh_vp_budget], 8
    jae .pin
    inc DWORD PTR [rip + s3bh_vp_budget]
    mov DWORD PTR [rsp + 0x28], 1
    mov rcx, rbx
    lea rdx, [rsp + 0x28]
    lea r8, [rsp + 0x48]
    mov rax, QWORD PTR [rbx]
    call QWORD PTR [rax + 0x2F8]                  // RSGetViewports
    mov DWORD PTR [rsp + 0x28], 1
    mov rcx, rbx
    lea rdx, [rsp + 0x28]
    lea r8, [rsp + 0x60]
    mov rax, QWORD PTR [rbx]
    call QWORD PTR [rax + 0x300]                  // RSGetScissorRects
    cvttss2si edx, DWORD PTR [rsp + 0x48]
    cvttss2si r8d, DWORD PTR [rsp + 0x4C]
    cvttss2si r9d, DWORD PTR [rsp + 0x50]
    cvttss2si eax, DWORD PTR [rsp + 0x54]
    mov DWORD PTR [rsp + 0x20], eax
    lea rcx, [rip + s3bh_msg_vp]
    call LOG_RVA
    mov edx, DWORD PTR [rsp + 0x60]
    mov r8d, DWORD PTR [rsp + 0x64]
    mov r9d, DWORD PTR [rsp + 0x68]
    mov eax, DWORD PTR [rsp + 0x6C]
    mov DWORD PTR [rsp + 0x20], eax
    lea rcx, [rip + s3bh_msg_sc]
    call LOG_RVA

.pin:
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
    inc DWORD PTR [rip + s3bh_reasserts]
    cmp BYTE PTR [rip + s3bh_logged], 0
    jne .done
    mov BYTE PTR [rip + s3bh_logged], 1
    lea rcx, [rip + s3bh_msg_pin]
    call LOG_RVA
.done:
    add rsp, 0x78
.out:
    jmp TITLE_ADAPTER_RVA

// --------------------------------------------------------------------------
// Replaces `call LOG` at 0x2763C (the "%s reticle upload: ..." stats line,
// 8 varargs: rdx/r8/r9 + five stack slots at caller [rsp+0x20..0x40]).
// Forwards the call with those five slots copied into a fresh frame, then
// runs the one-time dump.  This stub may clobber exactly what any callee may
// clobber; s3bh_dump preserves the non-volatiles it uses.
// --------------------------------------------------------------------------
s3bh_upload_tap:
    sub rsp, 0x68
    mov rax, QWORD PTR [rsp + 0x90]               // caller [rsp+0x20]
    mov QWORD PTR [rsp + 0x20], rax
    mov rax, QWORD PTR [rsp + 0x98]
    mov QWORD PTR [rsp + 0x28], rax
    mov rax, QWORD PTR [rsp + 0xA0]
    mov QWORD PTR [rsp + 0x30], rax
    mov rax, QWORD PTR [rsp + 0xA8]
    mov QWORD PTR [rsp + 0x38], rax
    mov rax, QWORD PTR [rsp + 0xB0]
    mov QWORD PTR [rsp + 0x40], rax
    call LOG_RVA                                  // the original stats line
    cmp BYTE PTR [rip + ACTIVE_TITLE_RVA], 4
    jne .tap_out
    cmp BYTE PTR [rip + s3bh_dumped], 0
    jne .tap_out
    inc DWORD PTR [rip + s3bh_windows]
    cmp DWORD PTR [rip + s3bh_windows], 3
    jb .tap_out
    mov BYTE PTR [rip + s3bh_dumped], 1
    call s3bh_dump
.tap_out:
    add rsp, 0x68
    ret

// --------------------------------------------------------------------------
// One-shot: copy the published capture texture to a staging texture, map it,
// and log a 32x32 intensity map.  Runs on the render thread, cold path.
// Frame after 4 pushes + sub rsp,0x98 (entry rsp%16==8 -> aligned):
//   [0x20],[0x28] outgoing stack args (Map)
//   [0x30..0x3F]  D3D11_MAPPED_SUBRESOURCE {pData, RowPitch, DepthPitch}
//   [0x40..0x6B]  D3D11_TEXTURE2D_DESC (0x2C bytes)
//   [0x70]        staging texture ptr
//   [0x78]        source texture ptr
// rbx=context, rsi=pData, edi=row, r12=staging survive the per-row LOG.
// --------------------------------------------------------------------------
s3bh_dump:
    push rbx
    push rsi
    push rdi
    push r12
    push r13
    push r14
    sub rsp, 0x98
    mov rax, QWORD PTR [rip + AUTHORED_TEX_RVA]
    test rax, rax
    jz .d_fail
    mov rbx, QWORD PTR [rip + G_CONTEXT_RVA]
    test rbx, rbx
    jz .d_fail
    mov QWORD PTR [rsp + 0x78], rax
    mov rcx, rax
    lea rdx, [rsp + 0x40]
    mov rax, QWORD PTR [rcx]
    call QWORD PTR [rax + 0x50]                   // ID3D11Texture2D::GetDesc
    // MipLevels and ArraySize are deliberately left exactly as the source
    // reports them: CopyResource requires identical dimensions AND mip/array
    // counts, and the authored texture is mipped (the coverage probe walks
    // desc.MipLevels).  Forcing 1 here would make the copy silently do
    // nothing.  Only the CPU-access fields below may differ.
    mov DWORD PTR [rsp + 0x5C], 3                 // Usage = STAGING
    mov DWORD PTR [rsp + 0x60], 0                 // BindFlags = 0
    mov DWORD PTR [rsp + 0x64], 0x20000           // CPUAccess = READ
    mov DWORD PTR [rsp + 0x68], 0                 // MiscFlags = 0
    mov rcx, QWORD PTR [rip + G_DEVICE_RVA]
    test rcx, rcx
    jz .d_fail
    lea rdx, [rsp + 0x40]
    xor r8d, r8d
    lea r9, [rsp + 0x70]
    mov QWORD PTR [rsp + 0x70], 0
    mov rax, QWORD PTR [rcx]
    call QWORD PTR [rax + 0x28]                   // CreateTexture2D
    test eax, eax
    js .d_fail
    mov r12, QWORD PTR [rsp + 0x70]
    test r12, r12
    jz .d_fail
    mov rcx, rbx
    mov rdx, r12
    mov r8, QWORD PTR [rsp + 0x78]
    mov rax, QWORD PTR [rbx]
    call QWORD PTR [rax + 0x178]                  // CopyResource(staging, src)
    mov rcx, rbx
    mov rdx, r12
    xor r8d, r8d
    mov r9d, 1                                    // D3D11_MAP_READ
    mov QWORD PTR [rsp + 0x20], 0                 // MapFlags
    lea rax, [rsp + 0x30]
    mov QWORD PTR [rsp + 0x28], rax
    mov rax, QWORD PTR [rbx]
    call QWORD PTR [rax + 0x70]                   // Map
    test eax, eax
    js .d_release
    mov edx, DWORD PTR [rsp + 0x40]               // Width
    mov r8d, DWORD PTR [rsp + 0x44]               // Height
    mov r9d, DWORD PTR [rsp + 0x50]               // Format
    mov eax, DWORD PTR [rsp + 0x38]               // RowPitch
    mov DWORD PTR [rsp + 0x20], eax
    lea rcx, [rip + s3bh_msg_hdr]
    call LOG_RVA
    mov rsi, QWORD PTR [rsp + 0x30]               // pData
    mov r13d, DWORD PTR [rsp + 0x38]              // RowPitch (zero-extended)
    xor edi, edi                                  // row
.d_row:
    // r10 = first pixel of this cell band = pData + (row*16)*pitch
    mov rax, rdi
    shl rax, 4
    imul rax, r13
    lea r10, [rsi + rax]
    xor r14d, r14d                                // col
.d_col:
    // r11 = top-left pixel of this cell = r10 + col*16*4 bytes
    mov eax, r14d
    shl eax, 6
    lea r11, [r10 + rax]
    xor r8d, r8d                                  // maxv over the whole cell
    mov edx, 16                                   // rows remaining in the cell
.d_cy:
    xor ecx, ecx                                  // dx
.d_cx:
    // v = max(R,G,B,A) of this texel; only the low byte is ever compared,
    // so each cmov may carry high garbage harmlessly.
    mov eax, DWORD PTR [r11 + rcx*4]
    mov r9d, eax
    shr r9d, 8
    cmp al, r9b
    cmovb eax, r9d
    mov r9d, DWORD PTR [r11 + rcx*4]
    shr r9d, 16
    cmp al, r9b
    cmovb eax, r9d
    mov r9d, DWORD PTR [r11 + rcx*4]
    shr r9d, 24
    cmp al, r9b
    cmovb eax, r9d
    movzx eax, al
    cmp r8d, eax
    cmovb r8d, eax
    inc ecx
    cmp ecx, 16
    jb .d_cx
    add r11, r13                                  // next pixel row in the cell
    dec edx
    jnz .d_cy
    // Every one of the cell's 256 texels has now been inspected, so a thin
    // arc or a 2.2-unit tick cannot fall between samples the way it could
    // under Stage 3BG's single-texel probe.
    mov eax, r8d
    cmp eax, 16
    jae .d_ink
    mov al, 0x2E                                  // '.'
    jmp .d_put
.d_ink:
    imul eax, eax, 10
    shr eax, 8
    add al, 0x30                                  // '0' + v*10/256
.d_put:
    lea rdx, [rip + s3bh_rowbuf]
    mov BYTE PTR [rdx + r14], al
    inc r14d
    cmp r14d, 32
    jb .d_col
    mov BYTE PTR [rdx + 32], 0
    mov r8, rdx
    mov edx, edi
    lea rcx, [rip + s3bh_msg_row]
    call LOG_RVA                                  // "S3BH %02u |%s|"
    inc edi
    cmp edi, 32
    jb .d_row
    mov rcx, rbx
    mov rdx, r12
    xor r8d, r8d
    mov rax, QWORD PTR [rbx]
    call QWORD PTR [rax + 0x78]                   // Unmap
.d_release:
    mov rcx, r12
    mov rax, QWORD PTR [r12]
    call QWORD PTR [rax + 0x10]                   // Release staging
    jmp .d_done
.d_fail:
    lea rcx, [rip + s3bh_msg_fail]
    call LOG_RVA
.d_done:
    add rsp, 0x98
    pop r14
    pop r13
    pop r12
    pop rdi
    pop rsi
    pop rbx
    ret

// --------------------------------------------------------------------------
s3bh_msg_pin:
    .asciz "S3BH: H4 captured draw reached with the capture target bound; pinning the engine's own measured CUI framing at every such draw"
s3bh_msg_vp:
    .asciz "S3BH vp: engine viewport at captured draw tl=%d,%d wh=%dx%d"
s3bh_msg_sc:
    .asciz "S3BH sc: engine scissor at captured draw %d,%d..%d,%d"
s3bh_msg_hdr:
    .asciz "S3BH map: published capture %ux%u fmt=%u pitch=%u; 32x32 cells follow ('.'=blank, 0-9=intensity)"
s3bh_msg_row:
    .asciz "S3BH %02u |%s|"
s3bh_msg_fail:
    .asciz "S3BH map: dump unavailable this session (texture/device/map step refused); behaviour otherwise unchanged"
