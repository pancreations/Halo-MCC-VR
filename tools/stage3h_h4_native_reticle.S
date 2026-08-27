.intel_syntax noprefix
.section .text

.global stage3h_measure_viewport
.global stage3h_reticle_delta
.global stage3h_publish_native_serial
.global stage3h_native_pair_proven
.global stage3h_effect_wrapper

// Exact Stage 3G C50 post-link symbols. The companion builder resolves every
// symbol as an RVA and links this section at 0x002EF000, so references remain
// RIP-relative and ASLR-safe.
.extern CORE_BASE_PTR_RVA
.extern CONFIG_CROSSHAIR_RVA
.extern CONFIG_DISTANCE_RVA
.extern CONFIG_SIZE_RVA
.extern STEREO_EYE_RVA
.extern WORLD_SCALE_RVA
.extern ENGINE_AIM_RVA
.extern CUI_SERIAL_RVA
.extern HALF_FOV_X_RVA
.extern HALF_FOV_Y_RVA
.extern RENDER_SERIAL_RVA
.extern CENTER_ROOT_RVA
.extern EYE_ROOT_RVA
.extern SAFE_READ_RVA
.extern TITLE_ACTIVE_RVA
.extern TANF_RVA
.extern IAT_GET_CURRENT_PROCESS_RVA
.extern IAT_FLUSH_INSTRUCTION_CACHE_RVA
.extern IAT_VIRTUAL_PROTECT_RVA
.extern VIEWPORT_HALF_WIDTH_RVA
.extern VIEWPORT_HALF_HEIGHT_RVA
.extern EFFECT_INSTALL_LOCK_RVA
.extern EFFECT_MODULE_BASE_RVA

.equ H4_LOCAL_EFFECT_SITE_RVA, 0x001012D5
.equ H4_LOCAL_EFFECT_ORIGINAL_RVA, 0x00100EBC
.equ H4_LOCAL_EFFECT_CONTINUE_RVA, 0x001012E7

// Called once for each exact owned gameplay-CUI pass. It safely measures the
// live int16 viewport bounds and opportunistically installs the independently
// signature-guarded Halo 4 local first-person-effect suppressor.
stage3h_measure_viewport:
    sub rsp, 0x58
    mov QWORD PTR [rsp + 0x40], rcx
    call stage3h_install_effect_hook

    mov rcx, QWORD PTR [rsp + 0x40]
    test rcx, rcx
    jz measure_done
    mov QWORD PTR [rsp + 0x30], 0
    lea rdx, [rsp + 0x30]
    mov r8d, 8
    call SAFE_READ_RVA
    test eax, eax
    jz measure_done

    movsx eax, WORD PTR [rsp + 0x34]
    movsx ecx, WORD PTR [rsp + 0x30]
    sub eax, ecx
    mov ecx, eax
    neg eax
    cmovs eax, ecx
    movsx edx, WORD PTR [rsp + 0x36]
    movsx ecx, WORD PTR [rsp + 0x32]
    sub edx, ecx
    mov ecx, edx
    neg edx
    cmovs edx, ecx
    mov r8d, eax
    mov ecx, edx
    cmp eax, edx
    cmovl ecx, r8d
    cmovl eax, edx
    // eax=max span (width), ecx=min span (height)
    cmp eax, 2
    jl measure_done
    cmp ecx, 2
    jl measure_done
    cmp eax, 32767
    jg measure_done
    cvtsi2ss xmm0, eax
    cvtsi2ss xmm1, ecx
    mulss xmm0, DWORD PTR [rip + k_half]
    mulss xmm1, DWORD PTR [rip + k_half]
    movss DWORD PTR [rip + VIEWPORT_HALF_WIDTH_RVA], xmm0
    movss DWORD PTR [rip + VIEWPORT_HALF_HEIGHT_RVA], xmm1

measure_done:
    add rsp, 0x58
    ret

