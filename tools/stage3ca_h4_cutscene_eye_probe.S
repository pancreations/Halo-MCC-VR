// Stage 3CA - measure Halo 4's actual cutscene eye images once per entry.
//
// This is diagnostic-only.  It changes no camera, FOV, capture, presentation,
// configuration, title capability, or gameplay path.  The existing BX state-4
// proof resets the existing M2 eye-pixel validator exactly once, causing its
// next Present to compare the two captured Halo 4 cutscene caches.  The local
// latch prevents the validator's blocking staging read from repeating.
//
// defsyms: S3BX_STATE=0x2FAFE0, VALIDATION_DONE=0x2AE85F,
//          S3BX_THUNK=0x2FACB0

.intel_syntax noprefix
.text

.globl s3ca_probe
s3ca_probe:
    movzx   eax, byte ptr [rip + S3BX_STATE]
    cmp     eax, 4
    jne     .Lnot_locked
    cmp     byte ptr [rip + s3ca_latched], 0
    jne     .Lchain
    mov     byte ptr [rip + s3ca_latched], 1
    mov     byte ptr [rip + VALIDATION_DONE], 0
    jmp     .Lchain
.Lnot_locked:
    mov     byte ptr [rip + s3ca_latched], 0
.Lchain:
    jmp     S3BX_THUNK
s3ca_latched:
    .byte 0
