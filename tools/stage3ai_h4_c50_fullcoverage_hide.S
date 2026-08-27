.intel_syntax noprefix
.section .text,"ax"
.global stage3ai_effect_bridge
.global stage3ai_effect_install
.global stage3ai_transient_wrapper
.global stage3ai_mode1_particle_patch
.global stage3ai_title_wrapper
.global stage3ai_effect_restore

.extern LOGGER_RVA
.extern TITLE_ACTIVE_RVA
.extern H4_MODULE_REF_RVA
.extern STAGE3X_HUD_INSTALL_RVA
.extern STAGE3X_CURV_INSTALL_RVA
.extern IAT_VIRTUAL_PROTECT_RVA
.extern STAGE3X_RESTORE_PROTECT_FLUSH_RVA

.equ H4_SITE_NEG, 0x1059A2
.equ H4_SITE_HELPER, 0x100EE8
.equ H4_CAVE, 0xB79C10
.equ H4_TRANSIENT, 0x1012D5
.equ H4_ORIG_HELPER, 0x100EBC
.equ H4_TRANSIENT_CONT, 0x1012E7
.equ H4_FP_PARTICLE_GATE_SITE, 0x27BD36
.equ PAGE_EXECUTE_READWRITE, 0x40
.equ FAR_FLOAT, 0x461c4000

# Stage3AI uses the exact public-C50 V11 routing/coverage, but applies the
# current project's suppression policy instead of C50's old reroot policy.
# rcx = effect record, rdx = resolved effect-location matrix.
# The C50 callers select the local first-person weapon-effect families,
# including the negative-designator path missed by the later Stage3H/X splice.
stage3ai_effect_bridge:
    cmp BYTE PTR [rip + fx_installed], 1
    jne .bridge_done
    test rcx, rcx
    jz .bridge_done
    test rdx, rdx
    jz .bridge_done
    mov al, BYTE PTR [rcx+0x4a]
    test al, 0x0f
    jz .bridge_done
    and al, 0xf0
    cmp al, 0xf0
    je .bridge_done
    # Same finite-hide coordinates used by the current Stage3X local-FP policy.
    # Keep pointer/rotation/scale valid; move only the resolved origin out of play.
    mov DWORD PTR [rdx+0x28], FAR_FLOAT
    mov DWORD PTR [rdx+0x2c], FAR_FLOAT
    mov DWORD PTR [rdx+0x30], FAR_FLOAT
    lock inc QWORD PTR [rip + fx_hidden_count]
.bridge_done:
    ret

# Exact C50 +0x1012D5 positive-designator wrapper, routed into the same hide.
stage3ai_transient_wrapper:
    sub rsp, 0x40
    xor eax, eax
    cmp WORD PTR [r14+rsi*4+0x18], ax
    mov rdx, r15
    mov rcx, rdi
    setne r9b
    call QWORD PTR [rip + fx_orig_helper]
    mov QWORD PTR [rsp+0x20], rax
    test rax, rax
    jz .trans_continue
    cmp DWORD PTR [r14+rsi*4+0x14], 0
    jne .trans_continue
    cmp WORD PTR [r14+rsi*4+0x18], 0
    je .trans_continue
    cmp WORD PTR [r14+rsi*4+0x1e], 1
    jne .trans_continue
    cmp WORD PTR [r15+2], 0
    jl .trans_continue
    mov rcx, rdi
    mov rdx, rax
    call stage3ai_effect_bridge
.trans_continue:
    mov rax, QWORD PTR [rsp+0x20]
    add rsp, 0x40
    jmp QWORD PTR [rip + fx_transient_continue]

