// Stage 3AQ - admit Halo 4's authored-reticle capture through the render-target
// redirect gate.
//
// VR_RedirectRenderTargets refuses every bind that is not inside an owned eye
// raster:
//
//     if ((!scope && !nativeHud && (eye < 0 || eye > 1)) || ...) return false;
//
// The Halo 4 capture reroute - the ONLY thing that puts Halo 4's CUI draws into
// the private authored-reticle texture, because Halo 4 rebinds the learned
// scene target inside CUI playback - sits below that refusal. When the gameplay
// CUI replay runs with no raster eye published, the refusal fires first, the
// engine's own scene bind stands, and the capture texture is never drawn into.
// That is the reported "0 exact capture OM reroutes ... art 0, pieces 0".
//
// This wrapper replaces the eye-range test with: admit unconditionally while
// Halo 4 has a capture open, otherwise perform the original test unchanged.
// It is strictly permissive and strictly Halo 4:
//   - the active-title byte must read GameTitle::Halo4 (4), and
//   - g_reticleCaptureState.active must be set.
// Every other title, and Halo 4 with no capture open, executes the identical
// `cmp r9d, 1 / ja refuse` it executed before.
//
// The block this admits re-checks capture state, title, and target validity on
// its own before touching a single bind.

.intel_syntax noprefix
.text
.global stage3aq_capture_eye_gate

.extern TITLE_ACTIVE_BYTE_RVA
.extern RETICLE_CAPTURE_ACTIVE_RVA
.extern REDIRECT_CONTINUE_RVA
.extern REDIRECT_REFUSE_RVA

stage3aq_capture_eye_gate:
    // GameTitle::Halo4 == 4. Read the adapter's byte directly rather than
    // calling the getter: rdx, r9d, r12d, r14, r15b and rsi are all live here
    // and a call would clobber the volatile set.
    cmp BYTE PTR [rip + TITLE_ACTIVE_BYTE_RVA], 4
    jne .Lstock
    cmp BYTE PTR [rip + RETICLE_CAPTURE_ACTIVE_RVA], 0
    jne .Ladmit

.Lstock:
    // Exact original instruction pair.
    cmp r9d, 1
    ja  REDIRECT_REFUSE_RVA

.Ladmit:
    jmp REDIRECT_CONTINUE_RVA
