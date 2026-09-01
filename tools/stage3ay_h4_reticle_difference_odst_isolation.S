.intel_syntax noprefix
.section .text,"ax"

.global stage3ay_capture_begin
.global stage3ay_capture_finish
.global stage3ay_prepare_full_replay
.global stage3ay_second_replay
.global stage3ay_select_difference_shader
.global stage3ay_restore_difference_resource
.global stage3ay_skip_odst_cinematic_scan

.extern BEGIN_CAPTURE_INTERNAL_RVA
.extern BEGIN_AUTHORED_CAPTURE_RVA
.extern END_CUI_REDIRECT_RVA
.extern HALO4_SAFE_WRITE_RVA
.extern CAPTURE_FINISH_CONTINUE_RVA
.extern LOGGER_RVA
.extern G_CONTEXT_RVA
.extern G_DEVICE_RVA
.extern G_DISCARD_TEXTURE_RVA
.extern G_DISCARD_RTV_RVA
.extern G_ACTIVE_TITLE_RVA
.extern G_TLS_INDEX_RVA
.extern D3D_BACKUP_RESTORE_RVA

// Called at the exact H4EK type-0x28 marker. captureAuthored is deliberately
// also the requested pass kind: 1 for the stock/full replay, 0 for the replay
// in which the already-proven native-reticle transforms are hidden.
stage3ay_capture_begin:
    cmp BYTE PTR [r15 + r8 + 3], 0
    je .begin_without_reticle
    call BEGIN_AUTHORED_CAPTURE_RVA
    ret
.begin_without_reticle:
    mov ecx, 1
    xor edx, edx
    xor r8d, r8d
    call BEGIN_CAPTURE_INTERNAL_RVA
    ret

stage3ay_prepare_full_replay:
    mov BYTE PTR [rbx + 3], 1
    mov DWORD PTR [rbx + 4], 1
    mov BYTE PTR [rsp + 0x30], r12b
    ret

// The capture-replay branch has already executed the command and copied the
// pushed 0x34-byte transform to [rsp+0x48]. For the second replay only, write
// the same proven offscreen result back to the live top stack entry. The full
// replay is untouched. The capture remains open through the engine's batching
// boundary, so the two textures differ only by these native reticle elements.
stage3ay_capture_finish:
    mov ecx, DWORD PTR [rip + G_TLS_INDEX_RVA]
    mov rax, QWORD PTR gs:[0x58]
    mov rax, QWORD PTR [rax + rcx * 8]
    cmp BYTE PTR [rax + 0x3BB], 0
    jne .finish
    test r15b, r15b
    je .finish
    test r14d, r14d
    je .finish
    cmp r14d, 0x60
    ja .finish
    mov DWORD PTR [rsp + 0x70], 0x49742400
    mov ecx, r14d
    dec ecx
    imul rcx, rcx, 0x34
    add rcx, 0x878
    add rcx, rsi
    lea rdx, [rsp + 0x48]
    mov r8d, 0x34
    call HALO4_SAFE_WRITE_RVA
.finish:
    movzx eax, r15b
    jmp CAPTURE_FINISH_CONTINUE_RVA

// Replaces the visible-pass call after a successful full capture. Replay the
// same command stream into the discard target, beginning at the same exact
// marker, while capture_finish hides the known reticle transforms. Then draw
// the normal eye pass once, with the native flat reticle hidden as before.
stage3ay_second_replay:
    sub rsp, 0x38
    mov BYTE PTR [rip + stage3ay_difference_ready], 0

    mov rcx, QWORD PTR [rip + G_CONTEXT_RVA]
    mov rdx, QWORD PTR [rip + G_DISCARD_RTV_RVA]
    test rcx, rcx
    je .normal_pass
    test rdx, rdx
    je .normal_pass
    mov rax, QWORD PTR [rcx]
    lea r8, [rip + stage3ay_clear]
    call QWORD PTR [rax + 0x190]

    mov BYTE PTR [rbx + 3], 0
    mov DWORD PTR [rbx + 4], 1
    mov BYTE PTR [rsp + 0x28], r12b
    mov DWORD PTR [rsp + 0x20], r15d
    mov r9, r13
    mov r8, QWORD PTR [rsp + 0xC8]
    mov edx, edi
    mov ecx, esi
    call r14
    mov BYTE PTR [rbx + 4], 0
    cmp BYTE PTR [rbx + 2], 0
    je .normal_pass
    xor ecx, ecx
    call END_CUI_REDIRECT_RVA
    test al, al
    je .normal_pass
    mov BYTE PTR [rip + stage3ay_difference_ready], 1