// Return AL=1, XMM0=delta X, XMM1=delta Y. The caller writes only the pushed
// type-0x28 reticle matrix and publishes its eye serial after the write lands.
// The finite target is the centre-camera engine shot ray at the configured
// distance, reprojected through the exact active-eye basis and FOV.
stage3h_reticle_delta:
    sub rsp, 0x68
    mov QWORD PTR [rsp + 0x20], rcx
    cmp BYTE PTR [rip + CONFIG_CROSSHAIR_RVA], 0
    je reticle_hide

    mov eax, DWORD PTR [rip + STEREO_EYE_RVA]
    cmp eax, 1
    ja reticle_failure
    mov DWORD PTR [rsp + 0x28], eax

    movss xmm0, DWORD PTR [rip + VIEWPORT_HALF_WIDTH_RVA]
    movss xmm1, DWORD PTR [rip + VIEWPORT_HALF_HEIGHT_RVA]
    ucomiss xmm0, xmm0
    jp reticle_failure
    ucomiss xmm1, xmm1
    jp reticle_failure
    comiss xmm0, DWORD PTR [rip + k_one]
    jb reticle_failure
    comiss xmm1, DWORD PTR [rip + k_one]
    jb reticle_failure
    comiss xmm0, DWORD PTR [rip + k_32768]
    ja reticle_failure
    comiss xmm1, DWORD PTR [rip + k_32768]
    ja reticle_failure
    movss DWORD PTR [rsp + 0x40], xmm0
    movss DWORD PTR [rsp + 0x44], xmm1

    lea r8, [rip + CENTER_ROOT_RVA]
    mov r10d, DWORD PTR [r8]
    test r10d, r10d
    jz reticle_failure
    test r10b, 1
    jnz reticle_failure
    mov DWORD PTR [rsp + 0x2c], r10d

    lea r9, [rip + EYE_ROOT_RVA]
    imul edx, eax, 0x34
    add r9, rdx
    mov r11d, DWORD PTR [r9]
    test r11d, r11d
    jz reticle_failure
    test r11b, 1
    jnz reticle_failure
    mov DWORD PTR [rsp + 0x30], r11d

    movss xmm5, DWORD PTR [rip + CONFIG_DISTANCE_RVA]
    ucomiss xmm5, xmm5
    jp reticle_failure
    maxss xmm5, DWORD PTR [rip + k_two]
    minss xmm5, DWORD PTR [rip + k_fifty]
    mulss xmm5, DWORD PTR [rip + WORLD_SCALE_RVA]
    ucomiss xmm5, xmm5
    jp reticle_failure
    comiss xmm5, DWORD PTR [rip + k_point_zero_one]
    jbe reticle_failure

    movss xmm0, DWORD PTR [rip + ENGINE_AIM_RVA]
    movss xmm1, DWORD PTR [rip + ENGINE_AIM_RVA + 4]
    movss xmm2, DWORD PTR [rip + ENGINE_AIM_RVA + 8]
    movaps xmm3, xmm0
    mulss xmm3, xmm0
    movaps xmm4, xmm1
    mulss xmm4, xmm1
    addss xmm3, xmm4
    movaps xmm4, xmm2
    mulss xmm4, xmm2
    addss xmm3, xmm4
    ucomiss xmm3, xmm3
    jp reticle_failure
    sqrtss xmm3, xmm3
    comiss xmm3, DWORD PTR [rip + k_point_zero_one]
    jbe reticle_failure
    divss xmm5, xmm3

    // eyeRay = centrePosition + normalizedEngineAim*range - eyePosition
    mulss xmm0, xmm5
    addss xmm0, DWORD PTR [r8 + 4]
    subss xmm0, DWORD PTR [r9 + 4]
    mulss xmm1, xmm5
    addss xmm1, DWORD PTR [r8 + 8]
    subss xmm1, DWORD PTR [r9 + 8]
    mulss xmm2, xmm5
    addss xmm2, DWORD PTR [r8 + 0x0c]
    subss xmm2, DWORD PTR [r9 + 0x0c]

    // Forward denominator.
    movss xmm3, DWORD PTR [r9 + 0x10]
    mulss xmm3, xmm0
    movss xmm4, DWORD PTR [r9 + 0x14]
    mulss xmm4, xmm1
    addss xmm3, xmm4
    movss xmm4, DWORD PTR [r9 + 0x18]
    mulss xmm4, xmm2
    addss xmm3, xmm4

    // X numerator is dot(ray,right), and published left is -right.
    movss xmm4, DWORD PTR [r9 + 0x1c]
    mulss xmm4, xmm0
    movss xmm5, DWORD PTR [r9 + 0x20]
    mulss xmm5, xmm1
    addss xmm4, xmm5
    movss xmm5, DWORD PTR [r9 + 0x24]
    mulss xmm5, xmm2
    addss xmm4, xmm5
    xorps xmm4, XMMWORD PTR [rip + k_sign_mask]

    // Positive CUI Y is camera-up on the validated pushed matrix.
    movss xmm5, DWORD PTR [r9 + 0x28]
    mulss xmm5, xmm0
    movss xmm0, DWORD PTR [r9 + 0x2c]
    mulss xmm0, xmm1
    addss xmm5, xmm0
    movss xmm0, DWORD PTR [r9 + 0x30]
    mulss xmm0, xmm2
    addss xmm5, xmm0

    cmp DWORD PTR [r8], r10d
    jne reticle_failure
    cmp DWORD PTR [r9], r11d
    jne reticle_failure
    ucomiss xmm3, xmm3
    jp reticle_failure
    comiss xmm3, DWORD PTR [rip + k_point_zero_one]
    jbe reticle_failure
    movss DWORD PTR [rsp + 0x34], xmm4
    movss DWORD PTR [rsp + 0x38], xmm5
    movss DWORD PTR [rsp + 0x3c], xmm3

    mov eax, DWORD PTR [rsp + 0x28]
    lea rdx, [rip + HALF_FOV_X_RVA]
    movss xmm0, DWORD PTR [rdx + rax*4]
    lea rdx, [rip + HALF_FOV_Y_RVA]
    movss xmm1, DWORD PTR [rdx + rax*4]
    ucomiss xmm0, xmm0
    jp reticle_failure
    ucomiss xmm1, xmm1
    jp reticle_failure
    comiss xmm0, DWORD PTR [rip + k_point_zero_one]
    jbe reticle_failure
    comiss xmm1, DWORD PTR [rip + k_point_zero_one]
    jbe reticle_failure
    comiss xmm0, DWORD PTR [rip + k_fov_max]
    jae reticle_failure
    comiss xmm1, DWORD PTR [rip + k_fov_max]
    jae reticle_failure
    movss DWORD PTR [rsp + 0x48], xmm0
    movss DWORD PTR [rsp + 0x4c], xmm1

    call TANF_RVA
    movss DWORD PTR [rsp + 0x50], xmm0
    movss xmm0, DWORD PTR [rsp + 0x4c]
    call TANF_RVA
    movss DWORD PTR [rsp + 0x54], xmm0
    ucomiss xmm0, xmm0
    jp reticle_failure
    comiss xmm0, DWORD PTR [rip + k_zero]
    jbe reticle_failure
    movss xmm1, DWORD PTR [rsp + 0x50]
    ucomiss xmm1, xmm1
    jp reticle_failure
    comiss xmm1, DWORD PTR [rip + k_zero]
    jbe reticle_failure

    // Normalize through the live eye FOV, then map through the exact viewport.
    mulss xmm1, DWORD PTR [rsp + 0x3c]
    movss xmm2, DWORD PTR [rsp + 0x34]
    divss xmm2, xmm1
    movss xmm1, DWORD PTR [rsp + 0x54]
    mulss xmm1, DWORD PTR [rsp + 0x3c]
    movss xmm3, DWORD PTR [rsp + 0x38]
    divss xmm3, xmm1
    movaps xmm4, xmm2
    andps xmm4, XMMWORD PTR [rip + k_abs_mask]
    comiss xmm4, DWORD PTR [rip + k_eight]
    ja reticle_failure
    movaps xmm4, xmm3
    andps xmm4, XMMWORD PTR [rip + k_abs_mask]
    comiss xmm4, DWORD PTR [rip + k_eight]
    ja reticle_failure
    mulss xmm2, DWORD PTR [rsp + 0x40]
    mulss xmm3, DWORD PTR [rsp + 0x44]
    movss DWORD PTR [rsp + 0x34], xmm2
    movss DWORD PTR [rsp + 0x38], xmm3

    // crosshair_size_deg retains its normal angular meaning while the weapon's
    // authored animation, bitmap selection, hit marker, and colour stay native.
    movss xmm0, DWORD PTR [rip + CONFIG_SIZE_RVA]
    ucomiss xmm0, xmm0
    jp reticle_failure
    comiss xmm0, DWORD PTR [rip + k_size_min]
    jb reticle_failure
    comiss xmm0, DWORD PTR [rip + k_size_max]
    ja reticle_failure
    mulss xmm0, DWORD PTR [rip + k_half_degree_radians]
    call TANF_RVA
    ucomiss xmm0, xmm0
    jp reticle_failure
    mulss xmm0, DWORD PTR [rsp + 0x44]
    mulss xmm0, DWORD PTR [rip + k_two]
    movss xmm1, DWORD PTR [rsp + 0x54]
    mulss xmm1, DWORD PTR [rip + k_nominal_reticle_height]
    divss xmm0, xmm1
    ucomiss xmm0, xmm0
    jp reticle_failure
    comiss xmm0, DWORD PTR [rip + k_scale_min]
    jb reticle_failure
    comiss xmm0, DWORD PTR [rip + k_scale_max]
    ja reticle_failure
    mov rcx, QWORD PTR [rsp + 0x20]
    movss DWORD PTR [rcx], xmm0
    movss xmm0, DWORD PTR [rsp + 0x34]
    movss xmm1, DWORD PTR [rsp + 0x38]
    mov al, 1
    add rsp, 0x68
    ret

