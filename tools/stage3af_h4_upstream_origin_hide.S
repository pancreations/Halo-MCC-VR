.intel_syntax noprefix
.section .text,"ax"
.global stage3af_h4_origin_hide
.extern STAGE3AF_TRAMPOLINE_RVA
.extern STAGE3X_FINITE_HIDE_RVA
.extern H4_BASE_DATA_RVA
.extern H4_HIT_FLAG_RVA
.extern LOGGER_RVA

.equ H4_ORIGIN_CALLER_RETURN, 0x001018F4

stage3af_h4_origin_hide:
    sub rsp, 0x58
    mov QWORD PTR [rsp + 0x20], rcx
    mov QWORD PTR [rsp + 0x28], rdx
    mov QWORD PTR [rsp + 0x30], r8
    mov QWORD PTR [rsp + 0x38], r9
    mov BYTE PTR [rsp + 0x40], 0

    mov r11, QWORD PTR [rsp + 0x58]
    mov rax, QWORD PTR [rip + H4_BASE_DATA_RVA]
    add rax, H4_ORIGIN_CALLER_RETURN
    cmp r11, rax
    jne .qual_done
    cmp BYTE PTR [rsp + 0x38], 0
    je .qual_done
    cmp WORD PTR [rsp + 0x82], 1
    jne .qual_done
    mov rdx, QWORD PTR [rsp + 0x28]
    test rdx, rdx
    je .qual_done
    cmp WORD PTR [rdx + 2], 0xfffe
    jg .qual_done
    mov BYTE PTR [rsp + 0x40], 1
.qual_done:
    mov rcx, QWORD PTR [rsp + 0x20]
    mov rdx, QWORD PTR [rsp + 0x28]
    mov r8,  QWORD PTR [rsp + 0x30]
    mov r9,  QWORD PTR [rsp + 0x38]
    call STAGE3AF_TRAMPOLINE_RVA
    mov QWORD PTR [rsp + 0x48], rax

    cmp BYTE PTR [rsp + 0x40], 1
    jne .return
    test rax, rax
    je .return
    call STAGE3X_FINITE_HIDE_RVA
    cmp DWORD PTR [rip + H4_HIT_FLAG_RVA], 0
    jne .return
    mov DWORD PTR [rip + H4_HIT_FLAG_RVA], 1
    lea rcx, [rip + .log_hit]
    call LOGGER_RVA
.return:
    mov rax, QWORD PTR [rsp + 0x48]
    add rsp, 0x58
    ret

.p2align 2
.log_hit: .asciz "H4AF HIT"
