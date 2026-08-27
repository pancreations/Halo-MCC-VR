.intel_syntax noprefix
.section .text

.global stage3q_title_select
.global stage3q_present_probe_4arg
.global stage3q_present_probe_5arg
.global stage3q_activity_probe
.global stage3q_hash_region
.global stage3q_evidence_table

.extern REENTRY_DATA_RVA
.extern VIRTUAL_QUERY_IAT_RVA
.extern GET_MODULE_HANDLE_W_IAT_RVA
.extern GET_TICK_COUNT64_IAT_RVA
.extern TITLE_DESCRIPTORS_RVA
.extern ACTIVE_TITLE_RVA

# Writable data layout at REENTRY_DATA_RVA:
# 00 qword cached Present caller AllocationBase
# 08 byte  cached Present title
# 09 byte  fresh Present title hint
# 10 qword Present hint timestamp
# 20 five activity states, 0x20 bytes each:
#      +00 module base
#      +08 last fingerprint
#      +10 last change timestamp
#      +18 saw-still byte
#      +19 already-running change-run byte
.equ DATA_CACHED_ALLOC, 0x00
.equ DATA_CACHED_TITLE, 0x08
.equ DATA_HINT_TITLE, 0x09
.equ DATA_HINT_STAMP, 0x10
.equ DATA_ACTIVITY, 0x20
.equ STATE_SIZE, 0x20

# ---------------------------------------------------------------------------
# TitleAdapter_PollLoaded ambiguity selector.
# This replaces the original 21-byte "count>1 => Unknown" value-selection
# block. R13=detectedCount, RDI=first detected TitleDescriptor*, R14=nowMs.
# The caller's moduleBases[6] start at caller RSP+0x68, which is helper
# RSP+0x70 after CALL pushes our return address.
# On a unique re-entry proof we intentionally set nonvolatile RDI to the
# winning descriptor and R13=1 so the stock function follows its normal
# single-title path. No proof => AL=Unknown and R13 remains >1, preserving the
# stock fail-closed ambiguity branch.
stage3q_title_select:
    cmp r13, 1
    ja .ambiguous
    test rdi, rdi
    je .single_none
    movzx eax, byte ptr [rdi]
    ret
.single_none:
    xor eax, eax
    ret

.ambiguous:
    # Fast path: a fresh DXGI Present caller that resolved inside a resident
    # title DLL. The hint never grants ownership; it only picks which adapter
    # may begin its normal liveness proof.
    movzx eax, byte ptr [rip + REENTRY_DATA_RVA + DATA_HINT_TITLE]
    test al, al
    je .activity
    cmp al, 7
    jae .activity
    cmp al, 5                 # CE is unsupported and never a re-entry winner.
    je .activity
    mov r10, qword ptr [rip + REENTRY_DATA_RVA + DATA_HINT_STAMP]
    movzx r9d, byte ptr [rip + REENTRY_DATA_RVA + DATA_HINT_TITLE]
    cmp al, r9b               # reject a concurrent title/stamp publication
    jne .activity
    test r10, r10
    je .activity
    cmp r14, r10
    jb .activity
    mov r11, r14
    sub r11, r10
    cmp r11, 250
    ja .activity
    movzx ecx, al
    cmp cl, 6
    je .hint_h2
    dec ecx                   # title 1..4 => slot 0..3
    jmp .hint_slot
.hint_h2:
    mov ecx, 5
.hint_slot:
    cmp qword ptr [rsp + rcx*8 + 0x70], 0
    jne .winner_al

.activity:
    lea rcx, [rsp + 0x70]
    mov rdx, r14
    sub rsp, 0x28
    call stage3q_activity_probe
    add rsp, 0x28
    test al, al
    jne .winner_al

    # No new title proved itself on this sample. Retain the already selected
    # raw adapter while its module remains resident. Runtime ownership is still
    # independently heartbeat/lifecycle-gated, so this cannot keep stale VR
    # capabilities alive; it only prevents selector flapping to Unknown.
    movzx eax, byte ptr [rip + ACTIVE_TITLE_RVA]
    test al, al
    je .unresolved
    cmp al, 7
    jae .unresolved
    cmp al, 5
    je .unresolved
    movzx ecx, al
    cmp cl, 6
    je .retain_h2
    dec ecx
    jmp .retain_slot
.retain_h2:
    mov ecx, 5
.retain_slot:
    cmp qword ptr [rsp + rcx*8 + 0x70], 0
    je .unresolved

