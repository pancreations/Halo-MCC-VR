.intel_syntax noprefix
.section .text,"ax"
.global stage3af_h4_origin_installer
.extern STAGE3AF_HOOK_RVA
.extern H4_BASE_DATA_RVA
.extern H4_HIT_FLAG_RVA
.extern H4_BAD_FLAG_RVA
.extern LOGGER_RVA
.extern IAT_VIRTUAL_PROTECT_RVA
.extern IAT_GET_CURRENT_PROCESS_RVA
.extern IAT_FLUSH_INSTRUCTION_CACHE_RVA

.equ H4_ORIGIN_SITE, 0x00100EBC
.equ H4_ORIGIN_PAGE, 0x00100000

stage3af_h4_origin_installer:
    push rbx
    push rdi
    sub rsp, 0x28
    mov rbx, rcx
    test rbx, rbx
    jz .bad
    mov QWORD PTR [rip + H4_BASE_DATA_RVA], rbx

    cmp DWORD PTR [rbx + H4_ORIGIN_SITE], 0x000025ff
    jne .check_stock
    cmp WORD PTR [rbx + H4_ORIGIN_SITE + 4], 0
    jne .check_stock
    lea rax, [rip + STAGE3AF_HOOK_RVA]
    cmp QWORD PTR [rbx + H4_ORIGIN_SITE + 6], rax
    je .ok
.check_stock:
    movabs rax, 0x83485708245c8948
    cmp QWORD PTR [rbx + H4_ORIGIN_SITE], rax
    jne .bad
    movabs rax, 0xd23345da8b4860ec
    cmp QWORD PTR [rbx + H4_ORIGIN_SITE + 8], rax
    jne .bad

    lea rcx, [rbx + H4_ORIGIN_PAGE]
    mov edx, 0x1000
    mov r8d, 0x40
    lea r9, [rsp + 0x20]
    call QWORD PTR [rip + IAT_VIRTUAL_PROTECT_RVA]
    test eax, eax
    jz .bad
    lea rdi, [rbx + H4_ORIGIN_SITE]
    mov DWORD PTR [rdi], 0x000025ff
    mov WORD PTR [rdi + 4], 0
    lea rax, [rip + STAGE3AF_HOOK_RVA]
    mov QWORD PTR [rdi + 6], rax
    mov WORD PTR [rdi + 14], 0x9090

    call QWORD PTR [rip + IAT_GET_CURRENT_PROCESS_RVA]
    mov rcx, rax
    mov rdx, rdi
    mov r8d, 16
    call QWORD PTR [rip + IAT_FLUSH_INSTRUCTION_CACHE_RVA]
    lea rcx, [rbx + H4_ORIGIN_PAGE]
    mov edx, 0x1000
    mov r8d, DWORD PTR [rsp + 0x20]
    lea r9, [rsp + 0x24]
    call QWORD PTR [rip + IAT_VIRTUAL_PROTECT_RVA]

    mov DWORD PTR [rip + H4_HIT_FLAG_RVA], 0
    mov DWORD PTR [rip + H4_BAD_FLAG_RVA], 0
    lea rcx, [rip + .log_in]
    call LOGGER_RVA
.ok:
    mov eax, 1
    jmp .done
.bad:
    cmp DWORD PTR [rip + H4_BAD_FLAG_RVA], 0
    jne .bad_done
    mov DWORD PTR [rip + H4_BAD_FLAG_RVA], 1
    lea rcx, [rip + .log_bad]
    call LOGGER_RVA
.bad_done:
    xor eax, eax
.done:
    add rsp, 0x28
    pop rdi
    pop rbx
    ret

.p2align 2
.log_in:  .asciz "H4AF IN"
.log_bad: .asciz "AFBAD"
