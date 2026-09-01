// Stage 3BS - Halo 4 uploads never sit behind the half-ink hold.
//
// The 16:20 log (Stage 3BQ, Steam): after firing, the capture's ink jumps
// ~97 -> ~1000 (bloom); the guard then treats the calm reticle (97) as
// suspiciously blank - `required = goodInk/2` - and refuses updates
// (`blankHeld 2..4`, whole 2 s windows with 0 uploads) until the 24-hold
// escape hatch. That is the reported "losing updates". The half-ink bar
// exists for titles whose identity key changes on a weapon swap; Halo 4
// publishes the fixed key 1, so `identityChanged` is false forever and the
// bar is applied to a widget whose ink legitimately swings >10x (bloom,
// red-on-enemy 45).
//
// Fix: for the active title Halo 4, force the `identityChanged` argument
// true at the top of UploadAuthoredReticle - exactly the treatment a weapon
// swap already gets: required = 0, holds counter cleared. A truly blank
// capture is still refused by the `ink >= kAuthoredReticleArtAlphaThreshold`
// bar, and the known-good machinery is otherwise untouched.
//
// Splice: 13 bytes at 0x2A701 (`movzx esi,dl` capturing the argument,
// `mov rax,[rip+0x283d85]` = [0x2AE490], `movzx ebp,cl`) become a call here
// + 8 nops. esi = identityChanged for the rest of the function.

.intel_syntax noprefix
.text

.globl s3bs_h4_identity
s3bs_h4_identity:
    movzx   esi, dl                            // displaced
    cmp     byte ptr [rip + ACTIVE_TITLE], 4   // Halo 4?
    jne     .Lkeep
    mov     esi, 1                             // every upload = fresh identity
.Lkeep:
    mov     rax, qword ptr [rip + UPLOAD_GLOB] // displaced
    movzx   ebp, cl                            // displaced
    ret
