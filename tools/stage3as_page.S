// Stage 3AS payload 2 - rebuilds the Stage 3AR page at 0x2FA000.
//
// The 2026-08-28 07:38 headset session proved the Stage 3AR visor filter ran
// (its splice and page bytes verified, .s3qd is RWX, and the H4DIAG gameplay
// histogram carried 0x26/0x27/0x20) yet the visor still drew, and the HUD
// showed no damage. Conclusion: the visor's draws are NOT command 0x20 inside
// a parallax bracket of the HOOKED GAMEPLAY root. The H4EK weapon screens all
// carry their own parallax_container (docs/HALO4-CUI-EVIDENCE.md), and this
// session measured THREE brackets per gameplay pass, not the two the visor
// theory predicted - the gameplay brackets are other widgets.
//
// This page therefore:
//   * keeps the exact Stage 3AR gameplay-context behavior (proven harmless),
//   * extends the drop to polyart inside parallax brackets of NON-gameplay
//     CUI roots (the overlay/auxiliary contexts, where the visor most likely
//     draws - it does not follow the HUD sliders, which transform only the
//     gameplay root),
//   * counts what it sees per context and logs the counters every 262144
//     commands (~4s), so the next headset log attributes the visor stream
//     with evidence instead of another guess,
//   * hosts the authored-reticle probe helper (see the builder: splice at
//     0x1DD8B) which measures alpha AND rgb ink of the 8x8 probe mip and
//     logs both every 64th measure - the datum that decides how the Halo 4
//     authored reticle gets its alpha.
//
// halomccvr.cfg key (unchanged): halo4_visor = 0 hides (default), 1 = stock.
// Setting 1 disables every drop this page can make; the counters keep
// counting.

.intel_syntax noprefix
.text

.global stage3as_filter_entry
.global stage3as_config_key
.global stage3as_probe

// ---------------------------------------------------------------------------
// Fixed data block. The two .text splices already in the image jump to
// +0x10 and +0x88, so those offsets are load-bearing and asserted by the
// builder.
// ---------------------------------------------------------------------------
stage3as_gp_depth:
    .byte 0                 // gameplay-root parallax bracket depth
stage3as_visor_show:
    .byte 0                 // 0 = visor hidden (default), 1 = fully stock
stage3as_ngp_depth:
    .byte 0                 // non-gameplay-root parallax bracket depth
    .byte 0
stage3as_key_name:
    .asciz "halo4_visor"

.org 0x10, 0x00
stage3as_filter_entry:
    jmp stage3as_filter_v2

.org 0x88, 0xcc
// ---------------------------------------------------------------------------
// Config parser unknown-key edge (unchanged from Stage 3AR).
// rbx = key, rsi = value, r14b = the parser's zero register.
// ---------------------------------------------------------------------------
stage3as_config_key:
    push rax
    push rcx
    push rdx
    lea rcx, [rip + stage3as_key_name]
    mov rdx, rbx
.Lcompare:
    mov al, BYTE PTR [rcx]
    cmp al, BYTE PTR [rdx]
    jne .Lnot_ours
    test al, al
    je .Lours
    inc rcx
    inc rdx
    jmp .Lcompare
.Lours:
    mov rdx, rsi
    test rdx, rdx
    jz .Lhandled
.Lskip_space:
    mov al, BYTE PTR [rdx]
    cmp al, 0x20
    je .Ladvance
    cmp al, 0x09
    jne .Lvalue
.Ladvance:
    inc rdx
    jmp .Lskip_space
.Lvalue:
    test al, al
    je .Lhandled
    cmp al, 0x30
    je .Lhide
    mov BYTE PTR [rip + stage3as_visor_show], 1
    jmp .Lhandled
.Lhide:
    mov BYTE PTR [rip + stage3as_visor_show], 0
.Lhandled:
    pop rdx
    pop rcx
    pop rax
    jmp CONFIG_CONSUMED_RVA
