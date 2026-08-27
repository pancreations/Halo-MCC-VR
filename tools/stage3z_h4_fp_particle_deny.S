.intel_syntax noprefix
.section .text
.global stage3z_h4_title_install_wrapper
.global stage3z_h4_fp_particle_patch
.global stage3z_h4_fp_particle_restore

.equ H4_FP_PARTICLE_GATE_SITE, 0x27BD36
.equ PAGE_EXECUTE_READWRITE, 0x40

// Stage 3Z title wrapper. This deliberately does NOT call Stage3X's old
// +0x1012D5 transform-splice installer: headset testing proved relocation is
// downstream of the visible flash. Instead install the retail camera-mode-1
// particle deny, then run the accepted Stage3X HUD/curvature installers.
stage3z_h4_title_install_wrapper:
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
    call stage3z_h4_fp_particle_patch
    mov rcx, QWORD PTR [rip + H4_MODULE_REF_RVA]
    test rcx, rcx
    jz .title_done
    call STAGE3X_HUD_INSTALL_RVA
    mov rcx, QWORD PTR [rip + H4_MODULE_REF_RVA]
    test rcx, rcx
    jz .title_done
    call STAGE3X_CURV_INSTALL_RVA
.title_done:
    mov eax, ebx
    add rsp, 0x30
    pop rbx
    ret

// rcx = retained halo4.dll base, edx = 1 install deny / 0 restore stock.
// Retail camera-mode switch proof:
//   +0x27BD1B test ecx,ecx          mode 0
//   +0x27BD1F sub ecx,1
//   +0x27BD22 je +0x27BD36          original mode 1 only
//   +0x27BD36 mov bl,r10b           allow FP-owned mode-1 particle
// Replacing only that 3-byte instruction with xor bl,bl+nop forces the
// function's existing return-5 skip result. Modes 0/2 never execute this site.
stage3z_h4_fp_particle_patch:
    push rbx
    sub rsp, 0x40
    mov ebx, edx
    lea rdx, [rcx + H4_FP_PARTICLE_GATE_SITE]
    mov QWORD PTR [rsp+0x20], rdx

    // Exact retail bytes: 41 8A DA = mov bl,r10b.
    cmp WORD PTR [rdx], 0x8A41
    jne .check_deny
    cmp BYTE PTR [rdx+2], 0xDA
    jne .fail
    test ebx, ebx
    jz .success
    jmp .write

.check_deny:
    // Exact Stage3Z bytes: 30 DB 90 = xor bl,bl; nop.
    cmp WORD PTR [rdx], 0xDB30
    jne .fail
    cmp BYTE PTR [rdx+2], 0x90
    jne .fail
    test ebx, ebx
    jnz .success

.write:
    mov rcx, rdx
    mov edx, 3
    mov r8d, PAGE_EXECUTE_READWRITE
    lea r9, [rsp+0x30]
    call QWORD PTR [rip + IAT_VIRTUAL_PROTECT_RVA]
    test eax, eax
    jz .fail
    mov rdx, QWORD PTR [rsp+0x20]
    test ebx, ebx
    jz .write_stock
    mov WORD PTR [rdx], 0xDB30
    mov BYTE PTR [rdx+2], 0x90
    jmp .flush
.write_stock:
    mov WORD PTR [rdx], 0x8A41
    mov BYTE PTR [rdx+2], 0xDA
.flush:
    mov rcx, rdx
    mov edx, 3
    mov r8d, DWORD PTR [rsp+0x30]
    call STAGE3X_RESTORE_PROTECT_FLUSH_RVA
.success:
    mov eax, 1
    jmp .done
.fail:
    xor eax, eax
.done:
    add rsp, 0x40
    pop rbx
    ret

// Drop-in target for Stage3X cleanup's first direct-effect restore call.
// Returning zero preserves Stage3X's fail-closed behavior: the halo4.dll pin
// is retained rather than freeing a module with unknown live code bytes.
stage3z_h4_fp_particle_restore:
    xor edx, edx
    jmp stage3z_h4_fp_particle_patch
