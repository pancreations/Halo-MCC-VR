.intel_syntax noprefix
.section .text

.global stage3x_h4_title_install_wrapper
.global stage3x_h4_pause_b_edge_wrapper
.global stage3x_h4_setup_heartbeat_bridge
.global stage3x_h4_poll_heartbeat_gate
.global stage3x_h4_cleanup_restore_release
.global stage3x_h4_hud_install
.global stage3x_h4_hud_wrapper
.global stage3x_h4_hud_trampoline
.global stage3x_h4_curvature_install
.global stage3x_h4_curvature_wrapper
.global stage3x_h4_hide_local_fp_effect

.extern ORIGINAL_LOG_RVA
.extern TITLE_ACTIVE_RVA
.extern PAUSE_TARGET_RVA
.extern REQUEST_PAUSE_RVA
.extern H4_EFFECT_INSTALLER_RVA
.extern H4_EFFECT_WRAPPER_RVA
.extern H4_MODULE_REF_RVA
.extern H4_INSTALLED_AT_RVA
.extern H4_CLEANUP_SCRATCH_RVA
.extern EFFECT_INSTALL_LOCK_RVA
.extern EFFECT_MODULE_BASE_RVA
.extern CONFIG_HUD_SIZE_RVA
.extern CONFIG_HUD_ASPECT_RVA
.extern CONFIG_HUD_CURVATURE_RVA
.extern CONFIG_HUD_VERTICAL_RVA
.extern HEARTBEAT_RVA
.extern HUD_CONTINUE_RVA
.extern CURV_CONTINUE_RVA
.extern STAGE3X_STATE_RVA
.extern PAUSE_GRACE_RVA
.extern IAT_GET_TICK_COUNT64_RVA
.extern IAT_VIRTUAL_PROTECT_RVA
.extern IAT_GET_CURRENT_PROCESS_RVA
.extern IAT_FLUSH_INSTRUCTION_CACHE_RVA
.extern IAT_FREE_LIBRARY_RVA
.extern H4_POLL_CONTINUE_RVA
.extern H4_POLL_REMOVE_RVA
.extern H4_SETUP_HEARTBEAT_CONTINUE_RVA
.extern H4_INSTALLED_RVA
.extern TLS_INDEX_RVA

.equ H4_EFFECT_SITE,       0x001012D5
.equ H4_HUD_SITE,          0x003F313C
.equ H4_HUD_CONTINUE,      0x003F314A
.equ H4_CURV_SITE,         0x00420D7E
.equ H4_CURV_CONTINUE,     0x00420D98
.equ PAGE_EXECUTE_READWRITE, 0x40
.equ HEARTBEAT_TIMEOUT_MS, 1500
.equ PAUSE_RESUME_GRACE_MS, 2000

.equ STATE_HUD_INSTALLED,  0x01
.equ STATE_CURV_INSTALLED, 0x02
.equ STATE_HUD_LIVE_LOGGED,0x04

// --------------------------------------------------------------------------
// Utility: restore protection and flush instruction cache after a code write.
// rcx=site, edx=len, r8d=oldProtect. Volatile-only helper.
// --------------------------------------------------------------------------
stage3x_restore_protect_flush:
    sub rsp, 0x38
    mov QWORD PTR [rsp+0x20], rcx
    mov DWORD PTR [rsp+0x28], edx
    mov DWORD PTR [rsp+0x2c], r8d
    lea r9, [rsp+0x30]
    call QWORD PTR [rip + IAT_VIRTUAL_PROTECT_RVA]
    call QWORD PTR [rip + IAT_GET_CURRENT_PROCESS_RVA]
    mov rcx, rax
    mov rdx, QWORD PTR [rsp+0x20]
    mov r8d, DWORD PTR [rsp+0x28]
    call QWORD PTR [rip + IAT_FLUSH_INSTRUCTION_CACHE_RVA]
    add rsp, 0x38
    ret