.Lnot_ours:
    pop rdx
    pop rcx
    pop rax
    cmp BYTE PTR [rbx], r14b
    je CONFIG_CONSUMED_RVA
    jmp CONFIG_UNKNOWN_RVA

// ---------------------------------------------------------------------------
// Dispatcher filter v2. Runs before Halo4CuiRenderCommandDetour's prologue.
// rcx = renderer, rdx = command, r8/r9 = remaining args (all preserved on the
// pass path). rax/r10/r11 are volatile and dead here.
// ---------------------------------------------------------------------------
stage3as_filter_v2:
    inc DWORD PTR [rip + stage3as_cnt_all]
    test rdx, rdx
    jz .Lpass
    movzx eax, WORD PTR [rdx]

    // Context: the renderer increments DLL TLS +0x38C around the hooked
    // gameplay CUI root (the exact Stage 3X HUD-bridge admission).
    mov r10d, DWORD PTR [rip + TLS_INDEX_RVA]
    mov r11, QWORD PTR gs:[0x58]
    test r11, r11
    jz .Lngp
    mov r11, QWORD PTR [r11 + r10*8]
    test r11, r11
    jz .Lngp
    cmp DWORD PTR [r11 + 0x38c], 0
    jg .Lgp

// ----- non-gameplay CUI roots (overlay / auxiliary / menus) -----
.Lngp:
    cmp eax, 0x26
    jne .Lngp_not26
    inc DWORD PTR [rip + stage3as_cnt_n26]
    cmp BYTE PTR [rip + stage3as_ngp_depth], 15
    jae .Ltrace
    inc BYTE PTR [rip + stage3as_ngp_depth]
    jmp .Ltrace
.Lngp_not26:
    cmp eax, 0x27
    jne .Lngp_not27
    cmp BYTE PTR [rip + stage3as_ngp_depth], 0
    je .Ltrace
    dec BYTE PTR [rip + stage3as_ngp_depth]
    jmp .Ltrace
.Lngp_not27:
    cmp eax, 0x22
    jne .Lngp_not22
    inc DWORD PTR [rip + stage3as_cnt_n22]
    jmp .Ltrace
.Lngp_not22:
    cmp eax, 0x20
    jne .Ltrace
    inc DWORD PTR [rip + stage3as_cnt_n20]
    cmp BYTE PTR [rip + stage3as_ngp_depth], 0
    je .Ltrace
    inc DWORD PTR [rip + stage3as_cnt_n20br]
    cmp BYTE PTR [rip + stage3as_visor_show], 0
    jne .Ltrace
    inc DWORD PTR [rip + stage3as_cnt_ndrop]
    mov al, 1
    ret

// ----- the hooked gameplay root (identical to Stage 3AR behavior) -----
.Lgp:
    cmp eax, 0x26
    jne .Lgp_not26
    cmp BYTE PTR [rip + stage3as_gp_depth], 15
    jae .Ltrace
    inc BYTE PTR [rip + stage3as_gp_depth]
    jmp .Ltrace
.Lgp_not26:
    cmp eax, 0x27
    jne .Lgp_not27
    cmp BYTE PTR [rip + stage3as_gp_depth], 0
    je .Ltrace
    dec BYTE PTR [rip + stage3as_gp_depth]
    jmp .Ltrace
.Lgp_not27:
    cmp eax, 0x20
    jne .Ltrace
    cmp BYTE PTR [rip + stage3as_gp_depth], 0
    je .Ltrace
    cmp BYTE PTR [rip + stage3as_visor_show], 0
    jne .Ltrace
    inc DWORD PTR [rip + stage3as_cnt_gdrop]
    mov al, 1
    ret