.winner_al:
    movzx ecx, al
    cmp cl, 6
    je .winner_h2_index
    dec ecx                   # descriptor index 0..3
    jmp .winner_index
.winner_h2_index:
    mov ecx, 5
.winner_index:
    imul ecx, ecx, 0x28
    lea rdi, [rip + TITLE_DESCRIPTORS_RVA]
    add rdi, rcx
    mov r13d, 1
    movzx eax, byte ptr [rdi]
    ret

.unresolved:
    mov eax, 7
    ret

# ---------------------------------------------------------------------------
# PresentHook entry trampoline for four-argument IDXGISwapChain::Present.
# CALL is injected over the first 15 bytes. At helper entry, [rsp+8] is the
# game's original return address. Replay the three overwritten nonvolatile
# saves against the ORIGINAL PresentHook stack (helper offsets are +8).
stage3q_present_probe_4arg:
    mov qword ptr [rsp+0x10], rbx
    mov qword ptr [rsp+0x18], rbp
    mov qword ptr [rsp+0x20], rsi
    jmp stage3q_present_common

# Present1Hook has the same first three 5-byte saves.
stage3q_present_probe_5arg:
    mov qword ptr [rsp+0x10], rbx
    mov qword ptr [rsp+0x18], rbp
    mov qword ptr [rsp+0x20], rsi
    # fallthrough

# Preserve all volatile argument registers, classify the original caller's
# allocation, and publish a short-lived title hint when that AllocationBase is
# one of MCC's title DLLs. Unknown/MCC-shell callers simply let the old hint
# expire after 250 ms.
stage3q_present_common:
    sub rsp, 0x90
    mov qword ptr [rsp+0x50], rax
    mov qword ptr [rsp+0x58], rcx
    mov qword ptr [rsp+0x60], rdx
    mov qword ptr [rsp+0x68], r8
    mov qword ptr [rsp+0x70], r9
    mov qword ptr [rsp+0x78], r10
    mov qword ptr [rsp+0x80], r11

    mov rcx, qword ptr [rsp+0x98]     # original game's return address
    lea rdx, [rsp+0x20]               # MEMORY_BASIC_INFORMATION (0x30)
    mov r8d, 0x30
    call qword ptr [rip + VIRTUAL_QUERY_IAT_RVA]
    test rax, rax
    je .present_restore
    mov r10, qword ptr [rsp+0x28]     # mbi.AllocationBase
    test r10, r10
    je .present_restore
    cmp r10, qword ptr [rip + REENTRY_DATA_RVA + DATA_CACHED_ALLOC]
    je .present_cached

    mov qword ptr [rip + REENTRY_DATA_RVA + DATA_CACHED_ALLOC], r10
    mov byte ptr [rip + REENTRY_DATA_RVA + DATA_CACHED_TITLE], 0
    lea r11, [rip + TITLE_DESCRIPTORS_RVA]
    xor r9d, r9d
.present_lookup_loop:
    mov rcx, qword ptr [r11+0x08]
    mov qword ptr [rsp+0x88], r10
    mov qword ptr [rsp+0x48], r11
    mov dword ptr [rsp+0x44], r9d
    call qword ptr [rip + GET_MODULE_HANDLE_W_IAT_RVA]
    mov r10, qword ptr [rsp+0x88]
    mov r11, qword ptr [rsp+0x48]
    mov r9d, dword ptr [rsp+0x44]
    cmp rax, r10
    je .present_lookup_hit
    inc r9d
    add r11, 0x28
    cmp r9d, 6
    jb .present_lookup_loop
    jmp .present_restore
.present_lookup_hit:
    mov al, byte ptr [r11]
    mov byte ptr [rip + REENTRY_DATA_RVA + DATA_CACHED_TITLE], al
    jmp .present_publish

.present_cached:
    mov al, byte ptr [rip + REENTRY_DATA_RVA + DATA_CACHED_TITLE]
.present_publish:
    test al, al
    je .present_restore
    cmp al, 5
    je .present_restore
    cmp al, 7
    jae .present_restore
    mov qword ptr [rip + REENTRY_DATA_RVA + DATA_HINT_STAMP], 0
    mov byte ptr [rip + REENTRY_DATA_RVA + DATA_HINT_TITLE], al
    call qword ptr [rip + GET_TICK_COUNT64_IAT_RVA]
    mov qword ptr [rip + REENTRY_DATA_RVA + DATA_HINT_STAMP], rax

