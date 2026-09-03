; C-H4-53: two ABI bridges that must resume in the middle of pinned retail
; functions. Everything else lives in maintained C++ and installs only after
; exact unique-signature proof. These bridges contain no allocation, logging,
; locks, file I/O, or scans.

option casemap:none

EXTERN g_halo4EffectOriginalHelper:QWORD
EXTERN g_halo4EffectTransientContinue:QWORD
EXTERN Halo4EffectHideBridge:PROC
EXTERN g_halo4CurvatureContinue:QWORD
EXTERN g_halo4HudGameplayThreadId:DWORD
EXTERN g_halo4HudCurvatureEnabled:BYTE
EXTERN g_halo4HudCurvatureValue:DWORD

.code

; Exact Stage 3AI transient positive-designator wrapper for
; halo4+0x1012D5. The splice enters by an absolute indirect jump with the
; parent function's rdi/rsi/r14/r15 contract intact.
Halo4EffectTransientWrapper PROC
    sub rsp, 40h
    xor eax, eax
    cmp WORD PTR [r14+rsi*4+18h], ax
    mov rdx, r15
    mov rcx, rdi
    setne r9b
    call QWORD PTR [g_halo4EffectOriginalHelper]
    mov QWORD PTR [rsp+20h], rax
    test rax, rax
    jz effect_continue
    cmp DWORD PTR [r14+rsi*4+14h], 0
    jne effect_continue
    cmp WORD PTR [r14+rsi*4+18h], 0
    je effect_continue
    cmp WORD PTR [r14+rsi*4+1eh], 1
    jne effect_continue
    cmp WORD PTR [r15+2], 0
    jl effect_continue
    mov rcx, rdi
    mov rdx, rax
    call Halo4EffectHideBridge
effect_continue:
    mov rax, QWORD PTR [rsp+20h]
    add rsp, 40h
    jmp QWORD PTR [g_halo4EffectTransientContinue]
Halo4EffectTransientWrapper ENDP

; Exact Stage 3X native prop_curvature_theta consumer bridge for
; halo4+0x420D7E. Gameplay admission is the current render thread ID published
; by the bounded Halo 4 CUI root; shell, pause, and menu calls replay stock.
Halo4CurvatureBridge PROC
    pushfq
    push rax
    push r10
    cmp BYTE PTR [g_halo4HudCurvatureEnabled], 1
    jne curvature_stock
    mov eax, DWORD PTR gs:[48h]
    cmp eax, DWORD PTR [g_halo4HudGameplayThreadId]
    jne curvature_stock
    test eax, eax
    jz curvature_stock
    movss xmm1, DWORD PTR [g_halo4HudCurvatureValue]
    jmp curvature_write
curvature_stock:
    movss xmm1, DWORD PTR [rdi+1e8h]
curvature_write:
    movss DWORD PTR [rbp-5ch], xmm1
    movss xmm1, DWORD PTR [rdi+1ech]
    movss DWORD PTR [rbp-54h], xmm1
    pop r10
    pop rax
    popfq
    jmp QWORD PTR [g_halo4CurvatureContinue]
Halo4CurvatureBridge ENDP

END
