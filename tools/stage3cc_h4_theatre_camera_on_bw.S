// Stage 3CC - Halo 4 authored theatre camera on exact Stage 3BW.

.intel_syntax noprefix
.text

.globl s3cc_get_render_snapshot
s3cc_get_render_snapshot:
    sub     rsp, 0x28
    mov     qword ptr [rsp + 0x20], rcx
    call    SNAPSHOT_FN
    test    al, al
    jz      .Ldone
    call    THEATRE_ACTIVE
    test    al, al
    jz      .Lsuccess

    mov     rcx, qword ptr [rsp + 0x20]
    movss   xmm0, dword ptr [rip + THEATRE_DEPTH]
    ucomiss xmm0, xmm0
    jnp     .Lfinite
    mov     eax, 0x3F800000
    movd    xmm0, eax
    jmp     .Lscale
.Lfinite:
    xorps   xmm1, xmm1
    maxss   xmm0, xmm1
    minss   xmm0, dword ptr [rip + s3cc_two]
.Lscale:
    shufps  xmm0, xmm0, 0
    movups  xmm1, xmmword ptr [rcx + 0x08]
    mulps   xmm1, xmm0
    movups  xmmword ptr [rcx + 0x08], xmm1
    movups  xmm1, xmmword ptr [rcx + 0x38]
    mulps   xmm1, xmm0
    movups  xmmword ptr [rcx + 0x38], xmm1

    xor     edx, edx
    mov     qword ptr [rcx + 0x14], rdx
    mov     dword ptr [rcx + 0x1C], edx
    mov     eax, 0x3F800000
    mov     dword ptr [rcx + 0x20], eax
    mov     qword ptr [rcx + 0x44], rdx
    mov     dword ptr [rcx + 0x4C], edx
    mov     dword ptr [rcx + 0x50], eax
    mov     byte ptr [rcx + 0x34], dl
    mov     byte ptr [rcx + 0x64], dl
    mov     byte ptr [rcx + 0x84], dl
.Lsuccess:
    mov     al, 1
.Ldone:
    add     rsp, 0x28
    ret

.balign 4
s3cc_two:
    .long 0x40000000
