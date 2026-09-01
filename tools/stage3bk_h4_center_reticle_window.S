// Stage 3BK - centre the reticle in the capture window.
//
// Both capture maps (whole-HUD 19:45 and reticle-only 20:31) show the one
// x-centred small element at texel (256, ~360): 104 px BELOW the texture
// centre.  Halo 4 anchors its reticle at the visible screen centre, which
// in its CUI canvas is (0, +52 units), not the canvas origin the engine's
// own framing centres.  The VR quad shows the texture centre at the aim
// point, so the reticle rode 104 px low - small and off-aim, reading as
// "still the placeholder".
//
// Fix: the 3BH draw-time gate already re-asserts the stored capture
// viewport (0x2AE774, 24-byte D3D11_VIEWPORT) on every captured draw.
// Instead of passing that struct straight to RSSetViewports, pass a
// private copy whose TopLeftY is 104.0 px lower (pixel_y = TL_y + f*H,
// so TL_y -= 104 moves canvas (0,+52u) from texel 360 to texel 256).
// Width/height/scale unchanged; the scissor (0,0..512,512) needs no
// change.  Begin rewrites the stored struct every capture, and this thunk
// biases a COPY, so nothing accumulates.
//
// Entered by `call` from the gate at 0x2FB992 (which replaced the
// mov rcx,rbx / mov edx,1 / lea r8,[0x2AE774] / mov rax,[rbx] /
// call [rax+0x160] sequence).  rbx = the immediate context, preserved.
// Clobbers rax/rcx/rdx/r8/xmm0 - all dead at the return point (the gate
// reloads everything for the scissor call that follows).

.intel_syntax noprefix
.text

.globl s3bk_biased_viewport
s3bk_biased_viewport:
    sub     rsp, 0x28                  // keep the D3D call at the gate's own alignment
    movups  xmm0, xmmword ptr [rip + CAPTURE_VIEWPORT]
    movups  xmmword ptr [rip + s3bk_viewport], xmm0
    mov     rax, qword ptr [rip + CAPTURE_VIEWPORT + 0x10]
    mov     qword ptr [rip + s3bk_viewport + 0x10], rax
    movss   xmm0, dword ptr [rip + s3bk_viewport + 4]   // TopLeftY
    subss   xmm0, dword ptr [rip + s3bk_bias]           // -= 104.0 px
    movss   dword ptr [rip + s3bk_viewport + 4], xmm0
    mov     rcx, rbx                   // context
    mov     edx, 1                     // one viewport
    lea     r8, [rip + s3bk_viewport]
    mov     rax, qword ptr [rbx]
    call    qword ptr [rax + 0x160]    // RSSetViewports
    add     rsp, 0x28
    ret

.p2align 2
.globl s3bk_data
s3bk_data:
s3bk_bias:     .long 0x42D00000        // 104.0f
s3bk_viewport: .zero 24               // private D3D11_VIEWPORT copy
