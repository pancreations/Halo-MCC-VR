// Stage 3AR - Halo 4 Mjolnir visor toggle, separate from the HUD.
//
// H4EK evidence (ui\hud\player_huds\mc_hud):
//
//   player_hud_view_reader.prop_hud_helmet_visible -> container_visor.prop_visible
//   player_hud_view_reader.prop_hud_helmet_visible -> container_visor_glow.prop_visible
//
// so one engine property owns the whole helmet, and those two containers are
// the ONLY widgets in mc_hud carrying prop_parallax_multiplier_x/y. The live
// gameplay CUI stream carries exactly two 0x26/0x27 parallax brackets per pass,
// which matches. Everything drawn inside those brackets is the visor.
//
// The visor is eight polyart_assets - helmet_top/bottom/left/right and
// glow_top/bottom/left/right - all drawn by command 0x20. Dropping only 0x20
// inside a parallax bracket therefore removes the helmet and its glow and
// nothing else. It also cannot unbalance anything: 0x20 is a leaf draw, so the
// 0x28/0x29 transform push/pop pairs and the 0x22/0x23 bitmap pairs inside the
// bracket still execute exactly as they did. (An earlier attempt that dropped
// every command inside the bracket threw the HUD offscreen for precisely that
// reason.)

.intel_syntax noprefix
.text

.global stage3ar_cui_visor_filter
.global stage3ar_config_key

// --------------------------------------------------------------------------
// Mutable state. This payload lands in .s3qd, which the Stage 3X helpers
// already treat as writable data.
// --------------------------------------------------------------------------
stage3ar_visor_depth:
    .byte 0                 // parallax-bracket nesting depth
stage3ar_visor_show:
    .byte 0                 // 0 = visor hidden (default), 1 = visor drawn
    .byte 0, 0

stage3ar_key_name:
    .asciz "halo4_visor"
    .align 16

// --------------------------------------------------------------------------
// Drop-in prologue for Halo4CuiRenderCommandDetour (HaloMCCVR.dll+0x53780).
// rcx = renderer, rdx = command, r8/r9 = the remaining two arguments.
// --------------------------------------------------------------------------
stage3ar_cui_visor_filter:
    // Nothing to do at all when the visor is wanted.
    cmp BYTE PTR [rip + stage3ar_visor_show], 0
    jne .Lpass
    test rdx, rdx
    jz .Lpass

    // This dispatcher also services Halo 4's auxiliary texture pass and its
    // menu/overlay UI, and Halo 4 menus DO use parallax. Admit only the
    // gameplay CUI root, using the exact TLS callback-depth field Stage 3X's
    // HUD bridge already gates on (+0x38C, index at HaloMCCVR.dll+0x2D5E48).
    mov r10d, DWORD PTR [rip + TLS_INDEX_RVA]
    mov rax, QWORD PTR gs:[0x58]
    test rax, rax
    jz .Lpass
    mov rax, QWORD PTR [rax + r10*8]
    test rax, rax
    jz .Lpass
    cmp DWORD PTR [rax + 0x38c], 0
    jle .Lpass

    // Halo4CuiCommandHeader is { int16 command; uint16 payloadSize; }. This is
    // the same pointer the engine itself just dereferenced to dispatch.
    movzx eax, WORD PTR [rdx]
    cmp eax, 0x26
    je .Lenter
    cmp eax, 0x27
    je .Lleave
    cmp eax, 0x20
    jne .Lpass
    cmp BYTE PTR [rip + stage3ar_visor_depth], 0
    je .Lpass
    // Suppress this visor draw. The dispatcher's contract is a bool success,
    // and the command is simply not issued.
    mov al, 1
    ret

.Lenter:
    inc BYTE PTR [rip + stage3ar_visor_depth]
    jmp .Lpass

.Lleave:
    cmp BYTE PTR [rip + stage3ar_visor_depth], 0
    je .Lpass
    dec BYTE PTR [rip + stage3ar_visor_depth]

.Lpass:
    // Exact displaced prologue, then back into the detour body.
    push rbp
    push rsi
    push r13
    lea rbp, [rsp - 0x20]
    jmp DETOUR_CONTINUE_RVA

// --------------------------------------------------------------------------
// Drop-in for the config parser's unknown-key edge (HaloMCCVR.dll+0x5CA0).
// rbx = key, rsi = value, r14b = the parser's zero register.
//
// halomccvr.cfg gains `halo4_visor = 0|1`. Absent key keeps the default, which
// is 0: the visor is off unless it is asked for.
// --------------------------------------------------------------------------
stage3ar_config_key:
    push rax
    push rcx
    push rdx
    lea rcx, [rip + stage3ar_key_name]
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
    je .Lhandled                    // empty value: keep the default
    cmp al, 0x30                    // '0'
    je .Lhide
    mov BYTE PTR [rip + stage3ar_visor_show], 1
    jmp .Lhandled
.Lhide:
    mov BYTE PTR [rip + stage3ar_visor_show], 0

.Lhandled:
    pop rdx
    pop rcx
    pop rax
    jmp CONFIG_CONSUMED_RVA         // key recognised; no "unknown key" line

.Lnot_ours:
    pop rdx
    pop rcx
    pop rax
    // Exact displaced instruction pair.
    cmp BYTE PTR [rbx], r14b
    je CONFIG_CONSUMED_RVA
    jmp CONFIG_UNKNOWN_RVA
