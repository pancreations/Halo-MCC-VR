.intel_syntax noprefix
.section .text
.global stage3h_h2_viewport

// Post-link helper for the exact Stage 3G C50 layout. It replaces only the
// existing .h2sf proof helper and keeps the same r15=context / rsi=mutation
// call contract. Addresses below are RVAs, so every reference remains valid
// under ASLR after linking this blob at RVA 0x002EE000.
.extern HUD_SIZE_RVA
.extern HUD_ASPECT_RVA
.extern HUD_VERTICAL_RVA
.extern STATE_FAILURES_RVA

stage3h_h2_viewport:
    sub rsp, 0x1b8
    mov DWORD PTR [rsi + 4], 0x10
    mov rax, QWORD PTR [r15]
    lea r8, [rsi + 8]
    lea rdx, [rsi + 4]
    mov rcx, r15
    call QWORD PTR [rax + 0x2f8]
    mov r10d, DWORD PTR [rsi + 4]
    test r10d, r10d
    jz state_failure
    cmp r10d, 0x10
    ja state_failure

    // xmm2 = horizontal fraction, xmm3 = vertical fraction,
    // xmm4 = signed pixel lift. The config parser already bounds these; the
    // clamps make the post-link boundary independently fail-safe.
    movss xmm3, DWORD PTR [rip + HUD_SIZE_RVA]
    maxss xmm3, DWORD PTR [rip + minimum_fraction]
    minss xmm3, DWORD PTR [rip + one]
    movaps xmm2, xmm3
    mulss xmm2, DWORD PTR [rip + HUD_ASPECT_RVA]
    maxss xmm2, DWORD PTR [rip + minimum_fraction]
    minss xmm2, DWORD PTR [rip + one]
    movss xmm4, DWORD PTR [rip + HUD_VERTICAL_RVA]
    maxss xmm4, DWORD PTR [rip + minus_three_hundred]
    minss xmm4, DWORD PTR [rip + plus_three_hundred]

    lea r8, [rsi + 8]
    lea r9, [rsp + 0x20]
    mov r11d, r10d

viewport_loop:
    movups xmm0, XMMWORD PTR [r8]
    movups XMMWORD PTR [r9], xmm0
    mov rax, QWORD PTR [r8 + 0x10]
    mov QWORD PTR [r9 + 0x10], rax

    // centered X = x + width * (1-scaleX)/2; width *= scaleX
    movss xmm0, DWORD PTR [r8 + 8]
    movaps xmm1, xmm0
    movss xmm5, DWORD PTR [rip + one]
    subss xmm5, xmm2
    mulss xmm0, xmm5
    mulss xmm0, DWORD PTR [rip + half]
    addss xmm0, DWORD PTR [r8]
    movss DWORD PTR [r9], xmm0
    mulss xmm1, xmm2
    movss DWORD PTR [r9 + 8], xmm1

    // centered Y = y + height * (1-scaleY)/2 - verticalOffset;
    // positive HUD height therefore raises the native planar HUD.
    movss xmm0, DWORD PTR [r8 + 0xc]
    movaps xmm1, xmm0
    movss xmm5, DWORD PTR [rip + one]
    subss xmm5, xmm3
    mulss xmm0, xmm5
    mulss xmm0, DWORD PTR [rip + half]
    addss xmm0, DWORD PTR [r8 + 4]
    subss xmm0, xmm4
    movss DWORD PTR [r9 + 4], xmm0
    mulss xmm1, xmm3
    movss DWORD PTR [r9 + 0xc], xmm1

    add r8, 0x18
    add r9, 0x18
    dec r11d
    jne viewport_loop

    mov rax, QWORD PTR [r15]
    lea r8, [rsp + 0x20]
    mov edx, r10d
    mov rcx, r15
    call QWORD PTR [rax + 0x160]
    mov BYTE PTR [rsi], 1
    jmp done

state_failure:
    lock inc QWORD PTR [rip + STATE_FAILURES_RVA]

done:
    add rsp, 0x1b8
    ret

.p2align 2
minimum_fraction: .float 0.15
half:              .float 0.5
one:               .float 1.0
minus_three_hundred: .float -300.0
plus_three_hundred:  .float 300.0