.normal_pass:
    mov BYTE PTR [rsp + 0x28], r12b
    mov DWORD PTR [rsp + 0x20], r15d
    mov r9, r13
    mov r8, QWORD PTR [rsp + 0xC8]
    mov edx, edi
    mov ecx, esi
    call r14
    add rsp, 0x38
    ret

// Drop-in replacement for Blit's PSSetShader call. Other titles execute that
// call with the original shader. H4 uses the two-source difference shader only
// after the matching hidden-reticle replay completed. Device/texture identity
// changes release and rebuild the tiny cached COM objects.
stage3ay_select_difference_shader:
    sub rsp, 0x38
    mov BYTE PTR [rip + stage3ay_difference_bound], 0
    cmp BYTE PTR [rip + G_ACTIVE_TITLE_RVA], 4
    jne .bind
    cmp BYTE PTR [rip + stage3ay_difference_ready], 0
    je .bind

    mov r10, QWORD PTR [rip + G_DEVICE_RVA]
    mov r11, QWORD PTR [rip + G_DISCARD_TEXTURE_RVA]
    test r10, r10
    je .bind
    test r11, r11
    je .bind
    cmp r10, QWORD PTR [rip + stage3ay_cached_device]
    je .check_texture

    mov rcx, QWORD PTR [rip + stage3ay_discard_srv]
    test rcx, rcx
    je .release_shader
    mov rax, QWORD PTR [rcx]
    call QWORD PTR [rax + 0x10]
    mov QWORD PTR [rip + stage3ay_discard_srv], 0
.release_shader:
    mov rcx, QWORD PTR [rip + stage3ay_difference_ps]
    test rcx, rcx
    je .new_device
    mov rax, QWORD PTR [rcx]
    call QWORD PTR [rax + 0x10]
    mov QWORD PTR [rip + stage3ay_difference_ps], 0
.new_device:
    mov r10, QWORD PTR [rip + G_DEVICE_RVA]
    mov QWORD PTR [rip + stage3ay_cached_device], r10
    mov QWORD PTR [rip + stage3ay_cached_texture], 0

.check_texture:
    mov r11, QWORD PTR [rip + G_DISCARD_TEXTURE_RVA]
    cmp r11, QWORD PTR [rip + stage3ay_cached_texture]
    je .ensure_shader
    mov rcx, QWORD PTR [rip + stage3ay_discard_srv]
    test rcx, rcx
    je .create_srv
    mov rax, QWORD PTR [rcx]
    call QWORD PTR [rax + 0x10]
    mov QWORD PTR [rip + stage3ay_discard_srv], 0
.create_srv:
    mov rcx, QWORD PTR [rip + G_DEVICE_RVA]
    mov rdx, QWORD PTR [rip + G_DISCARD_TEXTURE_RVA]
    xor r8d, r8d
    lea r9, [rip + stage3ay_discard_srv]
    mov rax, QWORD PTR [rcx]
    call QWORD PTR [rax + 0x38]
    test eax, eax
    js .bind
    mov rax, QWORD PTR [rip + G_DISCARD_TEXTURE_RVA]
    mov QWORD PTR [rip + stage3ay_cached_texture], rax

