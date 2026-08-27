.intel_syntax noprefix

.section .wrapper,"ax"
.global stage3ac_h4_title_install_wrapper
stage3ac_h4_title_install_wrapper:
    push rbx
    sub rsp, 0x30
    call TITLE_ACTIVE_RVA
    movzx ebx, al
    cmp bl, 4
    jne .title_done

    // 1) Keep Stage3Z's headset-proven camera-mode-1 particle deny.
    mov rcx, QWORD PTR [rip + H4_MODULE_REF_RVA]
    test rcx, rcx
    jz .title_done
    mov edx, 1
    call STAGE3Z_PARTICLE_PATCH_RVA

    // 2) Restore Stage3X's already-stable local first-person residual-effect
    //    splice. 3Z bypassed this installer; no new H4 hook is introduced.
    mov rcx, QWORD PTR [rip + H4_MODULE_REF_RVA]
    test rcx, rcx
    jz .title_done
    call STAGE3X_EFFECT_INSTALLER_RVA

    // Exact accepted Stage3X HUD/full-layout installers.
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

.section .cleanup,"ax"
.global stage3ac_combined_effect_restore
stage3ac_combined_effect_restore:
    push rbx
    sub rsp, 0x20
    mov rbx, rcx

    // Restore Stage3X's +0x1012D5 direct splice first. It is exact-byte
    // guarded and clears its original install lock/module state on success.
    call STAGE3X_EFFECT_RESTORE_RVA
    test eax, eax
    jz .restore_fail

    // Then restore Stage3Z's 3-byte H4 particle admission instruction.
    mov rcx, rbx
    call STAGE3Z_PARTICLE_RESTORE_RVA
    test eax, eax
    jz .restore_fail
    mov eax, 1
    jmp .restore_done
.restore_fail:
    xor eax, eax
.restore_done:
    add rsp, 0x20
    pop rbx
    ret