// --------------------------------------------------------------------------
// H4 HUD affine installer. rcx=retained halo4.dll HMODULE/base.
// Returns eax=1 if exact stock or exact Stage3X patch is present and usable.
// --------------------------------------------------------------------------
stage3x_h4_hud_install:
    push rbx
    push rsi
    sub rsp, 0x48
    mov rbx, rcx
    xor esi, esi
    test rbx, rbx
    jz .hud_install_done
    lea rdx, [rbx + H4_HUD_SITE]
    mov QWORD PTR [rsp+0x20], rdx

    // Already our exact 14-byte patch?
    cmp WORD PTR [rdx], 0xB848
    jne .hud_check_stock
    cmp WORD PTR [rdx+0x0a], 0xE0FF
    jne .hud_install_done
    lea rax, [rip + stage3x_h4_hud_wrapper]
    cmp QWORD PTR [rdx+2], rax
    jne .hud_install_done
    cmp WORD PTR [rdx+0x0c], 0x9090
    jne .hud_install_done
    mov esi, 1
    or DWORD PTR [rip + STAGE3X_STATE_RVA], STATE_HUD_INSTALLED
    jmp .hud_install_done

.hud_check_stock:
    // 40 55 48 8D 6C 24 A9 48 81 EC 90 00 00 00
    mov rax, 0x48A9246C8D485540
    cmp QWORD PTR [rdx], rax
    jne .hud_install_done
    cmp DWORD PTR [rdx+8], 0x0090EC81
    jne .hud_install_done
    cmp WORD PTR [rdx+12], 0x0000
    jne .hud_install_done

    lea rax, [rbx + H4_HUD_CONTINUE]
    mov QWORD PTR [rip + HUD_CONTINUE_RVA], rax

    mov rcx, QWORD PTR [rsp+0x20]
    mov edx, 14
    mov r8d, PAGE_EXECUTE_READWRITE
    lea r9, [rsp+0x30]
    call QWORD PTR [rip + IAT_VIRTUAL_PROTECT_RVA]
    test eax, eax
    jz .hud_install_done

    mov rdx, QWORD PTR [rsp+0x20]
    lea rax, [rip + stage3x_h4_hud_wrapper]
    mov WORD PTR [rdx], 0xB848
    mov QWORD PTR [rdx+2], rax
    mov WORD PTR [rdx+0x0a], 0xE0FF
    mov WORD PTR [rdx+0x0c], 0x9090

    mov rcx, rdx
    mov edx, 14
    mov r8d, DWORD PTR [rsp+0x30]
    call stage3x_restore_protect_flush
    mov esi, 1
    or DWORD PTR [rip + STAGE3X_STATE_RVA], STATE_HUD_INSTALLED
    lea rcx, [rip + msg_hud_active]
    call ORIGINAL_LOG_RVA

.hud_install_done:
    mov eax, esi
    add rsp, 0x48
    pop rsi
    pop rbx
    ret

// Replays the exact displaced retail H4 prologue then enters the original body.
stage3x_h4_hud_trampoline:
    push rbp
    lea rbp, [rsp-0x57]
    sub rsp, 0x90
    mov rax, QWORD PTR [rip + HUD_CONTINUE_RVA]
    jmp rax