.ensure_shader:
    cmp QWORD PTR [rip + stage3ay_difference_ps], 0
    jne .use_difference
    mov rcx, QWORD PTR [rip + G_DEVICE_RVA]
    lea rdx, [rip + stage3ay_shader_begin]
    mov r8d, 1000
    xor r9d, r9d
    lea rax, [rip + stage3ay_difference_ps]
    mov QWORD PTR [rsp + 0x20], rax
    mov rax, QWORD PTR [rcx]
    call QWORD PTR [rax + 0x78]
    test eax, eax
    js .bind

.use_difference:
    mov rcx, QWORD PTR [rip + G_CONTEXT_RVA]
    mov edx, 1
    mov r8d, 1
    lea r9, [rip + stage3ay_saved_srv1]
    mov rax, QWORD PTR [rcx]
    call QWORD PTR [rax + 0x248]
    mov BYTE PTR [rip + stage3ay_difference_bound], 1

    mov rcx, QWORD PTR [rip + G_CONTEXT_RVA]
    mov edx, 1
    mov r8d, 1
    lea r9, [rip + stage3ay_discard_srv]
    mov rax, QWORD PTR [rcx]
    call QWORD PTR [rax + 0x40]
    mov rbx, QWORD PTR [rip + stage3ay_difference_ps]

.bind:
    mov rcx, QWORD PTR [rip + G_CONTEXT_RVA]
    mov rdx, rbx
    xor r8d, r8d
    xor r9d, r9d
    mov rax, QWORD PTR [rcx]
    call QWORD PTR [rax + 0x48]
    add rsp, 0x38
    ret

// D3DStateBackup intentionally covers pixel-resource slot 0 only. The
// differential shader also uses slot 1, so restore the title's exact prior
// binding (and balance PSGetShaderResources' reference) before running the
// original backup restore.
stage3ay_restore_difference_resource:
    sub rsp, 0x28
    cmp BYTE PTR [rip + stage3ay_difference_bound], 0
    je .restore_backup
    mov rcx, QWORD PTR [rip + G_CONTEXT_RVA]
    mov edx, 1
    mov r8d, 1
    lea r9, [rip + stage3ay_saved_srv1]
    mov rax, QWORD PTR [rcx]
    call QWORD PTR [rax + 0x40]
    mov rcx, QWORD PTR [rip + stage3ay_saved_srv1]
    test rcx, rcx
    je .slot_restored
    mov rax, QWORD PTR [rcx]
    call QWORD PTR [rax + 0x10]
    mov QWORD PTR [rip + stage3ay_saved_srv1], 0
.slot_restored:
    mov BYTE PTR [rip + stage3ay_difference_bound], 0
.restore_backup:
    mov rdx, QWORD PTR [rip + G_CONTEXT_RVA]
    lea rcx, [rbp - 0x80]
    call D3D_BACKUP_RESTORE_RVA
    add rsp, 0x28
    ret

stage3ay_skip_odst_cinematic_scan:
    sub rsp, 0x28
    lea rcx, [rip + stage3ay_odst_log]
    call LOGGER_RVA
    add rsp, 0x28
    ret

.p2align 2
stage3ay_clear:
    .long 0, 0, 0, 0
stage3ay_odst_log:
    .asciz "ODST camera isolation: optional cutscene-facing scan skipped after the measured post-preflight stall; core camera installation continues"
.p2align 3
stage3ay_cached_device:
    .quad 0
stage3ay_cached_texture:
    .quad 0
stage3ay_discard_srv:
    .quad 0
stage3ay_difference_ps:
    .quad 0
stage3ay_saved_srv1:
    .quad 0
stage3ay_difference_ready:
    .byte 0
stage3ay_difference_bound:
    .byte 0
.p2align 4
stage3ay_shader_begin:
    .incbin "tools/stage3ay_h4_reticle_difference.cso"
stage3ay_shader_end:
