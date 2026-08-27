.intel_syntax noprefix
.section .text

.global stage3v_h4_install_log_release
.global stage3v_h4_enable_fail_release
.global stage3v_h4_cleanup_capture
.global stage3v_h4_cleanup_log_release
.global stage3v_h4_title_and_install_effect
.global stage3v_h4_hide_local_fp_effect

.extern ORIGINAL_LOG_RVA
.extern FREE_LIBRARY_IAT_RVA
.extern H4_GENERATION_RVA
.extern H4_MODULE_REF_RVA
.extern H4_CLEANUP_SCRATCH_RVA
.extern H4_CLEANUP_CONTINUE_RVA
.extern TITLE_ACTIVE_RVA
.extern H4_EFFECT_INSTALLER_RVA

// Install failure helper for the create-hook and GetModuleHandleEx failure
// tails. The original HMODULE local is caller [rsp+0x50]. A CALL puts it at
// helper [rsp+0x58], then our 0x28-byte shadow/alignment frame moves it to
// [rsp+0x80]. Log first with the original ABI, then release only an acquired
// reference that was never transferred into g_halo4Camera.moduleReference.
stage3v_h4_install_log_release:
    sub rsp, 0x28
    call ORIGINAL_LOG_RVA
    mov rcx, QWORD PTR [rsp + 0x80]
    test rcx, rcx
    jz .install_log_done
    call QWORD PTR [rip + FREE_LIBRARY_IAT_RVA]
.install_log_done:
    add rsp, 0x28
    ret

// Enable-hook failure arrives only after both MinHook targets have been
// disabled/removed and the global camera state/moduleReference have been
// cleared. Replay the overwritten generation clear, then release the still
// live owning HMODULE kept in the installer's stack local.
stage3v_h4_enable_fail_release:
    mov DWORD PTR [rip + H4_GENERATION_RVA], 0
    mov rcx, QWORD PTR [rsp + 0x58]
    test rcx, rcx
    jz .enable_fail_done
    sub rsp, 0x28
    call QWORD PTR [rip + FREE_LIBRARY_IAT_RVA]
    add rsp, 0x28
.enable_fail_done:
    ret

// Successful teardown must keep halo4.dll resident until every MinHook target
// is gone. Capture the retained module before the existing XMM clear zeros the
// camera state, then resume the untouched teardown body.
stage3v_h4_cleanup_capture:
    mov ebx, DWORD PTR [rip + H4_GENERATION_RVA]
    mov rax, QWORD PTR [rip + H4_MODULE_REF_RVA]
    mov QWORD PTR [rip + H4_CLEANUP_SCRATCH_RVA], rax
    xor edi, edi
    jmp H4_CLEANUP_CONTINUE_RVA

// Final successful teardown log occurs after all hook removal/state clearing.
// Only now is it safe to drop the title-DLL reference. Cleanup-failure paths
// never reach this helper, so their pin remains live for a later retry.
stage3v_h4_cleanup_log_release:
    sub rsp, 0x28
    call ORIGINAL_LOG_RVA
    mov rcx, QWORD PTR [rip + H4_CLEANUP_SCRATCH_RVA]
    test rcx, rcx
    jz .cleanup_log_done
    mov QWORD PTR [rip + H4_CLEANUP_SCRATCH_RVA], 0
    call QWORD PTR [rip + FREE_LIBRARY_IAT_RVA]
.cleanup_log_done:
    add rsp, 0x28
    ret

// Keep C-H4-50's accepted procedural bullet-ray reticle behavior exactly as it
// is. We wrap only its already-existing active-title query to opportunistically
// install the old, signature-guarded local first-person effect splice while H4
// owns the runtime. The subsequent cmp al,4 / return-false branch is untouched.
stage3v_h4_title_and_install_effect:
    sub rsp, 0x28
    call TITLE_ACTIVE_RVA
    mov BYTE PTR [rsp + 0x20], al
    cmp al, 4
    jne .title_done
    call H4_EFFECT_INSTALLER_RVA
    mov al, BYTE PTR [rsp + 0x20]
.title_done:
    add rsp, 0x28
    ret

// Stage 3H's scale=0 action was headset-ineffective. For only the existing
// narrow local/first-person effect classification, preserve the matrix pointer
// and authored orientation but move its finite origin far outside play space.
stage3v_h4_hide_local_fp_effect:
    mov DWORD PTR [rax + 0x30], 0x461c4000  # +10000.0f
    ret
