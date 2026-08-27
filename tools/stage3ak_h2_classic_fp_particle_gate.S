.intel_syntax noprefix
.section .text,"ax"
.global stage3ak_install_and_store
.global stage3ak_cleanup_restore
.global stage3ak_cleanup_release
.global stage3ak_h2_particle_gate
.global stage3ak_install_patch
.global stage3ak_restore_patch

.extern LOGGER_RVA
.extern H2_MODULE_REFERENCE_RVA
.extern IAT_VIRTUAL_PROTECT_RVA
.extern RESTORE_PROTECT_FLUSH_RVA
.extern STAGE3I_RESTORE_PIN_RVA
.extern STAGE3I_RELEASE_PIN_RVA

.equ H2_PARTICLE_RENDERER_RVA, 0x76DC90
.equ H2_PARTICLE_RENDERER_CONT_RVA, 0x76DC9F
.equ H2_RENDERER_GATE_RVA, 0xE70CF8
.equ PAGE_EXECUTE_READWRITE, 0x40

# ---------------------------------------------------------------------------
# Stage 3AK Halo 2 Classic local first-person particle gate.
#
# Pinned retail halo2.dll has three callers of +0x76DC90. The two effect
# traversal callers (+0x76E1A5 and +0x76E360) both pass the traversal's SIL
# classification as EDX. The main traversal derives SIL=1 only when the
# effect's owner resolves to the current output user and that output is a
# valid first-person user. The unrelated +0x721B32 caller passes EDX=0.
#
# Therefore the narrow runtime rule is:
#   suppress if DL != 0 AND halo2.dll+0xE70CF8 == 0 (Classic)
#   otherwise execute the exact retail renderer prologue and continue.
#
# Unlike Stage 3AJ, this hook is installed at the exact successful H2 module
# publication edge and stays resident across Classic/Anniversary switches.
# Anniversary naturally falls through stock because +0xE70CF8 == 1.
# ---------------------------------------------------------------------------
stage3ak_h2_particle_gate:
    test dl, dl
    jz .stock
    mov r10, QWORD PTR [rip + H2_MODULE_REFERENCE_RVA]
    test r10, r10
    jz .stock
    cmp BYTE PTR [r10 + H2_RENDERER_GATE_RVA], 0
    jne .stock

    lock inc QWORD PTR [rip + h2_suppressed_count]
    cmp BYTE PTR [rip + h2_hit_logged], 0
    jne .suppress_return
    mov BYTE PTR [rip + h2_hit_logged], 1
    # One-shot diagnostic only. At a Windows x64 function entry RSP is 8 mod 16;
    # subtract 0x28 to provide aligned call state plus the 32-byte shadow space.
    sub rsp, 0x28
    lea rcx, [rip + msg_hit]
    call LOGGER_RVA
    add rsp, 0x28
.suppress_return:
    ret

.stock:
    # Re-run the exact 15 overwritten retail bytes:
    # 48 8B C4          mov rax,rsp
    # 88 50 10          mov [rax+10h],dl
    # 89 48 08          mov [rax+08h],ecx
    # 55 53 56 57       push rbp/rbx/rsi/rdi
    # 41 55             push r13
    # then continue at +0x76DC9F (push r15).
    mov rax, rsp
    mov BYTE PTR [rax + 0x10], dl
    mov DWORD PTR [rax + 0x08], ecx
    push rbp
    push rbx
    push rsi
    push rdi
    push r13
    mov r10, QWORD PTR [rip + H2_MODULE_REFERENCE_RVA]
    test r10, r10
    jz .impossible_no_base
    add r10, H2_PARTICLE_RENDERER_CONT_RVA
    jmp r10
.impossible_no_base:
    # Installation/cleanup lifetime guarantees this cannot happen while the
    # hook is live. Fail closed rather than inventing a module address.
    add rsp, 0x28
    ret

# ---------------------------------------------------------------------------
# Install helper. rcx = retained halo2.dll base. eax = success.
# ---------------------------------------------------------------------------
stage3ak_install_patch:
    push rbx
    push rdi
    sub rsp, 0x38
    mov rbx, rcx
    test rbx, rbx
    jz .install_fail
    lea rdi, [rbx + H2_PARTICLE_RENDERER_RVA]

    # Already ours? Accept idempotently.
    cmp WORD PTR [rdi], 0xB848
    jne .guard_stock
    lea rax, [rip + stage3ak_h2_particle_gate]
    cmp QWORD PTR [rdi + 2], rax
    jne .install_fail
    cmp WORD PTR [rdi + 10], 0xE0FF
    jne .install_fail
    cmp WORD PTR [rdi + 12], 0x9090
    jne .install_fail
    cmp BYTE PTR [rdi + 14], 0x90
    jne .install_fail
    mov BYTE PTR [rip + h2_patch_installed], 1
    mov eax, 1
    jmp .install_done

