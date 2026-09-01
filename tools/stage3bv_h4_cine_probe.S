// Stage 3BV - Halo 4 cinematic-globals probe (log-only, fail-open).
//
// Purpose. The cutscene theatre needs a proven Halo 4 "authored cinematic in
// progress" state before TitleCapability_CutsceneTheater can ever be granted
// (docs/CUTSCENE-THEATRE-EVIDENCE.md "Future titles"). This stage adds ONLY a
// detector probe: it changes nothing the player sees and publishes nothing;
// it writes H4CINE log lines on state transitions so one watched cutscene
// proves (or disproves) the byte's semantics, exactly like Reach's accepted
// REACHCINE probe did.
//
// Evidence (H4EK-first, retail verifies - docs/HALO4-SIGNATURE-EVIDENCE.md):
//  - H4EK halo4_tag_test.exe registers game-state member "cinematic globals"
//    (0x54 bytes, from blofeld\cutscene\cinematics.cpp) and caches its pointer
//    in the engine TLS block; a null-checked kit leaf getter (0x234CD0,
//    161 callers) returns BYTE +5 of that member - the same "+5 = cinematic
//    active" layout proven for Halo 3/ODST.
//  - Retail halo4.dll registers the same two members ("cinematic globals"
//    0x54 / "... non deterministic" 0x28) at its unique registration site
//    0x12CA24: TLS index dword at halo4+0x1057218, member pointer cached at
//    TLS block +0xC8. The retail getter 0x12EF68 null-checks that pointer and
//    returns byte +5; the teardown path 0x12CCE2 writes byte +5 = 0.
//
// Splice. The Stage 3BU steady-state `jne` at 0x2C2BC (taken once per claimed
// Halo 4 eye end) is re-pointed here; this thunk ends with `jmp S3BU_THUNK`
// (0x2F9D10), so the accepted 3BU scene write-back runs unchanged after it.
// At entry rsp is 16-aligned; only volatile registers are touched; the 3BU
// thunk re-reads all of its own state from globals.
//
// Behavior. At most one sample per 250 ms (GetTickCount64 gate; on other
// ticks this is a compare + branch). A sample resolves halo4.dll through the
// import table, byte-verifies the unique registration site before trusting
// any offset, then reads the in-progress byte. The composite state is:
//   1 = proof unavailable (module missing/mismatched, TLS unreadable)
//   2 = cinematic globals pointer is null (member not allocated)
//   3 = cinematic globals present, in-progress byte +5 == 0
//   4 = cinematic globals present, in-progress byte +5 != 0
// A LOG line is written only when the state changes; bytes +4 and +5 ride
// along for layout evidence (-1 when unreadable). Fail-open: any failed check
// degrades to state 1 and the 3BU/engine path continues untouched.
//
// defsyms: LOGFN=0x1D90, IAT_TICK64=0x180150, IAT_GETMODW=0x1800F8,
//          S3BU_THUNK=0x2F9D10
// halo4.dll facts (byte-verified before use): REG_VERIFY_RVA=0x12CA85
// (8B 15 8D A7 F2 00 41 B9 | 28 00 00 00), TLSIDX_RVA=0x1057218,
// member slot +0xC8, in-progress byte +5.

.intel_syntax noprefix
.text

.globl s3bv_probe
s3bv_probe:
    sub     rsp, 0x40                        // 16-aligned; 0x20 shadow + args
    call    qword ptr [rip + IAT_TICK64]     // rax = now (ms)
    cmp     rax, qword ptr [rip + s3bv_next_ms]
    jb      .Ldone
    lea     rcx, [rax + 250]                 // next sample >= now + 250 ms
    mov     qword ptr [rip + s3bv_next_ms], rcx

    lea     rcx, [rip + s3bv_wstr_halo4]
    call    qword ptr [rip + IAT_GETMODW]    // rax = halo4.dll base or 0
    test    rax, rax
    jz      .Lbad
    cmp     rax, qword ptr [rip + s3bv_base] // already byte-verified?
    je      .Lhave
    // Verify the unique registration site before trusting any offset:
    // halo4+0x12CA85: mov edx,[rip->tlsIndex] ; mov r9d,0x28
    mov     rdx, qword ptr [rax + 0x12CA85]
    movabs  rcx, 0xB94100F2A78D158B
    cmp     rdx, rcx
    jne     .Lbad
    cmp     dword ptr [rax + 0x12CA8D], 0x28
    jne     .Lbad
    mov     qword ptr [rip + s3bv_base], rax
.Lhave:
    mov     ecx, dword ptr [rax + 0x1057218] // engine module TLS index
    cmp     ecx, 256                         // same bound the mod's own
    jae     .Lbad                            // H3/ODST TLS readers enforce
    mov     rdx, qword ptr gs:[0x58]
    test    rdx, rdx
    jz      .Lbad
    mov     rdx, qword ptr [rdx + rcx*8]     // engine TLS block, this thread
    test    rdx, rdx
    jz      .Lbad
    mov     rdx, qword ptr [rdx + 0xC8]      // cached "cinematic globals"
    test    rdx, rdx
    jz      .Lnull
    movzx   r9d,  byte ptr [rdx + 4]
    movzx   r10d, byte ptr [rdx + 5]         // the in-progress byte
    xor     eax, eax
    test    r10d, r10d
    setne   al
    add     eax, 3                           // 3 = idle, 4 = cinematic
    jmp     .Lcompare
.Lbad:
    mov     eax, 1
    or      r9d, -1
    or      r10d, -1
    jmp     .Lcompare
.Lnull:
    mov     eax, 2
    or      r9d, -1
    or      r10d, -1
.Lcompare:
    movzx   edx, byte ptr [rip + s3bv_prev]
    cmp     eax, edx
    je      .Ldone                           // steady state: no logging
    mov     byte ptr [rip + s3bv_prev], al
    mov     r8d, eax                         // new state
    mov     dword ptr [rsp + 0x20], r10d     // 5th vararg: byte +5
    lea     rcx, [rip + s3bv_fmt]
    call    LOGFN
.Ldone:
    add     rsp, 0x40
    jmp     S3BU_THUNK                       // accepted 3BU write-back

// ---------------------------------------------------------------------------
s3bv_fmt:
    .asciz "H4CINE: cinematic globals state %d -> %d (byte +4=%d, +5=%d) [1=no proof, 2=member null, 3=idle, 4=cinematic]"
s3bv_wstr_halo4:
    .short 0x68, 0x61, 0x6C, 0x6F, 0x34, 0x2E, 0x64, 0x6C, 0x6C, 0
    .balign 8
s3bv_next_ms:
    .quad 0
s3bv_base:
    .quad 0
s3bv_prev:
    .byte 0