reticle_hide:
    movss xmm0, DWORD PTR [rip + VIEWPORT_HALF_WIDTH_RVA]
    andps xmm0, XMMWORD PTR [rip + k_abs_mask]
    ucomiss xmm0, xmm0
    jp reticle_hide_from_art
    comiss xmm0, DWORD PTR [rip + k_one]
    jb reticle_hide_from_art
    comiss xmm0, DWORD PTR [rip + k_32768]
    ja reticle_hide_from_art
    jmp reticle_hide_ready
reticle_hide_from_art:
    mov rax, QWORD PTR [rsp + 0x20]
    movss xmm0, DWORD PTR [rax + 0x28]
    andps xmm0, XMMWORD PTR [rip + k_abs_mask]
    ucomiss xmm0, xmm0
    jp reticle_failure
    comiss xmm0, DWORD PTR [rip + k_one]
    jb reticle_failure
    comiss xmm0, DWORD PTR [rip + k_32768]
    ja reticle_failure
reticle_hide_ready:
    mulss xmm0, DWORD PTR [rip + k_four]
    xorps xmm1, xmm1
    mov al, 1
    add rsp, 0x68
    ret

reticle_failure:
    xor eax, eax
    xorps xmm0, xmm0
    xorps xmm1, xmm1
    add rsp, 0x68
    ret