# rcx = retained halo4.dll base. Returns eax=1 only if every exact C50 splice is live.
stage3ai_effect_install:
    cld
    push rbx
    push rsi
    push rdi
    sub rsp, 0x50
    mov rbx, rcx
    test rbx, rbx
    jz .install_fail
    cmp BYTE PTR [rip + fx_installed], 1
    je .install_success

    # Exact pinned retail guards from the public C50 V11 effect layer.
    cmp DWORD PTR [rbx+H4_SITE_NEG], 0x245c8b48
    jne .install_fail
    cmp BYTE PTR [rbx+H4_SITE_NEG+4], 0x48
    jne .install_fail
    cmp DWORD PTR [rbx+H4_SITE_HELPER], 0x004acbe8
    jne .install_fail
    cmp BYTE PTR [rbx+H4_SITE_HELPER+4], 0
    jne .install_fail
    mov rdi, rbx
    add rdi, H4_TRANSIENT
    mov rax, 0x8b4918b644394166
    cmp QWORD PTR [rdi], rax
    jne .install_fail
    mov rax, 0xfbd5e8c1950f41d7
    cmp QWORD PTR [rdi+8], rax
    jne .install_fail
    cmp WORD PTR [rdi+16], 0xffff
    jne .install_fail

    # The exact C50 reserved cave must still be untouched.
    lea rdi, [rbx+H4_CAVE]
    xor eax, eax
    mov ecx, 0x1e
    repe scasq
    jne .install_fail

    mov QWORD PTR [rip + fx_hidden_count], 0
    lea rax, [rbx+H4_ORIG_HELPER]
    mov QWORD PTR [rip + fx_orig_helper], rax
    lea rax, [rbx+H4_TRANSIENT_CONT]
    mov QWORD PTR [rip + fx_transient_continue], rax

    # Exact public-C50 cave payloads. Only their bridge pointer is relocated.
    lea rcx, [rbx+H4_CAVE]
    mov edx, 0xf0
    mov r8d, PAGE_EXECUTE_READWRITE
    lea r9, [rsp+0x20]
    call QWORD PTR [rip + IAT_VIRTUAL_PROTECT_RVA]
    test eax, eax
    jz .install_fail
    lea rsi, [rip + payload_neg]
    lea rdi, [rbx+H4_CAVE]
    mov ecx, 0x40
    rep movsb
    lea rsi, [rip + payload_helper]
    lea rdi, [rbx+H4_CAVE+0x70]
    mov ecx, 0x80
    rep movsb
    lea rax, [rip + stage3ai_effect_bridge]
    mov QWORD PTR [rbx+H4_CAVE+0x0e], rax
    mov QWORD PTR [rbx+H4_CAVE+0xbd], rax
    lea rcx, [rbx+H4_CAVE]
    mov edx, 0xf0
    mov r8d, DWORD PTR [rsp+0x20]
    call STAGE3X_RESTORE_PROTECT_FLUSH_RVA

    # +0x1059A2 -> exact C50 negative-designator cave bridge.
    lea rdi, [rbx+H4_SITE_NEG]
    mov rcx, rdi
    mov edx, 5
    mov r8d, PAGE_EXECUTE_READWRITE
    lea r9, [rsp+0x24]
    call QWORD PTR [rip + IAT_VIRTUAL_PROTECT_RVA]
    test eax, eax
    jz .install_fail
    mov DWORD PTR [rdi], 0xa74269e9
    mov BYTE PTR [rdi+4], 0
    mov rcx, rdi
    mov edx, 5
    mov r8d, DWORD PTR [rsp+0x24]
    call STAGE3X_RESTORE_PROTECT_FLUSH_RVA

    # +0x100EE8 -> exact C50 helper cave bridge.
    lea rdi, [rbx+H4_SITE_HELPER]
    mov rcx, rdi
    mov edx, 5
    mov r8d, PAGE_EXECUTE_READWRITE
    lea r9, [rsp+0x28]
    call QWORD PTR [rip + IAT_VIRTUAL_PROTECT_RVA]
    test eax, eax
    jz .install_fail
    mov DWORD PTR [rdi], 0xa78d93e8
    mov BYTE PTR [rdi+4], 0
    mov rcx, rdi
    mov edx, 5
    mov r8d, DWORD PTR [rsp+0x28]
    call STAGE3X_RESTORE_PROTECT_FLUSH_RVA

    # Exact C50 transient positive-designator splice at +0x1012D5.
    lea rdi, [rbx+H4_TRANSIENT]
    mov rcx, rdi
    mov edx, 18
    mov r8d, PAGE_EXECUTE_READWRITE
    lea r9, [rsp+0x2c]
    call QWORD PTR [rip + IAT_VIRTUAL_PROTECT_RVA]
    test eax, eax
    jz .install_fail
    mov WORD PTR [rdi], 0x25ff
    mov DWORD PTR [rdi+2], 0
    lea rax, [rip + stage3ai_transient_wrapper]
    mov QWORD PTR [rdi+6], rax
    mov DWORD PTR [rdi+14], 0x90909090
    mov rcx, rdi
    mov edx, 18
    mov r8d, DWORD PTR [rsp+0x2c]
    call STAGE3X_RESTORE_PROTECT_FLUSH_RVA

    mov BYTE PTR [rip + fx_installed], 1
    lea rcx, [rip + msg_active]
    call LOGGER_RVA
