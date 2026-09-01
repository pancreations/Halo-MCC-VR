// Stage 3CF - log-only probe: what id word does tonight's type-0x28 carry?
//
// The 3BP selector arms the capture rescue only when a type-0x28 command's
// word +2 equals 0xC - a value measured in one session, exactly like the
// framing constants 3CE just made live. Tonight's captures run but the
// dumps are byte-empty, consistent with the arm check never passing. This
// probe replaces the selector's `cmp word [rsp+0x42],0xC / jne` pair with a
// jump here: it logs the id word whenever it CHANGES (a handful of lines per
// session), then performs the exact original compare and rejoins the
// original arm/skip paths. rsp is unchanged at entry (jmp, not call), so
// [rsp+0x42] still addresses the selector's local. rbx is preserved around
// the LOG call; volatiles are dead at this point in the selector (the outer
// frame reloads rax/rdx/r8/r9 from its own stack slots).
//
// defsyms: LOGFN=0x1D90, ARM_PATH=0x2F9965, SKIP_PATH=0x2F9994

.intel_syntax noprefix
.text

.globl s3cf_id_probe
s3cf_id_probe:
    movzx   eax, word ptr [rsp + 0x42]
    cmp     ax, word ptr [rip + s3cf_last]
    je      .Lcompare
    mov     word ptr [rip + s3cf_last], ax
    push    rbx
    sub     rsp, 0x20
    movzx   edx, ax
    lea     rcx, [rip + s3cf_fmt]
    call    LOGFN
    add     rsp, 0x20
    pop     rbx
.Lcompare:
    cmp     word ptr [rsp + 0x42], 0xC
    je      ARM_PATH
    jmp     SKIP_PATH

s3cf_fmt:
    .asciz "H4ID: 0x28 word+2 = 0x%X (arm expects 0xC)"
    .balign 2
s3cf_last:
    .short 0xFFFF
