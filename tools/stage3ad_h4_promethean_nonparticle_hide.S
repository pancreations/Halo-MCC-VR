.intel_syntax noprefix

# Stage 3AD: suppress the remaining Halo 4 first-person non-particle weapon
# firing visuals at the retail effect dispatcher. Stage 3AC already combines
# Stage 3Z's first-person particle deny with Stage 3X's stable local-FP
# residual matrix hide. This adds the sibling gldf/lens/ltvl dispatcher paths
# used heavily by Promethean weapon firing effects.
#
# IMPORTANT: no code is placed in .s3qd+0. These helpers live only in verified
# post-function executable gaps inside .s3ic.

.equ H4_GLDF_TEST_RVA,    0x003A0822
.equ H4_LTVL_FP_TEST_RVA, 0x003A0AA1
.equ H4_LENS_FP_TEST_RVA, 0x003A0B1A
.equ H4_PATCH_PAGE_RVA,   0x003A0000

# Stock -> Stage3AD toggle bytes:
# +3A0822: 84 0D B8 2D D2 00  (retail gldf debug-global test)
#         45 84 ED 90 90 90  (test r13b,r13b; NOPs)
# Existing JNE then takes the retail skip only in first-person context.
# +3A0AA1: 84 C9 -> 31 C9     (ltvl first-person-only branch)
# +3A0B1A: 84 C9 -> 31 C9     (lens first-person-only branch)
# Existing JEs then take the retail skip. Non-FP control flow jumps around
# those two sites and remains stock.
#
# The XOR masks are intentionally self-inverse so install/restore share one
# exact transaction. The high two bytes of the qword mask are zero, leaving
# the following retail JNE bytes unchanged.

.section .patcher,"ax"
.global stage3ad_h4_nonparticle_patch
stage3ad_h4_nonparticle_patch:
    push rbx
    push rsi
    sub rsp, 0x28
    mov rbx, rcx                    # retained halo4.dll HMODULE/base
    mov esi, edx                    # desired state: 1=installed, 0=stock
    test rbx, rbx
    jz .patch_fail

    mov eax, DWORD PTR [rbx + H4_GLDF_TEST_RVA]
    test esi, esi
    jz .want_stock

    # Install is idempotent. Anything except exact stock or exact Stage3AD
    # marker fails closed and is left untouched.
    cmp eax, 0x90ED8445
    je .patch_ok
    cmp eax, 0x2DB80D84
    jne .patch_fail
    cmp BYTE PTR [rbx + H4_GLDF_TEST_RVA + 4], 0xD2
    jne .patch_fail
    jmp .toggle

.want_stock:
    cmp eax, 0x2DB80D84
    je .patch_ok
    cmp eax, 0x90ED8445
    jne .patch_fail
    cmp BYTE PTR [rbx + H4_GLDF_TEST_RVA + 4], 0x90
    jne .patch_fail

.toggle:
    lea rcx, [rbx + H4_PATCH_PAGE_RVA]
    mov edx, 0x1000
    mov r8d, 0x40                   # PAGE_EXECUTE_READWRITE
    lea r9, [rsp + 0x20]
    call QWORD PTR [rip + IAT_VIRTUAL_PROTECT_RVA]
    test eax, eax
    jz .patch_fail

    # Toggle the exact three dispatcher decisions.
    movabs rax, 0x00009042BD5589C1
    xor QWORD PTR [rbx + H4_GLDF_TEST_RVA], rax
    xor BYTE PTR [rbx + H4_LTVL_FP_TEST_RVA], 0xB5
    xor BYTE PTR [rbx + H4_LENS_FP_TEST_RVA], 0xB5

    # Reuse Stage 3X's already-live protection-restore + instruction-cache
    # flush helper instead of adding another duplicated Win32 helper.
    lea rcx, [rbx + H4_PATCH_PAGE_RVA]
    mov edx, 0x1000
    mov r8d, DWORD PTR [rsp + 0x20]
    call STAGE3X_RESTORE_PROTECT_FLUSH_RVA

.patch_ok:
    mov eax, 1
    jmp .patch_done
.patch_fail:
    xor eax, eax
.patch_done:
    add rsp, 0x28
    pop rsi
    pop rbx
    ret

.section .title,"ax"
.global stage3ad_h4_title_wrapper
stage3ad_h4_title_wrapper:
    push rbx
    sub rsp, 0x20
    call STAGE3AC_TITLE_WRAPPER_RVA
    mov ebx, eax
    cmp bl, 4                       # GameTitle::Halo4
    jne .title_done
    mov rcx, QWORD PTR [rip + H4_MODULE_REF_RVA]
    test rcx, rcx
    jz .title_done
    mov edx, 1
    call stage3ad_h4_nonparticle_patch
.title_done:
    mov eax, ebx
    add rsp, 0x20
    pop rbx
    ret

.section .cleanup,"ax"
.global stage3ad_h4_cleanup_wrapper
stage3ad_h4_cleanup_wrapper:
    push rbx
    sub rsp, 0x20
    mov rbx, rcx
    xor edx, edx
    call stage3ad_h4_nonparticle_patch
    test eax, eax
    jz .cleanup_fail
    mov rcx, rbx
    call STAGE3AC_CLEANUP_RVA
    jmp .cleanup_done
.cleanup_fail:
    xor eax, eax
.cleanup_done:
    add rsp, 0x20
    pop rbx
    ret
