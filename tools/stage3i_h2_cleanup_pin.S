.intel_syntax noprefix
.text
.extern CLEANUP_PIN_RVA
.extern MODULE_REFERENCE_RVA
.extern IAT_GET_MODULE_HANDLE_EX_RVA
.extern IAT_FREE_LIBRARY_RVA
.extern RESTORE_FP_RVA
.global stage3i_restore_fp_and_pin
.global stage3i_release_pin_and_clear_ref

# Stage 3I binary-equivalent helper for the exact Stage 3H DLL.
#
# The Stage 3H source keeps g_moduleReference non-owning. When level liveness
# closes, MCC can start unloading halo2.dll before MinHook gets to restore its
# two Classic patches. The headset log then records MH_ERROR_MEMORY_PROTECT (10)
# forever and Classic never installs again. Pin only for the cleanup window.

stage3i_restore_fp_and_pin:
    sub rsp, 0x28

    # If a prior cleanup attempt already owns the temporary reference, keep it.
    mov rax, qword ptr [rip + CLEANUP_PIN_RVA]
    test rax, rax
    jne .call_restore

    # g_moduleReference is the exact non-owning halo2.dll HMODULE/base retained
    # by the installed Classic core.
    mov rdx, qword ptr [rip + MODULE_REFERENCE_RVA]
    test rdx, rdx
    je .call_restore

    xor eax, eax
    mov qword ptr [rsp + 0x20], rax
    lea r8, [rsp + 0x20]
    mov ecx, 4                    # GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
    call qword ptr [rip + IAT_GET_MODULE_HANDLE_EX_RVA]
    test eax, eax
    je .call_restore

    mov rax, qword ptr [rsp + 0x20]
    test rax, rax
    je .call_restore
    cmp rax, qword ptr [rip + MODULE_REFERENCE_RVA]
    jne .release_mismatch
    mov qword ptr [rip + CLEANUP_PIN_RVA], rax
    jmp .call_restore

.release_mismatch:
    mov rcx, rax
    call qword ptr [rip + IAT_FREE_LIBRARY_RVA]

.call_restore:
    call RESTORE_FP_RVA
    add rsp, 0x28
    ret

stage3i_release_pin_and_clear_ref:
    sub rsp, 0x28
    mov rax, qword ptr [rip + CLEANUP_PIN_RVA]
    test rax, rax
    je .clear_reference
    mov qword ptr [rip + CLEANUP_PIN_RVA], 0
    mov rcx, rax
    call qword ptr [rip + IAT_FREE_LIBRARY_RVA]

.clear_reference:
    mov qword ptr [rip + MODULE_REFERENCE_RVA], 0
    add rsp, 0x28
    ret