// Drop-in absolute-jump target at halo4.dll+0x3F313C. It lets retail finish
// first, then applies the exact V11 affine/height transform to the returned
// gameplay CUI root. H4 pause presentation is excluded.
stage3x_h4_hud_wrapper:
    push rbx
    sub rsp, 0x30
    mov rbx, rcx
    call stage3x_h4_hud_trampoline

    cmp QWORD PTR [rip + H4_MODULE_REF_RVA], 0
    je .hud_wrapper_done
    call PAUSE_TARGET_RVA
    test al, al
    jne .hud_wrapper_done
    // Exact current H4 gameplay-CUI TLS admission. The renderer increments
    // TLS +0x38C around the gameplay CUI root; shell/pause/menu calls stay stock.
    mov r10d, DWORD PTR [rip + TLS_INDEX_RVA]
    mov rax, QWORD PTR gs:0x58
    test rax, rax
    jz .hud_wrapper_done
    mov rax, QWORD PTR [rax+r10*8]
    test rax, rax
    jz .hud_wrapper_done
    cmp DWORD PTR [rax+0x38c], 0
    jle .hud_wrapper_done
    test rbx, rbx
    jz .hud_wrapper_done

    mov eax, DWORD PTR [rbx+0x870]
    test eax, eax
    jle .hud_wrapper_done
    dec eax
    cmp eax, 0x60
    jae .hud_wrapper_done
    imul rax, rax, 0x34
    lea rdx, [rbx+rax+0x878]

    // size = clamp(hud_size, .30, 1.0)
    movss xmm0, DWORD PTR [rip + CONFIG_HUD_SIZE_RVA]
    ucomiss xmm0, xmm0
    jp .hud_wrapper_done
    maxss xmm0, DWORD PTR [rip + k_hud_size_min]
    minss xmm0, DWORD PTR [rip + k_one]

    // horizontal = clamp(size * hud_aspect, .15, 1.0)
    movaps xmm1, xmm0
    mulss xmm1, DWORD PTR [rip + CONFIG_HUD_ASPECT_RVA]
    ucomiss xmm1, xmm1
    jp .hud_wrapper_done
    maxss xmm1, DWORD PTR [rip + k_hud_horizontal_min]
    minss xmm1, DWORD PTR [rip + k_one]

    movss xmm2, DWORD PTR [rdx+0x04]
    mulss xmm2, xmm1
    movss DWORD PTR [rdx+0x04], xmm2
    movss xmm2, DWORD PTR [rdx+0x10]
    mulss xmm2, xmm1
    movss DWORD PTR [rdx+0x10], xmm2
    movss xmm2, DWORD PTR [rdx+0x1c]
    mulss xmm2, xmm1
    movss DWORD PTR [rdx+0x1c], xmm2
    movss xmm2, DWORD PTR [rdx+0x28]
    mulss xmm2, xmm1
    movss DWORD PTR [rdx+0x28], xmm2

    movss xmm2, DWORD PTR [rdx+0x08]
    mulss xmm2, xmm0
    movss DWORD PTR [rdx+0x08], xmm2
    movss xmm2, DWORD PTR [rdx+0x14]
    mulss xmm2, xmm0
    movss DWORD PTR [rdx+0x14], xmm2
    movss xmm2, DWORD PTR [rdx+0x20]
    mulss xmm2, xmm0
    movss DWORD PTR [rdx+0x20], xmm2
    movss xmm2, DWORD PTR [rdx+0x2c]
    mulss xmm2, xmm0
    movss DWORD PTR [rdx+0x2c], xmm2

    // Positive user offset raises H4 virtual-screen Y.
    movss xmm3, DWORD PTR [rip + CONFIG_HUD_VERTICAL_RVA]
    ucomiss xmm3, xmm3
    jp .hud_log_once
    maxss xmm3, DWORD PTR [rip + k_hud_vertical_min]
    minss xmm3, DWORD PTR [rip + k_hud_vertical_max]
    movss xmm4, DWORD PTR [rdx+0x2c]
    subss xmm4, xmm3
    movss DWORD PTR [rdx+0x2c], xmm4

.hud_log_once:
    test DWORD PTR [rip + STAGE3X_STATE_RVA], STATE_HUD_LIVE_LOGGED
    jnz .hud_wrapper_done
    or DWORD PTR [rip + STAGE3X_STATE_RVA], STATE_HUD_LIVE_LOGGED
    lea rcx, [rip + msg_hud_live]
    call ORIGINAL_LOG_RVA

.hud_wrapper_done:
    add rsp, 0x30
    pop rbx
    ret

// --------------------------------------------------------------------------
// Native H4 curvature installer + consumer bridge, exact V6 full-layout path.
// --------------------------------------------------------------------------
stage3x_h4_curvature_install:
    push rbx
    push rsi
    sub rsp, 0x48
    mov rbx, rcx
    xor esi, esi
    test rbx, rbx
    jz .curv_done
    lea rdx, [rbx + H4_CURV_SITE]
    mov QWORD PTR [rsp+0x20], rdx

    cmp WORD PTR [rdx], 0xB848
    jne .curv_check_stock
    lea rax, [rip + stage3x_h4_curvature_wrapper]
    cmp QWORD PTR [rdx+2], rax
    jne .curv_done
    cmp WORD PTR [rdx+0x0a], 0xE0FF
    jne .curv_done
    // Remaining bytes must be NOPs.
    mov rax, 0x9090909090909090
    cmp QWORD PTR [rdx+0x0c], rax
    jne .curv_done
    cmp DWORD PTR [rdx+0x14], 0x90909090
    jne .curv_done
    cmp WORD PTR [rdx+0x18], 0x9090
    jne .curv_done
    mov esi, 1
    or DWORD PTR [rip + STAGE3X_STATE_RVA], STATE_CURV_INSTALLED
    jmp .curv_done

