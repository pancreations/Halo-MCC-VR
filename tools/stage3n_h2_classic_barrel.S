.intel_syntax noprefix
.section .text
.global stage3n_config_key_hook
.global stage3n_save_classic_offsets
.global stage3n_menu_classic_offsets
.global stage3n_h2_classic_carrier

.extern STRCMP_RVA
.extern ATOF_RVA
.extern FPRINTF_RVA
.extern SLIDERFLOAT_RVA
.extern STAGE3M_CONFIG_HOOK_RVA
.extern STAGE3M_SAVE_HOOK_RVA
.extern STAGE3M_MENU_HOOK_RVA
.extern H2_BUILD_STABLE_CARRIERS_RVA

.extern CLASSIC_YAW_RVA
.extern CLASSIC_PITCH_RVA
.extern MINUS_30_RVA
.extern PLUS_30_RVA
.extern RAD_PER_DEG_RVA
.extern ONE_THIRD_RVA
.extern TWO_FIFTEENTHS_RVA
.extern KEY_YAW_RVA
.extern KEY_PITCH_RVA
.extern SAVE_FMT_YAW_RVA
.extern SAVE_FMT_PITCH_RVA
.extern MENU_LABEL_YAW_RVA
.extern MENU_LABEL_PITCH_RVA
.extern SLIDER_FMT_RVA

# Intercept only the two new Stage 3N keys. Every other key is passed into the
# exact Stage 3M parser, so Stage 3M remains the owner of gun_right_m/gun_up_m
# and all pre-existing configuration semantics.
stage3n_config_key_hook:
    lea rdx, [rip + KEY_YAW_RVA]
    mov rcx, rbx
    call STRCMP_RVA
    test eax, eax
    je .parse_yaw
    lea rdx, [rip + KEY_PITCH_RVA]
    mov rcx, rbx
    call STRCMP_RVA
    test eax, eax
    je .parse_pitch
    jmp STAGE3M_CONFIG_HOOK_RVA
.parse_yaw:
    mov rcx, rsi
    call ATOF_RVA
    cvtsd2ss xmm1, xmm0
    maxss xmm1, DWORD PTR [rip + MINUS_30_RVA]
    minss xmm1, DWORD PTR [rip + PLUS_30_RVA]
    movss DWORD PTR [rip + CLASSIC_YAW_RVA], xmm1
    # Stage 3M's successful parse target is unchanged.
    jmp 0x5CE9
.parse_pitch:
    mov rcx, rsi
    call ATOF_RVA
    cvtsd2ss xmm1, xmm0
    maxss xmm1, DWORD PTR [rip + MINUS_30_RVA]
    minss xmm1, DWORD PTR [rip + PLUS_30_RVA]
    movss DWORD PTR [rip + CLASSIC_PITCH_RVA], xmm1
    jmp 0x5CE9

# Preserve Stage 3M's original save transaction, then append the two Classic-only
# angular trims. rcx is the FILE* at the call boundary.
stage3n_save_classic_offsets:
    sub rsp, 0x38
    mov QWORD PTR [rsp + 0x30], rcx
    call STAGE3M_SAVE_HOOK_RVA
    mov rcx, QWORD PTR [rsp + 0x30]
    lea rdx, [rip + SAVE_FMT_YAW_RVA]
    movss xmm2, DWORD PTR [rip + CLASSIC_YAW_RVA]
    cvtps2pd xmm2, xmm2
    movq r8, xmm2
    call FPRINTF_RVA
    mov rcx, QWORD PTR [rsp + 0x30]
    lea rdx, [rip + SAVE_FMT_PITCH_RVA]
    movss xmm2, DWORD PTR [rip + CLASSIC_PITCH_RVA]
    cvtps2pd xmm2, xmm2
    movq r8, xmm2
    call FPRINTF_RVA
    add rsp, 0x38
    ret

# Stage 3M renders the shared Right/Up stock controls first. Add two angular
# controls immediately after them. The caller owns BL as the aggregate changed
# bit, matching the exact Stage 3M menu hook semantics.
stage3n_menu_classic_offsets:
    sub rsp, 0x38
    call STAGE3M_MENU_HOOK_RVA
    lea rcx, [rip + MENU_LABEL_YAW_RVA]
    lea rdx, [rip + CLASSIC_YAW_RVA]
    movss xmm2, DWORD PTR [rip + MINUS_30_RVA]
    movss xmm3, DWORD PTR [rip + PLUS_30_RVA]
    lea rax, [rip + SLIDER_FMT_RVA]
    mov QWORD PTR [rsp + 0x20], rax
    mov DWORD PTR [rsp + 0x28], 0
    call SLIDERFLOAT_RVA
    or bl, al
    lea rcx, [rip + MENU_LABEL_PITCH_RVA]
    lea rdx, [rip + CLASSIC_PITCH_RVA]
    movss xmm2, DWORD PTR [rip + MINUS_30_RVA]
    movss xmm3, DWORD PTR [rip + PLUS_30_RVA]
    lea rax, [rip + SLIDER_FMT_RVA]
    mov QWORD PTR [rsp + 0x20], rax
    mov DWORD PTR [rsp + 0x28], 0
    call SLIDERFLOAT_RVA
    or bl, al
    add rsp, 0x38
    ret

