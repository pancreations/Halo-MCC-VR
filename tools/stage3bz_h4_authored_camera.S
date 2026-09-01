// Stage 3BZ - Halo 4 authored cutscene camera and FOV ownership.
//
// Stage 3BY failed in-headset: the shared theatre presentation entered, but
// Halo4StereoTransaction continued applying HMD head pose and OpenXR FOV.  Its
// helper query did not observe the state at this camera consumer.  Do not stack
// that failed candidate: 3BZ is built directly from the accepted 3BX input.
//
// 3BX's own H4EK/retail-proven classifier publishes and logs byte state 4 only
// while the cinematic camera is type 6 and all current/pending player-look
// constraints are zero.  The 2026-08-30 headset log proves it held state 4 for
// the complete room-fixed theatre interval and returned to state 3 before the
// existing smooth immersive handoff.
//
// This wrapper replaces only Halo4StereoTransaction's existing call to
// VR_Halo4GetRenderSnapshot.  State 4 clears the three composition-valid bits:
//   eyes[0].fovValid  +0x2C
//   eyes[1].fovValid  +0x5C
//   headPoseValid     +0x7C
// The per-eye positions/orientations stay untouched, retaining Halo 4's
// existing stereo eye/IPD transaction without claiming a headset result for
// cutscene depth.  Every other state is a strict pass-through.
//
// defsyms: SNAPSHOT_FN=0x2EA60, S3BX_STATE=0x2FAFE0

.intel_syntax noprefix
.text

.globl s3bz_get_render_snapshot
s3bz_get_render_snapshot:
    sub     rsp, 0x28
    mov     qword ptr [rsp + 0x20], rcx
    call    SNAPSHOT_FN
    test    al, al
    jz      .Ldone
    cmp     byte ptr [rip + S3BX_STATE], 4
    jne     .Ldone
    mov     rcx, qword ptr [rsp + 0x20]
    mov     byte ptr [rcx + 0x2C], 0
    mov     byte ptr [rcx + 0x5C], 0
    mov     byte ptr [rcx + 0x7C], 0
.Ldone:
    add     rsp, 0x28
    ret