.curv_check_stock:
    // f3 0f 10 8f e8 01 00 00 f3 0f 11 4d a4 f3 0f 10
    mov rax, 0x000001E88F100FF3
    cmp QWORD PTR [rdx], rax
    jne .curv_fail_log
    mov rax, 0x100FF3A44D110FF3
    cmp QWORD PTR [rdx+8], rax
    jne .curv_fail_log
    mov rax, 0x110FF3000001EC8F
    cmp QWORD PTR [rdx+16], rax
    jne .curv_fail_log
    cmp WORD PTR [rdx+24], 0xAC4D
    jne .curv_fail_log

    lea rax, [rbx + H4_CURV_CONTINUE]
    mov QWORD PTR [rip + CURV_CONTINUE_RVA], rax
    mov rcx, QWORD PTR [rsp+0x20]
    mov edx, 26
    mov r8d, PAGE_EXECUTE_READWRITE
    lea r9, [rsp+0x30]
    call QWORD PTR [rip + IAT_VIRTUAL_PROTECT_RVA]
    test eax, eax
    jz .curv_fail_log

    mov rdx, QWORD PTR [rsp+0x20]
    lea rax, [rip + stage3x_h4_curvature_wrapper]
    mov WORD PTR [rdx], 0xB848
    mov QWORD PTR [rdx+2], rax
    mov WORD PTR [rdx+0x0a], 0xE0FF
    mov rax, 0x9090909090909090
    mov QWORD PTR [rdx+0x0c], rax
    mov DWORD PTR [rdx+0x14], 0x90909090
    mov WORD PTR [rdx+0x18], 0x9090

    mov rcx, rdx
    mov edx, 26
    mov r8d, DWORD PTR [rsp+0x30]
    call stage3x_restore_protect_flush
    mov esi, 1
    or DWORD PTR [rip + STAGE3X_STATE_RVA], STATE_CURV_INSTALLED
    lea rcx, [rip + msg_hud_full_active]
    call ORIGINAL_LOG_RVA
    jmp .curv_done

.curv_fail_log:
    lea rcx, [rip + msg_curv_refused]
    call ORIGINAL_LOG_RVA
.curv_done:
    mov eax, esi
    add rsp, 0x48
    pop rsi
    pop rbx
    ret

stage3x_h4_curvature_wrapper:
    pushfq
    push rax
    push r10
    cmp QWORD PTR [rip + H4_MODULE_REF_RVA], 0
    je .curv_stock
    mov r10d, DWORD PTR [rip + TLS_INDEX_RVA]
    mov rax, QWORD PTR gs:0x58
    test rax, rax
    jz .curv_stock
    mov rax, QWORD PTR [rax+r10*8]
    test rax, rax
    jz .curv_stock
    cmp DWORD PTR [rax+0x38c], 0
    jle .curv_stock
    movss xmm1, DWORD PTR [rip + CONFIG_HUD_CURVATURE_RVA]
    ucomiss xmm1, xmm1
    jp .curv_stock
    maxss xmm1, DWORD PTR [rip + k_zero]
    minss xmm1, DWORD PTR [rip + k_one]
    mulss xmm1, DWORD PTR [rip + k_two]
    jmp .curv_write
.curv_stock:
    movss xmm1, DWORD PTR [rdi+0x1e8]
.curv_write:
    movss DWORD PTR [rbp-0x5c], xmm1
    movss xmm1, DWORD PTR [rdi+0x1ec]
    movss DWORD PTR [rbp-0x54], xmm1
    pop r10
    pop rax
    popfq
    mov r10, QWORD PTR [rip + CURV_CONTINUE_RVA]
    jmp r10

