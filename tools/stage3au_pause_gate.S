// Stage 3AU - stop the Halo 4 pause-exit from immediately re-pausing.
//
// Measured, 2026-08-28 08:38 Steam log:
//
//   33.591  controller edge: B
//   33.591  Halo 4 pause exit: B edge while head-locked pause target active
//           -> stereo restore requested
//   33.599  pause transition: fade out -> stereo 3D
//   33.600  controller edge: Y                       <- 9 ms later
//   33.600  pause fallback: injecting Start, target=head-locked 2D
//   33.608  pause transition: fade out -> head-locked 2D
//   33.817  pause transition: presentation switched to head-locked 2D
//
// The pause chord is Y+B. Pressing B to LEAVE the pause menu satisfies the
// Halo 4 pause-exit edge (which correctly restores stereo) and then the
// trailing Y of the same physical press completes the chord again, injecting a
// second Start. The game leaves its pause menu while the mod goes back to
// head-locked 2D presentation and stays there - the black screen. Nothing
// after 33.817 ever switches it back.
//
// Fix: refuse Input_RequestPauseToggle for 400 ms after a Halo 4 pause-exit
// restore. Stage 3X already timestamps exactly that event at PAUSE_GRACE_RVA,
// so no new state is introduced. A grace of zero (no restore has ever
// happened) takes the stock path unchanged.

.intel_syntax noprefix
.text

.global stage3au_pause_gate

stage3au_pause_gate:
    // Displaced prologue of Input_RequestPauseToggle.
    sub rsp, 0x28

    cmp QWORD PTR [rip + PAUSE_GRACE_RVA], 0
    je .Lstock
    sub rsp, 0x20                                   // shadow space
    call QWORD PTR [rip + IAT_GET_TICK_COUNT64_RVA]
    add rsp, 0x20
    sub rax, QWORD PTR [rip + PAUSE_GRACE_RVA]
    cmp rax, 400
    jb .Lrefuse                                     // inside the grace window

.Lstock:
    // Displaced call to Game_AllowsPauseToggleInput, then back into the body
    // at its `test al, al`.
    call ALLOWS_TOGGLE_RVA
    jmp TOGGLE_CONTINUE_RVA

.Lrefuse:
    // The function's own refusal epilogue: add rsp, 0x28 / ret.
    jmp TOGGLE_REFUSE_RVA
