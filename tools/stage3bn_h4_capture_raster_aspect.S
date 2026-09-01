// Stage 3BN - give the Halo 4 capture viewport the raster's aspect ratio.
//
// Evidence (2026-08-29): the H4EK decompile of the type-0x28 transform
// compute (tag_test 0x9BC7B0 = retail 0x3F21CC) shows the reticle container's
// translation is the CAMERA PROJECTION of the aim direction, mapped through
// the window's int16 rects and divided by the UI scale.  The (-1033.6,+336.5)
// the mod logs is therefore the projected aim point under the VR-widened
// asymmetric projection - not canvas half-extents - and the 4x-of-that
// viewport (4134x1346, aspect 3.07) has no basis.  Halo 4's CUI projection
// keeps the raster's aspect (3786x2730 = 1.387), so a 3.07:1 viewport squashes
// everything 2.2x vertically: the round AR reticle (4 `ar_corner` quarter
// circles on a +-37 unit square) comes out as a 96x40 px arc, which is exactly
// every capture map since 3BH, and exactly "part of the damage indication".
//
// This thunk replaces the 3BK thunk at the same gate call (0x2FB992).  It
// builds the private viewport copy with
//     Height   = Width * backbuffer.Height / backbuffer.Width
//     TopLeftY = (512 - Height) * 0.5 - Height * (104 / 1346.196)
// i.e. the same raster-aspect framing the engine uses for its own pass, and
// the reticle-centring bias 3BK proved (104 px of a 1346 px viewport) carried
// over as the same fraction of the new height.  Width, TopLeftX, depth and
// the stored struct are untouched.  If the backbuffer size is unreadable
// (zero), it falls back to the exact 3BK behaviour.

.intel_syntax noprefix
.text

.globl s3bn_aspect_viewport
s3bn_aspect_viewport:
    sub     rsp, 0x28
    movups  xmm0, xmmword ptr [rip + CAPTURE_VIEWPORT]
    movups  xmmword ptr [rip + s3bn_viewport], xmm0
    mov     rax, qword ptr [rip + CAPTURE_VIEWPORT + 0x10]
    mov     qword ptr [rip + s3bn_viewport + 0x10], rax
    mov     eax, dword ptr [rip + BB_WIDTH]
    test    eax, eax
    jz      .Lfallback
    mov     ecx, dword ptr [rip + BB_HEIGHT]
    test    ecx, ecx
    jz      .Lfallback
    cvtsi2ss xmm2, eax                                  // bbW
    cvtsi2ss xmm1, ecx                                  // bbH
    movss   xmm0, dword ptr [rip + s3bn_viewport + 8]   // Width
    mulss   xmm0, xmm1
    divss   xmm0, xmm2                                  // Height = W*bbH/bbW
    movss   dword ptr [rip + s3bn_viewport + 12], xmm0
    movss   xmm1, dword ptr [rip + s3bn_k512]
    subss   xmm1, xmm0                                  // 512 - H
    mulss   xmm1, dword ptr [rip + s3bn_khalf]          // (512 - H) * 0.5
    movaps  xmm2, xmm0
    mulss   xmm2, dword ptr [rip + s3bn_kbias]          // H * (104/1346.196)
    subss   xmm1, xmm2
    movss   dword ptr [rip + s3bn_viewport + 4], xmm1   // TopLeftY
    jmp     .Lcall
.Lfallback:
    movss   xmm0, dword ptr [rip + s3bn_viewport + 4]
    subss   xmm0, dword ptr [rip + s3bn_k104]
    movss   dword ptr [rip + s3bn_viewport + 4], xmm0
.Lcall:
    mov     rcx, rbx
    mov     edx, 1
    lea     r8, [rip + s3bn_viewport]
    mov     rax, qword ptr [rbx]
    call    qword ptr [rax + 0x160]                     // RSSetViewports
    add     rsp, 0x28
    ret

.p2align 4
.globl s3bn_data
s3bn_data:
s3bn_k512:     .long 0x44000000        // 512.0f
s3bn_khalf:    .long 0x3F000000        // 0.5f
s3bn_kbias:    .long 0x3D9E37B9        // 0.0772547f = 104 / 1346.196
s3bn_k104:     .long 0x42D00000        // 104.0f (3BK fallback)
s3bn_viewport: .zero 24                // private D3D11_VIEWPORT copy
