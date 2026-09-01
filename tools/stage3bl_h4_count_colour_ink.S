// Stage 3BL - let Halo 4's ink measurement see the ink that is there.
//
// The 2026-08-29 00:00 log closed the chain: the capture map holds the
// reticle centred (rows 15-17, cols 13-18) while every upload window says
// `art 0, blankHeld ~230` - the publish guard measures ONLY the alpha
// channel and Halo 4's CUI draws its reticle without writing alpha, so a
// picture of the reticle is judged "blank" and the placeholder is kept.
//
// An earlier welded stage already instrumented the 8x8 probe loop
// (0x2FA25B, entered from 0x1DD8B): it sums alpha into EDI (the published
// ink) AND the RGB channels into R10D (telemetry only).  The colour ink
// has been measured all along and thrown away.
//
// This stage replaces the loop's 8-byte tail (0x2FA2D7: xor r8d,r8d;
// jmp 0x1DDD8) with a jump here: for the active title Halo 4 ONLY, when
// the colour sum exceeds the alpha sum, publish the colour sum.  Halo 4's
// alpha is measured ~0, so its ink metric becomes the RGB sum
// consistently - self-consistent for the guard's half-ink ratio.  Every
// other title keeps its alpha-only measure bit-for-bit.
//
// Registers here: EDI = alpha ink (returned as eax at 0x1DDF0),
// R10D = RGB ink, R8D must be zeroed for the tail path (as the displaced
// xor did).  ACTIVE_TITLE is the byte TitleAdapter_GetActiveTitle reads;
// Halo4 = 4.

.intel_syntax noprefix
.text

.globl s3bl_colour_ink
s3bl_colour_ink:
    cmp     byte ptr [rip + ACTIVE_TITLE], 4
    jne     .Ltail
    cmp     edi, r10d
    jae     .Ltail
    mov     edi, r10d              // publish the measured colour ink
.Ltail:
    xor     r8d, r8d               // displaced instruction
    jmp     MEASURE_TAIL           // Unmap + return edi