// --------------------------------------------------------------------------
// C-H4-50 title query wrapper. Preserve the exact procedural-reticle policy,
// but while the real H4 camera core owns a loader reference, install the old
// proven local-FP effect splice plus H4 HUD affine/full-layout bridges.
// --------------------------------------------------------------------------
stage3x_h4_title_install_wrapper:
    push rbx
    sub rsp, 0x30
    call TITLE_ACTIVE_RVA
    movzx ebx, al
    cmp bl, 4
    jne .title_done
    mov rcx, QWORD PTR [rip + H4_MODULE_REF_RVA]
    test rcx, rcx
    jz .title_done
    // Existing exact, guarded Stage3H/3V local-FP effect installer.
    call H4_EFFECT_INSTALLER_RVA
    mov rcx, QWORD PTR [rip + H4_MODULE_REF_RVA]
    test rcx, rcx
    jz .title_done
    call stage3x_h4_hud_install
    mov rcx, QWORD PTR [rip + H4_MODULE_REF_RVA]
    test rcx, rcx
    jz .title_done
    call stage3x_h4_curvature_install
.title_done:
    mov eax, ebx
    add rsp, 0x30
    pop rbx
    ret

// --------------------------------------------------------------------------
// Exact old H4 pause repair, ported to the current Stage3V ABI. The wrapper is
// called instead of the normal B-edge logger: it logs the edge, then restores
// stereo before H4 leaves its head-locked pause menu. It also starts a short
// heartbeat grace window so teardown cannot race the first resumed camera.
// --------------------------------------------------------------------------
stage3x_h4_pause_b_edge_wrapper:
    push rbx
    sub rsp, 0x30
    mov rbx, rcx
    call ORIGINAL_LOG_RVA
    call TITLE_ACTIVE_RVA
    cmp al, 4
    jne .pause_done
    cmp QWORD PTR [rip + H4_MODULE_REF_RVA], 0
    je .pause_done
    call PAUSE_TARGET_RVA
    test al, al
    je .pause_done
    xor ecx, ecx
    call REQUEST_PAUSE_RVA
    call QWORD PTR [rip + IAT_GET_TICK_COUNT64_RVA]
    mov QWORD PTR [rip + PAUSE_GRACE_RVA], rax
    lea rcx, [rip + msg_pause_restore]
    call ORIGINAL_LOG_RVA
.pause_done:
    mov rcx, rbx
    add rsp, 0x30
    pop rbx
    ret

// --------------------------------------------------------------------------
// Real H4 camera heartbeat. The 12-byte bridge at SetupDetour+0x47 replaces
// three TLS publication stores after all exact caller/observer checks passed.
// We time-stamp only this validated camera callback, then replay those stores.
// --------------------------------------------------------------------------
stage3x_h4_setup_heartbeat_bridge:
    sub rsp, 0x50
    mov QWORD PTR [rsp+0x20], rdx
    mov QWORD PTR [rsp+0x28], r8
    mov QWORD PTR [rsp+0x30], r9
    mov QWORD PTR [rsp+0x38], r11
    call QWORD PTR [rip + IAT_GET_TICK_COUNT64_RVA]
    mov QWORD PTR [rip + HEARTBEAT_RVA], rax
    mov rdx, QWORD PTR [rsp+0x20]
    mov r8, QWORD PTR [rsp+0x28]
    mov r9, QWORD PTR [rsp+0x30]
    mov r11, QWORD PTR [rsp+0x38]
    add rsp, 0x50
    // RAX was the parent [rsp+0x68] pointer before the bridge. Reload it.
    mov rax, QWORD PTR [rsp+0x68]
    mov BYTE PTR [r11], 1
    mov QWORD PTR [r11+8], rbx
    mov DWORD PTR [r11+0x10], edx
    jmp H4_SETUP_HEARTBEAT_CONTINUE_RVA

