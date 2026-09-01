// Stage 3BX - Halo 4 theatre obeys the ODST look-constraint rule.
//
// User requirement (2026-08-30): "if you're able to turn your head, then you
// should be in 6DOF in-game stereo; if player movement is locked, that is
// when we are in the 3D cutscene" - i.e. exactly the ODST standard, which
// exists because ODST's drop-pod shot is flagged cinematic while the player
// still has look freedom (docs/CUTSCENE-THEATRE-EVIDENCE.md, "ODST live look
// constraints"). Stage 3BW published AuthoredLocked on the in-progress byte
// alone - the same too-weak test ODST's first headset pass disproved.
//
// Halo 4 evidence (H4EK-first; retail verifies), all newly proven here:
//  - H4EK halo4_tag_test.exe carries the same authored block ODST has:
//    `cinematic_shot_user_input_constraints_block`
//    (sizeof(s_scene_shot_user_input_constraints)) with fields `ticks`,
//    `maximum look angles`, `frictional force`, and the script external
//    `cinematic_scripting_set_user_input_constraints` ("Set user input
//    constraints from a cinematic tag").
//  - Retail halo4.dll registers that external at 0x15EC5B; its implementation
//    0x2C9314 resolves scene -> shot -> constraint record and calls the live
//    writer 0x28D18C. That writer uses the engine TLS index (halo4+0x1057218),
//    the cinematic camera at TLS block +0x58, verifies camera type word
//    [cam+2] == 6, and computes per-tick rates into +0xD8/+0xDC/+0xE0/+0xE4
//    from targets vs the CURRENT limits at +0xC8/+0xCC/+0xD0/+0xD4, storing
//    the remaining tick count at +0xF0 (friction at +0xE8/+0xEC).
//  - The paired reset 0x28D00B zeroes the whole block (both movdqu stores)
//    and sets type 6, proving all-zero limits is the engine's own "no look
//    freedom" state; the per-tick consumer at 0x28C91B reads the same block.
//    ODST's proven layout is +0xC8..0xD4 / +0xD8..0xE4 / +0xF0 on camera type
//    5 at TLS+0x50 - Halo 4 is the same structure at TLS+0x58, type 6.
//
// Rule (ClassifyOdstCinematicControl, ported exactly):
//   in-progress byte 0                    -> PlayerControlled  (state 3)
//   any current limit non-zero            -> PlayerControlled  (state 5)
//   ticks > 0 with any non-zero rate      -> PlayerControlled  (state 5)
//   all limits zero, no pending freedom   -> AuthoredLocked    (state 4)
//   camera missing / wrong type / ticks
//   out of range / non-finite             -> Unknown           (state 6)
// Unknown and the no-proof states publish NOTHING: the shared resolver sees
// a stale publication and keeps the frame immersive. Non-finite values fail
// toward look freedom (immersive), never toward theatre: the |x| test is an
// integer compare on the masked exponent/mantissa, so a NaN or Inf limit
// reads as freedom.
//
// Splice: the jne at 0x2C2BC (3BW) lands here; the thunk still ends with
// `jmp S3BU_THUNK`, so the accepted 3BU write-back runs unchanged. The 3BW
// payload stays byte-identical but unreachable; capability grants from 3BW
// are untouched.
//
// defsyms: LOGFN=0x1D90, IAT_TICK64=0x180150, IAT_GETMODW=0x1800F8,
//          S3BU_THUNK=0x2F9D10, PUB_CTRL=0x88400, PUB_PROJ=0x884C0,
//          GEN_FN=0xA5A0, GTR=0x2BA538, BBW=0x2AEB58, BBH=0x2AEB5C

.intel_syntax noprefix
.text

// |x| > 1e-4 on the raw float bits (0x38D1B717); NaN/Inf compare above it.
#define LOOK_EPSILON_BITS 0x38D1B717

.globl s3bx_theatre
s3bx_theatre:
    sub     rsp, 0x50
    call    qword ptr [rip + IAT_TICK64]
    cmp     rax, qword ptr [rip + s3bx_next_ms]
    jb      .Ldone
    mov     qword ptr [rsp + 0x28], rax
    lea     rcx, [rax + 250]
    mov     qword ptr [rip + s3bx_next_ms], rcx

    lea     rcx, [rip + s3bx_wstr_halo4]
    call    qword ptr [rip + IAT_GETMODW]
    test    rax, rax
    jz      .Lbad
    cmp     rax, qword ptr [rip + s3bx_base]
    je      .Lhave
    mov     rdx, qword ptr [rax + 0x12CA85]
    movabs  rcx, 0xB94100F2A78D158B
    cmp     rdx, rcx
    jne     .Lbad
    cmp     dword ptr [rax + 0x12CA8D], 0x28
    jne     .Lbad
    mov     qword ptr [rip + s3bx_base], rax
