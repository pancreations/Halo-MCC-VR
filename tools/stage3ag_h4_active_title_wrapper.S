.intel_syntax noprefix
.section .text,"ax"
.global stage3ag_h4_active_title_wrapper
.extern STAGE3AD_TITLE_WRAPPER_RVA
.extern STAGE3AF_INSTALLER_RVA
.extern H4_MODULE_REF_RVA

# Active-edge wrapper for the real TitleAdapter query at HaloMCCVR+0x30894.
# Stage3AF accidentally chained its installer behind a dormant internal call.
# This wrapper first preserves the complete Stage3AD/3AC/3Z title path, then
# invokes Stage3AF's exact +0x100EBC upstream origin installer with the retained
# halo4.dll base. The installer's own H4AF IN / AFBAD markers are therefore
# runtime proof that this edge actually executed.
stage3ag_h4_active_title_wrapper:
    push rbx
    sub rsp, 0x20
    call STAGE3AD_TITLE_WRAPPER_RVA
    mov ebx, eax
    cmp bl, 4
    jne .done
    mov rcx, QWORD PTR [rip + H4_MODULE_REF_RVA]
    test rcx, rcx
    jz .done
    call STAGE3AF_INSTALLER_RVA
.done:
    mov eax, ebx
    add rsp, 0x20
    pop rbx
    ret