// --------------------------------------------------------------------------
// Poll gate. Replays the exact Stage3V admission checks; for an installed H4
// core only, lack of a *validated H4 camera callback* retires ownership after
// a short grace. Pause target and the dedicated B-edge resume grace are exempt.
// --------------------------------------------------------------------------
stage3x_h4_poll_heartbeat_gate:
    // Drop-in replacement for the Stage3V installed-byte load at RVA 0x8431C.
    // CL arrives holding the original H4 admission decision and must remain the
    // sole control byte consumed by the untouched Poll logic below this call.
    movzx ebx, BYTE PTR [rip + H4_INSTALLED_RVA]
    test bl, bl
    jz .poll_gate_done
    test cl, cl
    jz .poll_gate_done

    push rbx
    sub rsp, 0x30
    mov BYTE PTR [rsp+0x20], cl
    call PAUSE_TARGET_RVA
    test al, al
    jne .poll_gate_keep
    call QWORD PTR [rip + IAT_GET_TICK_COUNT64_RVA]
    mov rdx, QWORD PTR [rip + PAUSE_GRACE_RVA]
    test rdx, rdx
    jz .poll_gate_heartbeat
    mov r8, rax
    sub r8, rdx
    cmp r8, PAUSE_RESUME_GRACE_MS
    jb .poll_gate_keep
    mov QWORD PTR [rip + PAUSE_GRACE_RVA], 0
.poll_gate_heartbeat:
    mov rdx, QWORD PTR [rip + HEARTBEAT_RVA]
    test rdx, rdx
    jnz .poll_gate_origin
    mov rdx, QWORD PTR [rip + H4_INSTALLED_AT_RVA]
.poll_gate_origin:
    test rdx, rdx
    jz .poll_gate_keep
    sub rax, rdx
    cmp rax, HEARTBEAT_TIMEOUT_MS
    jb .poll_gate_keep
    mov BYTE PTR [rsp+0x20], 0
.poll_gate_keep:
    mov cl, BYTE PTR [rsp+0x20]
    add rsp, 0x30
    pop rbx
.poll_gate_done:
    ret

// Existing Stage3V narrow effect action was finite-Z only. Stage3X keeps the
// validated local/first-person admission boundary but displaces the selected
// matrix completely out of play space on all three axes.
stage3x_h4_hide_local_fp_effect:
    mov DWORD PTR [rax+0x28], 0x461c4000
    mov DWORD PTR [rax+0x2c], 0x461c4000
    mov DWORD PTR [rax+0x30], 0x461c4000
    ret

// --------------------------------------------------------------------------
// Exact inline-code restoration used at successful H4 teardown.
// Each helper returns eax=1 for exact stock or exact Stage3X/current expected
// patch; eax=0 for unknown bytes or VirtualProtect failure. Unknown bytes are
// never overwritten.
// --------------------------------------------------------------------------
stage3x_restore_effect:
    push rbx
    sub rsp, 0x40
    mov rbx, rcx
    lea rdx, [rbx + H4_EFFECT_SITE]
    mov QWORD PTR [rsp+0x20], rdx
    // Already stock?
    mov rax, 0x8B4918B644394166
    cmp QWORD PTR [rdx], rax
    jne .restore_effect_check_patch
    mov rax, 0xFBD5E8C1950F41D7
    cmp QWORD PTR [rdx+8], rax
    jne .restore_effect_fail
    cmp WORD PTR [rdx+16], 0xFFFF
    jne .restore_effect_fail
    jmp .restore_effect_success
.restore_effect_check_patch:
    cmp WORD PTR [rdx], 0x25FF
    jne .restore_effect_fail
    cmp DWORD PTR [rdx+2], 0
    jne .restore_effect_fail
    lea rax, [rip + H4_EFFECT_WRAPPER_RVA]
    cmp QWORD PTR [rdx+6], rax
    jne .restore_effect_fail
    cmp DWORD PTR [rdx+14], 0x90909090
    jne .restore_effect_fail

    mov rcx, rdx
    mov edx, 18
    mov r8d, PAGE_EXECUTE_READWRITE
    lea r9, [rsp+0x30]
    call QWORD PTR [rip + IAT_VIRTUAL_PROTECT_RVA]
    test eax, eax
    jz .restore_effect_fail
    mov rdx, QWORD PTR [rsp+0x20]
    mov rax, 0x8B4918B644394166
    mov QWORD PTR [rdx], rax
    mov rax, 0xFBD5E8C1950F41D7
    mov QWORD PTR [rdx+8], rax
    mov WORD PTR [rdx+16], 0xFFFF
    mov rcx, rdx
    mov edx, 18
    mov r8d, DWORD PTR [rsp+0x30]
    call stage3x_restore_protect_flush
    jmp .restore_effect_success
