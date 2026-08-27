.intel_syntax noprefix
.section .text

.global stage3r_reach_install_epilogue
.global stage3r_reach_cleanup_release_log

.extern FREE_LIBRARY_IAT_RVA
.extern REACH_MODULE_REF_RVA
.extern ORIGINAL_LOG_RVA

# Called in place of the normal function-epilogue R12 reload in
# InstallReachCameraCore.  Stage 3R changes the existing GetModuleHandleExW
# call to take a real loader reference.  If this install attempt exits without
# transferring that reference into g_reachCamera.moduleReference, return it to
# MCC here.  If it was retained, verified teardown owns the release instead.
#
# Caller RSP before CALL is the original InstallReachCameraCore frame.
# The retained HMODULE local is caller [rsp+0x78]; the overwritten R12 reload
# reads caller [rsp+0x1D8].  AL is the function return value and is preserved.
stage3r_reach_install_epilogue:
    push rax
    mov r10, qword ptr [rsp+0x88]
    test r10, r10
    je .replay_r12
    cmp r10, qword ptr [rip + REACH_MODULE_REF_RVA]
    je .replay_r12
    sub rsp, 0x20
    mov rcx, r10
    call qword ptr [rip + FREE_LIBRARY_IAT_RVA]
    add rsp, 0x20
.replay_r12:
    mov r12, qword ptr [rsp+0x1e8]
    pop rax
    ret

# Successful RemoveReachCameraCore has already disabled/removed every Reach
# hook, restored title-native bytes, cleared title pointers, set installed=0,
# and rearmed the level gate.  Earlier in that same success path Stage 3R moves
# the retained HMODULE into R14 instead of zeroing it.  Release it immediately
# before the function's existing final LOG call.  The log has no varargs, so
# preserving RCX is sufficient; tail-jumping to the original logger keeps the
# caller's return address exactly as before.
stage3r_reach_cleanup_release_log:
    push rcx
    mov qword ptr [rip + REACH_MODULE_REF_RVA], 0
    test r14, r14
    je .tail_log
    sub rsp, 0x20
    mov rcx, r14
    call qword ptr [rip + FREE_LIBRARY_IAT_RVA]
    add rsp, 0x20
.tail_log:
    pop rcx
    jmp ORIGINAL_LOG_RVA