# Input/output contract is the exact BuildStableFirstPersonCarriers call:
#   rcx publication, edx generation, r8 -> rightCarrier, r9 -> leftCarrier
# The packet-builder caller keeps publishToRenderer in nonvolatile r15b:
#   r15b != 0 = Anniversary/Saber consumer
#   r15b == 0 = Classic consumer
#
# We call the original carrier builder first. Only after it succeeds, and only
# on the Classic consumer, rotate the visible right carrier's forward/up basis.
# Position is untouched. The direct H2 shot/reticle transaction separately calls
# the original carrier builder and never passes through this wrapper, so the
# bullet ray/crosshair cannot inherit this visual-only correction.
stage3n_h2_classic_carrier:
    sub rsp, 0x68
    mov QWORD PTR [rsp + 0x58], r8
    call H2_BUILD_STABLE_CARRIERS_RVA
    test al, al
    je .carrier_done
    test r15b, r15b
    jne .carrier_success

    # Exact zero means byte-for-byte Stage 3M behavior for the default state.
    mov eax, DWORD PTR [rip + CLASSIC_YAW_RVA]
    and eax, 0x7fffffff
    mov edx, DWORD PTR [rip + CLASSIC_PITCH_RVA]
    and edx, 0x7fffffff
    or eax, edx
    je .carrier_success

    mov r10, QWORD PTR [rsp + 0x58]

    # right = normalize(cross(forward, up)); carrier layout is
    # position +0x00, forward +0x0c, up +0x18.
    movss xmm0, DWORD PTR [r10 + 0x10]
    mulss xmm0, DWORD PTR [r10 + 0x20]
    movss xmm1, DWORD PTR [r10 + 0x14]
    mulss xmm1, DWORD PTR [r10 + 0x1c]
    subss xmm0, xmm1
    movss DWORD PTR [rsp + 0x20], xmm0

    movss xmm0, DWORD PTR [r10 + 0x14]
    mulss xmm0, DWORD PTR [r10 + 0x18]
    movss xmm1, DWORD PTR [r10 + 0x0c]
    mulss xmm1, DWORD PTR [r10 + 0x20]
    subss xmm0, xmm1
    movss DWORD PTR [rsp + 0x24], xmm0

    movss xmm0, DWORD PTR [r10 + 0x0c]
    mulss xmm0, DWORD PTR [r10 + 0x1c]
    movss xmm1, DWORD PTR [r10 + 0x10]
    mulss xmm1, DWORD PTR [r10 + 0x18]
    subss xmm0, xmm1
    movss DWORD PTR [rsp + 0x28], xmm0

    movss xmm0, DWORD PTR [rsp + 0x20]
    mulss xmm0, xmm0
    movss xmm1, DWORD PTR [rsp + 0x24]
    mulss xmm1, xmm1
    addss xmm0, xmm1
    movss xmm1, DWORD PTR [rsp + 0x28]
    mulss xmm1, xmm1
    addss xmm0, xmm1
    sqrtss xmm0, xmm0
    # Valid carriers are orthonormal. If anything impossible reaches this hook,
    # fail open to the unmodified Stage 3M carrier rather than divide by zero.
    xorps xmm1, xmm1
    ucomiss xmm0, xmm1
    jbe .carrier_success
    movss xmm1, DWORD PTR [rsp + 0x20]
    divss xmm1, xmm0
    movss DWORD PTR [rsp + 0x20], xmm1
    movss xmm1, DWORD PTR [rsp + 0x24]
    divss xmm1, xmm0
    movss DWORD PTR [rsp + 0x24], xmm1
    movss xmm1, DWORD PTR [rsp + 0x28]
    divss xmm1, xmm0
    movss DWORD PTR [rsp + 0x28], xmm1

    # tan(yaw) over the clamped +/-30 degree range using
    # x + x^3/3 + 2*x^5/15. Error is tiny for calibration purposes and avoids
    # importing or guessing a CRT trig boundary in this post-link candidate.
    movss xmm0, DWORD PTR [rip + CLASSIC_YAW_RVA]
    call .tan_degrees
    movss DWORD PTR [rsp + 0x2c], xmm0

    # F1 = normalize(F + right * tan(yaw)). U remains unchanged, so this is an
    # exact yaw rotation after normalization for an orthonormal input basis.
    movss xmm1, DWORD PTR [rsp + 0x20]
    mulss xmm1, xmm0
    addss xmm1, DWORD PTR [r10 + 0x0c]
    movss DWORD PTR [rsp + 0x30], xmm1
    movss xmm1, DWORD PTR [rsp + 0x24]
    mulss xmm1, xmm0
    addss xmm1, DWORD PTR [r10 + 0x10]
    movss DWORD PTR [rsp + 0x34], xmm1
    movss xmm1, DWORD PTR [rsp + 0x28]
    mulss xmm1, xmm0
    addss xmm1, DWORD PTR [r10 + 0x14]
    movss DWORD PTR [rsp + 0x38], xmm1

    movss xmm0, DWORD PTR [rsp + 0x30]
    mulss xmm0, xmm0
    movss xmm1, DWORD PTR [rsp + 0x34]
    mulss xmm1, xmm1
    addss xmm0, xmm1
    movss xmm1, DWORD PTR [rsp + 0x38]
    mulss xmm1, xmm1
    addss xmm0, xmm1
    sqrtss xmm0, xmm0
    xorps xmm1, xmm1
    ucomiss xmm0, xmm1
    jbe .carrier_success
    movss xmm1, DWORD PTR [rsp + 0x30]
    divss xmm1, xmm0
    movss DWORD PTR [rsp + 0x30], xmm1
    movss xmm1, DWORD PTR [rsp + 0x34]
    divss xmm1, xmm0
    movss DWORD PTR [rsp + 0x34], xmm1
    movss xmm1, DWORD PTR [rsp + 0x38]
    divss xmm1, xmm0
    movss DWORD PTR [rsp + 0x38], xmm1

    # Pitch: F2 = normalize(F1 + U*t), U2 = normalize(U - F1*t).
    # Because F1 and U remain orthogonal after yaw, both share sqrt(1+t^2).
    movss xmm0, DWORD PTR [rip + CLASSIC_PITCH_RVA]
    call .tan_degrees
    movss DWORD PTR [rsp + 0x3c], xmm0

    movss xmm1, DWORD PTR [r10 + 0x18]
    mulss xmm1, xmm0
    addss xmm1, DWORD PTR [rsp + 0x30]
    movss DWORD PTR [rsp + 0x40], xmm1
    movss xmm1, DWORD PTR [r10 + 0x1c]
    mulss xmm1, xmm0
    addss xmm1, DWORD PTR [rsp + 0x34]
    movss DWORD PTR [rsp + 0x44], xmm1
    movss xmm1, DWORD PTR [r10 + 0x20]
    mulss xmm1, xmm0
    addss xmm1, DWORD PTR [rsp + 0x38]
    movss DWORD PTR [rsp + 0x48], xmm1

    movss xmm1, DWORD PTR [rsp + 0x30]
    mulss xmm1, xmm0
    movss xmm2, DWORD PTR [r10 + 0x18]
    subss xmm2, xmm1
    movss DWORD PTR [rsp + 0x4c], xmm2
    movss xmm1, DWORD PTR [rsp + 0x34]
    mulss xmm1, xmm0
    movss xmm2, DWORD PTR [r10 + 0x1c]
    subss xmm2, xmm1
    movss DWORD PTR [rsp + 0x50], xmm2
    movss xmm1, DWORD PTR [rsp + 0x38]
    mulss xmm1, xmm0
    movss xmm2, DWORD PTR [r10 + 0x20]
    subss xmm2, xmm1
    movss DWORD PTR [rsp + 0x54], xmm2

    movss xmm0, DWORD PTR [rsp + 0x40]
    mulss xmm0, xmm0
    movss xmm1, DWORD PTR [rsp + 0x44]
    mulss xmm1, xmm1
    addss xmm0, xmm1
    movss xmm1, DWORD PTR [rsp + 0x48]
    mulss xmm1, xmm1
    addss xmm0, xmm1
    sqrtss xmm0, xmm0
    xorps xmm1, xmm1
    ucomiss xmm0, xmm1
    jbe .carrier_success

    movss xmm1, DWORD PTR [rsp + 0x40]
    divss xmm1, xmm0
    movss DWORD PTR [r10 + 0x0c], xmm1
    movss xmm1, DWORD PTR [rsp + 0x44]
    divss xmm1, xmm0
    movss DWORD PTR [r10 + 0x10], xmm1
    movss xmm1, DWORD PTR [rsp + 0x48]
    divss xmm1, xmm0
    movss DWORD PTR [r10 + 0x14], xmm1
    movss xmm1, DWORD PTR [rsp + 0x4c]
    divss xmm1, xmm0
    movss DWORD PTR [r10 + 0x18], xmm1
    movss xmm1, DWORD PTR [rsp + 0x50]
    divss xmm1, xmm0
    movss DWORD PTR [r10 + 0x1c], xmm1
    movss xmm1, DWORD PTR [rsp + 0x54]
    divss xmm1, xmm0
    movss DWORD PTR [r10 + 0x20], xmm1

.carrier_success:
    mov al, 1
.carrier_done:
    add rsp, 0x68
    ret

# xmm0 = degrees, returns xmm0 ~= tan(degrees), valid for +/-30 degrees.
.tan_degrees:
    mulss xmm0, DWORD PTR [rip + RAD_PER_DEG_RVA]
    movaps xmm1, xmm0
    mulss xmm1, xmm1
    movaps xmm2, xmm1
    mulss xmm2, DWORD PTR [rip + TWO_FIFTEENTHS_RVA]
    addss xmm2, DWORD PTR [rip + ONE_THIRD_RVA]
    mulss xmm2, xmm1
    mulss xmm2, xmm0
    addss xmm0, xmm2
    ret