.install_success:
    mov eax, 1
    jmp .install_done
.install_fail:
    lea rcx, [rip + msg_fail]
    call LOGGER_RVA
    xor eax, eax
.install_done:
    add rsp, 0x50
    pop rdi
    pop rsi
    pop rbx
    ret

# Exact Stage3Z accepted camera-mode-1 particle deny, copied into Stage3AI so
# Stage3AH's broader mode-2 contrast experiment is bypassed completely.
# rcx = halo4.dll base, edx = 1 install / 0 restore.
stage3ai_mode1_particle_patch:
    push rbx
    sub rsp, 0x40
    mov ebx, edx
    lea rdx, [rcx + H4_FP_PARTICLE_GATE_SITE]
    mov QWORD PTR [rsp+0x20], rdx
    cmp WORD PTR [rdx], 0x8A41
    jne .mode1_check_deny
    cmp BYTE PTR [rdx+2], 0xDA
    jne .mode1_fail
    test ebx, ebx
    jz .mode1_success
    jmp .mode1_write
.mode1_check_deny:
    cmp WORD PTR [rdx], 0xDB30
    jne .mode1_fail
    cmp BYTE PTR [rdx+2], 0x90
    jne .mode1_fail
    test ebx, ebx
    jnz .mode1_success
.mode1_write:
    mov rcx, rdx
    mov edx, 3
    mov r8d, PAGE_EXECUTE_READWRITE
    lea r9, [rsp+0x30]
    call QWORD PTR [rip + IAT_VIRTUAL_PROTECT_RVA]
    test eax, eax
    jz .mode1_fail
    mov rdx, QWORD PTR [rsp+0x20]
    test ebx, ebx
    jz .mode1_write_stock
    mov WORD PTR [rdx], 0xDB30
    mov BYTE PTR [rdx+2], 0x90
    jmp .mode1_flush
.mode1_write_stock:
    mov WORD PTR [rdx], 0x8A41
    mov BYTE PTR [rdx+2], 0xDA
.mode1_flush:
    mov rcx, rdx
    mov edx, 3
    mov r8d, DWORD PTR [rsp+0x30]
    call STAGE3X_RESTORE_PROTECT_FLUSH_RVA
.mode1_success:
    mov eax, 1
    jmp .mode1_done
.mode1_fail:
    xor eax, eax
.mode1_done:
    add rsp, 0x40
    pop rbx
    ret

# Active H4 title query wrapper. This is the only install path Stage3AI uses:
# current mode-1 muzzle deny + current HUD/curvature + recovered C50 coverage.
# AD/AE/AF/AH experimental effect installers are not called.
stage3ai_title_wrapper:
    push rbx
    sub rsp, 0x30
    call TITLE_ACTIVE_RVA
    movzx ebx, al
    cmp bl, 4
    jne .title_done
    mov rcx, QWORD PTR [rip + H4_MODULE_REF_RVA]
    test rcx, rcx
    jz .title_done
    mov edx, 1
    call stage3ai_mode1_particle_patch
    mov rcx, QWORD PTR [rip + H4_MODULE_REF_RVA]
    test rcx, rcx
    jz .title_done
    call STAGE3X_HUD_INSTALL_RVA
    mov rcx, QWORD PTR [rip + H4_MODULE_REF_RVA]
    test rcx, rcx
    jz .title_done
    call STAGE3X_CURV_INSTALL_RVA
    mov rcx, QWORD PTR [rip + H4_MODULE_REF_RVA]
    test rcx, rcx
    jz .title_done
    call stage3ai_effect_install
