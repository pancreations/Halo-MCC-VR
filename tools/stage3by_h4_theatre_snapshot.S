// Stage 3BY - Halo 4 authored-theatre camera ownership.
//
// This replaces only Halo4StereoTransaction's call to the already-proven
// VR_Halo4GetRenderSnapshot.  It first returns the ordinary snapshot, then
// asks the shared compositor whether the fully-black handoff has made the
// authored theatre active.  Only then it suppresses the snapshot inputs that
// compose headset head tracking and OpenXR projection FOV onto Halo's camera.
//
// The two eye positions and orientations are deliberately left untouched:
// the existing accepted per-eye 3D presentation (including its depth/IPD
// split) continues through a cutscene.  Outside the compositor-owned theatre
// screen this wrapper is bit-for-bit behavioral pass-through.
//
// Halo4VrRenderSnapshot layout, verified from src/dll/vr.h:
//   eyes[0].fovValid  +0x2C
//   eyes[1].fovValid  +0x5C
//   headPoseValid     +0x7C
//
// defsyms: SNAPSHOT_FN=0x2EA60, THEATRE_ACTIVE=0x2EE60

.intel_syntax noprefix
.text

.globl s3by_get_render_snapshot
s3by_get_render_snapshot:
    // At a Windows x64 call boundary RSP is 8 mod 16.  Reserve shadow space,
    // keep the output pointer across both calls, and preserve the first call's
    // boolean result (the compositor query itself also returns a bool).
    sub     rsp, 0x38
    mov     qword ptr [rsp + 0x20], rcx
    call    SNAPSHOT_FN
    test    al, al
    jz      .Ldone
    mov     byte ptr [rsp + 0x28], al

    // This is the exact shared presentation state: it changes at full black,
    // so entry and exit retain the established smooth transition.
    call    THEATRE_ACTIVE
    test    al, al
    jz      .Lrestore

    mov     rcx, qword ptr [rsp + 0x20]
    mov     byte ptr [rcx + 0x2C], 0
    mov     byte ptr [rcx + 0x5C], 0
    mov     byte ptr [rcx + 0x7C], 0

.Lrestore:
    mov     al, byte ptr [rsp + 0x28]
.Ldone:
    add     rsp, 0x38
    ret