.present_restore:
    mov rax, qword ptr [rsp+0x50]
    mov rcx, qword ptr [rsp+0x58]
    mov rdx, qword ptr [rsp+0x60]
    mov r8, qword ptr [rsp+0x68]
    mov r9, qword ptr [rsp+0x70]
    mov r10, qword ptr [rsp+0x78]
    mov r11, qword ptr [rsp+0x80]
    add rsp, 0x90
    ret

# ---------------------------------------------------------------------------
# Hash one committed/readable range using the same FNV-1a basis as the
# existing PlayerViewLivenessGate. RCX=address, EDX=bytes, R8=expected
# AllocationBase (zero means any allocation, used for H2's heap singleton).
# Returns RAX=nonzero fingerprint or 0 on an unprovable range.
stage3q_hash_region:
    push rbx
    push rsi
    push rdi
    sub rsp, 0x50
    mov rbx, rcx
    mov esi, edx
    mov rdi, r8
    mov rcx, rbx
    lea rdx, [rsp+0x20]
    mov r8d, 0x30
    call qword ptr [rip + VIRTUAL_QUERY_IAT_RVA]
    test rax, rax
    je .hash_bad
    cmp dword ptr [rsp+0x40], 0x1000   # State == MEM_COMMIT
    jne .hash_bad
    mov eax, dword ptr [rsp+0x44]      # Protect
    test eax, 0x100                    # PAGE_GUARD
    jne .hash_bad
    and eax, 0xff
    test eax, eax
    je .hash_bad
    cmp eax, 1                         # PAGE_NOACCESS
    je .hash_bad
    test rdi, rdi
    je .hash_alloc_ok
    cmp qword ptr [rsp+0x28], rdi      # AllocationBase
    jne .hash_bad
.hash_alloc_ok:
    mov r10, qword ptr [rsp+0x20]      # BaseAddress
    cmp rbx, r10
    jb .hash_bad
    mov r11, qword ptr [rsp+0x38]      # RegionSize
    add r11, r10
    mov eax, esi
    add rax, rbx
    jc .hash_bad
    cmp rax, r11
    ja .hash_bad

    mov rax, 1469598103934665603
    mov r9, 1099511628211
    mov rcx, rbx
    mov edx, esi
.hash_loop:
    test edx, edx
    je .hash_done
    movzx r8d, byte ptr [rcx]
    xor rax, r8
    imul rax, r9
    inc rcx
    dec edx
    jmp .hash_loop
.hash_done:
    test rax, rax
    jne .hash_out
    mov eax, 1
    jmp .hash_out
.hash_bad:
    xor eax, eax
.hash_out:
    add rsp, 0x50
    pop rdi
    pop rsi
    pop rbx
    ret

# ---------------------------------------------------------------------------
# Ambiguous-resident fallback. RCX=&moduleBases[0], RDX=nowMs.
# It samples ONLY proven title-owned liveness records, never scans modules and
# never writes game memory. A frozen/still sample followed by a change is an
# immediate candidate. If observation began mid-game, 120 changing samples
# (6 s) match the existing level-gate conservative already-running rule.
# Exactly one fresh candidate wins; zero/multiple return None.
stage3q_activity_probe:
    push rbx
    push rbp
    push rsi
    push rdi
    push r12
    push r13
    push r14
    push r15
    sub rsp, 0x58
    mov r12, rcx                     # moduleBases
    mov r13, rdx                     # nowMs
    xor r14d, r14d                   # evidence index
    xor r15d, r15d                   # candidate count
    xor ebp, ebp                     # winner title

.probe_loop:
    lea r10, [rip + stage3q_evidence_table]
    mov eax, r14d
    shl rax, 4
    add r10, rax
    movzx ecx, byte ptr [r10+8]      # module slot
    mov rbx, qword ptr [r12+rcx*8]
    mov eax, r14d
    shl rax, 5
    lea rdi, [rip + REENTRY_DATA_RVA + DATA_ACTIVITY]
    add rdi, rax                     # state
    test rbx, rbx
    jne .probe_have_module
    pxor xmm0, xmm0
    movdqu xmmword ptr [rdi], xmm0
    movdqu xmmword ptr [rdi+0x10], xmm0
    jmp .probe_next
.probe_have_module:
    cmp qword ptr [rdi], rbx
    je .probe_same_module
    pxor xmm0, xmm0
    movdqu xmmword ptr [rdi], xmm0
    movdqu xmmword ptr [rdi+0x10], xmm0
    mov qword ptr [rdi], rbx
