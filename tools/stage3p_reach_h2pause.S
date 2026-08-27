.intel_syntax noprefix
.section .text

.global stage3p_reach_memtype_filter
.global stage3p_reach_scan_start
.global stage3p_h3_authoritative_pause_proof
.global stage3p_h3_read_pause_scoped
.global stage3p_h3_pause_recovery_guard
.global stage3p_reach_scan_continue
.global stage3p_str_welcome

.extern ACTIVE_TITLE_RVA
.extern ENGINE_PAUSE_VALIDATED_RVA
.extern READ_ENGINE_PAUSED_RVA
.extern REACH_FAST_GENERATION_RVA
.extern REACH_SCAN_LOOP_RVA

# SAFEFRAME storage filter. H3/ODST retain their private-only rule. Reach's
# headset-proven six records are MEM_PRIVATE/PAGE_READWRITE, while mapped tag
# copies are also legal for this adapter, so Reach accepts either type.
# Contract at patched site: EAX=mbi.Type, RCX=&adapter->scanMappedRegions.
stage3p_reach_memtype_filter:
    cmp byte ptr [rcx], 0
    je .private_only
    cmp eax, 0x20000
    je .yes
    cmp eax, 0x40000
    sete al
    ret
.private_only:
    cmp eax, 0x20000
    sete al
    ret
.yes:
    mov al, 1
    ret

# Replays the three overwritten instructions after GetSystemInfo:
#   rax = lpMaximumApplicationAddress; xmm0 = 0; r15 = minimum address.
# On the first Reach scan of each title generation, begin in the proven
# 0x00007FF4... tag-allocation band. If that scan fails, the next auto-rescan
# has the same generation and therefore uses the original full-range minimum.
stage3p_reach_scan_start:
    mov rax, qword ptr [rbp-0x08]
    xorps xmm0, xmm0
    mov r15, qword ptr [rbp-0x10]
    cmp r14d, 3
    jne .scan_done
    mov ecx, dword ptr [rbp-0x70]
    cmp ecx, dword ptr [rip + REACH_FAST_GENERATION_RVA]
    je .scan_done
    mov dword ptr [rip + REACH_FAST_GENERATION_RVA], ecx
    mov rdx, 0x00007ff400000000
    cmp r15, rdx
    cmovb r15, rdx
.scan_done:
    ret

# g_enginePauseValidated is a Halo 3-only proof. The original function used it
# before checking title ownership, so a resident halo3.dll could make H2 look
# authoritative and suppress H2's edge-driven head-locked pause presentation.
stage3p_h3_authoritative_pause_proof:
    cmp byte ptr [rip + ACTIVE_TITLE_RVA], 1
    jne .auth_false
    mov al, byte ptr [rip + ENGINE_PAUSE_VALIDATED_RVA]
    ret
.auth_false:
    xor eax, eax
    ret

# Game_AutoVrTick already computed r12b = Halo3 active/owned. Do not even read
# the stale Halo 3 native pause byte while another title owns the runtime.
stage3p_h3_read_pause_scoped:
    test r12b, r12b
    je .read_false
    jmp READ_ENGINE_PAUSED_RVA
.read_false:
    xor eax, eax
    ret

# The second native-pause test gates Halo 3's restart-level recovery helper.
# For non-H3 titles return true so the existing JNE skips that H3-only block.
stage3p_h3_pause_recovery_guard:
    test r12b, r12b
    je .recovery_skip
    mov al, byte ptr [rip + ENGINE_PAUSE_VALIDATED_RVA]
    ret
.recovery_skip:
    mov al, 1
    ret

# Called instead of the scan loop's final range compare. The six accepted Reach
# records are the exact full-set proof from the known-good scan. Once all six
# were found in the current high allocation, discard our own return address and
# skip the rest of the address space. Other titles, or Reach with <6 hits, keep
# the stock range test and loop behavior. RAX is scratch at the loop head.
stage3p_reach_scan_continue:
    cmp r14d, 3
    jne .range_test
    cmp ebx, 6
    jge .scan_stop
.range_test:
    cmp r15, qword ptr [rbp-0x58]
    jae .scan_stop
    pop rax
    jmp REACH_SCAN_LOOP_RVA
.scan_stop:
    ret

stage3p_str_welcome:
    .asciz "Hi everyone, pancreations here!\nThank you for taking the time to get this up and running.\nIf you have any issues please reach out and let me know.\nExpect the rest of the collection to be added over time.\n\nPass-on/community build maintained by @MeWhenINameMyself.\nJoin the Flat2VR Discord to report bugs or share feedback."
