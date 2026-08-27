.intel_syntax noprefix
.section .text,"ax"
.global stage3af_h4_title_chain
.global stage3af_h4_origin_trampoline
.extern STAGE3AC_H4_PATCH_RVA
.extern STAGE3AF_INSTALLER_RVA
.extern H4_BASE_DATA_RVA

stage3af_h4_title_chain:
    push rbx
    sub rsp, 0x20
    mov rbx, rcx
    call STAGE3AC_H4_PATCH_RVA
    mov rcx, rbx
    call STAGE3AF_INSTALLER_RVA
    add rsp, 0x20
    pop rbx
    ret

.p2align 4
stage3af_h4_origin_trampoline:
    mov QWORD PTR [rsp + 0x08], rbx
    push rdi
    sub rsp, 0x60
    mov rbx, rdx
    xor r10d, r10d
    mov rax, QWORD PTR [rip + H4_BASE_DATA_RVA]
    add rax, 0x00100ECC
    jmp rax