.Lhave:
    mov     ecx, dword ptr [rax + 0x1057218]
    cmp     ecx, 256
    jae     .Lbad
    mov     r11, qword ptr gs:[0x58]
    test    r11, r11
    jz      .Lbad
    mov     r11, qword ptr [r11 + rcx*8]     // engine TLS block
    test    r11, r11
    jz      .Lbad
    mov     rdx, qword ptr [r11 + 0xC8]      // cinematic globals
    test    rdx, rdx
    jz      .Lnull
    movzx   r10d, byte ptr [rdx + 5]         // in-progress byte
    test    r10d, r10d
    jz      .Lidle

    // --- cinematic in progress: does the player still own the look? ---
    mov     rdx, qword ptr [r11 + 0x58]      // cinematic camera
    test    rdx, rdx
    jz      .Lunknown
    cmp     word ptr [rdx + 2], 6            // engine's own camera type
    jne     .Lunknown
    mov     eax, dword ptr [rdx + 0xC8]      // current maximum look angles
    and     eax, 0x7FFFFFFF
    cmp     eax, LOOK_EPSILON_BITS
    ja      .Lfree
    mov     eax, dword ptr [rdx + 0xCC]
    and     eax, 0x7FFFFFFF
    cmp     eax, LOOK_EPSILON_BITS
    ja      .Lfree
    mov     eax, dword ptr [rdx + 0xD0]
    and     eax, 0x7FFFFFFF
    cmp     eax, LOOK_EPSILON_BITS
    ja      .Lfree
    mov     eax, dword ptr [rdx + 0xD4]
    and     eax, 0x7FFFFFFF
    cmp     eax, LOOK_EPSILON_BITS
    ja      .Lfree
    mov     ecx, dword ptr [rdx + 0xF0]      // interpolation ticks remaining
    test    ecx, ecx
    js      .Lunknown                        // negative = unusable state
    cmp     ecx, 360000
    ja      .Lunknown
    test    ecx, ecx
    jz      .Llocked                         // nothing pending
    mov     eax, dword ptr [rdx + 0xD8]      // pending per-tick rates
    and     eax, 0x7FFFFFFF
    cmp     eax, LOOK_EPSILON_BITS
    ja      .Lfree
    mov     eax, dword ptr [rdx + 0xDC]
    and     eax, 0x7FFFFFFF
    cmp     eax, LOOK_EPSILON_BITS
    ja      .Lfree
    mov     eax, dword ptr [rdx + 0xE0]
    and     eax, 0x7FFFFFFF
    cmp     eax, LOOK_EPSILON_BITS
    ja      .Lfree
    mov     eax, dword ptr [rdx + 0xE4]
    and     eax, 0x7FFFFFFF
    cmp     eax, LOOK_EPSILON_BITS
    ja      .Lfree
.Llocked:
    mov     eax, 4                           // authored lock -> theatre
    jmp     .Llog
.Lfree:
    mov     eax, 5                           // look freedom -> immersive 6DOF
    jmp     .Llog
.Lunknown:
    mov     eax, 6                           // constraints unreadable
    jmp     .Llog
.Lidle:
    mov     eax, 3
    jmp     .Llog
.Lbad:
    mov     eax, 1
    jmp     .Llog
.Lnull:
    mov     eax, 2
.Llog:
    mov     dword ptr [rsp + 0x34], eax
    movzx   edx, byte ptr [rip + s3bx_prev]
    cmp     eax, edx
    je      .Lpublish
    mov     byte ptr [rip + s3bx_prev], al
    mov     r8d, eax
    lea     rcx, [rip + s3bx_fmt]
    call    LOGFN

.Lpublish:
    mov     eax, dword ptr [rsp + 0x34]
    cmp     eax, 3
    jb      .Ldone                           // 1/2: no proof
    cmp     eax, 6
    je      .Ldone                           // Unknown: publish nothing
    lea     rcx, [rip + GTR]
    mov     edx, 4                           // GameTitle::Halo4
    call    GEN_FN
    test    eax, eax
    jz      .Ldone
    mov     dword ptr [rsp + 0x30], eax
    mov     r8d, dword ptr [rsp + 0x34]
    cmp     r8d, 4
    mov     r8d, 1                           // PlayerControlled
    je      .Lauthored
    jmp     .Lctrl
.Lauthored:
    mov     r8d, 2                           // AuthoredLocked
.Lctrl:
    mov     edx, eax                         // generation
    mov     r9, qword ptr [rsp + 0x28]
    mov     ecx, 4
    call    PUB_CTRL
    mov     eax, dword ptr [rip + BBW]
    test    eax, eax
    jle     .Ldone
    mov     ecx, dword ptr [rip + BBH]
    test    ecx, ecx
    jle     .Ldone
    cvtsi2ss xmm2, eax
    cvtsi2ss xmm1, ecx
    divss   xmm2, xmm1                       // authored raster aspect
    mov     edx, dword ptr [rsp + 0x30]
    mov     r9, qword ptr [rsp + 0x28]
    mov     ecx, 4
    call    PUB_PROJ
.Ldone:
    add     rsp, 0x50
    jmp     S3BU_THUNK

// ---------------------------------------------------------------------------
s3bx_fmt:
    .asciz "H4CINE: state %d -> %d [1=no proof, 2=member null, 3=gameplay, 4=cinematic LOCKED (theatre), 5=cinematic but look is free (immersive), 6=constraints unreadable]"
s3bx_wstr_halo4:
    .short 0x68, 0x61, 0x6C, 0x6F, 0x34, 0x2E, 0x64, 0x6C, 0x6C, 0
    .balign 8
s3bx_next_ms:
    .quad 0
s3bx_base:
    .quad 0
s3bx_prev:
    .byte 0