.restore_effect_success:
    mov DWORD PTR [rip + EFFECT_INSTALL_LOCK_RVA], 0
    mov QWORD PTR [rip + EFFECT_MODULE_BASE_RVA], 0
    mov eax, 1
    jmp .restore_effect_done
.restore_effect_fail:
    xor eax, eax
.restore_effect_done:
    add rsp, 0x40
    pop rbx
    ret

stage3x_restore_hud:
    sub rsp, 0x48
    lea rdx, [rcx + H4_HUD_SITE]
    mov QWORD PTR [rsp+0x20], rdx
    mov rax, 0x48A9246C8D485540
    cmp QWORD PTR [rdx], rax
    jne .restore_hud_check_patch
    cmp DWORD PTR [rdx+8], 0x0090EC81
    jne .restore_hud_fail
    cmp WORD PTR [rdx+12], 0
    jne .restore_hud_fail
    mov eax, 1
    jmp .restore_hud_done
.restore_hud_check_patch:
    cmp WORD PTR [rdx], 0xB848
    jne .restore_hud_fail
    lea rax, [rip + stage3x_h4_hud_wrapper]
    cmp QWORD PTR [rdx+2], rax
    jne .restore_hud_fail
    cmp WORD PTR [rdx+0x0a], 0xE0FF
    jne .restore_hud_fail
    cmp WORD PTR [rdx+0x0c], 0x9090
    jne .restore_hud_fail
    mov rcx, rdx
    mov edx, 14
    mov r8d, PAGE_EXECUTE_READWRITE
    lea r9, [rsp+0x30]
    call QWORD PTR [rip + IAT_VIRTUAL_PROTECT_RVA]
    test eax, eax
    jz .restore_hud_fail
    mov rdx, QWORD PTR [rsp+0x20]
    mov rax, 0x48A9246C8D485540
    mov QWORD PTR [rdx], rax
    mov DWORD PTR [rdx+8], 0x0090EC81
    mov WORD PTR [rdx+12], 0
    mov rcx, rdx
    mov edx, 14
    mov r8d, DWORD PTR [rsp+0x30]
    call stage3x_restore_protect_flush
    mov eax, 1
    jmp .restore_hud_done
.restore_hud_fail:
    xor eax, eax
.restore_hud_done:
    add rsp, 0x48
    ret

stage3x_restore_curvature:
    sub rsp, 0x48
    lea rdx, [rcx + H4_CURV_SITE]
    mov QWORD PTR [rsp+0x20], rdx
    mov rax, 0x000001E88F100FF3
    cmp QWORD PTR [rdx], rax
    jne .restore_curv_check_patch
    mov rax, 0x100FF3A44D110FF3
    cmp QWORD PTR [rdx+8], rax
    jne .restore_curv_fail
    mov rax, 0x110FF3000001EC8F
    cmp QWORD PTR [rdx+16], rax
    jne .restore_curv_fail
    cmp WORD PTR [rdx+24], 0xAC4D
    jne .restore_curv_fail
    mov eax, 1
    jmp .restore_curv_done
.restore_curv_check_patch:
    cmp WORD PTR [rdx], 0xB848
    jne .restore_curv_fail
    lea rax, [rip + stage3x_h4_curvature_wrapper]
    cmp QWORD PTR [rdx+2], rax
    jne .restore_curv_fail
    cmp WORD PTR [rdx+0x0a], 0xE0FF
    jne .restore_curv_fail
    mov rax, 0x9090909090909090
    cmp QWORD PTR [rdx+0x0c], rax
    jne .restore_curv_fail
    cmp DWORD PTR [rdx+0x14], 0x90909090
    jne .restore_curv_fail
    cmp WORD PTR [rdx+0x18], 0x9090
    jne .restore_curv_fail
    mov rcx, rdx
    mov edx, 26
    mov r8d, PAGE_EXECUTE_READWRITE
    lea r9, [rsp+0x30]
    call QWORD PTR [rip + IAT_VIRTUAL_PROTECT_RVA]
    test eax, eax
    jz .restore_curv_fail
    mov rdx, QWORD PTR [rsp+0x20]
    mov rax, 0x000001E88F100FF3
    mov QWORD PTR [rdx], rax
    mov rax, 0x100FF3A44D110FF3
    mov QWORD PTR [rdx+8], rax
    mov rax, 0x110FF3000001EC8F
    mov QWORD PTR [rdx+16], rax
    mov WORD PTR [rdx+24], 0xAC4D
    mov rcx, rdx
    mov edx, 26
    mov r8d, DWORD PTR [rsp+0x30]
    call stage3x_restore_protect_flush
    mov eax, 1
    jmp .restore_curv_done