.guard_stock:
    # Exact retail bytes from pinned halo2.dll SHA-256
    # DE65B4F4FDBF3F0A5EAB7431FE530DA17DD815599182DFD6AE9B7E21CF171946.
    mov rax, 0x4889105088c48b48
    cmp QWORD PTR [rdi], rax
    jne .install_fail
    cmp DWORD PTR [rdi + 8], 0x56535508
    jne .install_fail
    cmp WORD PTR [rdi + 12], 0x4157
    jne .install_fail
    cmp BYTE PTR [rdi + 14], 0x55
    jne .install_fail

    mov rcx, rdi
    mov edx, 15
    mov r8d, PAGE_EXECUTE_READWRITE
    lea r9, [rsp + 0x20]
    call QWORD PTR [rip + IAT_VIRTUAL_PROTECT_RVA]
    test eax, eax
    jz .install_fail

    # mov rax, <our absolute gate>; jmp rax; nop;nop;nop
    mov WORD PTR [rdi], 0xB848
    lea rax, [rip + stage3ak_h2_particle_gate]
    mov QWORD PTR [rdi + 2], rax
    mov WORD PTR [rdi + 10], 0xE0FF
    mov WORD PTR [rdi + 12], 0x9090
    mov BYTE PTR [rdi + 14], 0x90

    mov rcx, rdi
    mov edx, 15
    mov r8d, DWORD PTR [rsp + 0x20]
    call RESTORE_PROTECT_FLUSH_RVA

    mov QWORD PTR [rip + h2_suppressed_count], 0
    mov BYTE PTR [rip + h2_hit_logged], 0
    mov BYTE PTR [rip + h2_patch_installed], 1
    lea rcx, [rip + msg_active]
    call LOGGER_RVA
    mov eax, 1
    jmp .install_done

.install_fail:
    lea rcx, [rip + msg_install_fail]
    call LOGGER_RVA
    xor eax, eax
.install_done:
    add rsp, 0x38
    pop rdi
    pop rbx
    ret

# ---------------------------------------------------------------------------
# Restore helper. rcx = halo2.dll base. eax = success.
# ---------------------------------------------------------------------------
stage3ak_restore_patch:
    push rbx
    push rdi
    sub rsp, 0x38
    mov rbx, rcx
    test rbx, rbx
    jz .restore_fail
    lea rdi, [rbx + H2_PARTICLE_RENDERER_RVA]

    # Already stock is success.
    mov rax, 0x4889105088c48b48
    cmp QWORD PTR [rdi], rax
    jne .check_ours
    cmp DWORD PTR [rdi + 8], 0x56535508
    jne .restore_fail
    cmp WORD PTR [rdi + 12], 0x4157
    jne .restore_fail
    cmp BYTE PTR [rdi + 14], 0x55
    jne .restore_fail
    mov BYTE PTR [rip + h2_patch_installed], 0
    mov eax, 1
    jmp .restore_done

.check_ours:
    cmp WORD PTR [rdi], 0xB848
    jne .restore_fail
    lea rax, [rip + stage3ak_h2_particle_gate]
    cmp QWORD PTR [rdi + 2], rax
    jne .restore_fail
    cmp WORD PTR [rdi + 10], 0xE0FF
    jne .restore_fail
    cmp WORD PTR [rdi + 12], 0x9090
    jne .restore_fail
    cmp BYTE PTR [rdi + 14], 0x90
    jne .restore_fail

    mov rcx, rdi
    mov edx, 15
    mov r8d, PAGE_EXECUTE_READWRITE
    lea r9, [rsp + 0x20]
    call QWORD PTR [rip + IAT_VIRTUAL_PROTECT_RVA]
    test eax, eax
    jz .restore_fail

    mov rax, 0x4889105088c48b48
    mov QWORD PTR [rdi], rax
    mov DWORD PTR [rdi + 8], 0x56535508
    mov WORD PTR [rdi + 12], 0x4157
    mov BYTE PTR [rdi + 14], 0x55

    mov rcx, rdi
    mov edx, 15
    mov r8d, DWORD PTR [rsp + 0x20]
    call RESTORE_PROTECT_FLUSH_RVA

    mov BYTE PTR [rip + h2_patch_installed], 0
    mov rdx, QWORD PTR [rip + h2_suppressed_count]
    lea rcx, [rip + msg_restored]
    call LOGGER_RVA
    mov eax, 1
    jmp .restore_done