.probe_same_module:
    movzx eax, byte ptr [r10+10]     # special: 1 = H2 game time
    test eax, eax
    jne .probe_h2

    mov ecx, dword ptr [r10]         # player-view RVA
    add rcx, rbx
    mov edx, dword ptr [r10+4]       # stride
    mov r8, rbx
    mov qword ptr [rsp+0x50], r10
    call stage3q_hash_region
    mov r10, qword ptr [rsp+0x50]
    test rax, rax
    je .probe_invalid
    xor esi, esi                     # explicitStill=0
    jmp .probe_update

.probe_h2:
    mov rcx, qword ptr [rbx+0x015FE008]
    test rcx, rcx
    je .probe_invalid
    mov edx, 0x0c
    xor r8d, r8d
    mov qword ptr [rsp+0x50], r10
    mov qword ptr [rsp+0x48], rcx
    call stage3q_hash_region
    mov r10, qword ptr [rsp+0x50]
    mov rcx, qword ptr [rsp+0x48]
    test rax, rax
    je .probe_invalid
    movzx esi, byte ptr [rcx]        # initialized
    cmp esi, 1
    ja .probe_invalid
    test esi, esi
    je .probe_h2_still
    mov eax, dword ptr [rcx+8]       # current tick
    shl rax, 32
    xor rax, rcx
    test rax, rax
    jne .probe_h2_active_fp
    mov eax, 1
.probe_h2_active_fp:
    xor esi, esi                     # explicitStill=0
    jmp .probe_update
.probe_h2_still:
    mov rax, rcx
    test rax, rax
    jne .probe_h2_still_fp
    mov eax, 1
.probe_h2_still_fp:
    mov esi, 1                       # explicitStill=1
    jmp .probe_update

.probe_invalid:
    mov byte ptr [rdi+0x19], 0
    jmp .probe_next

.probe_update:
    mov rcx, qword ptr [rdi+0x08]
    test rcx, rcx
    jne .probe_have_baseline
    mov qword ptr [rdi+0x08], rax
    test esi, esi
    je .probe_next
    mov byte ptr [rdi+0x18], 1
    mov byte ptr [rdi+0x19], 0
    jmp .probe_next

.probe_have_baseline:
    test esi, esi
    jne .probe_still
    cmp rcx, rax
    je .probe_still
    mov qword ptr [rdi+0x08], rax
    cmp byte ptr [rdi+0x18], 0
    jne .probe_ready_change
    movzx eax, byte ptr [rdi+0x19]
    cmp eax, 120
    jae .probe_ready_change
    inc eax
    mov byte ptr [rdi+0x19], al
    cmp eax, 120
    jb .probe_next
.probe_ready_change:
    mov qword ptr [rdi+0x10], r13
    jmp .probe_candidate

.probe_still:
    mov qword ptr [rdi+0x08], rax
    mov byte ptr [rdi+0x18], 1
    mov byte ptr [rdi+0x19], 0
    jmp .probe_next

.probe_candidate:
    mov rax, qword ptr [rdi+0x10]
    test rax, rax
    je .probe_next
    cmp r13, rax
    jb .probe_next
    mov rcx, r13
    sub rcx, rax
    cmp rcx, 250
    ja .probe_next
    inc r15d
    movzx ebp, byte ptr [r10+9]      # title enum

.probe_next:
    inc r14d
    cmp r14d, 5
    jb .probe_loop
    cmp r15d, 1
    jne .probe_none
    mov eax, ebp
    jmp .probe_out
.probe_none:
    xor eax, eax
.probe_out:
    add rsp, 0x58
    pop r15
    pop r14
    pop r13
    pop r12
    pop rdi
    pop rsi
    pop rbp
    pop rbx
    ret

.align 16
# rva, stride, slot, title, special, pad[5]
stage3q_evidence_table:
    .long 0x02D2F680, 0x2820
    .byte 0, 1, 0, 0, 0, 0, 0, 0
    .long 0x02D73590, 0x2810
    .byte 1, 2, 0, 0, 0, 0, 0, 0
    .long 0x029F2B90, 0x0A40
    .byte 2, 3, 0, 0, 0, 0, 0, 0
    .long 0x030AD1C0, 0x0AD0
    .byte 3, 4, 0, 0, 0, 0, 0, 0
    .long 0x00000000, 0x0000
    .byte 5, 6, 1, 0, 0, 0, 0, 0