// ----- throttled trace line -----
.Ltrace:
    mov eax, DWORD PTR [rip + stage3as_cnt_all]
    test eax, 0x3FFFF
    jnz .Lpass
    push rcx
    push rdx
    push r8
    push r9
    sub rsp, 0x38
    mov eax, DWORD PTR [rip + stage3as_cnt_ndrop]
    mov DWORD PTR [rsp + 0x20], eax
    mov eax, DWORD PTR [rip + stage3as_cnt_gdrop]
    mov DWORD PTR [rsp + 0x28], eax
    mov eax, DWORD PTR [rip + stage3as_cnt_n22]
    mov DWORD PTR [rsp + 0x30], eax
    lea rcx, [rip + stage3as_fmt_trace]
    mov edx, DWORD PTR [rip + stage3as_cnt_n26]
    mov r8d, DWORD PTR [rip + stage3as_cnt_n20]
    mov r9d, DWORD PTR [rip + stage3as_cnt_n20br]
    call ORIGINAL_LOG_RVA
    add rsp, 0x38
    pop r9
    pop r8
    pop rdx
    pop rcx

.Lpass:
    // Exact displaced detour prologue, then back into the body.
    push rbp
    push rsi
    push r13
    lea rbp, [rsp - 0x20]
    jmp DETOUR_CONTINUE_RVA

// ---------------------------------------------------------------------------
// Authored-reticle probe helper. Entered by jmp from 0x1DD8B, immediately
// after a successful Map of the 8x8 probe staging in
// MeasureAuthoredReticleCoverage. Contract with the compiled tail at
// PROBE_RESUME_RVA (0x1DDD8): edi = alpha ink, r8 = 0 (the tail passes r8 to
// Unmap as the subresource index). edi arrives 0. [rsp+0x58] = pData,
// [rsp+0x60] = RowPitch; rsp is 16-aligned in this frame.
// ---------------------------------------------------------------------------
stage3as_probe:
    mov rdx, QWORD PTR [rsp + 0x58]
    mov r9d, DWORD PTR [rsp + 0x60]
    xor r8d, r8d
    xor r10d, r10d
.Lprobe_row:
    xor ecx, ecx
.Lprobe_col:
    movzx eax, BYTE PTR [rdx + rcx*4 + 3]
    add edi, eax
    movzx eax, BYTE PTR [rdx + rcx*4]
    add r10d, eax
    movzx eax, BYTE PTR [rdx + rcx*4 + 1]
    add r10d, eax
    movzx eax, BYTE PTR [rdx + rcx*4 + 2]
    add r10d, eax
    inc ecx
    cmp ecx, 8
    jb .Lprobe_col
    mov eax, r9d
    add rdx, rax
    inc r8d
    cmp r8d, 8
    jb .Lprobe_row
    mov DWORD PTR [rip + stage3as_probe_rgb], r10d
    inc DWORD PTR [rip + stage3as_probe_n]
    mov eax, DWORD PTR [rip + stage3as_probe_n]
    test eax, 0x3F
    jnz .Lprobe_out
    sub rsp, 0x20
    lea rcx, [rip + stage3as_fmt_probe]
    mov edx, edi
    mov r8d, r10d
    mov r9d, eax
    call ORIGINAL_LOG_RVA
    add rsp, 0x20
.Lprobe_out:
    xor r8d, r8d
    jmp PROBE_RESUME_RVA

// ---------------------------------------------------------------------------
// Counters and strings (this page is RWX).
// ---------------------------------------------------------------------------
.align 4
stage3as_cnt_all:   .long 0
stage3as_cnt_n26:   .long 0
stage3as_cnt_n20:   .long 0
stage3as_cnt_n20br: .long 0
stage3as_cnt_n22:   .long 0
stage3as_cnt_ndrop: .long 0
stage3as_cnt_gdrop: .long 0
stage3as_probe_rgb: .long 0
stage3as_probe_n:   .long 0

stage3as_fmt_trace:
    .asciz "S3AS ngp CUI: c26 %u c20 %u c20br %u drops %u gpDrops %u c22 %u"
stage3as_fmt_probe:
    .asciz "S3AS reticle probe: alpha %u rgb %u n %u"