// The retail command body used to publish its eye serial after either a
// position or a hide. Stage 3H routes that publication here so only a real
// native placement can suppress the procedural fallback. RCX is the CUI TLS
// scope and RDX is the core module base used by the original body.
stage3h_publish_native_serial:
    cmp BYTE PTR [rip + CONFIG_CROSSHAIR_RVA], 0
    je publish_native_done
    movsxd r8, DWORD PTR [rcx + 0x3c0]
    cmp r8d, 1
    ja publish_native_done
    mov rax, QWORD PTR [rcx + 0x3d0]
    test rax, rax
    jz publish_native_done
    mov QWORD PTR [rdx + r8*8 + 0x1190], rax
publish_native_done:
    ret

// Shared-compositor suppression is allowed only after both native CUI writes
// match both render FOV serials for the current stereo pair.
stage3h_native_pair_proven:
    sub rsp, 0x28
    call TITLE_ACTIVE_RVA
    cmp al, 4
    jne native_pair_no
    mov rax, QWORD PTR [rip + CUI_SERIAL_RVA]
    test rax, rax
    jz native_pair_no
    cmp rax, QWORD PTR [rip + CUI_SERIAL_RVA + 8]
    jne native_pair_no
    cmp rax, QWORD PTR [rip + RENDER_SERIAL_RVA]
    jne native_pair_no
    cmp rax, QWORD PTR [rip + RENDER_SERIAL_RVA + 8]
    jne native_pair_no
    mov al, 1
    add rsp, 0x28
    ret
native_pair_no:
    xor eax, eax
    add rsp, 0x28
    ret

// Exact local-FP effect hook installer. It fails closed on any byte mismatch;
// no world/enemy/environment effect site is touched.
stage3h_install_effect_hook:
    sub rsp, 0x68
    mov rax, QWORD PTR [rip + CORE_BASE_PTR_RVA]
    test rax, rax
    jz effect_install_done
    mov QWORD PTR [rsp + 0x30], rax
    lea rcx, [rax + H4_LOCAL_EFFECT_SITE_RVA]
    mov QWORD PTR [rsp + 0x20], rcx
    lea r10, [rip + stage3h_effect_wrapper]
    mov QWORD PTR [rsp + 0x28], r10

    cmp WORD PTR [rcx], 0x25ff
    jne effect_check_stock
    cmp DWORD PTR [rcx + 2], 0
    jne effect_install_done
    cmp QWORD PTR [rcx + 6], r10
    je effect_install_done
    jmp effect_install_done

