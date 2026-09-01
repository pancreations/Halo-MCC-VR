// Stage 3BP - capture ONLY the weapon screen's reticle container.
//
// Evidence (H4EK, 2026-08-30):
//   * base_hud.cui_screen instantiates grenade_indicator and damage_indicator;
//     each holds one reticule_offset_container whose only drawing children are
//     polyart_widgets (hud_grenade_indicator / hud_hit_indicator polyart).
//   * assault_rifle.cui_screen's reticule_offset_container holds bitmap
//     widgets (4x ar_corner quarter circles, 4x white_pix ticks) and
//     hit_indicator_art; no polyart_widget anywhere in the weapon screen.
//   * halo4_tag_test.exe, serialise slot 27 of each widget class (vtables
//     resolved from RTTI): PolyartWidget (0xADA660) allocates CUI command
//     type 0x20 (payload 0xB0) or 0x1F (payload 0xF0, three-colour variant);
//     c_bitmap_widget (0xB02530 -> 0xB027B0) allocates type 0x0B / 0x11 /
//     0x0C; ReticuleOffsetContainerWidget allocates 0x28 (payload 0xC) and
//     0x29 - the same allocator (0x9B4600) and the same type numbering the
//     retail executor switches on.
//
// So, within the hidden containers the capture keeps (Stage 3BI), a command
// of type 0x20 or 0x1F marks a polyart container - the damage or grenade
// indicator - and never the weapon reticle.
//
// This thunk replaces the 3BI selector at the same splice (0x53921) and keeps
// every 3BI rule, adding one: when a 0x20/0x1F command arrives while inside a
// 0x28 container, the container is flagged polyart and its transform-stack
// top is moved offscreen (the proven hide shift) BEFORE the command runs -
// transforms are baked into vertex data at command time - and it stays
// offscreen until the matching 0x29.  The weapon container never trips the
// flag and is captured exactly as 3BI captured it.
//
// The header is read before the original call (through the SEH-protected
// Halo4SafeRead) and reused after it for the 3BI classification.  SafeRead
// clobbers the argument registers, so rdx/r8/r9 are saved and restored
// around it.  Same register contract as 3BI: rsi=renderer, r13=command
// (= rdx), r8=openRenderSections, r9=renderContext, r15=TLS base (scope at
// +0x3B8, +2 redirectActive), rbx untouched.

.intel_syntax noprefix
.text

.globl s3bp_capture_select
s3bp_capture_select:
    sub     rsp, 0x58                  // entry rsp==8 mod16 -> 0 mod16
    mov     rax, qword ptr [rip + G_ORIG_CUI_RENDER]
    test    rax, rax
    jz      .Lfail
    mov     qword ptr [rsp + 0x20], rax
    mov     qword ptr [rsp + 0x28], rdx
    mov     qword ptr [rsp + 0x30], r8
    mov     qword ptr [rsp + 0x38], r9
    mov     dword ptr [rsp + 0x40], 0xFFFFFFFF   // header (invalid until read)
    movzx   r10d, byte ptr [r15 + 0x3BA]         // scope.redirectActive
    mov     byte ptr [rsp + 0x44], r10b
    test    r10b, r10b
    jz      .Lcall                     // stock pass: hands off, no read
    test    r13, r13
    jz      .Lcall
    mov     rcx, r13                   // SEH-protected 4-byte header read
    lea     rdx, [rsp + 0x40]
    mov     r8d, 4
    call    HALO4_SAFE_READ
    test    eax, eax
    jnz     .Lpre
    mov     dword ptr [rsp + 0x40], 0xFFFFFFFF
    jmp     .Lcall
.Lpre:
    movzx   eax, word ptr [rsp + 0x40]           // header.command
    cmp     eax, 0x20                            // PolyartWidget draw
    je      .Lpoly
    cmp     eax, 0x1F                            // PolyartWidget draw (3-colour)
    jne     .Lcall