.restore_curv_fail:
    xor eax, eax
.restore_curv_done:
    add rsp, 0x48
    ret

// Final successful camera-core teardown lands here instead of Stage3V's old
// log+FreeLibrary helper. Restore every direct halo4.dll splice first; only
// then log "stock again" and release the retained HMODULE. On any unknown
// byte state, keep the pin and fail closed rather than touching unknown code.
stage3x_h4_cleanup_restore_release:
    push rbx
    push rsi
    sub rsp, 0x38
    mov rbx, rcx
    mov DWORD PTR [rsp+0x20], edx
    mov rsi, QWORD PTR [rip + H4_CLEANUP_SCRATCH_RVA]
    test rsi, rsi
    jz .cleanup_log_only

    mov rcx, rsi
    call stage3x_restore_effect
    test eax, eax
    jz .cleanup_restore_failed
    mov rcx, rsi
    call stage3x_restore_hud
    test eax, eax
    jz .cleanup_restore_failed
    mov rcx, rsi
    call stage3x_restore_curvature
    test eax, eax
    jz .cleanup_restore_failed

    mov QWORD PTR [rip + HEARTBEAT_RVA], 0
    mov QWORD PTR [rip + HUD_CONTINUE_RVA], 0
    mov QWORD PTR [rip + CURV_CONTINUE_RVA], 0
    mov DWORD PTR [rip + STAGE3X_STATE_RVA], 0
    mov QWORD PTR [rip + PAUSE_GRACE_RVA], 0
    mov rcx, rbx
    mov edx, DWORD PTR [rsp+0x20]
    call ORIGINAL_LOG_RVA
    mov QWORD PTR [rip + H4_CLEANUP_SCRATCH_RVA], 0
    mov rcx, rsi
    call QWORD PTR [rip + IAT_FREE_LIBRARY_RVA]
    jmp .cleanup_done

.cleanup_log_only:
    mov rcx, rbx
    mov edx, DWORD PTR [rsp+0x20]
    call ORIGINAL_LOG_RVA
    jmp .cleanup_done

.cleanup_restore_failed:
    lea rcx, [rip + msg_cleanup_failed]
    call ORIGINAL_LOG_RVA
    // Keep H4_CLEANUP_SCRATCH_RVA nonzero: this is our retained safety pin.
.cleanup_done:
    add rsp, 0x38
    pop rsi
    pop rbx
    ret

.align 4
k_zero:               .long 0x00000000
k_one:                .long 0x3f800000
k_two:                .long 0x40000000
k_hud_size_min:       .long 0x3e99999a
k_hud_horizontal_min: .long 0x3e19999a
k_hud_vertical_min:   .long 0xc3960000
k_hud_vertical_max:   .long 0x43960000

msg_hud_active:
    .asciz "Halo 4 HUD affine-scale bridge ACTIVE: HUD size/aspect scale gameplay CUI basis + centre-relative origin"
msg_hud_live:
    .asciz "Halo 4 HUD affine-scale LIVE: gameplay CUI root basis and origin scaled; legacy scalar left untouched"
msg_hud_full_active:
    .asciz "Halo 4 HUD full-layout extension ACTIVE: HUD height translates gameplay CUI Y; HUD curvature drives H4 native prop_curvature_theta; size/aspect affine path unchanged"
msg_curv_refused:
    .asciz "Halo 4 HUD curvature NOT INSTALLED: exact +0x420D7E consumer bytes or protection check failed; size/aspect/height remain active"
msg_pause_restore:
    .asciz "Halo 4 pause exit: B edge while head-locked pause target active -> stereo restore requested before Halo 4 leaves its pause menu"
msg_cleanup_failed:
    .asciz "Halo 4 Stage3X teardown: direct inline-code restore refused; retaining halo4.dll pin and leaving unknown bytes untouched"