.title_done:
    mov eax, ebx
    add rsp, 0x30
    pop rbx
    ret

# rcx=halo4.dll base; drop-in for Stage3X teardown's first effect restore slot.
# Restore the recovered C50 splices and Stage3AI mode-1 deny. Existing teardown
# still independently restores current HUD/curvature and retains the module pin
# if this returns zero on any unknown bytes.
stage3ai_effect_restore:
    cld
    push rbx
    push rdi
    sub rsp, 0x48
    mov rbx, rcx
    test rbx, rbx
    jz .restore_fail

    # transient +0x1012D5: accept exact stock or our exact pointer splice.
    lea rdi, [rbx+H4_TRANSIENT]
    mov rax, 0x8b4918b644394166
    cmp QWORD PTR [rdi], rax
    je .trans_stock_tail
    cmp WORD PTR [rdi], 0x25ff
    jne .restore_fail
    cmp DWORD PTR [rdi+2], 0
    jne .restore_fail
    lea rax, [rip + stage3ai_transient_wrapper]
    cmp QWORD PTR [rdi+6], rax
    jne .restore_fail
    cmp DWORD PTR [rdi+14], 0x90909090
    jne .restore_fail
    mov rcx, rdi
    mov edx, 18
    mov r8d, PAGE_EXECUTE_READWRITE
    lea r9, [rsp+0x20]
    call QWORD PTR [rip + IAT_VIRTUAL_PROTECT_RVA]
    test eax, eax
    jz .restore_fail
    mov rax, 0x8b4918b644394166
    mov QWORD PTR [rdi], rax
    mov rax, 0xfbd5e8c1950f41d7
    mov QWORD PTR [rdi+8], rax
    mov WORD PTR [rdi+16], 0xffff
    mov rcx, rdi
    mov edx, 18
    mov r8d, DWORD PTR [rsp+0x20]
    call STAGE3X_RESTORE_PROTECT_FLUSH_RVA
    jmp .restore_bridge
.trans_stock_tail:
    mov rax, 0xfbd5e8c1950f41d7
    cmp QWORD PTR [rdi+8], rax
    jne .restore_fail
    cmp WORD PTR [rdi+16], 0xffff
    jne .restore_fail

.restore_bridge:
    cmp DWORD PTR [rbx+H4_SITE_NEG], 0x245c8b48
    jne .bridge_not_stock
    cmp BYTE PTR [rbx+H4_SITE_NEG+4], 0x48
    je .bridge_stock
.bridge_not_stock:
    cmp DWORD PTR [rbx+H4_SITE_NEG], 0xa74269e9
    jne .restore_fail
    cmp BYTE PTR [rbx+H4_SITE_NEG+4], 0
    jne .restore_fail
    cmp DWORD PTR [rbx+H4_SITE_HELPER], 0xa78d93e8
    jne .restore_fail
    cmp BYTE PTR [rbx+H4_SITE_HELPER+4], 0
    jne .restore_fail

    lea rdi, [rbx+H4_SITE_NEG]
    mov rcx, rdi
    mov edx, 5
    mov r8d, PAGE_EXECUTE_READWRITE
    lea r9, [rsp+0x24]
    call QWORD PTR [rip + IAT_VIRTUAL_PROTECT_RVA]
    test eax,eax
    jz .restore_fail
    mov DWORD PTR [rdi], 0x245c8b48
    mov BYTE PTR [rdi+4], 0x48
    mov rcx,rdi
    mov edx,5
    mov r8d,DWORD PTR [rsp+0x24]
    call STAGE3X_RESTORE_PROTECT_FLUSH_RVA

    lea rdi, [rbx+H4_SITE_HELPER]
    mov rcx,rdi
    mov edx,5
    mov r8d,PAGE_EXECUTE_READWRITE
    lea r9,[rsp+0x28]
    call QWORD PTR [rip + IAT_VIRTUAL_PROTECT_RVA]
    test eax,eax
    jz .restore_fail
    mov DWORD PTR [rdi],0x004acbe8
    mov BYTE PTR [rdi+4],0
    mov rcx,rdi
    mov edx,5
    mov r8d,DWORD PTR [rsp+0x28]
    call STAGE3X_RESTORE_PROTECT_FLUSH_RVA

    lea rdi,[rbx+H4_CAVE]
    mov rcx,rdi
    mov edx,0xf0
    mov r8d,PAGE_EXECUTE_READWRITE
    lea r9,[rsp+0x2c]
    call QWORD PTR [rip + IAT_VIRTUAL_PROTECT_RVA]
    test eax,eax
    jz .restore_fail
    xor eax,eax
    mov ecx,0x1e
    rep stosq
    lea rcx,[rbx+H4_CAVE]
    mov edx,0xf0
    mov r8d,DWORD PTR [rsp+0x2c]
    call STAGE3X_RESTORE_PROTECT_FLUSH_RVA
    jmp .restore_particle