.Lpoly:
    cmp     dword ptr [rip + s3bp_inside], 0
    je      .Lcall                     // polyart outside any 0x28: already offscreen
    mov     dword ptr [rip + s3bp_poly], 1
    call    .Lenforce                  // offscreen BEFORE this draw bakes its vertices
.Lcall:
    mov     rax, qword ptr [rsp + 0x20]
    mov     rdx, qword ptr [rsp + 0x28]
    mov     r8, qword ptr [rsp + 0x30]
    mov     r9, qword ptr [rsp + 0x38]
    mov     rcx, rsi                   // displaced: renderer arg
    call    rax                        // original(renderer, cmd, sections, ctx)
    mov     byte ptr [rsp + 0x48], al  // preserve the dispatch result
    cmp     byte ptr [rsp + 0x44], 0
    je      .Ldone                     // no private target: stock pass, hands off
    movzx   eax, word ptr [rsp + 0x40]           // header.command (same read)
    cmp     eax, 0x28
    jne     .Lnot28
    cmp     word ptr [rsp + 0x42], 0x0C          // header.payloadSize
    jne     .Lenf
    mov     dword ptr [rip + s3bp_inside], 1
    mov     dword ptr [rip + s3bp_poly], 0
    jmp     .Lenf
.Lnot28:
    cmp     eax, 0x29
    jne     .Lenf
    mov     dword ptr [rip + s3bp_inside], 0
    mov     dword ptr [rip + s3bp_poly], 0
.Lenf:
    call    .Lenforce
.Ldone:
    movzx   eax, byte ptr [rsp + 0x48]
    add     rsp, 0x58
    ret
.Lfail:
    xor     eax, eax                   // torn-down dispatcher: command fails
    add     rsp, 0x58                  // closed, exactly as the C++ does
    ret

// Enforce the 3BI rule on the transform-stack top, with
//   effective_inside = inside && !poly.
// Registers only (rsi in; rax, rcx, edx, xmm0-2 scratch); no stack locals.
.Lenforce:
    test    rsi, rsi
    jz      .Lret
    mov     ecx, dword ptr [rsi + 0x870]   // transform stack count
    test    ecx, ecx
    jz      .Lret
    cmp     ecx, 0x60                  // kHalo4CuiTransformStackMaximum
    ja      .Lret
    lea     eax, [rcx - 1]             // top entry = count-1
    imul    rax, rax, 0x34
    lea     rax, [rsi + rax + 0x878]
    movss   xmm0, dword ptr [rax + 0x28]   // translation x
    ucomiss xmm0, xmm0
    jp      .Lret                      // NaN: leave the engine's data alone
    movss   xmm1, dword ptr [rip + CAPTURE_FRAMING_CONST]  // 4*halfWidth
    movaps  xmm2, xmm1
    mulss   xmm2, dword ptr [rip + s3bp_half]              // 2*halfWidth
    mov     edx, dword ptr [rip + s3bp_inside]
    test    edx, edx
    jz      .Loutside
    cmp     dword ptr [rip + s3bp_poly], 0
    jne     .Loutside                  // polyart container: treated as outside
    comiss  xmm0, xmm2                 // the weapon reticle container:
    jb      .Lret                      //   already on target
    subss   xmm0, xmm1                 //   cancel an inherited offset
    movss   dword ptr [rax + 0x28], xmm0
    ret
.Loutside:
    comiss  xmm0, xmm2                 // everything else:
    jae     .Lret                      //   already offscreen
    addss   xmm0, xmm1                 //   the proven hide shift, applied
    movss   dword ptr [rax + 0x28], xmm0
.Lret:
    ret

.p2align 2
.globl s3bp_data
s3bp_data:
s3bp_inside: .long 0                   // 1 between a 0x28 begin and its 0x29
s3bp_poly:   .long 0                   // 1 once a 0x20/0x1F draw was seen inside
s3bp_half:   .long 0x3F000000          // 0.5f
