.intel_syntax noprefix
.section .text,"ax"
.global stage3al_acquire_odst_pin
.global stage3al_release_odst_pin

.extern ODST_MODULE_REFERENCE_RVA
.extern ODST_INSTALL_MAPPING_FAIL_RVA
.extern ODST_CLEAR_POINTERS_RVA
.extern IAT_GET_MODULE_HANDLE_EX_W_RVA
.extern IAT_FREE_LIBRARY_RVA

# Called only after the exact Stage3AK ODST preflight and prior-ownership checks
# have succeeded. RSI is the verified halo3odst.dll base in the surrounding
# InstallOdstCameraCore. The original earlier GetModuleHandleExW remains
# identity-only; this helper takes the one real loader reference we own.
stage3al_acquire_odst_pin:
    sub rsp, 0x28
    mov QWORD PTR [rsp + 0x20], 0
    lea r8, [rsp + 0x20]
    mov rdx, rsi
    mov ecx, 4
    call QWORD PTR [rip + IAT_GET_MODULE_HANDLE_EX_W_RVA]
    test eax, eax
    jz .acquire_fail
    mov rax, QWORD PTR [rsp + 0x20]
    cmp rax, rsi
    je .acquire_store
    test rax, rax
    jz .acquire_fail
    mov rcx, rax
    call QWORD PTR [rip + IAT_FREE_LIBRARY_RVA]
    jmp .acquire_fail
.acquire_store:
    mov QWORD PTR [rip + ODST_MODULE_REFERENCE_RVA], rax
    add rsp, 0x28
    ret
.acquire_fail:
    add rsp, 0x28
    # Discard this helper call's return address and enter the existing compiled
    # install-failure block with the caller's stack/frame exactly restored.
    add rsp, 8
    jmp ODST_INSTALL_MAPPING_FAIL_RVA

# Both successful teardown and install rollback call this wrapper. Snapshot the
# owned HMODULE, execute the exact old ClearOdstCameraPointers body, then release
# the one loader pin. No release occurs while any ODST detour/native patch is owned.
stage3al_release_odst_pin:
    push rbx
    sub rsp, 0x20
    mov rbx, QWORD PTR [rip + ODST_MODULE_REFERENCE_RVA]
    call ODST_CLEAR_POINTERS_RVA
    test rbx, rbx
    jz .release_done
    mov rcx, rbx
    call QWORD PTR [rip + IAT_FREE_LIBRARY_RVA]
.release_done:
    add rsp, 0x20
    pop rbx
    ret
