// Stage 3BI - capture ONLY the element the hide already proves is the
// reticle.  The user stated the requirement exactly: "you somehow were able
// to hide it from the hud? that hidden element is what needs to be projected
// on the crosshair".
//
// The shipped DLL has two selectors that disagree:
//
//   VISIBLE pass  (Halo4CuiRenderCommandBody, 0x53a79..):  for each
//     type-0x28 / payload-0xC begin, shift that container's pushed
//     transform +4*halfWidth in x.  PROVEN by headset: the reticle
//     disappears from the flat HUD.
//   CAPTURE replay (0x538c8..0x53a74):  "Capture replay owns the whole CUI
//     command stream" - NO selection at all; it replays every HUD element
//     onto the 512x512 texture and relies on the viewport to crop.  The 3BH
//     full-coverage map proved the crop keeps the health bar, objective and
//     other bands.  That is what the user sees on the crosshair quad.
//
// This stage applies the PROVEN selector to the capture, inverted:
//
//   after every command the capture replay executes (splice at the one
//   per-command original() call site, 0x53921):
//     - only while scope.redirectActive (the private capture target is
//       live; a failed Begin means this replay IS the user's one visible
//       stock pass and must not be touched),
//     - type 0x28 + payload 0xC begin  -> inside := 1
//     - type 0x29 end                  -> inside := 0
//     - then enforce on the transform-stack TOP entry (renderer+0x878 +
//       (count-1)*0x34, translation x at +0x28), idempotently:
//         inside=1: x must be ON  target (x <  2*halfWidth); if it carries
//                   the offscreen offset (inherited composition), x -= 4*hw
//         inside=0: x must be OFF target (x >= 2*halfWidth); if on target,
//                   x += 4*hw   - the same shift, same axis, same magnitude
//                   as the proven hide.
//
// 4*halfWidth is read from the Stage 3BD/3BH framing constant at 0x2FB800
// (4134.312 = the engine's own measured capture viewport width), so the
// threshold 2*halfWidth adapts if that constant is ever retuned.  The
// transform write is the exact operation the accepted hide performs each
// frame; transforms are baked into vertex data at command time, which is
// why this survives the render-section batching boundary that defeated
// draw-level scoping (C-H4-43j / Stage 3BC).
//
// The command header is read through the DLL's own SEH-protected
// Halo4SafeRead (0x56c10).  The stack-top write targets the same entry the
// proven hide writes.  No allocation, no logging, no locks; deterministic.
//
// Registers at the splice (0x53921, capture branch of
// Halo4CuiRenderCommandBody): rsi=renderer, r13=command, rdx=command,
// r8=openRenderSections, r9=renderContext, r15=TLS base (scope at +0x3B8:
// +2 redirectActive), rbx=0 (caller relies on it; not touched).  The
// displaced instructions are `mov rcx,rsi` + `call qword ptr [rsp+0x30]`;
// [rsp+0x30] is the function's local copy of g_halo4OrigCuiRenderCommand
// (0x2B9B18), never modified after entry, so calling through the global is
// equivalent; teardown drains activeCallbacks before clearing it.

.intel_syntax noprefix
.text

.globl s3bi_capture_select
s3bi_capture_select:
    sub     rsp, 0x38                  // entry rsp==8 mod16 -> 0 mod16
    mov     rax, qword ptr [rip + G_ORIG_CUI_RENDER]
    test    rax, rax
    jz      .Lfail
    movzx   r10d, byte ptr [r15 + 0x3BA]   // scope.redirectActive
    mov     byte ptr [rsp + 0x34], r10b
    mov     rcx, rsi                   // displaced: renderer arg
    call    rax                        // original(renderer, cmd, sections, ctx)
    mov     byte ptr [rsp + 0x30], al  // preserve the dispatch result
    cmp     byte ptr [rsp + 0x34], 0
    je      .Ldone                     // no private target: stock pass, hands off
    test    rsi, rsi
    jz      .Ldone
    mov     ecx, dword ptr [rsi + 0x870]   // transform stack count
    test    ecx, ecx
    jz      .Ldone
    cmp     ecx, 0x60                  // kHalo4CuiTransformStackMaximum
    ja      .Ldone
    mov     dword ptr [rsp + 0x28], ecx
    mov     dword ptr [rsp + 0x2C], 0xFFFFFFFF
    test    r13, r13
    jz      .Lclassify
    mov     rcx, r13                   // SEH-protected 4-byte header read
    lea     rdx, [rsp + 0x2C]
    mov     r8d, 4
    call    HALO4_SAFE_READ
    test    eax, eax
    jnz     .Lclassify
    mov     dword ptr [rsp + 0x2C], 0xFFFFFFFF
.Lclassify:
    mov     ecx, dword ptr [rsp + 0x28]
    movzx   eax, word ptr [rsp + 0x2C]     // header.command
    cmp     eax, 0x28
    jne     .Lnot28
    cmp     word ptr [rsp + 0x2E], 0x0C    // header.payloadSize
    jne     .Lenforce
    mov     dword ptr [rip + s3bi_inside], 1
    jmp     .Lenforce
.Lnot28:
    cmp     eax, 0x29
    jne     .Lenforce
    mov     dword ptr [rip + s3bi_inside], 0
.Lenforce:
    lea     eax, [rcx - 1]             // top entry = count-1
    imul    rax, rax, 0x34
    lea     rax, [rsi + rax + 0x878]
    movss   xmm0, dword ptr [rax + 0x28]   // translation x
    ucomiss xmm0, xmm0
    jp      .Ldone                     // NaN: leave the engine's data alone
    movss   xmm1, dword ptr [rip + CAPTURE_FRAMING_CONST]  // 4*halfWidth
    movaps  xmm2, xmm1
    mulss   xmm2, dword ptr [rip + s3bi_half]              // 2*halfWidth
    cmp     dword ptr [rip + s3bi_inside], 0
    jne     .Linside
    comiss  xmm0, xmm2                 // outside the reticle container:
    jae     .Ldone                     //   already offscreen
    addss   xmm0, xmm1                 //   the proven hide shift, applied
    movss   dword ptr [rax + 0x28], xmm0
    jmp     .Ldone
.Linside:
    comiss  xmm0, xmm2                 // the hidden element itself:
    jb      .Ldone                     //   already on target
    subss   xmm0, xmm1                 //   cancel an inherited offset
    movss   dword ptr [rax + 0x28], xmm0
.Ldone:
    movzx   eax, byte ptr [rsp + 0x30]
    add     rsp, 0x38
    ret
.Lfail:
    xor     eax, eax                   // torn-down dispatcher: command fails
    add     rsp, 0x38                  // closed, exactly as the C++ does
    ret

.p2align 2
.globl s3bi_data
s3bi_data:
s3bi_inside: .long 0                   // 1 between a 0x28 begin and its 0x29
s3bi_half:   .long 0x3F000000          // 0.5f