.bridge_stock:
    cmp DWORD PTR [rbx+H4_SITE_HELPER],0x004acbe8
    jne .restore_fail
    cmp BYTE PTR [rbx+H4_SITE_HELPER+4],0
    jne .restore_fail

.restore_particle:
    mov rcx, rbx
    xor edx, edx
    call stage3ai_mode1_particle_patch
    test eax,eax
    jz .restore_fail
    mov BYTE PTR [rip+fx_installed],0
    mov QWORD PTR [rip+fx_hidden_count],0
    mov QWORD PTR [rip+fx_orig_helper],0
    mov QWORD PTR [rip+fx_transient_continue],0
    mov eax,1
    jmp .restore_done
.restore_fail:
    xor eax,eax
.restore_done:
    add rsp,0x48
    pop rdi
    pop rbx
    ret

.section .data,"aw"
.align 16
fx_hidden_count: .quad 0
fx_installed: .byte 0
.align 8
fx_orig_helper: .quad 0
fx_transient_continue: .quad 0

.align 16
payload_neg:
    .byte 0x66,0x83,0xff,0xfe,0x7f,0x12,0x48,0x89,0xe9,0x48,0x89,0xda,0x48,0xb8
    .quad 0x1111222233334444
    .byte 0xff,0xd0,0x48,0x8b,0x5c,0x24,0x48,0xe9,0x75,0xbd,0x58,0xff
    .zero 0x40-(.-payload_neg)
.align 16
payload_helper:
    .byte 0x48,0x83,0xec,0x58,0x48,0x89,0x4c,0x24,0x20,0x48,0x89,0x54,0x24,0x28
    .byte 0x4c,0x89,0x44,0x24,0x30,0x4c,0x89,0x4c,0x24,0x38,0xe8,0x1b,0xbd,0x58,0xff
    .byte 0x4c,0x8b,0x4c,0x24,0x38,0x45,0x84,0xc9,0x74,0x30,0x48,0x8b,0x44,0x24,0x28
    .byte 0x66,0x83,0xf8,0xfe,0x7f,0x25,0x48,0x8b,0x4c,0x24,0x20,0xf6,0x41,0x4a,0x0f,0x74,0x1a
    .byte 0x8a,0x41,0x4a,0x24,0xf0,0x3c,0xf0,0x74,0x11,0x48,0x8b,0x54,0x24,0x30,0x48,0xb8
    .quad 0x1111222233334444
    .byte 0xff,0xd0,0x48,0x83,0xc4,0x58,0xc3
    .zero 0x80-(.-payload_helper)

msg_active: .asciz "Halo 4 C50FX FULL-COVERAGE HIDE ACTIVE: public-C50 negative/helper/transient effect routes restored; selected local FP effects move finite-far; current mode-1 muzzle deny preserved"
msg_fail: .asciz "Halo 4 C50FX FULL-COVERAGE HIDE NOT INSTALLED: exact C50 retail guards/caves/protection failed; AD-AH speculative effect installers remain bypassed"
