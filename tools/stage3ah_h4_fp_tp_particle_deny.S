.intel_syntax noprefix
.section .text
.global stage3ah_h4_fp_tp_particle_patch

.equ H4_PARTICLE_MODE2_ALLOW_SITE, 0x27BD32
.equ PAGE_EXECUTE_READWRITE, 0x40

// Drop-in replacement for Stage3Z's patch routine at the same RVA.
// rcx = retained halo4.dll base, edx = 1 install / 0 restore.
// Retail switch proof from the user's pinned halo4.dll:
//   +0x27BD32  8A DA       mov bl,dl     (camera mode 2 allow)
//   +0x27BD34  EB 08       jmp +0x27BD3E (invariant layout guard)
//   +0x27BD36  41 8A DA    mov bl,r10b   (camera mode 1 allow)
// Stage3AH zeroes BL at BOTH allow writes. This is deliberately a broad
// diagnostic contrast; mode 0 remains on its stock +0x27BD3B path.
stage3ah_h4_fp_tp_particle_patch:
    push rbx
    push rsi
    sub rsp, 0x28
    mov ebx, edx
    lea rsi, [rcx + H4_PARTICLE_MODE2_ALLOW_SITE]
    movzx eax, WORD PTR [rsi]
    test ebx, ebx
    jz .restore_guard
    cmp ax, 0xDB30
    je .success
    cmp ax, 0xDA8A
    jne .fail
    jmp .write
.restore_guard:
    cmp ax, 0xDA8A
    je .success
    cmp ax, 0xDB30
    jne .fail
.write:
    mov rcx, rsi
    mov edx, 7
    push PAGE_EXECUTE_READWRITE
    pop r8
    lea r9, [rsp+0x20]
    call QWORD PTR [rip + IAT_VIRTUAL_PROTECT_RVA]
    test eax, eax
    jz .fail
    test ebx, ebx
    jz .write_stock
    mov WORD PTR [rsi], 0xDB30
    mov WORD PTR [rsi+4], 0xDB30
    mov BYTE PTR [rsi+6], 0x90
    jmp .flush
.write_stock:
    mov WORD PTR [rsi], 0xDA8A
    mov WORD PTR [rsi+4], 0x8A41
    mov BYTE PTR [rsi+6], 0xDA
.flush:
    mov rcx, rsi
    mov edx, 7
    mov r8d, DWORD PTR [rsp+0x20]
    call STAGE3X_RESTORE_PROTECT_FLUSH_RVA
.success:
    mov eax, 1
.done:
    add rsp, 0x28
    pop rsi
    pop rbx
    ret
.fail:
    xor eax, eax
    jmp .done
