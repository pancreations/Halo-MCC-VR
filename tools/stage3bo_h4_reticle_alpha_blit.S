// Stage 3BO - give the captured Halo 4 reticle an alpha channel on its way
// to the VR crosshair quad.
//
// Evidence (2026-08-30, Steam, Stage 3BN 1e440f34 run at 10:23): the four
// 3BM raw dumps of the published capture hold a 73x69 px figure at the
// centre - an inner ring plus four thick arcs at N/S/E/W - in HUD blue
// (0,152,229) in dumps 1/3/4 and in RED (176,0,0) in dump 2.  The H4EK tag
// `ui/hud/reticles/unsc_assaultrifle/ar_corner.bitmap` decodes to exactly
// such a thick 96x96 arc (white RGB, shape in alpha): the figure IS the
// assault-rifle reticle, drawn blue and turning red on an enemy.  Every
// texel of it has ALPHA 0.  The VR reticle quad is composited with
// XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT, so a picture with
// no alpha draws as nothing - "the reticle is completely invisible".
//
// The upload copies the capture with a plain CopyResource (the log's only
// blit-path line is "FAST CopyResource"), so no shader ever touches it.
// This stage, for the active title Halo 4 and a 512-px destination only:
//
//   1. makes Blit (0x11E00) take its shader path: the inline fast-path
//      decision at 0x11E76 (10 bytes: cmp [rdi+0x14],1 / ja / mov bl,1 /
//      jmp) becomes a call to s3bo_fastpath, which returns bl = 0 (slow)
//      for H4 + 512-px destinations and the exact original answer
//      otherwise;
//   2. keeps the PERF transition log quiet for that blit: the gate at
//      0x11EB6 (movzx eax,bl / cmp [loggedPath],eax) becomes a call to
//      s3bo_loggate, which yields ZF=1 (skip) for H4 + 512 and the
//      original compare otherwise, so eye blits never see a transition;
//   3. repoints the blit pipeline's HLSL text (the `static const char*
//      src` slot at 0x2A6770) to a copy whose two pixel shaders, for a
//      512x512 source only, set alpha = max(alpha, max(r,g,b)) and
//      un-premultiply the colour by it.  Eye blits (3786x2730) and every
//      other size pass through bit-for-bit as before.
//
// Registers at 0x11E76: rdi = &srcDesc, r12d = dstH, bl = result.  At
// 0x11EB6: bl = fastPath, r12d = dstH still live, eax is being defined.
// Neither thunk touches anything else; `ret` preserves the flags the
// caller's `je` consumes.

.intel_syntax noprefix
.text

.globl s3bo_fastpath
s3bo_fastpath:
    cmp     dword ptr [rdi + 0x14], 1            // srcDesc.SampleDesc.Count
    ja      .Lslow                               // multisampled: never a copy (original)
    cmp     byte ptr [rip + ACTIVE_TITLE], 4     // Halo 4?
    jne     .Lfast
    cmp     r12d, 512                            // reticle chain destination?
    jne     .Lfast
.Lslow:
    xor     bl, bl
    ret
.Lfast:
    mov     bl, 1
    ret

.globl s3bo_loggate
s3bo_loggate:
    movzx   eax, bl                              // displaced instruction
    cmp     byte ptr [rip + ACTIVE_TITLE], 4
    jne     .Lnormal
    cmp     r12d, 512
    jne     .Lnormal
    cmp     eax, eax                             // ZF=1: skip the transition log
    ret
.Lnormal:
    cmp     dword ptr [rip + LOGGED_PATH], eax   // displaced instruction
    ret