.restore_fail:
    lea rcx, [rip + msg_restore_fail]
    call LOGGER_RVA
    xor eax, eax
.restore_done:
    add rsp, 0x38
    pop rdi
    pop rbx
    ret

# ---------------------------------------------------------------------------
# Drop-in replacement for successful H2 core module-reference store at
# HaloMCCVR+0x8F66B. RAX is the exact retained halo2.dll module/base.
# Preserve every register/flags/XMM value because the original store did.
# ---------------------------------------------------------------------------
stage3ak_install_and_store:
    mov QWORD PTR [rip + H2_MODULE_REFERENCE_RVA], rax
    pushfq
    push rax
    push rcx
    push rdx
    push rbx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    sub rsp, 0x88
    movaps XMMWORD PTR [rsp + 0x20], xmm0
    movaps XMMWORD PTR [rsp + 0x30], xmm1
    movaps XMMWORD PTR [rsp + 0x40], xmm2
    movaps XMMWORD PTR [rsp + 0x50], xmm3
    movaps XMMWORD PTR [rsp + 0x60], xmm4
    movaps XMMWORD PTR [rsp + 0x70], xmm5
    mov rcx, QWORD PTR [rip + H2_MODULE_REFERENCE_RVA]
    call stage3ak_install_patch
    movaps xmm0, XMMWORD PTR [rsp + 0x20]
    movaps xmm1, XMMWORD PTR [rsp + 0x30]
    movaps xmm2, XMMWORD PTR [rsp + 0x40]
    movaps xmm3, XMMWORD PTR [rsp + 0x50]
    movaps xmm4, XMMWORD PTR [rsp + 0x60]
    movaps xmm5, XMMWORD PTR [rsp + 0x70]
    add rsp, 0x88
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rbp
    pop rbx
    pop rdx
    pop rcx
    pop rax
    popfq
    ret

# First H2 teardown edge: let Stage3I acquire/retain its temporary module pin
# and restore its own H2 hooks, then restore our particle renderer bytes.
stage3ak_cleanup_restore:
    sub rsp, 0x28
    call STAGE3I_RESTORE_PIN_RVA
    mov rcx, QWORD PTR [rip + H2_MODULE_REFERENCE_RVA]
    test rcx, rcx
    jz .cleanup_done
    call stage3ak_restore_patch
.cleanup_done:
    add rsp, 0x28
    ret

# Second H2 teardown edge: never release Stage3I's temporary halo2.dll pin or
# clear the module reference while our function-entry jump remains installed.
stage3ak_cleanup_release:
    sub rsp, 0x28
    cmp BYTE PTR [rip + h2_patch_installed], 0
    je .release_stage3i
    mov rcx, QWORD PTR [rip + H2_MODULE_REFERENCE_RVA]
    test rcx, rcx
    jz .hold_pin
    call stage3ak_restore_patch
    test eax, eax
    jz .hold_pin
.release_stage3i:
    call STAGE3I_RELEASE_PIN_RVA
    jmp .release_done
.hold_pin:
    lea rcx, [rip + msg_hold_pin]
    call LOGGER_RVA
.release_done:
    add rsp, 0x28
    ret

.section .data,"aw"
.align 8
h2_suppressed_count:
    .quad 0
h2_patch_installed:
    .byte 0
h2_hit_logged:
    .byte 0
.align 8
msg_active:
    .asciz "Halo 2 Classic muzzle/effects Stage 3AK ACTIVE: exact H2 module edge installed the current-user FP particle gate; Anniversary remains stock"
msg_hit:
    .asciz "Halo 2 Classic muzzle/effects Stage 3AK HIT: suppressed a Classic current-user first-person particle dispatch"
msg_install_fail:
    .asciz "Halo 2 Classic muzzle/effects Stage 3AK REFUSED: exact retail particle-renderer bytes/protection did not match; effects left stock"
msg_restored:
    .asciz "Halo 2 Classic muzzle/effects Stage 3AK restored: retail particle renderer active; suppressed dispatch count=%llu"
msg_restore_fail:
    .asciz "Halo 2 Classic muzzle/effects Stage 3AK restore FAILED: retaining cleanup ownership for retry"
msg_hold_pin:
    .asciz "Halo 2 Classic muzzle/effects Stage 3AK cleanup holding halo2.dll pin until exact retail renderer restore succeeds"
