// Stage 3BR - draw the Halo 4 capture 2.5x larger about the reticle centre.
//
// Headset result (2026-08-30 16:20, Stage 3BQ, Steam): the weapon reticle
// finally rides the VR crosshair but is "too small" (73 px of the 512 quad,
// ~14%). The capture viewport maps the CUI at 1:1 engine scale; enlarging
// the viewport about the point the reticle lands on (texel 256,256 - that is
// what the 3BN TopLeftY bias arranges) scales the artwork without moving it.
//
// This thunk replaces the 3BN thunk at the same gate call (0x2FB992). It
// performs exactly the 3BN computation (raster-aspect Height, centring
// TopLeftY, 3BK fallback), then applies
//     W *= k;  H *= k;  TLx = 256 - k*(256 - TLx);  TLy = 256 - k*(256 - TLy)
// with k = 2.5. The stored struct is never written.

.intel_syntax noprefix
.text

.globl s3br_scaled_viewport
s3br_scaled_viewport:
    sub     rsp, 0x28
    movups  xmm0, xmmword ptr [rip + CAPTURE_VIEWPORT]
    movups  xmmword ptr [rip + s3br_viewport], xmm0
    mov     rax, qword ptr [rip + CAPTURE_VIEWPORT + 0x10]
    mov     qword ptr [rip + s3br_viewport + 0x10], rax
    mov     eax, dword ptr [rip + BB_WIDTH]
    test    eax, eax
    jz      .Lfallback
    mov     ecx, dword ptr [rip + BB_HEIGHT]
    test    ecx, ecx
    jz      .Lfallback
    cvtsi2ss xmm2, eax                                  // bbW
    cvtsi2ss xmm1, ecx                                  // bbH
    movss   xmm0, dword ptr [rip + s3br_viewport + 8]   // Width
    mulss   xmm0, xmm1
    divss   xmm0, xmm2                                  // Height = W*bbH/bbW
    movss   dword ptr [rip + s3br_viewport + 12], xmm0
    movss   xmm1, dword ptr [rip + s3br_k512]
    subss   xmm1, xmm0                                  // 512 - H
    mulss   xmm1, dword ptr [rip + s3br_khalf]
    movaps  xmm2, xmm0
    mulss   xmm2, dword ptr [rip + s3br_kbias]          // H * (104/1346.196)
    subss   xmm1, xmm2
    movss   dword ptr [rip + s3br_viewport + 4], xmm1   // TopLeftY
    jmp     .Lscale
.Lfallback:
    movss   xmm0, dword ptr [rip + s3br_viewport + 4]
    subss   xmm0, dword ptr [rip + s3br_k104]
    movss   dword ptr [rip + s3br_viewport + 4], xmm0
.Lscale:
    movss   xmm3, dword ptr [rip + s3br_kscale]         // k
    movss   xmm4, dword ptr [rip + s3br_k256]           // 256.0
    movss   xmm0, dword ptr [rip + s3br_viewport + 8]
    mulss   xmm0, xmm3
    movss   dword ptr [rip + s3br_viewport + 8], xmm0   // W *= k
    movss   xmm0, dword ptr [rip + s3br_viewport + 12]
    mulss   xmm0, xmm3
    movss   dword ptr [rip + s3br_viewport + 12], xmm0  // H *= k
    movss   xmm0, dword ptr [rip + s3br_viewport]
    movaps  xmm1, xmm4
    subss   xmm1, xmm0                                  // 256 - TLx
    mulss   xmm1, xmm3
    movaps  xmm0, xmm4
    subss   xmm0, xmm1                                  // 256 - k*(256 - TLx)
    movss   dword ptr [rip + s3br_viewport], xmm0
    movss   xmm0, dword ptr [rip + s3br_viewport + 4]
    movaps  xmm1, xmm4
    subss   xmm1, xmm0
    mulss   xmm1, xmm3
    movaps  xmm0, xmm4
    subss   xmm0, xmm1
    movss   dword ptr [rip + s3br_viewport + 4], xmm0
    mov     rcx, rbx
    mov     edx, 1
    lea     r8, [rip + s3br_viewport]
    mov     rax, qword ptr [rbx]
    call    qword ptr [rax + 0x160]                     // RSSetViewports
    add     rsp, 0x28
    ret

.p2align 4
.globl s3br_data
s3br_data:
s3br_k512:     .long 0x44000000        // 512.0f
s3br_khalf:    .long 0x3F000000        // 0.5f
s3br_kbias:    .long 0x3D9E37B9        // 0.0772547f = 104 / 1346.196
s3br_k104:     .long 0x42D00000        // 104.0f (3BK fallback)
s3br_kscale:   .long 0x40200000        // 2.5f
s3br_k256:     .long 0x43800000        // 256.0f
s3br_viewport: .zero 24                // private D3D11_VIEWPORT copy