effect_check_stock:
    mov rax, 0x8b4918b644394166
    cmp QWORD PTR [rcx], rax
    jne effect_install_done
    mov rax, 0xfbd5e8c1950f41d7
    cmp QWORD PTR [rcx + 8], rax
    jne effect_install_done
    cmp WORD PTR [rcx + 16], 0xffff
    jne effect_install_done

    xor eax, eax
    mov r10d, 1
    lock cmpxchg DWORD PTR [rip + EFFECT_INSTALL_LOCK_RVA], r10d
    jne effect_install_done

    mov rcx, QWORD PTR [rsp + 0x20]
    mov rax, 0x8b4918b644394166
    cmp QWORD PTR [rcx], rax
    jne effect_install_unlock
    mov rax, 0xfbd5e8c1950f41d7
    cmp QWORD PTR [rcx + 8], rax
    jne effect_install_unlock
    cmp WORD PTR [rcx + 16], 0xffff
    jne effect_install_unlock

    mov edx, 0x12
    mov r8d, 0x40
    lea r9, [rsp + 0x38]
    call QWORD PTR [rip + IAT_VIRTUAL_PROTECT_RVA]
    test eax, eax
    jz effect_install_unlock

    mov r11, QWORD PTR [rsp + 0x20]
    mov rax, QWORD PTR [rsp + 0x30]
    mov QWORD PTR [rip + EFFECT_MODULE_BASE_RVA], rax
    mov DWORD PTR [r11], 0x000025ff
    mov WORD PTR [r11 + 4], 0
    mov rax, QWORD PTR [rsp + 0x28]
    mov QWORD PTR [r11 + 6], rax
    mov DWORD PTR [r11 + 14], 0x90909090

    mov rcx, r11
    mov edx, 0x12
    mov r8d, DWORD PTR [rsp + 0x38]
    lea r9, [rsp + 0x3c]
    call QWORD PTR [rip + IAT_VIRTUAL_PROTECT_RVA]
    call QWORD PTR [rip + IAT_GET_CURRENT_PROCESS_RVA]
    mov rcx, rax
    mov rdx, QWORD PTR [rsp + 0x20]
    mov r8d, 0x12
    call QWORD PTR [rip + IAT_FLUSH_INSTRUCTION_CACHE_RVA]

effect_install_unlock:
    mov DWORD PTR [rip + EFFECT_INSTALL_LOCK_RVA], 0
effect_install_done:
    add rsp, 0x68
    ret

// Mid-function halo4.dll wrapper. It reproduces the displaced retail helper
// call, then zero-scales only: authored location 0 + local effect + camera mode
// 1 + nonnegative first-person attachment/designator.
stage3h_effect_wrapper:
    sub rsp, 0x40
    xor eax, eax
    cmp WORD PTR [r14 + rsi*4 + 0x18], ax
    mov rdx, r15
    mov rcx, rdi
    setne r9b
    mov rax, QWORD PTR [rip + EFFECT_MODULE_BASE_RVA]
    add rax, H4_LOCAL_EFFECT_ORIGINAL_RVA
    call rax
    mov QWORD PTR [rsp + 0x20], rax
    test rax, rax
    jz effect_wrapper_continue
    cmp DWORD PTR [r14 + rsi*4 + 0x14], 0
    jne effect_wrapper_continue
    cmp WORD PTR [r14 + rsi*4 + 0x18], 0
    je effect_wrapper_continue
    cmp WORD PTR [r14 + rsi*4 + 0x1e], 1
    jne effect_wrapper_continue
    cmp WORD PTR [r15 + 2], 0
    jl effect_wrapper_continue
    mov DWORD PTR [rax], 0
effect_wrapper_continue:
    mov rax, QWORD PTR [rsp + 0x20]
    add rsp, 0x40
    mov r10, QWORD PTR [rip + EFFECT_MODULE_BASE_RVA]
    add r10, H4_LOCAL_EFFECT_CONTINUE_RVA
    jmp r10

.p2align 4
k_abs_mask: .long 0x7fffffff, 0x7fffffff, 0x7fffffff, 0x7fffffff
k_sign_mask: .long 0x80000000, 0, 0, 0
k_zero: .float 0.0
k_point_zero_one: .float 0.01
k_scale_min: .float 0.001
k_half: .float 0.5
k_one: .float 1.0
k_two: .float 2.0
k_four: .float 4.0
k_eight: .float 8.0
k_fifty: .float 50.0
k_fov_max: .float 1.56
k_size_min: .float 0.3
k_size_max: .float 20.0
k_scale_max: .float 128.0
k_32768: .float 32768.0
k_half_degree_radians: .float 0.008726646259971648
k_nominal_reticle_height: .float 81.92
