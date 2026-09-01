// Stage 3BW - Halo 4 joins the shared 3D cutscene theatre.
//
// The 3BV probe proved the H4EK-evidenced cinematic byte live in the headset
// (2026-08-30 18:13:28 state 0->4 across the authored intro sequence,
// 18:18:04 state 4->3 at the exact second the user regained control, user-
// confirmed from inside the headset). This stage turns the proven detector
// into the two publications the title-blind theatre requires
// (docs/CUTSCENE-THEATRE-EVIDENCE.md "Future titles"):
//
//   1. TitleAdapter_PublishCinematicControl(Halo4, generation,
//      AuthoredLocked/PlayerControlled, nowMs)      [0x88400]
//   2. TitleAdapter_PublishCutsceneTheaterProjection(Halo4, generation,
//      rasterAspect, nowMs)                          [0x884C0]
//
// plus the TitleCapability_CutsceneTheater (1<<8) grant patched by the
// builder into both compiled capability masks (registry descriptor dword and
// the kHalo4RuntimeCapabilities lifecycle immediate). Everything else - the
// room-fixed screen, F1 Theatre category, enable checkbox, width/distance,
// cine bars, flip depth, entry/exit fades - is the existing shared theatre.
//
// Splice: the jne at 0x2C2BC (3BV) now lands here; this thunk ends with
// `jmp S3BU_THUNK` so the accepted 3BU scene write-back still runs. The 3BV
// payload at 0x2F9E10 stays byte-identical but unreachable; this thunk
// carries the same probe (same H4CINE transition logging, same 250 ms gate,
// same registration byte-verification) with the publications appended.
//
// Publication cadence: once per 250 ms sample, well inside the 500 ms
// freshness window. On unclaimed frames (pause, menu, teardown) no samples
// occur, the publication goes stale, and the shared resolver returns
// Unknown - the theatre exits on its own. States 1/2 (no proof / member
// null) publish nothing for the same fail-open reason. Generation comes from
// TitleRuntime::Generation(Halo4) [0xA5A0, rcx = &g_titleRuntime]; a zero
// generation publishes nothing. The authored aspect is the raster shape the
// engine draws this cutscene into: backbuffer W/H (0x2AEB58/5C, the
// 3BN/3BR-proven globals), 3786x2730 = 1.387:1, inside the publisher's
// 0.25..4.0 bounds; the shared cine-bar matte crops it to the configured
// shape exactly as it does for Halo 3.
//
// Known 3BW limit (next stage): the captured eyes still carry head-look
// during theatre, so the picture on the screen pans with the head until the
// authored-camera lock (3BX) lands.
//
// defsyms: LOGFN=0x1D90, IAT_TICK64=0x180150, IAT_GETMODW=0x1800F8,
//          S3BU_THUNK=0x2F9D10, PUB_CTRL=0x88400, PUB_PROJ=0x884C0,
//          GEN_FN=0xA5A0, GTR=0x2BA538, BBW=0x2AEB58, BBH=0x2AEB5C

.intel_syntax noprefix
.text

.globl s3bw_theatre
s3bw_theatre:
    sub     rsp, 0x50                        // 16-aligned; shadow + spills
    call    qword ptr [rip + IAT_TICK64]
    cmp     rax, qword ptr [rip + s3bw_next_ms]
    jb      .Ldone
    mov     qword ptr [rsp + 0x28], rax      // heartbeat for both publishes
    lea     rcx, [rax + 250]
    mov     qword ptr [rip + s3bw_next_ms], rcx

    lea     rcx, [rip + s3bw_wstr_halo4]
    call    qword ptr [rip + IAT_GETMODW]
    test    rax, rax
    jz      .Lbad
    cmp     rax, qword ptr [rip + s3bw_base]
    je      .Lhave
    mov     rdx, qword ptr [rax + 0x12CA85]  // unique registration site
    movabs  rcx, 0xB94100F2A78D158B
    cmp     rdx, rcx
    jne     .Lbad
    cmp     dword ptr [rax + 0x12CA8D], 0x28
    jne     .Lbad
    mov     qword ptr [rip + s3bw_base], rax
.Lhave:
    mov     ecx, dword ptr [rax + 0x1057218] // engine module TLS index
    cmp     ecx, 256
    jae     .Lbad
    mov     rdx, qword ptr gs:[0x58]
    test    rdx, rdx
    jz      .Lbad
    mov     rdx, qword ptr [rdx + rcx*8]
    test    rdx, rdx
    jz      .Lbad
    mov     rdx, qword ptr [rdx + 0xC8]      // cinematic globals member
    test    rdx, rdx
    jz      .Lnull
    movzx   r9d,  byte ptr [rdx + 4]
    movzx   r10d, byte ptr [rdx + 5]         // in-progress byte (proven)
    xor     eax, eax
    test    r10d, r10d
    setne   al
    add     eax, 3                           // 3 = idle, 4 = cinematic
    jmp     .Llog
.Lbad:
    mov     eax, 1
    or      r9d, -1
    or      r10d, -1
    jmp     .Llog
.Lnull:
    mov     eax, 2
    or      r9d, -1
    or      r10d, -1
.Llog:
    mov     dword ptr [rsp + 0x34], eax      // composite state
    movzx   edx, byte ptr [rip + s3bw_prev]
    cmp     eax, edx
    je      .Lpublish
    mov     byte ptr [rip + s3bw_prev], al
    mov     r8d, eax
    mov     dword ptr [rsp + 0x20], r10d     // 5th vararg: byte +5
    lea     rcx, [rip + s3bw_fmt]
    call    LOGFN
.Lpublish:
    mov     eax, dword ptr [rsp + 0x34]
    cmp     eax, 3
    jb      .Ldone                           // no proof: stale -> Unknown
    lea     rcx, [rip + GTR]                 // &g_titleRuntime
    mov     edx, 4                           // GameTitle::Halo4
    call    GEN_FN                           // eax = generation (0 = none)
    test    eax, eax
    jz      .Ldone
    mov     dword ptr [rsp + 0x30], eax
    mov     edx, eax                         // generation
    mov     r8d, dword ptr [rsp + 0x34]
    sub     r8d, 2                           // 3->PlayerControlled(1), 4->AuthoredLocked(2)
    mov     r9, qword ptr [rsp + 0x28]       // heartbeat
    mov     ecx, 4
    call    PUB_CTRL
    mov     eax, dword ptr [rip + BBW]       // raster width (3BN/3BR-proven)
    test    eax, eax
    jle     .Ldone
    mov     ecx, dword ptr [rip + BBH]
    test    ecx, ecx
    jle     .Ldone
    cvtsi2ss xmm2, eax
    cvtsi2ss xmm1, ecx
    divss   xmm2, xmm1                       // authored raster aspect
    mov     edx, dword ptr [rsp + 0x30]      // generation
    mov     r9, qword ptr [rsp + 0x28]       // heartbeat
    mov     ecx, 4
    call    PUB_PROJ
.Ldone:
    add     rsp, 0x50
    jmp     S3BU_THUNK                       // accepted 3BU write-back

// ---------------------------------------------------------------------------
s3bw_fmt:
    .asciz "H4CINE: cinematic globals state %d -> %d (byte +4=%d, +5=%d) [1=no proof, 2=member null, 3=idle, 4=cinematic]"
s3bw_wstr_halo4:
    .short 0x68, 0x61, 0x6C, 0x6F, 0x34, 0x2E, 0x64, 0x6C, 0x6C, 0
    .balign 8
s3bw_next_ms:
    .quad 0
s3bw_base:
    .quad 0
s3bw_prev:
    .byte 0
