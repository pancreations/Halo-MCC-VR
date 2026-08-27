.intel_syntax noprefix
.section .text

.global stage3u_reach_camera_heartbeat
.global stage3u_reach_publish_real_heartbeat

.extern GET_TICK_COUNT64_IAT_RVA
.extern REACH_ACTIVE_CALLBACKS_RVA
.extern REACH_LAST_CAM_COPY_MS_RVA
.extern REACH_PAUSE_CACHE_RVA
.extern REACH_TEARDOWN_REQUESTED_RVA

# Replaces the lock-inc instruction at the admitted Reach main-render detour.
# Preserve the incoming volatile argument registers because the original
# instruction touched none of them, then publish a timestamp from the actual
# title camera callback. This is the liveness evidence used by runtime
# ownership; Present itself must never fabricate it.
stage3u_reach_camera_heartbeat:
    push rcx
    push rdx
    push r8
    push r9
    push r10
    push r11
    lock inc DWORD PTR [rip + REACH_ACTIVE_CALLBACKS_RVA]
    sub rsp, 0x28
    call QWORD PTR [rip + GET_TICK_COUNT64_IAT_RVA]
    add rsp, 0x28
    mov QWORD PTR [rip + REACH_LAST_CAM_COPY_MS_RVA], rax
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdx
    pop rcx
    ret

# Replaces AutoVrTick's synthetic GetTickCount64()+store pair. Return the
# timestamp written by the real camera callback so the existing heartbeat
# publication carries genuine title liveness. If an armed, unpaused Reach core
# has not executed its admitted camera path for >1 second, request verified
# worker teardown. The worker then disables/removes hooks while Stage 3T's
# loader reference is still valid, releases that reference only after cleanup,
# and lets the normal stock-screen/shell presentation take back ownership.
stage3u_reach_publish_real_heartbeat:
    mov r10, QWORD PTR [rip + REACH_LAST_CAM_COPY_MS_RVA]
    test r10, r10
    je .zero
    cmp DWORD PTR [rip + REACH_PAUSE_CACHE_RVA], 1
    je .return_timestamp
    sub rsp, 0x28
    mov QWORD PTR [rsp + 0x20], r10
    call QWORD PTR [rip + GET_TICK_COUNT64_IAT_RVA]
    mov r10, QWORD PTR [rsp + 0x20]
    add rsp, 0x28
    sub rax, r10
    cmp rax, 1000
    jbe .return_timestamp
    mov BYTE PTR [rip + REACH_TEARDOWN_REQUESTED_RVA], 1
.return_timestamp:
    mov rax, r10
    ret
.zero:
    xor eax, eax
    ret
